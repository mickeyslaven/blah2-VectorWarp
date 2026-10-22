#!/usr/bin/env python3
import importlib.util
from pathlib import Path
import unittest

import numpy as np

SCRIPT = Path(__file__).parents[2] / "script" / "analyze-noisy-reference-glrt.py"
SPEC = importlib.util.spec_from_file_location("noisy_reference_glrt", SCRIPT)
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


class NoisyReferenceGlrtTest(unittest.TestCase):
    def test_statistics_increase_with_shared_signal(self):
        uncorrelated = MODULE.cell_statistics(100.0, 100.0, 0j, 1.0)
        correlated = MODULE.cell_statistics(100.0, 100.0, 90 + 0j, 1.0)
        self.assertTrue(all(right > left for left, right in
                            zip(uncorrelated, correlated)))
        self.assertAlmostEqual(uncorrelated[1], .5)
        self.assertAlmostEqual(correlated[2], .81)

    def test_score_retains_and_ranks_every_candidate_before_labeling(self):
        shape = (7, 7)
        base = np.ones(shape)
        surfaces = {name: [base.copy()] for name in
                    ("glrt_known_estimated_noise", "glrt_unknown_noise", "normalized_caf")}
        for values in surfaces.values():
            values[0][3, 3] = 2.0
            values[0][1, 1] = 4.0
        result, candidates, _, _ = MODULE.score(
            surfaces, np.arange(-3, 4), [{"doppler_hz": 0, "delay_km":
            3 * MODULE.MCHQ.C_KM_S / MODULE.MCHQ.SAMPLE_RATE}], guard=0,
            controls_stride=1)
        self.assertEqual(len(candidates), 49)
        self.assertEqual(result["glrt_unknown_noise"]["target_rank_of_all_candidates"], 2)
        self.assertFalse(result["glrt_unknown_noise"]["validated_at_plus_3db"])
        self.assertTrue(all("ranks" in candidate and "scores" in candidate
                            for candidate in candidates))


if __name__ == "__main__":
    unittest.main()
