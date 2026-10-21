#!/usr/bin/env python3
import importlib.util
import unittest
from pathlib import Path

import numpy as np

SCRIPT = Path(__file__).parents[2] / 'script' / 'analyze-loaded-reciprocal-mchq.py'
SPEC = importlib.util.spec_from_file_location('loaded_reciprocal', SCRIPT)
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


class LoadedReciprocalTest(unittest.TestCase):
    def test_score_uses_one_cell_per_trajectory_frame(self):
        maps = {0.1: [np.ones((5, 12)) for _ in range(4)]}
        trajectory = [{'delay_km': 2 * MODULE.C_KM_S / MODULE.SAMPLE_RATE,
                       'doppler_hz': 0} for _ in range(4)]
        scores, controls = MODULE.score(maps, np.arange(-2, 3), trajectory,
                                        [0.1], guard=0, training=1,
                                        controls_stride=2)
        self.assertEqual(len(controls['target_rows']), 4)
        self.assertEqual(len(scores['0.1']['per_frame_ratio_db']), 4)

    def test_loading_is_finite_on_zero_reference_bins(self):
        iq = np.zeros((2, 4096), dtype=np.complex64)
        axis, maps, diagnostics = MODULE.caf_frame(iq, 1024, 16, 100, [.01])
        self.assertTrue(np.isfinite(maps[.01]).all())
        self.assertTrue(np.isfinite(diagnostics['0.01']['filter_energy']))
        self.assertTrue(np.isfinite(diagnostics['0.01']['relative_noise_gain_db_at_equal_mean_response']))
        self.assertGreater(len(axis), 0)


if __name__ == '__main__':
    unittest.main()
