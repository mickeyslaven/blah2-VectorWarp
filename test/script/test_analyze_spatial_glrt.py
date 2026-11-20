#!/usr/bin/env python3

import importlib.util
import copy
import math
from pathlib import Path
import unittest


SCRIPT = Path(__file__).parents[2] / "script" / "analyze-spatial-glrt.py"
SPEC = importlib.util.spec_from_file_location("spatial_glrt", SCRIPT)
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


class SpatialGlrtTest(unittest.TestCase):
    def test_shared_algebra_preserves_empty_shape_nonfinite_and_alias_behavior(self):
        for name in ("inverse", "multiply", "dot"):
            self.assertIs(getattr(MODULE, name), getattr(MODULE.CAL, name))
        self.assertEqual(MODULE.inverse([]), [])
        self.assertEqual(MODULE.inverse([[2 + 2j]]), [[.25 - .25j]])
        matrix = [[0, 2], [1, 3]]
        before = copy.deepcopy(matrix)
        inverse = MODULE.inverse(matrix)
        self.assertEqual(inverse, [[-1.5, 1.0], [.5, 0.0]])
        self.assertEqual(matrix, before)
        inverse[0][0] = 99
        self.assertEqual(matrix, before)
        self.assertEqual(inverse[1], [.5, 0.0])
        row = [1, 2]
        with self.assertRaisesRegex(ValueError, "singular covariance"):
            MODULE.inverse([row, row])
        self.assertEqual(row, [1, 2])
        # Preserve the existing zip truncation and empty-sum contracts.
        self.assertEqual(MODULE.multiply([[1, 2, 3], [4]], [2, 3]), [8, 8])
        self.assertEqual(MODULE.multiply([[], [1]], []), [0, 0])
        self.assertEqual(MODULE.dot([1 + 2j, 3], [2 - 1j]), -5j)
        self.assertEqual(MODULE.dot([], []), 0)
        self.assertTrue(math.isnan(MODULE.inverse([[float("nan")]])[0][0]))
        self.assertEqual(MODULE.inverse([[float("inf")]]), [[0.0]])
        self.assertTrue(math.isnan(MODULE.multiply([[float("inf")]], [0])[0]))
        self.assertEqual(MODULE.inverse([[]]), [[]])

    def test_complex_inverse(self):
        matrix = [[4 + 0j, 1 + 1j], [1 - 1j, 3 + 0j]]
        result = MODULE.inverse(matrix)
        for column in range(2):
            product = MODULE.multiply(matrix, [result[row][column] for row in range(2)])
            self.assertAlmostEqual(product[column].real, 1)
            self.assertAlmostEqual(product[1 - column].real, 0)
            self.assertAlmostEqual(product[1 - column].imag, 0)


if __name__ == "__main__":
    unittest.main()
