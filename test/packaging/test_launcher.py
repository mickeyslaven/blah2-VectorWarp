"""No host services: exact launcher command/privilege routing regressions."""
import contextlib
import importlib.machinery
import importlib.util
import io
import os
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch, Mock

ROOT = Path(__file__).resolve().parents[2]
loader = importlib.machinery.SourceFileLoader('vectorwarp_launcher', str(ROOT / 'script/vectorwarp'))
spec = importlib.util.spec_from_loader(loader.name, loader)
launcher = importlib.util.module_from_spec(spec)
loader.exec_module(launcher)


class LauncherTests(unittest.TestCase):
    def setUp(self):
        self.output = io.StringIO()
        self.redirect = contextlib.redirect_stdout(self.output)
        self.redirect.__enter__()
        self.guard = patch.object(launcher, 'upgrade_guard', contextlib.nullcontext)
        self.guard.start()

    def tearDown(self):
        self.guard.stop()
        self.redirect.__exit__(None, None, None)

    @staticmethod
    def unit_reply(args, **kwargs):
        if args[:2] == [launcher.SYSTEMCTL, 'show']:
            unit = args[-1]
            pid = '0' if unit != launcher.BROKER else '41'
            state = 'inactive' if unit == launcher.PROCESSOR else 'active'
            if unit == launcher.SOCKET:
                return Mock(stdout='ActiveState=active\n')
            return Mock(stdout=f'ActiveState={state}\nMainPID={pid}\n')
        return Mock(stdout='')

    def test_default_opens_only_web(self):
        with patch.object(launcher, 'open_web') as open_web:
            self.assertEqual(launcher.main([]), 0)
            open_web.assert_called_once_with()

    def test_explicit_open_uses_the_same_web_only_path(self):
        with patch.object(launcher, 'open_web') as open_web:
            self.assertEqual(launcher.main(['open']), 0)
            open_web.assert_called_once_with()

    def test_help_aliases_are_unprivileged_and_never_change_services(self):
        for command in ('help', '-h', '--help'):
            with self.subTest(command=command), patch.object(launcher, 'run') as run:
                self.assertEqual(launcher.main([command]), 0)
                self.assertIn('Usage: vectorwarp', self.output.getvalue())
                run.assert_not_called()

    def test_version_aliases_read_metadata_without_service_actions(self):
        with tempfile.TemporaryDirectory() as temporary:
            prefix = Path(temporary)
            (prefix / 'PACKAGE-METADATA').write_text('version=1.2.3\ncommit=abc\n')
            for command in ('version', '--version'):
                with self.subTest(command=command), \
                     patch.object(launcher, 'PREFIX', prefix), \
                     patch.object(launcher, 'run') as run:
                    self.assertEqual(launcher.main([command]), 0)
                    self.assertIn('version=1.2.3', self.output.getvalue())
                    run.assert_not_called()

    def test_start_uses_checked_service_not_root_dsp(self):
        with patch.object(launcher.os, 'geteuid', return_value=0), \
             patch.object(launcher, 'run', side_effect=self.unit_reply) as run, \
             patch.object(launcher, 'api_url', return_value='http://127.0.0.1:3000/'), \
             patch.object(launcher, 'wait_web') as wait_web:
            launcher.main(['start'])
            self.assertEqual(run.call_args_list[-1].args[0],
                             ['/usr/bin/systemctl', 'start', 'vectorwarp-restart.service'])
            self.assertEqual([call.args[0] for call in run.call_args_list[:3]], [
                ['/usr/bin/systemctl', 'start', launcher.SOCKET],
                ['/usr/bin/systemctl', 'start', launcher.BROKER],
                ['/usr/bin/systemctl', 'start', launcher.API]])
            wait_web.assert_called_once_with('http://127.0.0.1:3000/')

    def test_start_does_not_interrupt_already_running_radar(self):
        def active(args, **kwargs):
            result = self.unit_reply(args, **kwargs)
            if args[-1:] == [launcher.PROCESSOR]:
                return Mock(stdout='ActiveState=active\nMainPID=77\n')
            return result
        with patch.object(launcher.os, 'geteuid', return_value=0), \
             patch.object(launcher, 'run', side_effect=active) as run, \
             patch.object(launcher, 'api_url', return_value='http://127.0.0.1:3000/'), \
             patch.object(launcher, 'wait_web'):
            launcher.main(['start'])
            self.assertNotIn([launcher.SYSTEMCTL, 'start', launcher.RESTART],
                             [call.args[0] for call in run.call_args_list])
            self.assertIn('already running', self.output.getvalue())

    def test_stop_uses_exclusive_helper_without_shared_launcher_guard(self):
        self.guard.stop()
        def stopped(args, **kwargs):
            return Mock(stdout='ActiveState=inactive\nMainPID=0\n')
        with patch.object(launcher.os, 'geteuid', return_value=0), \
             patch.object(launcher, 'upgrade_guard') as guard, \
             patch.object(launcher.subprocess, 'run') as process, \
             patch.object(launcher, 'run', side_effect=stopped) as run:
            launcher.main(['stop'])
            guard.assert_not_called()
            process.assert_called_once_with([str(launcher.QUIESCE), 'manual-stop'], check=True)
            self.assertEqual([call.args[0][-1] for call in run.call_args_list],
                             [launcher.API, launcher.PROCESSOR, launcher.RESTART,
                              launcher.BROKER, launcher.SOCKET])
            self.assertIn('web interface', self.output.getvalue())

    def test_stop_does_not_claim_success_if_a_service_remains_running(self):
        def busy(args, **kwargs):
            if args[-1] == launcher.RESTART:
                return Mock(stdout='ActiveState=activating\nMainPID=87\n')
            return Mock(stdout='ActiveState=inactive\nMainPID=0\n')
        with patch.object(launcher.os, 'geteuid', return_value=0), \
             patch.object(launcher.subprocess, 'run'), \
             patch.object(launcher, 'run', side_effect=busy):
            with self.assertRaisesRegex(RuntimeError, 'still running'):
                launcher.main(['stop'])
            self.assertNotIn('services stopped', self.output.getvalue())

    def test_stop_failure_never_starts_any_service(self):
        with patch.object(launcher.os, 'geteuid', return_value=0), \
             patch.object(launcher.subprocess, 'run', side_effect=subprocess.CalledProcessError(1, ['quiesce'])), \
             patch.object(launcher, 'run') as run:
            with self.assertRaises(subprocess.CalledProcessError):
                launcher.main(['restart'])
            run.assert_not_called()

    def test_restart_quiesces_then_reacquires_guard_for_full_start(self):
        events = []
        @contextlib.contextmanager
        def guarded():
            events.append('guard-enter')
            try:
                yield
            finally:
                events.append('guard-exit')
        def process(args, **kwargs):
            events.append(('quiesce', args, kwargs))
            return Mock()
        def command(args, **kwargs):
            events.append(('systemctl', args))
            if args[:2] == [launcher.SYSTEMCTL, 'show']:
                unit = args[-1]
                if events[0][0] == 'quiesce' and unit in (launcher.API, launcher.PROCESSOR,
                                                         launcher.RESTART, launcher.BROKER,
                                                         launcher.SOCKET):
                    # All stop verification observations occur before guard.
                    if 'guard-enter' not in events:
                        return Mock(stdout='ActiveState=inactive\nMainPID=0\n')
            return self.unit_reply(args, **kwargs)
        with patch.object(launcher.os, 'geteuid', return_value=0), \
             patch.object(launcher, 'upgrade_guard', guarded), \
             patch.object(launcher.subprocess, 'run', side_effect=process), \
             patch.object(launcher, 'run', side_effect=command), \
             patch.object(launcher, 'api_url', return_value='http://127.0.0.1:3000/'), \
             patch.object(launcher, 'wait_web'):
            launcher.main(['restart'])
        self.assertEqual(events[0][0], 'quiesce')
        self.assertEqual(events[6], 'guard-enter')
        self.assertEqual(events[-1], 'guard-exit')

    def test_restart_intervening_upgrade_leaves_stack_stopped(self):
        def stopped(args, **kwargs):
            return Mock(stdout='ActiveState=inactive\nMainPID=0\n')
        with patch.object(launcher.os, 'geteuid', return_value=0), \
             patch.object(launcher.subprocess, 'run'), \
             patch.object(launcher, 'run', side_effect=stopped) as run, \
             patch.object(launcher, 'upgrade_guard', side_effect=RuntimeError('package update')):
            with self.assertRaisesRegex(RuntimeError, 'package update'):
                launcher.main(['restart'])
            self.assertEqual(run.call_count, 5)

    def test_nonroot_start_uses_standard_administrator_authentication(self):
        with patch.object(launcher.os, 'geteuid', return_value=1000), \
             patch.object(launcher.subprocess, 'run', return_value=Mock(stdout='')) as run:
            launcher.main(['start'])
            self.assertEqual(run.call_args.args[0], ['/usr/bin/sudo', '--', '/usr/bin/vectorwarp', 'start'])
            self.assertNotIn('timeout', run.call_args.kwargs)

    def test_nonroot_stop_does_not_timeout_privileged_drain(self):
        with patch.object(launcher.os, 'geteuid', return_value=1000), \
             patch.object(launcher.subprocess, 'run', return_value=Mock(stdout='')) as run:
            launcher.main(['stop'])
            self.assertEqual(run.call_args.args[0], ['/usr/bin/sudo', '--', '/usr/bin/vectorwarp', 'stop'])
            self.assertNotIn('timeout', run.call_args.kwargs)

    def test_unknown_command_never_reaches_systemctl(self):
        with patch.object(launcher, 'run') as run, contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(launcher.main(['; reboot']), 64)
            self.assertEqual(launcher.main(['start', 'sshd.service']), 64)
            run.assert_not_called()

    def test_internal_command_requires_root(self):
        with patch.object(launcher.os, 'geteuid', return_value=1000):
            with self.assertRaises(RuntimeError):
                launcher.main(['_web'])

    def test_headless_prints_url_and_does_not_spawn_browser(self):
        with patch.object(launcher, 'privileged', return_value='http://127.0.0.1:4321/'), \
             patch.dict(launcher.os.environ, {'SSH_CONNECTION': 'test', 'DISPLAY': ':0'}, clear=True), \
             patch.object(launcher.os, 'geteuid', return_value=1000), patch.object(launcher.shutil, 'which') as which:
            launcher.open_web()
            which.assert_not_called()
            self.assertIn(':4321/', self.output.getvalue())

    def test_browser_runs_only_as_desktop_user(self):
        with patch.object(launcher, 'privileged', return_value='http://[::1]:4321/'), \
             patch.dict(launcher.os.environ, {'WAYLAND_DISPLAY': 'wayland-0'}, clear=True), \
             patch.object(launcher.os, 'geteuid', return_value=1000), \
             patch.object(launcher.shutil, 'which', return_value='/usr/bin/xdg-open'), \
             patch.object(launcher.subprocess, 'run', return_value=Mock(returncode=0)) as run:
            launcher.open_web()
            self.assertEqual(run.call_args.args[0], ['/usr/bin/xdg-open', 'http://[::1]:4321/'])

    def test_root_never_starts_desktop_browser(self):
        with patch.object(launcher, 'privileged', return_value='http://127.0.0.1:3000/'), \
             patch.dict(launcher.os.environ, {'DISPLAY': ':0'}, clear=True), \
             patch.object(launcher.os, 'geteuid', return_value=0), patch.object(launcher.shutil, 'which') as which:
            launcher.open_web()
            which.assert_not_called()

    def test_status_is_unprivileged_and_propagates_stopped_exit_status(self):
        with patch.object(launcher.subprocess, 'run', return_value=Mock(returncode=3)) as run:
            self.assertEqual(launcher.main(['status']), 3)
            self.assertNotIn('sudo', str(run.call_args))

    def test_logs_is_unprivileged_and_never_routes_through_service_actions(self):
        with patch.object(launcher.subprocess, 'run', return_value=Mock(returncode=0)) as run, \
             patch.object(launcher, 'run') as service_run:
            self.assertEqual(launcher.main(['logs']), 0)
            self.assertEqual(run.call_args.args[0][:4],
                             ['/usr/bin/journalctl', '--no-pager', '-n', '100'])
            service_run.assert_not_called()

    def test_failure_is_not_reported_as_success(self):
        with patch.object(launcher.os, 'geteuid', return_value=0), \
             patch.object(launcher, 'run', side_effect=subprocess.CalledProcessError(1, ['systemctl'])):
            with self.assertRaises(subprocess.CalledProcessError):
                launcher.main(['start'])

    def test_readiness_requires_matching_service_identity(self):
        response = Mock(status=200)
        response.__enter__ = Mock(return_value=response)
        response.__exit__ = Mock(return_value=False)
        response.read.side_effect = [b'<html>unrelated web page</html>',
                                    json.dumps({'serverId': '999-123', 'restart': {}}).encode(),
                                    json.dumps({'serverId': '42-123', 'restart': {}}).encode()]
        opener = Mock()
        opener.open.return_value = response
        with patch.object(launcher.urllib.request, 'build_opener', return_value=opener) as build, \
             patch.object(launcher.urllib.request, 'ProxyHandler') as proxy, \
             patch.object(launcher.time, 'sleep'), \
             patch.object(launcher, 'run', return_value=Mock(stdout='42\n')):
            launcher.wait_web('http://127.0.0.1:4321/')
            self.assertEqual(opener.open.call_count, 3)
            proxy.assert_called_once_with({})
            build.assert_called_once_with(proxy.return_value)

    def test_real_configuration_reader_matches_api_recovery(self):
        # CI runs this after npm ci; local verification can point at an
        # already installed API, without installing anything on the host.
        api = Path(os.environ.get('VECTORWARP_LAUNCHER_TEST_API', str(ROOT / 'api')))
        node = os.environ.get('VECTORWARP_LAUNCHER_TEST_NODE') or launcher.shutil.which('node')
        self.assertIsNotNone(node, 'Node is needed for the real configuration-reader regression')
        self.assertTrue((api / 'node_modules/js-yaml').is_dir(), 'Run npm ci --prefix api first')
        with tempfile.TemporaryDirectory() as temporary:
            prefix = Path(temporary)
            (prefix / 'runtime/node/bin').mkdir(parents=True)
            (prefix / 'runtime/node/bin/node').symlink_to(node)
            (prefix / 'current').mkdir()
            (prefix / 'current/api').symlink_to(api)
            config = prefix / 'config.yml'
            cases = [('0.0.0.0', 4321, 'http://127.0.0.1:4321/'),
                     ('::', 4322, 'http://[::1]:4322/'),
                     ('192.0.2.5', 4323, 'http://192.0.2.5:4323/'),
                     ('::1', 4324, 'http://[::1]:4324/')]
            with patch.object(launcher, 'PREFIX', prefix), patch.object(launcher, 'CONFIG', config):
                for host, port, expected in cases:
                    with self.subTest(host=host):
                        config.write_text(json.dumps({'network': {'ip': host, 'ports': {'api': port}}}))
                        self.assertEqual(launcher.api_url(), expected)
                config.write_text('network: [invalid YAML')
                self.assertEqual(launcher.api_url(), 'http://127.0.0.1:3000/')

    def test_upgrade_guard_blocks_active_transaction_and_between_hooks(self):
        self.guard.stop()
        with tempfile.TemporaryDirectory() as temporary:
            lock = Path(temporary) / 'lock'
            state = Path(temporary) / 'state'
            with patch.object(launcher, 'UPGRADE_LOCK', lock), patch.object(launcher, 'UPGRADE_STATE', state):
                # fstat ownership is the only substitute on non-root CI. File
                # opening, locking and contention use real descriptors.
                original = launcher.os.fstat
                def trusted(fd):
                    value = original(fd)
                    return Mock(st_mode=value.st_mode, st_uid=0)
                with patch.object(launcher.os, 'fstat', side_effect=trusted):
                    with launcher.upgrade_guard():
                        pass
                    with lock.open('r+') as held:
                        launcher.fcntl.flock(held.fileno(), launcher.fcntl.LOCK_EX)
                        with self.assertRaisesRegex(RuntimeError, 'update is in progress'):
                            with launcher.upgrade_guard():
                                self.fail('Upgrade lock was ignored')
                    state.touch()
                    with self.assertRaisesRegex(RuntimeError, 'has not completed'):
                        with launcher.upgrade_guard():
                            self.fail('Between-hook marker was ignored')

    def test_upgrade_guard_rejects_symlink(self):
        self.guard.stop()
        with tempfile.TemporaryDirectory() as temporary:
            target = Path(temporary) / 'target'
            target.touch()
            lock = Path(temporary) / 'lock'
            lock.symlink_to(target)
            with patch.object(launcher, 'UPGRADE_LOCK', lock):
                with self.assertRaises(OSError):
                    with launcher.upgrade_guard():
                        self.fail('Symlink was followed')


if __name__ == '__main__':
    unittest.main()
