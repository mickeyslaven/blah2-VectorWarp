#!/usr/bin/env python3
"""Apply and measure the lightly clipped 551 MHz OWL companion stage."""

import importlib.util
import json
from pathlib import Path
import sys
import time


SOURCE = Path(__file__).with_name("run-551-owl-high-gain-trial.py")
SPEC = importlib.util.spec_from_file_location("gain_trial", SOURCE)
trial = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(trial)
EDGE_GAINS = [33.8, 33.8, 36.4, 29.7, 28.0]


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: run-551-owl-edge-stage.py ABSOLUTE_OUTPUT_DIR")
    root = Path(sys.argv[1])
    trial.require(root.is_absolute(), "output path must be absolute")
    root.mkdir(parents=True, exist_ok=False)
    original, revision = trial.request("/api/config")
    trial.atomic(root / "original-config.json", original)
    state = {"schema": "vectorwarp-551-owl-edge-stage-v1", "status": "applying",
        "started_at": time.time(), "original_revision": revision,
        "original_sha256": trial.canonical_sha(original), "gains_db": EDGE_GAINS}
    trial.atomic(root / "state.json", state)
    try:
        _selected, revision = trial.apply_stage(original, EDGE_GAINS, revision,
            root, "edge")
        trial.wait_healthy(EDGE_GAINS)
        time.sleep(30)
        system, _ = trial.request("/api/system/status", timeout=20)
        upstream, _ = trial.request("/api/upstream/status", timeout=20)
        before = trial.adc_snapshot(upstream)
        started = trial.toggle_recording(True)
        time.sleep(10)
        stopped = trial.toggle_recording(False)
        system2, _ = trial.request("/api/system/status", timeout=20)
        upstream2, _ = trial.request("/api/upstream/status", timeout=20)
        after = trial.adc_snapshot(upstream2)
        state.update({"status": "complete", "completed_at": time.time(),
            "revision": revision, "adc_before_recording": before,
            "adc_after_recording": after,
            "worst_rail_fraction": max(c["railFraction"] for c in after),
            "worst_near_rail_fraction": max(c["nearRailFraction"] for c in after),
            "worst_p999_fraction": max(c["p999Fraction"] for c in after),
            "recording_file": started.get("recordingFile"),
            "recorded_samples": stopped.get("recordedSamples"),
            "first_frame_at": system.get("lastFrameAt"),
            "last_frame_at": system2.get("lastFrameAt")})
        trial.atomic(root / "state.json", state)
    except Exception as error:
        state.update({"status": "failed-restoring", "error": str(error)})
        trial.atomic(root / "state.json", state)
        deadline = time.monotonic() + 180
        while True:
            try:
                _current, revision = trial.request("/api/config", timeout=20)
                break
            except Exception:
                if time.monotonic() >= deadline:
                    raise
                time.sleep(2)
        restored, revision, receipt = trial.apply_config(original, revision)
        trial.atomic(root / "restore-receipt.json", receipt)
        trial.wait_reconfigurable()
        state.update({"status": "failed-restored", "restored_revision": revision,
            "restored_sha256": trial.canonical_sha(restored)})
        trial.atomic(root / "state.json", state)
        raise


if __name__ == "__main__":
    main()
