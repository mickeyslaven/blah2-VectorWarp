import importlib.util
from pathlib import Path
import unittest

import numpy as np


SCRIPT = Path(__file__).parents[2] / "script" / "qualify-external-reference.py"
SPEC = importlib.util.spec_from_file_location("external_reference", SCRIPT)
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


class ExternalReferenceQualificationTest(unittest.TestCase):
    def test_refines_sub_bin_carrier_offset(self):
        rng = np.random.default_rng(19)
        rate, count, expected = 240_000, 240_000, 13.37
        source = (rng.normal(size=count) + 1j * rng.normal(size=count)).astype(
            np.complex64)
        time = np.arange(count) / rate
        template = source * np.exp(2j * np.pi * expected * time)
        refined, residual = MODULE.refine_cfo(template, source, rate, 13.0,
                                              max_residual_hz=1.0)
        self.assertAlmostEqual(refined, expected, delta=.01)
        self.assertAlmostEqual(residual, .37, delta=.01)

    def test_recovers_independent_clock_and_carrier(self):
        rng = np.random.default_rng(7)
        hp_rate, owl_rate, duration = 240_000, 200_000, 2.3
        expected_ppm, seed = 37.0, 120_000
        source = rng.normal(size=int(duration * hp_rate)) + 1j * rng.normal(
            size=int(duration * hp_rate))
        spectrum = np.fft.fft(source)
        frequency = np.fft.fftfreq(len(source), 1 / hp_rate)
        spectrum[np.abs(frequency) > 70_000] = 0
        hp = np.fft.ifft(spectrum).astype(np.complex64)
        actual_owl_rate = owl_rate * (1 + expected_ppm * 1e-6)
        index = np.arange(int(seed + duration * actual_owl_rate + 1000))
        hp_position = (index - seed) / actual_owl_rate * hp_rate
        left = np.floor(hp_position).astype(int)
        valid = (left >= 0) & (left + 1 < len(hp))
        owl = np.zeros(len(index), np.complex64)
        fraction = hp_position[valid] - left[valid]
        owl[valid] = (hp[left[valid]] * (1 - fraction) +
                      hp[left[valid] + 1] * fraction)
        owl *= np.exp(2j * np.pi * 321 * index / actual_owl_rate)
        noise = .1 * (rng.normal(size=len(hp)) + 1j * rng.normal(size=len(hp)))
        qualified, candidates = MODULE.qualify(
            np.stack((noise, hp, .2 * hp)).astype(np.complex64), owl,
            hp_rate, owl_rate, seed)
        self.assertTrue(qualified)
        self.assertIn(candidates[0]["kraken_channel"], (1, 2))
        self.assertAlmostEqual(candidates[0]["sample_clock_offset_ppm"],
                               expected_ppm, delta=15)
        self.assertGreater(candidates[0]["median_coherence"], .8)


if __name__ == "__main__":
    unittest.main()
