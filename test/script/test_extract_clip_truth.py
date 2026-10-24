#!/usr/bin/env python3

import importlib.util
from pathlib import Path
import unittest


SCRIPT = Path(__file__).parents[2] / "script" / "extract-clip-truth.py"
SPEC = importlib.util.spec_from_file_location("extract_clip_truth", SCRIPT)
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


class ExtractClipTruthTest(unittest.TestCase):
    def test_signal_time_interpolation(self):
        event_id = "eligibility:test"
        statistics = {"eligibilityEvents": [{"id": event_id, "hex": "abc123",
            "outcome": "no_radar_return", "truth_samples": [
                {"position_timestamp_s": 1, "delay_km": 10, "doppler_hz": 20},
                {"position_timestamp_s": 2, "delay_km": 12, "doppler_hz": 16}]}]}
        sidecar = {"eventIds": [event_id], "clipRevision": 1,
                   "windowStart": 1000, "windowEnd": 2000}
        result = MODULE.extract(statistics, sidecar, .5)
        self.assertEqual(len(result["trajectory"]), 2)
        self.assertAlmostEqual(result["trajectory"][0]["delay_km"], 10.5)
        self.assertAlmostEqual(result["trajectory"][1]["doppler_hz"], 17)


if __name__ == "__main__":
    unittest.main()
