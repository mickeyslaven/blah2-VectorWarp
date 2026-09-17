"""Offline broker failure injection: never call systemctl, packages or hardware."""
import importlib.util
import ctypes
import hashlib
import json
import os
import pathlib
import socket
import struct
import tempfile
import threading
import unittest
from unittest import mock

ROOT = pathlib.Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('receiver_helper', ROOT / 'script/vectorwarp-receiver-helper.py')
helper = importlib.util.module_from_spec(spec)
spec.loader.exec_module(helper)


class FakeInspector:
    def __init__(self):
        self.ready = False
        self.generation = 1
        self.calls = []
        self.exit_code = 0
        self.timeout = False
        self.verify = True
        self.raised = False
        self.entered = self.release = None

    def inspect(self, action):
        return {'ready': self.ready, 'generation': self.generation}

    def execute(self, action, revision=None):
        self.calls.append(action['id'])
        if self.entered:
            self.entered.set()
            self.release.wait(5)
        if self.raised:
            raise OSError('injected runner failure')
        if self.verify:
            self.ready = True
        return {'exitCode': self.exit_code, 'timedOut': self.timeout}

    def run(self, argv, timeout=5):
        self.calls.append((argv, timeout))
        if self.entered:
            self.entered.set()
            self.release.wait(5)
        return {'exitCode': self.exit_code, 'timedOut': self.timeout, 'output': ''}


