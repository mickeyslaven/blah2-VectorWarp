#!/usr/bin/env python3
"""Behavioral simulations for the fixed SDRplay service helper; no host calls."""

from __future__ import annotations

import importlib.util
from pathlib import Path
import unittest
from unittest import mock


ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location('sdrplay_service', ROOT / 'script/vectorwarp-sdrplay-service.py')
service = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(service)


def properties(unit='sdrplay.service', state='inactive', **changes):
    values = {name: '' for name in service.PROPERTIES}
    values.update({'Id': unit, 'LoadState': 'loaded', 'ActiveState': state,
      'FragmentPath': '/etc/systemd/system/sdrplay.service', 'NeedDaemonReload': 'no',
      'ExecStart': '{ path=/usr/sbin/sdrplay_apiService ; argv[]=/usr/sbin/sdrplay_apiService ; ignore_errors=no ; }',
      'WorkingDirectory': '/', 'User': 'root', 'Group': 'root', 'Type': 'simple'})
    values.update(changes)
    return '\n'.join(f'{name}={values[name]}' for name in service.PROPERTIES) + '\n'


class FakeSystemd:
    def __init__(self, state='inactive', unit='sdrplay.service'):
        self.state, self.unit, self.calls = state, unit, []
        self.extra = {}
        self.start_result = {'exitCode': 0, 'output': '', 'timedOut': False}
        self.start_activates = True

    def run(self, argv, timeout=None):
        self.calls.append((tuple(argv), timeout))
        if argv == ['/usr/bin/systemd-detect-virt', '--chroot']:
            return {'exitCode': 1, 'output': '', 'timedOut': False}
        if argv[:3] == ['/usr/bin/systemctl', 'show', argv[2]]:
            alias = argv[2]
            if alias != self.unit:
                return {'exitCode': 1, 'output': 'LoadState=not-found\n', 'timedOut': False}
            return {'exitCode': 0, 'output': properties(self.unit, self.state, **self.extra), 'timedOut': False}
        if argv[:3] in (['/usr/bin/systemctl', 'start', '--'],
                        ['/usr/bin/deb-systemd-invoke', 'start', self.unit]):
            if self.start_result['exitCode'] == 0 and self.start_activates:
                self.state = 'active'
            return self.start_result
        raise AssertionError(f'unexpected command: {argv}')


