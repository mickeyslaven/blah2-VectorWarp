"""Canonical replay lifecycle with loopback peers and tiny fake children, never radar/GPU."""
import importlib.util
import http.client
import json
import os
import signal
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("test_replay_session_owner", ROOT / "script/replay_session.py")
SESSION = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(SESSION)
CHILD = r'''
import json, pathlib, signal, socket, subprocess, sys, time, urllib.request
config = json.loads(pathlib.Path(sys.argv[2]).read_text())
mode = config['test_mode']
status = {'input': 'replay', 'state': 'playing', 'error': '', 'loops': 0,
          'positionSamples': 10, 'totalSamples': 10, 'trailingSamples': 0}
def post():
    request = urllib.request.Request('http://127.0.0.1:%s/api/processor/status' % config['network']['ports']['api'],
                                     json.dumps(status).encode(), {'Content-Type': 'application/json'})
    urllib.request.urlopen(request, timeout=2).close()
stopped = False
def stop(*_):
    global stopped
    stopped = True
signal.signal(signal.SIGTERM, stop)
if mode == 'worker':
    worker = subprocess.Popen([sys.executable, '-c', 'import signal,time;signal.signal(signal.SIGTERM,signal.SIG_IGN);print("ready",flush=True);time.sleep(30)'],
                              stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
    assert worker.stdout.readline() == b'ready\n'
post()
if mode == 'early':
    sys.exit(0)
streams = [socket.create_connection(('127.0.0.1', config['network']['ports'][name]), timeout=2)
           for name in ('timing', 'detection', 'track')]
payload = b'{"nCpi":1,"data":[1.5,-0.0]}'
if mode == 'partial': payload = b'{"nCpi":'
if mode == 'nonfinite': payload = b'{"nCpi":NaN}'
if mode == 'overflow': payload = b'x' * 4096
for stream in streams: stream.sendall(payload)
if mode == 'log':
    sys.stdout.write('x' * (2 << 20))
    sys.stdout.flush()
if mode == 'identity':
    pathlib.Path(sys.argv[0]).write_text('# changed during replay\n')
if mode != 'timeout':
    status['state'] = 'complete'
    if mode == 'wrong-eof': status['totalSamples'] = 11
    post()
while not stopped:
    time.sleep(.01)
for stream in streams: stream.close()
status.update(state='stopped', captureStopped=True, captureDroppedSamples=[0, 0], captureBacklogSamples=[0, 0])
if mode == 'loss': status['captureDroppedSamples'] = [0, 1]
post()
'''