class BrokerTest(unittest.TestCase):
    def test_reviewed_package_policy_uses_qualified_uhd_41_floor(self):
        policy = {'schemaVersion': 1, 'apiUser': 'vectorwarp-api', 'configPath': '/fixture/config.yml',
                  'artifactManifest': '/fixture/manifest', 'actions': [
                      {'id': 'install-uhd', 'receiverType': 'Usrp', 'kind': 'install-packages',
                       'review': 'offline fixture', 'artifactSha256': 'a' * 64, 'platform': 'ubuntu:22.04',
                       'manager': 'apt', 'packages': [{'name': 'uhd-host', 'version': '4.1.0.5-3'}]}]}
        with mock.patch.object(helper, 'trusted_path'), mock.patch.object(helper, 'parse_manifest'), \
             mock.patch.object(helper, 'bounded_read', side_effect=lambda *_: json.dumps(policy).encode()):
            self.assertEqual(helper.load_policy('/fixture/policy'), policy)
            policy['actions'][0]['packages'][0]['version'] = '4.0.0'
            with self.assertRaises(helper.Refused) as caught:
                helper.load_policy('/fixture/policy')
            self.assertEqual(caught.exception.code, 'INVALID_POLICY')

    def setUp(self):
        self.now = 0
        self.revision = 'a' * 64
        self.inspector = FakeInspector()
        self.action = {'id': 'reviewed-hackrf', 'receiverType': 'HackRF', 'kind': 'install-packages', 'review': 'fixture'}
        self.broker = helper.Broker({'actions': [self.action]}, 1001, self.inspector,
                                    now=lambda: self.now, revision=lambda: self.revision)

    def plan(self):
        return self.broker.handle({'verb': 'plan', 'actionId': self.action['id'], 'configRevision': self.revision}, 1001)['planId']

    def authorize(self, plan):
        return self.broker.handle({'verb': 'authorize', 'planId': plan}, 0)

    def execute(self, plan, revision=None):
        return self.broker.handle({'verb': 'execute', 'planId': plan, 'configRevision': revision or self.revision}, 1001)

    def refused(self, code, action):
        with self.assertRaises(helper.Refused) as caught:
            action()
        self.assertEqual(caught.exception.code, code)

    def test_peer_credentials_and_exact_schema(self):
        self.refused('UNAUTHORIZED_PEER', lambda: self.broker.handle({'verb': 'discover'}, 9999))
        self.refused('INVALID_REQUEST', lambda: self.broker.handle({'verb': 'discover', 'command': 'id'}, 1001))
        self.refused('ACTION_NOT_REVIEWED', lambda: self.broker.handle({'verb': 'plan', 'actionId': '/bin/sh', 'configRevision': self.revision}, 1001))

    def test_gpu_access_only_reads_fixed_helper_metadata(self):
        self.refused('UNAUTHORIZED_PEER', lambda: self.broker.handle({'verb': 'gpu-access'}, 9999))
        for extra in ({'path': '/dev/anything'}, {'command': '--configure-service-access'}, {'user': 'root'}):
            self.refused('INVALID_REQUEST', lambda: self.broker.handle({'verb': 'gpu-access', **extra}, 1001))
        access = {'state': 'group-access-needed', 'nodes': []}
        with mock.patch.object(helper, 'trusted_path', return_value=pathlib.Path('/fixture/vectorwarp-gpu-setup')), \
             mock.patch.object(self.inspector, 'run', return_value={'exitCode': 0, 'timedOut': False, 'output': json.dumps(access)}) as run:
            self.assertEqual(self.broker.handle({'verb': 'gpu-access'}, 1001), {'ok': True, 'serviceAccess': access})
            run.assert_called_once_with(['/usr/bin/python3', '-I', '/fixture/vectorwarp-gpu-setup', '--access-status', '--json'], timeout=1)
        for result in ({'exitCode': 1, 'timedOut': False}, {'exitCode': 0, 'timedOut': True},
                       {'exitCode': 0, 'timedOut': False, 'output': '[]'}):
            with mock.patch.object(helper, 'trusted_path'), mock.patch.object(self.inspector, 'run', return_value=result):
                self.refused('GPU_ACCESS_UNAVAILABLE', lambda: self.broker.handle({'verb': 'gpu-access'}, 1001))

    def test_restart_is_a_fixed_api_only_request(self):
        self.refused('UNAUTHORIZED_PEER', lambda: self.broker.handle({'verb': 'restart'}, 9999))
        self.refused('UNAUTHORIZED_PEER', lambda: self.broker.handle({'verb': 'restart'}, 0))
        for extra in ({'unit': 'other.service'}, {'command': '/bin/sh'}, {'configRevision': self.revision}):
            self.refused('INVALID_REQUEST', lambda: self.broker.handle({'verb': 'restart', **extra}, 1001))
        accepted = self.broker.handle({'verb': 'restart'}, 1001)
        self.assertEqual(accepted['status'], 'accepted')
        self.assertEqual(self.inspector.calls, [(['/usr/bin/systemctl', '--no-block', 'start',
                                                  'vectorwarp-restart.service'], 5)])
        self.assertEqual(self.broker.handle({'verb': 'restart'}, 1001)['status'], 'accepted',
                         'A repeated fixed start remains idempotent at systemd; it cannot select another unit.')

    def test_restart_failure_timeout_and_busy_are_not_accepted(self):
        for field in ('exit_code', 'timeout'):
            self.setUp()
            setattr(self.inspector, field, 1)
            self.refused('RESTART_REQUEST_FAILED', lambda: self.broker.handle({'verb': 'restart'}, 1001))
            self.assertFalse(self.broker.operation.locked())
        self.setUp()
        self.broker.operation.acquire()
        self.refused('MANAGEMENT_BUSY', lambda: self.broker.handle({'verb': 'restart'}, 1001))
        self.broker.operation.release()

    def test_restart_cli_accepts_only_fixed_verb_without_root(self):
        with mock.patch.object(helper.sys, 'argv', ['vectorwarp-receiver-helper', 'request-restart']), \
             mock.patch.object(helper.os, 'geteuid', return_value=1001), \
             mock.patch.object(helper, 'exchange', return_value={'ok': True, 'status': 'accepted', 'message': 'queued'}) as exchange:
            helper.main()
            exchange.assert_called_once_with({'verb': 'restart'})
        with mock.patch.object(helper.sys, 'argv', ['vectorwarp-receiver-helper', 'request-restart', 'other.service']), \
             mock.patch.object(helper, 'exchange') as exchange:
            self.refused('INVALID_REQUEST', helper.main)
            exchange.assert_not_called()

    def test_api_cannot_authorize_and_root_grant_is_one_use(self):
        plan = self.plan()
        self.refused('LOCAL_AUTHORIZATION_REQUIRED', lambda: self.broker.handle({'verb': 'authorize', 'planId': plan}, 1001))
        self.refused('LOCAL_AUTHORIZATION_REQUIRED', lambda: self.execute(plan))
        self.assertEqual(self.inspector.calls, [])
        self.authorize(plan)
        self.assertEqual(self.execute(plan)['status'], 'complete')
        self.refused('PLAN_EXPIRED', lambda: self.execute(plan))
        self.assertEqual(len(self.inspector.calls), 1)

    def test_recomputed_public_hash_is_not_a_grant(self):
        self.refused('PLAN_EXPIRED', lambda: self.execute(helper.digest(self.action)))

    def test_revision_drift_before_grant_and_before_execute(self):
        plan = self.plan()
        self.revision = 'b' * 64
        self.refused('CONFIG_CHANGED', lambda: self.authorize(plan))
        plan = self.plan()
        self.authorize(plan)
        self.revision = 'c' * 64
        self.refused('CONFIG_CHANGED', lambda: self.execute(plan))
        self.refused('PLAN_EXPIRED', lambda: self.execute(plan))
        self.assertEqual(self.inspector.calls, [])

    def test_expired_and_restarted_broker_plans_are_invalid(self):
        plan = self.plan()
        self.authorize(plan)
        self.now = 301
        self.refused('PLAN_EXPIRED', lambda: self.execute(plan))
        self.broker.plans.clear()
        self.refused('PLAN_EXPIRED', lambda: self.execute(plan))

    def test_state_drift_consumes_grant_without_executing(self):
        plan = self.plan()
        self.authorize(plan)
        self.inspector.generation = 2
        self.refused('STATUS_CHANGED', lambda: self.execute(plan))
        self.refused('PLAN_EXPIRED', lambda: self.execute(plan))
        self.assertEqual(self.inspector.calls, [])

    def test_exit_zero_requires_positive_postcondition(self):
        self.inspector.verify = False
        plan = self.plan()
        self.authorize(plan)
        result = self.execute(plan)
        self.assertEqual(result['status'], 'failed')
        self.assertFalse(result['postconditionVerified'])

    def test_nonzero_and_timeout_are_not_success_even_when_software_appears(self):
        for field in ('exit_code', 'timeout'):
            with self.subTest(field=field):
                self.setUp()
                setattr(self.inspector, field, 1)
                plan = self.plan()
                self.authorize(plan)
                self.assertEqual(self.execute(plan)['status'], 'failed')

    def test_failure_releases_lock_without_reusing_grant(self):
        self.inspector.raised = True
        plan = self.plan()
        self.authorize(plan)
        with self.assertRaises(OSError):
            self.execute(plan)
        self.assertFalse(self.broker.operation.locked())
        self.refused('PLAN_EXPIRED', lambda: self.execute(plan))

    def test_concurrent_execution_cannot_run_two_commands(self):
        plan1, plan2 = self.plan(), self.plan()
        self.authorize(plan1)
        self.authorize(plan2)
        self.inspector.entered, self.inspector.release = threading.Event(), threading.Event()
        worker = threading.Thread(target=lambda: self.execute(plan1))
        worker.start()
        self.assertTrue(self.inspector.entered.wait(2))
        self.refused('MANAGEMENT_BUSY', lambda: self.execute(plan2))
        self.inspector.release.set()
        worker.join(2)
        self.assertEqual(len(self.inspector.calls), 1)


