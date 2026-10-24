#!/usr/bin/env python3
import importlib.util
import unittest

SPEC = importlib.util.spec_from_file_location(
    "calibrated", "script/analyze-calibrated-complex-replay.py")
MODULE = importlib.util.module_from_spec(SPEC); SPEC.loader.exec_module(MODULE)


class CalibratedReplayTest(unittest.TestCase):
    def test_nearby_detection_uses_the_closest_candidate(self):
        truth = {"delay": 8.6, "doppler": -30}
        documents = [{"delay": [8.7, 8.61], "doppler": [-20, -29], "snr": [9, 4]}]
        matches = MODULE.nearby_detections(documents, truth, .5, 25)
        self.assertEqual(len(matches), 1)
        self.assertAlmostEqual(matches[0]["delay_bias_km"], .01)
        self.assertAlmostEqual(matches[0]["doppler_bias_hz"], 1)

    def test_track_delay_bins_convert_to_kilometres(self):
        truth = {"delay": MODULE.C / 2400000 / 1000 * 10, "doppler": 20}
        tracks = [{"data": [{"id": "1", "state": "ACTIVE",
            "associated_delay": [10, 10, 10], "associated_doppler": [20, 20, 20]}]}]
        evidence = MODULE.track_evidence(tracks, truth, 2400000, .1, 1)
        self.assertEqual(evidence["1"]["near_samples"], 3)
        self.assertTrue(evidence["1"]["active"])

    def test_p99_uses_the_empirical_upper_tail(self):
        self.assertEqual(MODULE.p99(list(range(100))), 99)


if __name__ == "__main__":
    unittest.main()
