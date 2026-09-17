#!/usr/bin/env python3
"""Exercise real package hooks and launcher in a container, never radio hardware.

This is a same-version reinstall/migration test, not a claim to test every
historical package upgrade. The SDK is absent and no receiver is started.
"""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import socket
import sys
import time
import urllib.request


def run(*args):
    result = subprocess.run(args, check=True, text=True, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, timeout=180,
                            env=os.environ | {'DEBIAN_FRONTEND': 'noninteractive'})
    return result.stdout.strip()


def property_of(unit, name):
    return run('systemctl', 'show', '-p', name, '--value', unit)


def assert_restarted(unit, previous_pid):
    assert property_of(unit, 'ActiveState') == 'active', (unit, 'did not restart')
    new_pid = property_of(unit, 'MainPID')
    assert int(new_pid) > 0 and new_pid != previous_pid, (unit, previous_pid, new_pid)


def assert_stopped_stack():
    for unit in ('vectorwarp-api.service', 'vectorwarp-processor.service',
                 'vectorwarp-restart.service', 'vectorwarp-receiver.service',
                 'vectorwarp-receiver.socket'):
        state = property_of(unit, 'ActiveState')
        pid = property_of(unit, 'MainPID')
        assert state in ('inactive', 'failed'), (unit, state)
        # systemd sockets have no MainPID property at all.
        if unit == 'vectorwarp-receiver.socket':
            assert pid in ('', '0'), (unit, pid)
        else:
            assert pid == '0', (unit, pid)


def assert_management_stack():
    for unit in ('vectorwarp-api.service', 'vectorwarp-receiver.service',
                 'vectorwarp-receiver.socket'):
        assert property_of(unit, 'ActiveState') == 'active', (unit, 'not active')
        if unit != 'vectorwarp-receiver.socket':
            assert int(property_of(unit, 'MainPID')) > 0, (unit, 'missing PID')


def reinstall(package, success=True):
    if package.suffix == '.deb':
        args = ['apt-get', '--yes', '--reinstall', '-o', 'Dpkg::Options::=--force-confold', 'install', str(package)]
    else:
        args = ['dnf', '--assumeyes', 'reinstall', str(package)]
    result = subprocess.run(args, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            timeout=180, env=os.environ | {'DEBIAN_FRONTEND': 'noninteractive'})
    print(result.stdout)
    assert (result.returncode == 0) == success, (result.returncode, success, result.stdout)


def ready_replay(after):
    until = time.monotonic() + 50
    last = None
    while time.monotonic() < until:
        try:
            with urllib.request.urlopen('http://127.0.0.1:3000/api/system/status', timeout=2) as response:
                last = json.load(response)
            if (last.get('processorFresh') and last.get('processor', {}).get('input') == 'replay' and
                    last.get('processor', {}).get('state') == 'playing' and
                    (last.get('lastFrameAt') or 0) > after):
                return last
        except (OSError, ValueError):
            pass
        time.sleep(.2)
    raise AssertionError(('No fresh replay after installed upgrade/launcher action', last))


def activate_idle_broker():
    # A real socket-activated broker, not a replacement test server. The
    # deliberately invalid harmless request makes no privileged change.
    with socket.socket(socket.AF_UNIX) as client:
        client.settimeout(8)
        client.connect('/run/vectorwarp-receiver/management.sock')
        client.sendall(b'{"verb":"test-invalid-request"}\n')
        reply = json.loads(client.makefile('rb').readline(65536))
        assert reply['ok'] is False and reply['code'] == 'INVALID_REQUEST', reply
    return property_of('vectorwarp-receiver.service', 'MainPID')


assert os.geteuid() == 0 and Path('/run/.containerenv').exists()
package = Path(sys.argv[1])
assert package in (Path('/tmp/package.deb'), Path('/tmp/package.rpm')) and package.is_file()
assert sys.argv[2:] in ([], ['--running-replay'])
config = Path('/etc/vectorwarp/config.yml')
before = hashlib.sha256(config.read_bytes()).hexdigest()
assert Path('/usr/bin/vectorwarp').is_file(), 'Every DEB/RPM must install the launcher in PATH'
assert 'Usage: vectorwarp' in run('vectorwarp', '--help')
assert 'vectorwarp stop' in run('vectorwarp', 'help')
assert 'version=' in run('vectorwarp', 'version')

if sys.argv[2:] == ['--running-replay']:
    ready_replay(0)
    assert 'http://127.0.0.1:3000/' in run('vectorwarp')
    api_before = property_of('vectorwarp-api.service', 'MainPID')
    processor_before = property_of('vectorwarp-processor.service', 'MainPID')
    broker_before = activate_idle_broker()
    assert int(broker_before) > 0 and int(processor_before) > 0
    started = int(time.time() * 1000)
    reinstall(package)
    assert_restarted('vectorwarp-api.service', api_before)
    assert_restarted('vectorwarp-processor.service', processor_before)
    assert_restarted('vectorwarp-receiver.service', broker_before)
    assert hashlib.sha256(config.read_bytes()).hexdigest() == before
    ready_replay(started)
    print('PASS: reinstall restarts running API/broker/processor and produces fresh replay frames')
    run('vectorwarp', 'stop')
    assert_stopped_stack()
    started = int(time.time() * 1000)
    run('vectorwarp', 'start')
    assert_management_stack()
    ready_replay(started)
    started = int(time.time() * 1000)
    run('vectorwarp', 'restart')
    assert_management_stack()
    ready_replay(started)
    assert hashlib.sha256(config.read_bytes()).hexdigest() == before
    print('PASS: installed launcher full-stack stop/start/restart uses restricted processor and real replay')
    sys.exit(0)
# Do not repair a broken install hook by starting the API in the test. A fresh
# package must make its web interface available without an extra service command.
assert run('systemctl', 'is-enabled', 'vectorwarp-api.service') == 'enabled'
assert run('systemctl', 'is-active', 'vectorwarp-api.service') == 'active'
assert run('systemctl', 'show', '-p', 'ActiveState', '--value', 'vectorwarp-processor.service') == 'inactive'
pid = run('systemctl', 'show', '-p', 'MainPID', '--value', 'vectorwarp-api.service')
assert int(pid) > 0
assert 'http://127.0.0.1:3000/' in run('vectorwarp')
assert property_of('vectorwarp-api.service', 'MainPID') == pid, 'Opening must reuse the running API'
sudoers = Path('/etc/sudoers.d/vectorwarp')
assert sudoers.is_file() and not sudoers.is_symlink()
preserved = '# Administrator note: preserve this customization.\nroot ALL=(ALL) ALL\n'
sudoers.write_text(preserved + 'Defaults:vectorwarp-api !requiretty\n'
    'vectorwarp-api ALL=(root) NOPASSWD: /usr/bin/systemctl start --no-block vectorwarp-sdrplay-build.service\n')
sudoers.chmod(0o440)
broker_pid = activate_idle_broker()
assert int(broker_pid) > 0
reinstall(package)
assert sudoers.read_text() == preserved, 'Retire the known grant, preserving the administrator content'
assert hashlib.sha256(config.read_bytes()).hexdigest() == before, 'Reinstall changed user configuration'
assert_restarted('vectorwarp-api.service', pid)
assert_restarted('vectorwarp-receiver.service', broker_pid)
assert run('systemctl', 'show', '-p', 'ActiveState', '--value', 'vectorwarp-processor.service') == 'inactive'
assert 'http://127.0.0.1:3000/' in run('vectorwarp')
print('PASS: fresh install starts only API; reinstall refreshes API/broker, preserves config/customization and leaves processing stopped')
