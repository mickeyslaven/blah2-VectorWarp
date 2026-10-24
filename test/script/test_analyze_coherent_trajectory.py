#!/usr/bin/env python3

import importlib.util
import math
from pathlib import Path
import unittest


SCRIPT = Path(__file__).parents[2] / "script" / "analyze-coherent-trajectory.py"
SPEC = importlib.util.spec_from_file_location("coherent_trajectory", SCRIPT)
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


class CoherentTrajectoryTest(unittest.TestCase):
    def test_phase_and_migration_compensation(self):
        frames = 6
        cpi = .5
        doppler0 = 20
        rate = 2
        cube = []
        cutouts = []
        for frame in range(frames):
            cutouts.append({"delay_km": [0, 1, 2], "doppler_hz": [18, 20, 22, 24, 26]})
            beam = [[0j] * 3 for _ in range(5)]
            time = frame * cpi
            row = MODULE.nearest(cutouts[-1]["doppler_hz"], doppler0 + rate * time)
            beam[row][1] = complex(math.cos(2 * math.pi * (doppler0 * time + .5 * rate * time ** 2)),
                                   math.sin(2 * math.pi * (doppler0 * time + .5 * rate * time ** 2)))
            cube.append([beam])
        result = MODULE.score_anchor(cube, cutouts, 1, 1, cpi, [0], [rate], [0])
        self.assertAlmostEqual(result["score"], frames)
        self.assertEqual(result["doppler_rate_hz_s"], rate)


if __name__ == "__main__":
    unittest.main()