class InspectorTest(unittest.TestCase):
    def setUp(self):
        self.manifest = b'compiled_receivers=Kraken,HackRF\n'
        self.unit = b'[Service]\nExecStart=/opt/reviewed-suite/receiver\n'
        self.binary = b'fixture executable, never run'
        self.action = {'id': 'reviewed-suite', 'receiverType': 'Kraken', 'kind': 'start-service',
                       'artifactSha256': hashlib.sha256(self.manifest).hexdigest(),
                       'unit': 'vectorwarp-heimdall.service', 'unitSha256': hashlib.sha256(self.unit).hexdigest(),
                       'executable': '/opt/reviewed-suite/receiver', 'executableSha256': hashlib.sha256(self.binary).hexdigest()}
        self.calls = []
        self.properties = 'LoadState=loaded\nActiveState=inactive\nFragmentPath=/usr/lib/systemd/system/vectorwarp-heimdall.service\nDropInPaths=\nNeedDaemonReload=no\n'
        def run(argv, **kwargs):
            self.calls.append(argv)
            return {'exitCode': 0, 'timedOut': False, 'output': self.properties}
        self.inspector = helper.Inspector({'artifactManifest': '/opt/vectorwarp/current/.vectorwarp-build'}, run=run)
        self.patches = [mock.patch.object(helper, 'trusted_path', side_effect=lambda path: path),
                        mock.patch.object(helper, 'parse_manifest', return_value=['Kraken', 'HackRF']),
                        mock.patch.object(helper, 'bounded_read', side_effect=lambda path, *args: self.manifest if str(path).endswith('.vectorwarp-build') else self.unit if str(path).endswith('.service') else self.binary)]
        for patch in self.patches:
            patch.start()
            self.addCleanup(patch.stop)

    def test_owned_service_active_state_and_fixed_command(self):
        self.assertFalse(self.inspector.inspect(self.action)['ready'])
        self.inspector.execute(self.action)
        self.assertEqual(self.calls[-1], ['/usr/bin/systemctl', 'start', '--', 'vectorwarp-heimdall.service'])
        self.properties = self.properties.replace('inactive', 'active')
        self.assertTrue(self.inspector.inspect(self.action)['ready'])

    def test_unit_override_binary_or_artifact_drift_blocks_before_mutation(self):
        for field in ('unitSha256', 'executableSha256', 'artifactSha256'):
            with self.subTest(field=field):
                action = {**self.action, field: '0' * 64}
                with self.assertRaises(helper.Refused):
                    self.inspector.inspect(action)
        self.properties = self.properties.replace('DropInPaths=\n', 'DropInPaths=/etc/systemd/system/override.conf\n')
        with self.assertRaises(helper.Refused):
            self.inspector.inspect(self.action)
        self.assertFalse(any(argv[1] == 'start' for argv in self.calls))

    def test_package_mutation_is_explicitly_unavailable(self):
        with self.assertRaises(helper.Refused) as caught:
            self.inspector.execute({'kind': 'install-packages', 'manager': 'apt'})
        self.assertEqual(caught.exception.code, 'INSTALL_TRANSACTION_REVIEW_REQUIRED')
        self.assertEqual(self.calls, [])


