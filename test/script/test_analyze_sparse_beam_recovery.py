#!/usr/bin/env python3

import importlib.util
import unittest
from pathlib import Path

import numpy as np


SCRIPT = Path(__file__).parents[2] / "script" / "analyze-sparse-beam-recovery.py"
SPEC = importlib.util.spec_from_file_location("sparse_beam", SCRIPT)
SPARSE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(SPARSE)


class SparseBeamRecoveryTest(unittest.TestCase):
    def test_complex_lasso_retains_dominant_sparse_component(self):
        dictionary = np.eye(4, dtype=np.complex128)
        observation = np.asarray([3 + 4j, .05j, 0, 0], dtype=np.complex128)
        recovered = SPARSE.lasso(dictionary, observation, .2)
        self.assertGreater(abs(recovered[0]), 4.7)
        self.assertEqual(0, abs(recovered[1]))
        self.assertEqual(0, abs(recovered[2]))

    def test_soft_threshold_preserves_complex_phase(self):
        value = np.asarray([3 + 4j], dtype=np.complex128)
        shrunk = SPARSE.soft(value, 1)[0]
        self.assertAlmostEqual(4.0, abs(shrunk))
        self.assertAlmostEqual((3 + 4j) / 5, shrunk / abs(shrunk))


if __name__ == "__main__":
    unittest.main()