class SdrplayServiceTest(unittest.TestCase):
    def setUp(self):
        self.fake = FakeSystemd()
        self.trusted = []
        self.exists = lambda path: path == '/run/systemd/system'
        self.open_patch = mock.patch.object(service, 'open', mock.mock_open(read_data=b'\x7fELF'), create=True)
        self.access_patch = mock.patch.object(service.os, 'access', return_value=True)
        self.open_patch.start(); self.access_patch.start()

    def tearDown(self):
        self.open_patch.stop(); self.access_patch.stop()

    def trust(self, path):
        self.trusted.append(path)
        return Path(path)

    def starts(self):
        return [call for call in self.fake.calls if call[0][:2] == ('/usr/bin/systemctl', 'start')]

    def assert_refused(self, action, message):
        with self.assertRaises(service.broker.Refused) as raised:
            action()
        self.assertIn(message, str(raised.exception))

    def test_missing_service_never_starts(self):
        self.fake.unit = 'absent.service'
        self.assert_refused(lambda: service.start_service(self.fake.run, self.trust, self.exists),
                            'SDRplay API service was not found')
        self.assertEqual(self.starts(), [])

    def test_inactive_standard_alias_starts_exactly_once(self):
        message = service.start_service(self.fake.run, self.trust, self.exists)
        self.assertIn('started', message)
        self.assertEqual(len(self.starts()), 1)
        self.assertIn('/etc/systemd/system/sdrplay.service', self.trusted)
        self.assertIn('/usr/sbin/sdrplay_apiService', self.trusted)

    def test_active_service_is_reused_without_start(self):
        self.fake.state = 'active'
        self.assertIn('already running', service.start_service(self.fake.run, self.trust, self.exists))
        self.assertEqual(self.starts(), [])

    def test_custom_active_service_is_reused_but_custom_inactive_service_is_refused(self):
        for key, value, refusal in [
                ('Environment', 'VENDOR_OPTION=1', 'overrides, extra commands or environment settings'),
                ('ExecStart', '{ path=/opt/vendor/launcher ; argv[]=/opt/vendor/launcher ; ignore_errors=no ; }',
                 'must start its installed daemon directly')]:
            with self.subTest(key=key):
                self.fake = FakeSystemd(state='active')
                self.fake.extra[key] = value
                self.assertIn('already running', service.start_service(self.fake.run, self.trust, self.exists))
                self.assertEqual(self.starts(), [])
                self.fake = FakeSystemd(state='inactive')
                self.fake.extra[key] = value
                self.assert_refused(lambda: service.start_service(self.fake.run, self.trust, self.exists), refusal)
                self.assertEqual(self.starts(), [])

    def test_failed_service_is_started_once(self):
        self.fake.state = 'failed'
        self.assertIn('started', service.start_service(self.fake.run, self.trust, self.exists))
        self.assertEqual(len(self.starts()), 1)

    def test_start_failure_or_policy_success_without_activation_is_rejected(self):
        self.fake.start_result = {'exitCode': 1, 'output': '', 'timedOut': False}
        self.assert_refused(lambda: service.start_service(self.fake.run, self.trust, self.exists),
                            'SDRplay could not start')
        self.assertEqual(len(self.starts()), 1)
        self.fake = FakeSystemd(); self.fake.start_activates = False
        self.assert_refused(lambda: service.start_service(self.fake.run, self.trust, self.exists),
                            'SDRplay did not become active')
        self.assertEqual(len(self.starts()), 1)

    def test_aliases_are_individually_observed_and_ambiguity_or_transition_blocks(self):
        service.start_service(self.fake.run, self.trust, self.exists)
        shows = [call[0][2] for call in self.fake.calls if call[0][:2] == ('/usr/bin/systemctl', 'show')]
        self.assertEqual(shows[:3], list(service.UNITS))
        self.fake = FakeSystemd(); self.fake.extra['ActiveState'] = 'activating'
        self.assert_refused(lambda: service.start_service(self.fake.run, self.trust, self.exists),
                            'SDRplay is changing state')
        self.fake = FakeSystemd()
        original = self.fake.run
        def ambiguous(argv, timeout=None):
            if argv[:3] == ['/usr/bin/systemctl', 'show', 'sdrplay_apiService.service']:
                return {'exitCode': 0, 'output': properties('sdrplay_apiService.service'), 'timedOut': False}
            return original(argv, timeout)
        self.assert_refused(lambda: service.start_service(ambiguous, self.trust, self.exists),
                            'Multiple SDRplay services were found')

    def test_duplicate_alias_for_the_same_unit_is_deduplicated(self):
        original = self.fake.run
        def duplicate(argv, timeout=None):
            if argv[:3] == ['/usr/bin/systemctl', 'show', 'sdrplay_apiService.service']:
                # A second alias can resolve to the same unit.  Its observation
                # must track the unit through the post-start recheck.
                return {'exitCode': 0, 'output': properties('sdrplay.service', self.fake.state), 'timedOut': False}
            return original(argv, timeout)
        self.assertIn('started', service.start_service(duplicate, self.trust, self.exists))
        self.assertEqual(len(self.starts()), 1)

    def test_multiple_installed_reuses_only_one_active_without_starting_others(self):
        original = self.fake.run
        second_state = 'active'
        def second(argv, timeout=None):
            if argv[:3] == ['/usr/bin/systemctl', 'show', 'sdrplay_apiService.service']:
                return {'exitCode': 0, 'output': properties('sdrplay_apiService.service', second_state), 'timedOut': False}
            return original(argv, timeout)
        self.assertIn('already running', service.start_service(second, self.trust, self.exists))
        self.assertEqual(self.starts(), [])
        self.fake.state = 'active'
        self.assert_refused(lambda: service.start_service(second, self.trust, self.exists), 'Multiple SDRplay services')
        self.fake.state = 'activating'
        self.assert_refused(lambda: service.start_service(second, self.trust, self.exists), 'Multiple SDRplay services')
        self.assertEqual(self.starts(), [])

    def test_timeout_and_unsafe_unit_metadata_block_before_start(self):
        def timed_out(argv, timeout=None):
            if argv[:2] == ['/usr/bin/systemctl', 'show']:
                return {'exitCode': 1, 'output': '', 'timedOut': True}
            return self.fake.run(argv, timeout)
        self.assert_refused(lambda: service.start_service(timed_out, self.trust, self.exists),
                            'Checking the SDRplay service timed out')
        for key, value, message in [
                ('Environment', 'X=1', 'overrides, extra commands or environment settings'),
                ('ExecStartPre', 'x', 'overrides, extra commands or environment settings'),
                ('NeedDaemonReload', 'yes', 'overrides, extra commands or environment settings'),
                ('WorkingDirectory', '/tmp', 'layout requires local administrator review'),
                ('ExecStart', '{ path=/bin/sh ; argv[]=/bin/sh ; ignore_errors=no ; }',
                 'must start its installed daemon directly'),
                ('ExecStart', '{ path=/usr/sbin/sdrplay_apiService ; argv[]=/usr/sbin/sdrplay_apiService --extra ; ignore_errors=no ; }',
                 'unsupported startup arguments or commands')]:
            with self.subTest(key=key):
                self.fake = FakeSystemd(); self.fake.extra[key] = value
                self.assert_refused(lambda: service.start_service(self.fake.run, self.trust, self.exists), message)
                self.assertEqual(self.starts(), [])
        def unsafe_trust(path):
            if path.endswith('sdrplay_apiService'):
                raise service.broker.Refused('UNTRUSTED_PATH', 'unsafe executable')
            return Path(path)
        self.fake = FakeSystemd()
        self.assert_refused(lambda: service.start_service(self.fake.run, unsafe_trust, self.exists),
                            'unsafe executable')
        self.assertEqual(self.starts(), [])

    def test_offline_or_chroot_never_runs_service_commands(self):
        calls = []
        self.assert_refused(lambda: service.start_service(lambda *args: calls.append(args), self.trust, lambda _: False),
                            'No running systemd manager')
        self.assertEqual(calls, [])
        def chroot(argv, timeout=None):
            return {'exitCode': 0, 'output': '', 'timedOut': False}
        self.assert_refused(lambda: service.start_service(chroot, self.trust, self.exists),
                            'disabled in a chroot')

    def test_main_rejects_bad_manifest_without_vendor_download(self):
        with mock.patch.object(service.os, 'geteuid', return_value=0), \
             mock.patch.object(service.broker, 'parse_manifest', return_value=('Kraken',)), \
             mock.patch.object(service, 'start_service') as start:
            self.assertIn('no RSPduo adapter', service.main(['install']))
            start.assert_not_called()
        text = (ROOT / 'script/vectorwarp-sdrplay-service.py').read_text()
        self.assertNotIn('urllib', text); self.assertNotIn('requests', text)

    def test_main_allows_only_fixed_operations_for_rspduo_builds(self):
        with mock.patch.object(service.os, 'geteuid', return_value=0), \
             mock.patch.object(service.broker, 'parse_manifest', return_value=('Kraken', 'RspDuo')), \
             mock.patch.object(service, 'start_service', return_value='started') as start:
            self.assertEqual(service.main(['install']), 'started')
            self.assertEqual(service.main(['start']), 'started')
            self.assert_refused(lambda: service.main(['start', 'sdrplay.service']),
                                'accepts only install or start')
            self.assertEqual(start.call_count, 2)


if __name__ == '__main__':
    unittest.main()
