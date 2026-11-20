#!/usr/bin/env python3
import importlib.util
from pathlib import Path


script = Path(__file__).parents[2] / "script" / "run-iq-protection-replay.py"
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
    for bad in ("GPU_TRACKER_PREPARED frame=1 detections=2\n" * 2,
                "GPU_TRACKER_PREPARED frame=1 detections=-1",
                "x" * ((16 << 20) + 1)):
        try:
            module.parse_tracker_diagnostics(bad)
        except ValueError:
            pass
        else:
            raise AssertionError("Invalid/duplicate/unbounded tracker diagnostics accepted")
    assert module.parse_tracker_diagnostics("NOT_GPU_TRACKER_PREPARED frame=1 detections=1")["frames"] == 0
