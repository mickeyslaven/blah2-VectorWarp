#!/usr/bin/env python3
import importlib.util
from pathlib import Path


script = Path(__file__).parents[2] / "script" / "replay-fusion-ab-cass.py"
spec = importlib.util.spec_from_file_location("replay_fusion", script)
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


def test_aggregates_all_prepared_frames():
    log = """
noise
GPU_TRACKER_PREPARED frame=1 detections=8 device_associated=2 unmatched_detections=6 stationary_clutter_suppressed=3 admitted_detections=3 dropped_capacity_detections=0
GPU_TRACKER_PREPARED frame=2 detections=5 device_associated=1 unmatched_detections=4 stationary_clutter_suppressed=1 admitted_detections=2 dropped_capacity_detections=1
"""
    assert module.parse_tracker_diagnostics(log) == {
        "frames": 2,
        "totals": {
            "detections": 13,
            "device_associated": 3,
            "unmatched_detections": 10,
            "stationary_clutter_suppressed": 4,
            "admitted_detections": 5,
            "dropped_capacity_detections": 1,
        },
    }


if __name__ == "__main__":
    test_aggregates_all_prepared_frames()
