#!/usr/bin/env python3
"""Offline diagnostic-gate fixtures: no GPU, debugger, Homebrew or receiver."""
import contextlib
import importlib.util
import io
import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

SPEC = importlib.util.spec_from_file_location('probe', Path(__file__).with_name('standalone_driver_probe.py'))
probe = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(probe)


class DriverProbeTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.output = self.root / 'stdout'
        self.valid = {'version': 1, 'available': True, 'qualification': 'not-run', 'devices': []}
        self.output.write_text(json.dumps(self.valid))

    def test_protocol_and_nonzero_status_are_fail_closed(self):
        good = {'returncode': 0, 'timed_out': False}
        self.assertTrue(probe.valid_response(good, self.output))
        for result in ({'returncode': -6, 'timed_out': False},
                       {'returncode': 1, 'timed_out': False},
                       {'returncode': 0, 'timed_out': True}):
            self.assertFalse(probe.valid_response(result, self.output))
        for payload in ('invalid', '[]', '{}', ' ' * 32769,
                        json.dumps({**self.valid, 'qualification': 'passed'})):
            self.output.write_text(payload)
            self.assertFalse(probe.valid_response(good, self.output))

    def test_actual_timeout_kills_and_reaps_child(self):
        result = probe.bounded(['/bin/sh', '-c', 'exec /bin/sleep 30'], dict(os.environ),
                               self.output, self.root / 'stderr', 0.05)
        self.assertTrue(result['timed_out'])
        self.assertEqual(result['returncode'], -9)

    def run_main(self, observe):
        worker = self.root / 'worker'
        worker.touch()
        evidence = self.root / ('observe' if observe else 'gate')
        calls = []
        def command(argv, environment, stdout, stderr, timeout):
            calls.append(argv)
            stdout.write_text('')
            stderr.write_text('fixture failure')
            return {'returncode': -6 if len(calls) == 1 else 0, 'timed_out': False}
        args = ['probe', '--worker', str(worker), '--evidence', str(evidence)]
        if observe:
            args.append('--observe-only')
        with patch.object(probe, 'bounded', command), patch('sys.argv', args), contextlib.redirect_stdout(io.StringIO()):
            code = probe.main()
        result = json.loads((evidence / 'result.json').read_text())
        self.assertFalse(result['accepted'])
        self.assertEqual(result['returncode'], -6)
        self.assertEqual(result['debugger']['returncode'], 0)
        self.assertEqual(len(calls), 2)
        debugger = calls[1]
        self.assertIn('--no-lldbinit', debugger)
        crash_commands = [debugger[i + 1] for i, value in enumerate(debugger) if value == '-k']
        self.assertEqual(crash_commands, ['thread backtrace all', 'image list', 'process kill', 'quit'])
        return code

    def test_successful_debugger_cannot_clear_failed_gate(self):
        self.assertEqual(self.run_main(False), 1)

    def test_explicit_control_records_failure_without_becoming_acceptance(self):
        self.assertEqual(self.run_main(True), 0)


if __name__ == '__main__':
    unittest.main()