class LocalBoundaryTest(unittest.TestCase):
    def test_enrollment_uses_installed_allowlisted_service_without_starting_it(self):
        manifest = b'compiled_receivers=Kraken\n'
        definition = b'[Service]\nExecStart=/opt/suite/receiver\n'
        output = ('LoadState=loaded\nActiveState=inactive\nFragmentPath=/usr/lib/systemd/system/krakensdr-suite-v2.service\n'
                  'DropInPaths=\nNeedDaemonReload=no\nExecStart={ path=/opt/suite/receiver ; argv[]=/opt/suite/receiver ; }\n')
        run = mock.Mock(return_value={'exitCode': 0, 'timedOut': False, 'output': output})
        with mock.patch.object(helper, 'parse_manifest', return_value=['Kraken']), \
             mock.patch.object(helper, 'trusted_path', side_effect=lambda value: pathlib.Path(value)), \
             mock.patch.object(helper, 'bounded_read', side_effect=lambda value, *args:
                 manifest if str(value).endswith('.vectorwarp-build') else definition if str(value).endswith('.service') else b'binary'):
            action, text = helper.enrolled_service({'artifactManifest': '/opt/vectorwarp/current/.vectorwarp-build'},
                                                   'Kraken', 'krakensdr-suite-v2.service', run)
            self.assertEqual(action['unit'], 'krakensdr-suite-v2.service')
            self.assertEqual(action['unitSha256'], hashlib.sha256(definition).hexdigest())
            self.assertEqual(text, definition.decode())
            self.assertTrue(all(call[0][0][1] == 'show' for call in run.call_args_list))
            with self.assertRaises(helper.Refused):
                helper.enrolled_service({}, 'Kraken', 'ssh.service', run)

    def test_policy_reload_invalidates_old_grants(self):
        inspector = FakeInspector()
        action = {'id': 'start-kraken', 'receiverType': 'Kraken', 'kind': 'start-service', 'review': 'fixture'}
        policy = {'actions': [action]}
        current = [policy]
        broker = helper.Broker(policy, 1001, inspector, revision=lambda: 'a' * 64,
                               policy_loader=lambda: current[0])
        plan = broker.handle({'verb': 'plan', 'actionId': action['id'], 'configRevision': 'a' * 64}, 1001)
        current[0] = {'actions': []}
        self.assertEqual(broker.handle({'verb': 'discover'}, 1001)['actions'], [])
        with self.assertRaises(helper.Refused) as caught:
            broker.handle({'verb': 'authorize', 'planId': plan['planId']}, 0)
        self.assertEqual(caught.exception.code, 'PLAN_EXPIRED')

    def test_config_revision_refuses_symlinks_and_fifo(self):
        with tempfile.TemporaryDirectory(prefix='vectorwarp-helper-boundary-') as directory:
            path = pathlib.Path(directory)
            config = path / 'config.yml'
            config.write_bytes(b'fixture\n')
            self.assertEqual(helper.config_revision(config), hashlib.sha256(b'fixture\n').hexdigest())
            link = path / 'link.yml'
            link.symlink_to(config)
            with self.assertRaises(OSError):
                helper.config_revision(link)
            fifo = path / 'fifo.yml'
            os.mkfifo(fifo)
            with self.assertRaises(helper.Refused):
                helper.config_revision(fifo)

    def test_socket_peer_uid_comes_from_os_not_request_json(self):
        if not hasattr(socket, 'SO_PEERCRED'):
            self.skipTest('Linux SO_PEERCRED required')
        first, second = socket.socketpair()
        with first, second:
            _, uid, _ = struct.unpack('3i', first.getsockopt(socket.SOL_SOCKET, socket.SO_PEERCRED, struct.calcsize('3i')))
            self.assertEqual(uid, os.getuid())

    def test_restart_wire_rejects_malformed_oversized_and_stalled_requests(self):
        broker = helper.Broker({'actions': []}, os.getuid(), FakeInspector())
        for raw, code in ((b'{bad json\n', 'INVALID_REQUEST'),
                          (b'{"verb":"restart","unit":"other.service"}\n', 'INVALID_REQUEST'),
                          (b'x' * (helper.MAX_BYTES + 1), 'INVALID_REQUEST')):
            with self.subTest(code=code, length=len(raw)):
                server, client = socket.socketpair()
                with server, client:
                    client.sendall(raw)
                    helper.serve_client(server, broker)
                    result = json.loads(client.recv(4096))
                    self.assertEqual(result['code'], code)
        server, client = socket.socketpair()
        with server, client:
            client.sendall(b'{"verb":')
            helper.serve_client(server, broker, timeout=0.05)
            self.assertEqual(json.loads(client.recv(4096))['code'], 'REQUEST_TIMEOUT')
            self.assertEqual(broker.inspector.calls, [])

    def test_unprivileged_no_new_privileges_peer_can_only_request_fixed_restart(self):
        if not hasattr(socket, 'SO_PEERCRED') or not hasattr(os, 'fork'):
            self.skipTest('Linux peer credentials and fork required')
        api_uid = 65534 if os.getuid() == 0 else os.getuid()
        inspector = FakeInspector()
        broker = helper.Broker({'actions': []}, api_uid, inspector)
        with tempfile.TemporaryDirectory() as directory:
            os.chmod(directory, 0o755)
            address = str(pathlib.Path(directory) / 'broker.sock')
            listener = socket.socket(socket.AF_UNIX)
            with listener:
                listener.bind(address)
                os.chmod(address, 0o666)
                listener.listen(1)
                listener.settimeout(2)
                read_fd, write_fd = os.pipe()
                pid = os.fork()
                if pid == 0:
                    try:
                        listener.close()
                        os.close(read_fd)
                        assert ctypes.CDLL(None).prctl(38, 1, 0, 0, 0) == 0
                        if os.getuid() == 0:
                            os.setgroups([])
                            os.setgid(api_uid)
                            os.setuid(api_uid)
                        with socket.socket(socket.AF_UNIX) as client:
                            client.settimeout(2)
                            client.connect(address)
                            client.sendall(b'{"verb":"restart"}\n')
                            response = client.recv(4096)
                        with open('/proc/self/status') as status:
                            assert 'NoNewPrivs:\t1' in status.read()
                        os.write(write_fd, response)
                        os._exit(0)
                    except Exception as error:
                        os.write(write_fd, str(error).encode())
                        os._exit(1)
                os.close(write_fd)
                try:
                    connection, _ = listener.accept()
                    helper.serve_client(connection, broker)
                    result = os.read(read_fd, 4096)
                    _, status = os.waitpid(pid, 0)
                    self.assertEqual(status, 0, result.decode(errors='replace'))
                    self.assertEqual(json.loads(result)['status'], 'accepted')
                    self.assertEqual(len(inspector.calls), 1)
                finally:
                    os.close(read_fd)


if __name__ == '__main__':
    unittest.main()