class ReplaySessionContract(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.binary = self.root / "fake-processor"
        self.binary.write_text(f"#!{sys.executable}\n" + CHILD)
        self.binary.chmod(0o700)

    def execute(self, mode, **options):
        experiment = SESSION.Experiment(self.binary, options.pop("timeout", 3), **options)
        with SESSION.ReplaySession(experiment, self.root) as session:
            self.session = session
            config = {"test_mode": mode, "capture": {"device": {"channel_count": 2},
                      "replay": {"state": True, "format": "auto"}},
                      "network": {"ports": dict(api=session.status.port, **session.sinks.ports())}}
            products = session.execute(config)
        return products

    def test_large_log_is_drained_concurrently_without_stdbuf(self):
        self.assertEqual(self.execute("log"), {"timing": [{"nCpi": 1, "data": [1.5, -0.0]}]})
        self.assertEqual((self.root / "processor.log").stat().st_size, 2 << 20)
        self.assertEqual(self.session.receipt["exit_code"], 0)
        self.assertFalse(self.session.receipt["forced_stop"])
        self.assertEqual(self.session.receipt["shutdown"], "graceful-eof-stop")
        self.assertEqual(self.session.process.poll(), 0)

    def test_timeout_and_early_exit_keep_evidence_and_reap_child(self):
        for mode, message in (("timeout", "deadline"), ("early", "before EOF")):
            with self.subTest(mode=mode), tempfile.TemporaryDirectory() as temporary:
                self.root = Path(temporary)
                with self.assertRaisesRegex(RuntimeError, message):
                    self.execute(mode, timeout=.5)
                self.assertIsNotNone(self.session.process.poll())
                self.assertTrue((self.root / "status.json").is_file())

    def test_hostile_eof_products_loss_and_identity_fail_closed(self):
        cases = (("wrong-eof", "sample contract", {}), ("partial", "concatenated JSON", {}),
                 ("nonfinite", "non-finite", {}), ("loss", "stopped-capture", {}),
                 ("overflow", "retention|truncated", {"retained": {"timing": 16}}),
                 ("identity", "pinned artifact changed", {}))
        for mode, message, options in cases:
            with self.subTest(mode=mode), tempfile.TemporaryDirectory() as temporary:
                self.root = Path(temporary)
                with self.assertRaisesRegex((RuntimeError, AssertionError, ValueError), message):
                    self.execute(mode, **options)
                self.assertIsNotNone(self.session.process.poll())
                self.assertTrue((self.root / "timing.raw").is_file())

    def test_explicit_digest_mismatch_refuses_before_private_peers(self):
        with patch.object(SESSION, "StatusServer") as status:
            with self.assertRaisesRegex(ValueError, "SHA-256 mismatch"):
                self.execute("normal", binary_sha256="0" * 64)
            status.assert_not_called()

    def test_surviving_worker_forces_only_its_private_group_cleanup(self):
        stop_group = SESSION.stop_group
        with patch.object(SESSION, "stop_group", lambda child: stop_group(child, grace=.1)), \
             patch.object(SESSION.os, "killpg", wraps=os.killpg) as kill:
            with self.assertRaisesRegex(RuntimeError, "surviving private worker"):
                self.execute("worker")
        self.assertTrue(self.session.receipt["forced_stop"])
        kill.assert_any_call(self.session.process.pid, signal.SIGKILL)
        self.assertTrue(all(call.args[0] == self.session.process.pid for call in kill.call_args_list))

    def test_status_body_shape_size_and_retention_are_bounded(self):
        status = SESSION.StatusServer()
        status.start()
        try:
            connection = http.client.HTTPConnection(SESSION.HOST, status.port, timeout=2)
            connection.request("GET", "/capture/request")
            response = connection.getresponse()
            self.assertEqual(response.status, 200)
            self.assertEqual(json.loads(response.read()), {})
            connection.close()
            for body in (b"[]", b"null", b"not-json"):
                connection = http.client.HTTPConnection(SESSION.HOST, status.port, timeout=2)
                connection.request("POST", "/api/processor/status", body)
                self.assertEqual(connection.getresponse().status, 400)
                connection.close()
            connection = http.client.HTTPConnection(SESSION.HOST, status.port, timeout=2)
            connection.putrequest("POST", "/api/processor/status")
            connection.putheader("Content-Length", str((1 << 20) + 1))
            connection.endheaders()  # Reject the declared bound without waiting for a body.
            self.assertEqual(connection.getresponse().status, 400)
            connection.close()
            status.statuses = [{}] * 32768
            connection = http.client.HTTPConnection(SESSION.HOST, status.port, timeout=2)
            connection.request("POST", "/api/processor/status", b'{"state":"complete"}')
            self.assertEqual(connection.getresponse().status, 204)
            connection.close()
            self.assertTrue(status.overflow)
            self.assertEqual(len(status.snapshot()), 32768)
        finally:
            status.close()

    def test_manifold_is_a_first_class_bounded_replay_product(self):
        self.assertIn('manifold', SESSION.PORT_NAMES)
        sinks = SESSION.SinkGroup(retained={'manifold': 1024})
        try:
            self.assertIn('manifold', sinks.ports())
            self.assertEqual(sinks.payload_limits['manifold'], 1024)
            SESSION.Experiment(self.binary, 3, required=('manifold',),
                               retained={'manifold': 1024})
        finally:
            sinks.close()

    def test_macos_lifecycle_reserves_every_canonical_output_port(self):
        for name in ('processor_lifecycle_test', 'homebrew_service_test'):
            with self.subTest(fixture=name):
                spec = importlib.util.spec_from_file_location(name, ROOT / f"test/macos/{name}.py")
                adapter = importlib.util.module_from_spec(spec)
                spec.loader.exec_module(adapter)
                fixture = adapter.fixture if hasattr(adapter, 'fixture') else adapter.replay
                names = (*fixture.PORT_NAMES, 'future-output')
                with patch.object(sys, 'argv', ['lifecycle', '--binary', str(self.binary)]), \
                     patch.object(Path, 'home', return_value=self.root), \
                     patch.object(adapter.shutil, 'which', return_value=sys.executable), \
                     patch.object(adapter.subprocess, 'check_output', return_value=str(self.root)), \
                     patch.object(adapter.subprocess, 'run', return_value=subprocess.CompletedProcess([], 1)), \
                     patch.object(fixture, 'write_blah2iq'), patch.object(fixture, 'PORT_NAMES', names), \
                     patch.object(fixture, 'config', side_effect=RuntimeError('ports captured')) as config:
                    with self.assertRaisesRegex(RuntimeError, 'ports captured'):
                        adapter.main()
                args = config.call_args.args
                self.assertEqual(set(args[4]), set(names))
                self.assertEqual(len({args[3], *args[4].values()}), len(names) + 1)

    def test_macos_configuration_matrix_respects_tracker_dependency(self):
        spec = importlib.util.spec_from_file_location("macos_configuration_fixture",
            ROOT / "test/macos/config_runtime_test.py")
        adapter = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(adapter)
        with patch.object(sys, 'argv', ['configuration', '--binary', str(self.binary)]), \
             patch.object(adapter, 'run_case', return_value={}) as run, patch('builtins.print'):
            adapter.main()
        self.assertEqual(run.call_count, 11)
        for call in run.call_args_list:
            name, changes = call.args[1:3]
            config = adapter.replay.config('Usrp', 2, self.root / 'recording', 3000,
                {key: 3001 + i for i, key in enumerate(adapter.replay.PORT_NAMES)})
            for key, value in changes.items():
                adapter.set_field(config, key, value)
            process = config['process']
            self.assertFalse(process['tracker']['enable'] and not process['detection']['enable'], name)
            if name == 'detection-disabled-recording-requested':
                self.assertFalse(process['detection']['enable'])
                self.assertTrue(config['save']['detection'])

    def test_scientific_adapter_uses_same_session_and_retains_artifact_receipt(self):
        spec = importlib.util.spec_from_file_location("scientific_replay_adapter", ROOT / "script/run-iq-protection-replay.py")
        adapter = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(adapter)
        recording = self.root / "recording"
        recording.write_bytes(b"fake recording, never consumed by radar")
        digest = adapter.file_sha256(recording)
        profile = {"configuration_complete": True, "capture": {"source_receiver": "Kraken",
                   "sample_rate_hz": 100, "frequency_hz": 1000, "channel_count": 2,
                   "reference_channel": 0, "surveillance_channels": [1], "array_geometry": None},
                   "process": {"performance": {"acceleration": "cpu"}}}
        for name, value in (("profile", profile), ("metadata", {"dataSha256": digest}),
                            ("timeline", {"frames": [{"timestamp_ms": 1}]})):
            (self.root / name).write_text(json.dumps(value))
        args = adapter.parse_args(["--binary", str(self.binary), "--recording", str(recording),
            "--profile", str(self.root / "profile"), "--metadata", str(self.root / "metadata"),
            "--timeline", str(self.root / "timeline"), "--sha256", digest,
            "--output", str(self.root / "result.json"), "--timeout", "3"])
        execute = adapter.SESSION.ReplaySession.execute
        def fake_child_config(session, config):
            config["test_mode"] = "normal"
            products = execute(session, config)
            for frames in products.values():
                for frame in frames:
                    frame['timestamp'] = 1
            return products
        with patch.object(adapter.SESSION.ReplaySession, "execute", fake_child_config):
            result = adapter.run(args)
        self.assertEqual(result["frames"]["timing"],
                         [{"nCpi": 1, "data": [1.5, -0.0], "timestamp": 1}])
        self.assertEqual(result["session"]["exit_code"], 0)
        self.assertIn("artifact_acceptance", result["session"])
        self.assertEqual(json.loads(args.output.read_text())["session"], result["session"])

    def test_descriptor_limits_fail_before_spawn(self):
        for options in ({"timeout": float("nan")}, {"timeout": 1801}, {"error_stall_seconds": 0},
                        {"required": ()}, {"required": ("iqdata",)}, {"retained": {}},
                        {"retained": {"timing": True}}, {"retained": {"timing": 129 << 20}}):
            with self.subTest(options=options), self.assertRaises(ValueError):
                SESSION.Experiment(self.binary, options.pop("timeout", 1), **options)

    def test_detached_fixture_import_uses_declared_staged_owner(self):
        fixture = self.root / "processor_replay_test.py"
        fixture.write_bytes((ROOT / "test/recording/processor_replay_test.py").read_bytes())
        result = subprocess.run([sys.executable, "-I", str(fixture), "--help"],
            env={**os.environ, "VECTORWARP_REPLAY_SESSION": str(ROOT / "script/replay_session.py")},
            capture_output=True, text=True, timeout=5)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("--binary", result.stdout)


if __name__ == "__main__":
    unittest.main()
