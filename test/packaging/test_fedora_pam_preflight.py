#!/usr/bin/env python3
"""Unit checks for the diagnostic-only Fedora PAM CI preflight."""

import importlib.util
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest import mock


SPEC = importlib.util.spec_from_file_location(
    'fedora_pam_preflight', Path(__file__).with_name('fedora_pam_preflight.py'))
PREFLIGHT = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(PREFLIGHT)


class FedoraPamPreflightTests(unittest.TestCase):
    def test_uses_pinned_image_and_installed_test_runtime_bounds(self):
        build = PREFLIGHT.build_args('localhost/probe')
        run = PREFLIGHT.run_args('probe', 'localhost/probe')
        self.assertIn('BASE_IMAGE=' + PREFLIGHT.IMAGE, build)
        self.assertIn('Containerfile.service-rpm', ' '.join(build))
        for option in ('--memory=2g', '--memory-swap=2g'):
            self.assertIn(option, build)
            self.assertIn(option, run)
        for option in ('--systemd=always', '--cgroupns=private',
                       '--cap-add=SYS_ADMIN', '--security-opt=label=disable',
                       '--security-opt=apparmor=unconfined', '--pids-limit=512'):
            self.assertIn(option, run)
        self.assertNotIn('--privileged', run)
        self.assertNotIn('--volume', run)
        self.assertNotIn('--device', run)

    def test_kernel_evidence_is_helper_only_and_bounded(self):
        raw = 'unrelated secret kernel record\n' + '\n'.join(
            f'apparmor="DENIED" profile="unix-chkpwd" capname="dac_override" #{i}'
            for i in range(120))
        lines = PREFLIGHT.helper_kernel_lines(raw)
        self.assertEqual(len(lines), 100)
        self.assertNotIn('unrelated secret', '\n'.join(lines))
        self.assertTrue(all('unix-chkpwd' in line for line in lines))

    def test_failed_pam_is_not_retried_or_healed_and_container_is_removed(self):
        calls = []

        def fake_command(args, *, output=None, check=True):
            calls.append((args, check))
            if 'is-system-running' in args:
                value = 'running\n'
            elif args[-2:] == ['-u', 'vectorwarp-package-test']:
                value = '1000\n'
            elif args[-2:] == ['-g', 'vectorwarp-package-test']:
                value = '1000\n'
            else:
                value = ''
            status = 1 if '/usr/bin/sudo' in args else 0
            return subprocess.CompletedProcess(args, status, value)

        with tempfile.TemporaryDirectory() as directory, \
                mock.patch.object(PREFLIGHT, 'command', side_effect=fake_command), \
                mock.patch.object(PREFLIGHT, 'host_diagnostics') as diagnostics, \
                mock.patch.object(PREFLIGHT.os, 'geteuid', return_value=0):
            with self.assertRaisesRegex(RuntimeError, 'sudo/PAM failed'):
                PREFLIGHT.probe(Path(directory))
            diagnostics.assert_called_once()
        sudo = [args for args, _ in calls if '/usr/bin/sudo' in args]
        self.assertEqual(len(sudo), 1)
        self.assertEqual(sudo[0][-4:], ['/usr/bin/sudo', '-n', '--', '/usr/bin/true'])
        self.assertTrue(any(args[:2] == ['podman', 'stop'] for args, _ in calls))
        self.assertTrue(any(args[:2] == ['podman', 'rm'] for args, _ in calls))
        self.assertTrue(any(args[:2] == ['podman', 'rmi'] for args, _ in calls))
        all_arguments = '\n'.join(' '.join(args) for args, _ in calls)
        self.assertIn('NOPASSWD: /usr/bin/true', all_arguments)
        self.assertNotIn('apparmor_parser', all_arguments)
        self.assertNotIn('chpasswd', all_arguments)

    def test_diagnostic_failure_does_not_hide_pam_failure_or_skip_cleanup(self):
        calls = []

        def fake_command(args, *, output=None, check=True):
            calls.append(args)
            if 'is-system-running' in args:
                value = 'running\n'
            elif args[-2:] in (['-u', 'vectorwarp-package-test'],
                               ['-g', 'vectorwarp-package-test']):
                value = '1000\n'
            else:
                value = ''
            return subprocess.CompletedProcess(args, int('/usr/bin/sudo' in args), value)

        with tempfile.TemporaryDirectory() as directory, \
                mock.patch.object(PREFLIGHT, 'command', side_effect=fake_command), \
                mock.patch.object(PREFLIGHT, 'host_diagnostics',
                                  side_effect=OSError('journal unavailable')), \
                mock.patch.object(PREFLIGHT.os, 'geteuid', return_value=0):
            with self.assertRaisesRegex(RuntimeError, 'sudo/PAM failed'):
                PREFLIGHT.probe(Path(directory))
        self.assertTrue(any(args[:2] == ['podman', 'stop'] for args in calls))
        self.assertTrue(any(args[:2] == ['podman', 'rm'] for args in calls))
        self.assertTrue(any(args[:2] == ['podman', 'rmi'] for args in calls))

    def test_root_required_before_any_container_change(self):
        with tempfile.TemporaryDirectory() as directory, \
                mock.patch.object(PREFLIGHT.os, 'geteuid', return_value=1000), \
                mock.patch.object(PREFLIGHT, 'command') as command:
            with self.assertRaisesRegex(RuntimeError, 'Run with sudo'):
                PREFLIGHT.probe(Path(directory))
            command.assert_not_called()


if __name__ == '__main__':
    unittest.main()
