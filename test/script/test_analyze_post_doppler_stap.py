#!/usr/bin/env python3

import importlib.util
import math
from pathlib import Path
import unittest


SCRIPT = Path(__file__).parents[2] / "script" / "analyze-post-doppler-stap.py"
SPEC = importlib.util.spec_from_file_location("post_doppler_stap", SCRIPT)
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


class PostDopplerStapTest(unittest.TestCase):
    def test_complex_inverse(self):
        matrix = [[4 + 0j, 1 + 1j], [1 - 1j, 3 + 0j]]
        result = MODULE.inverse(matrix)
        for column in range(2):
            product = MODULE.multiply(matrix, [result[row][column] for row in range(2)])
            self.assertAlmostEqual(product[column].real, 1)
            self.assertAlmostEqual(product[1 - column].real, 0)
            self.assertAlmostEqual(product[1 - column].imag, 0)

    def test_doppler_signature_peaks_at_exact_bin(self):
        frame = {"doppler_hz": [-2.0, 0.0, 2.0]}
        signature = MODULE.doppler_signature(frame, 0.0, 1, 1, 1)
        self.assertAlmostEqual(abs(signature[0]), 0.0, places=12)
        self.assertAlmostEqual(abs(signature[1]), 1.0)
        self.assertAlmostEqual(abs(signature[2]), 0.0, places=12)

    def test_empirical_percentile_is_nearest_rank(self):
        self.assertEqual(MODULE.percentile(list(range(100)), .99), 98)

    def test_covariance_uses_three_doppler_by_five_channel_snapshot(self):
        frame = {"doppler_hz": list(range(9)), "delay_km": list(range(9)),
                 "channels": []}
        for channel in range(5):
            frame["channels"].append([
                [[float((row + 1) * (channel + 1) + column),
                  float((column + 1) * (channel + 2) - row)]
                 for column in range(9)] for row in range(9)])
        precision, report = MODULE.robust_covariance(
            frame, 4, 4, radius=1, exclusion=1, loading=.1, energy_clip=3)
        self.assertEqual(report["dimension"], 15)
        self.assertEqual(len(precision), 15)
        self.assertGreaterEqual(report["raw_samples"], 15)


if __name__ == "__main__":
    unittest.main()
