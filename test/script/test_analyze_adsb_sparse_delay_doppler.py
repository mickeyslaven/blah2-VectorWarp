#!/usr/bin/env python3

import importlib.util
import unittest
from pathlib import Path


SCRIPT = Path(__file__).parents[2] / "script" / "analyze-adsb-sparse-delay-doppler.py"
SPEC = importlib.util.spec_from_file_location("adsb_sparse_dd", SCRIPT)
SPARSE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(SPARSE)


class AdsbSparseDelayDopplerTest(unittest.TestCase):
    def test_group_soft_uses_common_five_channel_support(self):
        values = [[value, 0] for value in [1, 1j, -1, -1j, 1]]
        recovered = SPARSE.group_soft(values, 1.0)
        self.assertTrue(all(abs(row[0]) > 0 for row in recovered))
        self.assertTrue(all(row[1] == 0 for row in recovered))

    def test_sparse_solve_rejects_smooth_nuisance_and_keeps_echo(self):
        delay = list(range(9)) * 5
        doppler = [value for value in range(5) for _ in range(9)]
        nuisance = [2 + .1 * d + .2j * f for d, f in zip(delay, doppler)]
        observations = [[(channel + 1) * value for value in nuisance]
                        for channel in range(5)]
        for row, echo in zip(observations, [4, 3j, -2, 2 + 2j, -3j]):
            row[22] += echo
        coefficients, _ = SPARSE.group_sparse_solve(
            observations, delay, doppler, penalty=.25, iterations=250)
        recovered = [sum(abs(row[column]) ** 2 for row in coefficients) ** .5
                     for column in range(len(delay))]
        self.assertEqual(22, max(range(len(recovered)), key=recovered.__getitem__))
        self.assertGreater(recovered[22], 5 * sorted(recovered)[len(recovered) // 2])

    def test_physical_range_rate_penalizes_wrong_doppler(self):
        trajectory = [
            {"timestamp_ms": 1000 * index, "delay_km": 10 - .05 * index}
            for index in range(5)]
        expected = 563e6 * 50 / SPARSE.C_MPS
        good, good_rms, _ = SPARSE.physical_consistency(
            trajectory, [expected] * 5, 563e6, 20)
        bad, bad_rms, _ = SPARSE.physical_consistency(
            trajectory, [-expected] * 5, 563e6, 20)
        self.assertGreater(good, bad)
        self.assertLess(good_rms, bad_rms)

    def test_corrected_manifest_trajectory_is_used(self):
        row = {"event_id": "event", "hex": "abc123", "role": "miss",
               "outcome": "no_radar_return", "truth_source": "interpolated_position_samples",
               "capture": {"revision": 7},
               "replay_trajectory": [{"timestamp_ms": 1, "delay_km": 2,
                                      "doppler_hz": 3}]}
        trajectory, provenance = SPARSE.trajectory_from_document(
            {"rows": [row]}, event_id="event", revision=7)
        self.assertEqual(row["replay_trajectory"], trajectory)
        self.assertEqual("corrected_historical_manifest", provenance["source"])
        self.assertEqual("interpolated_position_samples", provenance["truth_source"])

    def test_window_is_not_silently_clamped_at_doppler_edge(self):
        frame = {"doppler_hz": list(range(-24, 25, 2)), "delay_km": list(range(9)),
                 "channels": [[[[0, 0] for _ in range(9)] for _ in range(25)]
                              for _ in range(5)]}
        self.assertIsNotNone(SPARSE._window(
            frame, 12, 4, SPARSE.DEFAULT_DELAY_OFFSETS,
            SPARSE.DEFAULT_DOPPLER_OFFSETS_HZ))
        self.assertIsNone(SPARSE._window(
            frame, 0, 4, SPARSE.DEFAULT_DELAY_OFFSETS,
            SPARSE.DEFAULT_DOPPLER_OFFSETS_HZ))


if __name__ == "__main__":
    unittest.main()
