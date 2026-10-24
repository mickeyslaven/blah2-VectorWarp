#!/usr/bin/env python3

import importlib.util
import json
import tempfile
import unittest
from pathlib import Path


SCRIPT = Path(__file__).parents[2] / "script" / "audit-adsb-uncertainty-tubes.py"
SPEC = importlib.util.spec_from_file_location("audit_adsb_tubes", SCRIPT)
AUDIT = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(AUDIT)


def manifest(frame):
    return {"adsb_reconciliation": {"frequency_hz": 527e6}, "rows": [{
        "event_id": "e", "hex": "abc123", "role": "miss", "outcome": "no_radar_return",
        "capture": {"file": "/recording.mchq", "revision": 1,
                    "window_start": 1000, "window_end": 11000},
        "replay_trajectory": [{"timestamp_ms": 1250, "delay_km": 10,
            "doppler_hz": 100, "adsb_reconciliation": frame}]}]}


class AuditAdsbUncertaintyTubesTest(unittest.TestCase):
    def run_one(self, frame):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "x.json"
            path.write_text(json.dumps(manifest(frame)))
            return AUDIT.audit_manifest(path)[0]

    def test_missing_observation_is_unbounded_and_quarantined(self):
        row = self.run_one({"candidates": [], "timing_error_ms": None})
        self.assertIn("unbounded_truth", row["reasons"])
        self.assertTrue(row["outside_fixed_search"])

    def test_position_only_source_disagreement_cannot_bound_doppler(self):
        row = self.run_one({"candidates": [{}, {}], "timing_error_ms": 100,
                            "observation_event_time_ms": 1150,
                            "disagreement": {"horizontal_km": .2}})
        self.assertIsNone(row["required_doppler_radius_hz"])
        self.assertIn("unbounded_truth", row["reasons"])

    def test_fresh_bounded_frame_still_requires_projection_and_multisource(self):
        row = self.run_one({"candidates": [{}], "timing_error_ms": 100,
                            "observation_event_time_ms": 1150})
        self.assertNotIn("non_fresh_truth", row["reasons"])
        self.assertIn("held_not_projected_to_cpi", row["reasons"])
        self.assertIn("not_multisource_every_cpi", row["reasons"])
        self.assertAlmostEqual(row["required_doppler_radius_hz"],
                               2 * 527e6 / AUDIT.C_MPS, places=9)


if __name__ == "__main__":
    unittest.main()
