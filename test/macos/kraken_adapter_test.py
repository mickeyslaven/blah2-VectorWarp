#!/usr/bin/env python3
"""Exercise the real foreground owner with a fake native telemetry process.

This covers process ownership/readiness; native SDK/DSP simulation is separate
in test/macos/kraken. None of these tests opens hardware.
"""
import json
import importlib.util
import os
from pathlib import Path
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location('kraken_lifecycle', ROOT / 'script/vectorwarp-macos.py')
LIFECYCLE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(LIFECYCLE)
FAKE = '''#!PYTHON
import json, os, signal, socket, sys, time
from pathlib import Path
Path('opened.json').write_text(json.dumps({'pid':os.getpid(),'cwd':os.getcwd(),'env':{k:v for k,v in os.environ.items() if k.startswith('HEIMDALL_')}}))
def stop(*args):
 Path('closed').write_text('clean'); sys.exit(0)
signal.signal(signal.SIGTERM,stop)
if os.getenv('FAKE_MODE')=='exit': sys.exit(17)
s=socket.socket(); s.setsockopt(socket.SOL_SOCKET,socket.SO_REUSEADDR,1)
s.bind(('127.0.0.1',int(os.environ['HEIMDALL_CONTROL_PORT']))); s.listen()
while True:
 c,_=s.accept()
 message={'instance_token':os.environ['HEIMDALL_INSTANCE_TOKEN'],'settings':{'center_freq':int(os.environ['HEIMDALL_CENTER_FREQ_HZ']),'sample_rate':2400000,'gain':28.0},'num_channels':5,'operating_mode':'coherent','calibration_state':'PENDING'}
 if os.getenv('FAKE_MODE')=='foreign': message['instance_token']='foreign'
 if os.getenv('FAKE_MODE')=='mismatch': message['settings']['sample_rate']=2000000
 if os.getenv('FAKE_MODE')=='failed': message['calibration_state']='FAILED'
 try: c.sendall((json.dumps(message)+'\\n').encode())
 except OSError: pass
 c.close()
'''


class AdapterTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='vectorwarp-kraken-adapter-')
        self.addCleanup(self.temporary.cleanup)
        self.state = Path(self.temporary.name)
        # Reserve together so all four ports are distinct.
        sockets = [socket.socket() for _ in range(4)]
        for sock in sockets:
            sock.bind(('127.0.0.1', 0))
        self.ports = [sock.getsockname()[1] for sock in sockets]
        for sock in sockets:
            sock.close()
        self.native = self.state / 'fake-native'
        self.native.write_text(FAKE.replace('PYTHON', sys.executable))
        self.native.chmod(0o700)
        (self.state / 'index.html').write_text('<html>fixture</html>')
        self.config = self.state / 'config.yml'
        self.profile = {'capture': {'fs': 2400000, 'fc': 123500000,
            'device': {'type': 'Kraken', 'channel_count': 5,
                       'heimdall': {'host': '127.0.0.1', 'port': self.ports[0],
                                    'control_port': self.ports[1], 'gain': 28}},
            'replay': {'state': False}}}
        self.env = dict(os.environ, VECTORWARP_MACOS_ROOT=str(ROOT),
            VECTORWARP_MACOS_NODE=shutil.which('node'),
            VECTORWARP_MACOS_HEIMDALL_EXECUTABLE=str(self.native),
            VECTORWARP_MACOS_HEIMDALL_HTML=str(self.state / 'index.html'),
            VECTORWARP_MACOS_HEIMDALL_WEB_PORT=str(self.ports[2]),
            VECTORWARP_MACOS_HEIMDALL_RTL_PORT=str(self.ports[3]))
        self.ready = self.state / 'ready.json'

    def launch(self, mode=''):
        self.config.write_text(json.dumps(self.profile))
        command = [sys.executable, str(ROOT / 'script/vectorwarp-kraken-macos.py'), 'run-local-v1',
            '--config', str(self.config), '--bind-host', '127.0.0.1', '--iq-port', str(self.ports[0]),
            '--control-port', str(self.ports[1]), '--state-dir', str(self.state / 'controller'),
            '--ready-file', str(self.ready), '--instance-token', '1234abcd']
        child = subprocess.Popen(command, env=dict(self.env, FAKE_MODE=mode),
                                 stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        def cleanup():
            if child.poll() is None:
                child.terminate()
                try:
                    child.wait(timeout=12)
                except subprocess.TimeoutExpired:
                    child.kill()
                    child.wait(timeout=3)
        self.addCleanup(cleanup)
        deadline = time.monotonic() + 8
        while not self.ready.exists() and child.poll() is None and time.monotonic() < deadline:
            time.sleep(.05)
        self.assertTrue(self.ready.exists(), f'No readiness document; status={child.poll()}')
        return child, json.loads(self.ready.read_text())

    def test_ready_pending_and_clean_stop_keep_private_working_directory(self):
        child, ready = self.launch()
        self.assertEqual(ready['status'], 'ready')
        self.assertEqual(ready['calibration'], 'PENDING')
        self.assertEqual(ready['pid'], child.pid)
        opened = json.loads((self.state / 'controller/opened.json').read_text())
        self.assertEqual(opened['cwd'], str((self.state / 'controller').resolve()))
        self.assertEqual(opened['env']['HEIMDALL_GAIN_TENTHS'], '280')
        child.send_signal(signal.SIGTERM)
        self.assertEqual(child.wait(timeout=12), 0)
        self.assertTrue((self.state / 'controller/closed').is_file())

    def test_mismatched_configuration_closes_owned_native_child(self):
        child, ready = self.launch('mismatch')
        self.assertEqual(ready['status'], 'error')
        self.assertEqual(child.wait(timeout=12), 1)
        self.assertTrue((self.state / 'controller/closed').is_file())

    def test_foreign_endpoint_is_not_ready(self):
        child, ready = self.launch('foreign')
        self.assertEqual(ready['status'], 'error')
        self.assertIn('different controller', ready['error'])
        self.assertEqual(child.wait(timeout=12), 1)

    def test_calibration_failure_is_not_ready(self):
        child, ready = self.launch('failed')
        self.assertEqual(ready['status'], 'error')
        self.assertIn('calibration failure', ready['error'])
        self.assertEqual(child.wait(timeout=12), 1)

    def test_no_device_exit_is_reported(self):
        child, ready = self.launch('exit')
        self.assertEqual(ready['status'], 'error')
        self.assertEqual(child.wait(timeout=12), 1)

    def test_replay_cannot_open_native_controller(self):
        self.profile['capture']['replay']['state'] = True
        child, ready = self.launch()
        self.assertEqual(child.wait(timeout=12), 1)
        self.assertEqual(ready['status'], 'error')
        self.assertFalse((self.state / 'controller/opened.json').exists())

    def test_occupied_port_fails_before_native_usb_open(self):
        with socket.socket() as busy:
            busy.bind(('127.0.0.1', self.ports[0]))
            child, ready = self.launch()
            self.assertEqual(child.wait(timeout=12), 1)
            self.assertEqual(ready['status'], 'error')
            self.assertFalse((self.state / 'controller/opened.json').exists())

    def test_closed_connection_does_not_block_restart(self):
        with socket.socket() as previous:
            previous.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            previous.bind(('127.0.0.1', self.ports[0]))
            previous.listen(1)
            with socket.create_connection(previous.getsockname(), timeout=2) as client:
                connection, _ = previous.accept()
                with connection:
                    connection.settimeout(2)
                    connection.shutdown(socket.SHUT_WR)
                    self.assertEqual(client.recv(1), b'')
                    client.shutdown(socket.SHUT_WR)
                    self.assertEqual(connection.recv(1), b'')
        # Prove this fixture retains the TCP state that rejected the old
        # preflight, although no server is listening any more.
        with socket.socket() as without_reuse:
            with self.assertRaises(OSError):
                without_reuse.bind(('127.0.0.1', self.ports[0]))
        child, ready = self.launch()
        self.assertEqual(ready['status'], 'ready')
        child.terminate()
        self.assertEqual(child.wait(timeout=12), 0)

    def test_active_reusable_listener_still_blocks_native_start(self):
        with socket.socket() as busy:
            busy.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            busy.bind(('127.0.0.1', self.ports[0]))
            busy.listen(1)
            child, ready = self.launch()
            self.assertEqual(child.wait(timeout=12), 1)
            self.assertEqual(ready['status'], 'error')
            self.assertFalse((self.state / 'controller/opened.json').exists())

    def test_readiness_reader_refuses_fifo_symlink_and_unbounded_file(self):
        fifo = self.state / 'ready-fifo'
        os.mkfifo(fifo, 0o600)
        started = time.monotonic()
        with self.assertRaises(RuntimeError):
            LIFECYCLE.bounded_private_json(fifo)
        self.assertLess(time.monotonic() - started, .5)
        regular = self.state / 'regular.json'
        regular.write_text('{"status":"ready"}')
        regular.chmod(0o600)
        self.assertEqual(LIFECYCLE.bounded_private_json(regular)['status'], 'ready')
        link = self.state / 'ready-link'
        link.symlink_to(regular)
        with self.assertRaises(OSError):
            LIFECYCLE.bounded_private_json(link)
        regular.write_bytes(b' ' * 65537)
        with self.assertRaises(RuntimeError):
            LIFECYCLE.bounded_private_json(regular)


if __name__ == '__main__':
    unittest.main()
