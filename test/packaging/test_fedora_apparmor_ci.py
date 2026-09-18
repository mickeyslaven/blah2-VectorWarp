#!/usr/bin/env python3
"""Regression checks for temporary, add-only CI AppArmor profile lifecycle."""

import importlib.util
import os
from pathlib import Path
import subprocess
import unittest
from unittest import mock


SPEC = importlib.util.spec_from_file_location(
    'fedora_apparmor_ci', Path(__file__).with_name('fedora_apparmor_ci.py'))
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


class FedoraAppArmorCiTests(unittest.TestCase):
    def test_app_armor_disabled_runs_child_without_profile(self):
        with mock.patch.object(MODULE, 'apparmor_enabled', return_value=False), \
                mock.patch.object(MODULE.os, 'geteuid', return_value=0), \
                mock.patch.dict(os.environ, {MODULE.PROFILE_ENV: 'stale'}, clear=False), \
                mock.patch.object(MODULE, 'run_child', return_value=7) as child, \
                mock.patch.object(MODULE.subprocess, 'run') as parser:
            self.assertEqual(MODULE.run_with_profile(['true']), 7)
        child.assert_called_once()
        self.assertNotIn(MODULE.PROFILE_ENV, child.call_args.args[1])
        parser.assert_not_called()

    def test_profile_is_unique_add_only_and_removed_after_child(self):
        operations = []
        source = []

        def parser(args, **kwargs):
            operations.append(args[1])
            if args[0] == 'podman':
                return subprocess.CompletedProcess(args, 0, '')
            source.append(Path(args[-1]).read_text())
            return subprocess.CompletedProcess(args, 0, '')

        def child(command, environment):
            operations.append('child')
            self.assertRegex(environment[MODULE.PROFILE_ENV],
                             r'^vectorwarp-fedora-ci-[0-9a-f]{32}$')
            return 0

        with mock.patch.object(MODULE, 'apparmor_enabled', return_value=True), \
                mock.patch.object(MODULE.os, 'geteuid', return_value=0), \
                mock.patch.object(Path, 'is_file', return_value=True), \
                mock.patch.object(MODULE.subprocess, 'run', side_effect=parser), \
                mock.patch.object(MODULE, 'run_child', side_effect=child):
            self.assertEqual(MODULE.run_with_profile(['true']), 0)
        self.assertEqual(operations, ['-Q', '-a', 'child', 'ps', '-R'])
        self.assertTrue(all('flags=(default_allow)' in item and '/** ix,' in item
                            for item in source))
        self.assertTrue(all('unix-chkpwd' not in args for args in source))

    def test_add_collision_does_not_remove_existing_profile_or_run_child(self):
        operations = []

        def parser(args, **kwargs):
            operations.append(args[1])
            if args[1] == '-a':
                raise subprocess.CalledProcessError(1, args)
            return subprocess.CompletedProcess(args, 0, '')

        with mock.patch.object(MODULE, 'apparmor_enabled', return_value=True), \
                mock.patch.object(MODULE.os, 'geteuid', return_value=0), \
                mock.patch.object(Path, 'is_file', return_value=True), \
                mock.patch.object(MODULE.subprocess, 'run', side_effect=parser), \
                mock.patch.object(MODULE, 'run_child') as child:
            with self.assertRaises(subprocess.CalledProcessError):
                MODULE.run_with_profile(['true'])
        self.assertEqual(operations, ['-Q', '-a'])
        child.assert_not_called()

    def test_child_failure_is_preserved_and_unload_still_runs(self):
        operations = []

        def parser(args, **kwargs):
            operations.append(args[1])
            return subprocess.CompletedProcess(args, 0, '')

        with mock.patch.object(MODULE, 'apparmor_enabled', return_value=True), \
                mock.patch.object(MODULE.os, 'geteuid', return_value=0), \
                mock.patch.object(Path, 'is_file', return_value=True), \
                mock.patch.object(MODULE.subprocess, 'run', side_effect=parser), \
                mock.patch.object(MODULE, 'run_child', return_value=19):
            self.assertEqual(MODULE.run_with_profile(['false']), 19)
        self.assertEqual(operations, ['-Q', '-a', 'ps', '-R'])

    def test_unload_error_fails_ci(self):
        def parser(args, **kwargs):
            if args[1] == '-R':
                raise subprocess.CalledProcessError(1, args)
            return subprocess.CompletedProcess(args, 0, '')

        with mock.patch.object(MODULE, 'apparmor_enabled', return_value=True), \
                mock.patch.object(MODULE.os, 'geteuid', return_value=0), \
                mock.patch.object(Path, 'is_file', return_value=True), \
                mock.patch.object(MODULE.subprocess, 'run', side_effect=parser), \
                mock.patch.object(MODULE, 'run_child', return_value=0):
            with self.assertRaises(subprocess.CalledProcessError):
                MODULE.run_with_profile(['true'])

    def test_remaining_task_container_blocks_profile_unload(self):
        operations = []

        def parser(args, **kwargs):
            operations.append(args[1])
            output = 'container-id\n' if args[:2] == ['podman', 'ps'] else ''
            return subprocess.CompletedProcess(args, 0, output)

        with mock.patch.object(MODULE, 'apparmor_enabled', return_value=True), \
                mock.patch.object(MODULE.os, 'geteuid', return_value=0), \
                mock.patch.object(Path, 'is_file', return_value=True), \
                mock.patch.object(MODULE.subprocess, 'run', side_effect=parser), \
                mock.patch.object(MODULE, 'run_child', return_value=0):
            with self.assertRaisesRegex(RuntimeError, 'container still exists'):
                MODULE.run_with_profile(['true'])
        self.assertEqual(operations, ['-Q', '-a', 'ps'])

    def test_container_query_failure_blocks_profile_unload(self):
        operations = []

        def parser(args, **kwargs):
            operations.append(args[1])
            if args[:2] == ['podman', 'ps']:
                raise subprocess.CalledProcessError(1, args)
            return subprocess.CompletedProcess(args, 0, '')

        with mock.patch.object(MODULE, 'apparmor_enabled', return_value=True), \
                mock.patch.object(MODULE.os, 'geteuid', return_value=0), \
                mock.patch.object(Path, 'is_file', return_value=True), \
                mock.patch.object(MODULE.subprocess, 'run', side_effect=parser), \
                mock.patch.object(MODULE, 'run_child', return_value=0):
            with self.assertRaises(subprocess.CalledProcessError):
                MODULE.run_with_profile(['true'])
        self.assertEqual(operations, ['-Q', '-a', 'ps'])

    def test_enabled_apparmor_without_parser_fails_closed(self):
        with mock.patch.object(MODULE, 'apparmor_enabled', return_value=True), \
                mock.patch.object(MODULE.os, 'geteuid', return_value=0), \
                mock.patch.object(Path, 'is_file', return_value=False), \
                mock.patch.object(MODULE, 'run_child') as child:
            with self.assertRaisesRegex(RuntimeError, 'parser is missing'):
                MODULE.run_with_profile(['true'])
        child.assert_not_called()


if __name__ == '__main__':
    unittest.main()
