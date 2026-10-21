#!/usr/bin/env python3

import importlib.util
from pathlib import Path
import unittest


SCRIPT = Path(__file__).parents[2] / "script" / "analyze-spatial-glrt.py"
SPEC = importlib.util.spec_from_file_location("spatial_glrt", SCRIPT)
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


class SpatialGlrtTest(unittest.TestCase):
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
