#!/usr/bin/env python3
import importlib.util
import unittest

SPEC = importlib.util.spec_from_file_location('accelerated', 'script/analyze-accelerated-motion-truth.py')
MODULE = importlib.util.module_from_spec(SPEC); SPEC.loader.exec_module(MODULE)


def frame(target_column):
    data = [[0] * 31 for _ in range(6)]
    data[3][target_column] = 20
    return {'delay': list(range(31)), 'doppler': [-400, -200, 0, 20, 200, 400], 'data': data}


class AcceleratedMotionTest(unittest.TestCase):
    def test_quadratic_path_beats_stationary_control(self):
        # Doppler remains 20; a quadratic delay path hits bins 10, 10, 12.
        maps = [frame(10), frame(10), frame(12)]
        result = MODULE.analyze(maps, {'target': {'delay': 10.5, 'doppler': 20}}, 1,
            '-1:0:1', '0:0:1', '1:1:1', '0:0:1', guard=0, train=1, min_doppler=1)
        target = result['aircraft']['target']
        self.assertEqual(target['best_delay_acceleration_km_s2'], 1)
        self.assertGreater(target['integrated_local_ratio_db'], target['control_p99_local_ratio_db'])
        self.assertTrue(target['candidate_only'])

    def test_invalid_range_is_rejected(self):
        with self.assertRaises(ValueError):
            MODULE.values('1:0:1')


if __name__ == '__main__':
    unittest.main()
