#!/usr/bin/env python3
"""Pure regression of the actual candidate predicate; no processor is launched."""
import importlib.util
from pathlib import Path
import sys
import unittest

HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location('candidate_replay_test', HERE / 'processor_replay_test.py')
module = importlib.util.module_from_spec(spec)
sys.dont_write_bytecode = True
spec.loader.exec_module(module)


def progress(**changes):
    value = {'input': 'replay', 'state': 'draining', 'error': '', 'loops': 2}
    value.update(changes)
    return value


class LoopProgressPredicateTest(unittest.TestCase):
    def test_actual_saved_draining_history_proves_completed_loops(self):
        observed = [progress(loops=count) for count in (64, 94, 121)]
        self.assertEqual([item['loops'] for item in observed], [64, 94, 121])
        self.assertTrue(module.loop_progress_observed(observed))

    def test_both_active_phases_at_threshold(self):
        for state in ('playing', 'draining'):
            with self.subTest(state=state):
                self.assertTrue(module.loop_progress_observed([progress(state=state)]))

    def test_insufficient_or_invalid_counts(self):
        for value in (0, 1, -1, True, False, None, 2.0, '2', [], {}):
            with self.subTest(value=value):
                self.assertFalse(module.loop_progress_observed([progress(loops=value)]))
        missing = progress(); del missing['loops']
        self.assertFalse(module.loop_progress_observed([missing]))

    def test_nonactive_or_invalid_states(self):
        for value in ('loading', 'error', 'stopped', 'complete', '', None, False, 2):
            with self.subTest(state=value):
                self.assertFalse(module.loop_progress_observed([progress(state=value)]))

    def test_error_or_wrong_input_does_not_pass(self):
        for value in ('read failed', None, False, 0):
            with self.subTest(error=value):
                self.assertFalse(module.loop_progress_observed([progress(error=value)]))
        missing = progress(); del missing['error']
        self.assertFalse(module.loop_progress_observed([missing]))
        self.assertFalse(module.loop_progress_observed([progress(input='live')]))
        missing = progress(); del missing['input']
        self.assertFalse(module.loop_progress_observed([missing]))

    def test_empty_malformed_and_retained_history(self):
        self.assertFalse(module.loop_progress_observed([]))
        self.assertFalse(module.loop_progress_observed([None, 'bad', 2, {}, progress(loops=1)]))
        self.assertTrue(module.loop_progress_observed([progress(), progress(state='stopped')]))
        self.assertFalse(module.loop_progress_observed([progress(loops=1), progress(state='stopped')]))


if __name__ == '__main__':
    unittest.main()
