#!/usr/bin/env python3
"""Prepare exact signal-time replay jobs for one retained configuration epoch."""

import argparse
import hashlib
import importlib.util
import json
import math
from pathlib import Path


SPEC = importlib.util.spec_from_file_location(
    "truth", Path(__file__).with_name("extract-clip-truth.py"))
TRUTH = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(TRUTH)
# Seven single/stack hypotheses under each of two gain treatments, plus the
# separately gated signed inversion hypothesis.
TRIAL_VARIANTS = 15


def trajectory(event, window, cpi=.5):
    positions = TRUTH.unique_positions(event.get("truth_samples", []))
    midpoint = (window["window_start"] + window["window_end"]) / 2
    if not positions or min(abs(timestamp - midpoint) for timestamp, _ in positions) > 10000:
        positions = [(midpoint, event["predicted"])]
        source = "event_prediction_fallback"
    else:
        source = "interpolated_position_samples"
    count = round((window["window_end"] - window["window_start"]) / (cpi * 1000))
    values = []
    for frame in range(count):
        timestamp = window["window_start"] + (frame + .5) * cpi * 1000
        values.append({"frame": frame, "timestamp_ms": timestamp,
                       **TRUTH.interpolate(positions, timestamp)})
    return source, values


def prepare(document, epoch):
    jobs = []
    for event in document["eligibility_events"]:
        if (event.get("config_epoch") != epoch or event.get("state") != "finalized" or
                event.get("outcome") not in ("no_radar_return", "tracked")):
            continue
        windows = [item for item in event.get("capture_windows", [])
                   if item.get("state") == "acknowledged" and item.get("file")]
        family_trials = TRIAL_VARIANTS * len(windows)
        for window in windows:
            source, values = trajectory(event, window)
            delays = [item["delay_km"] for item in values]
            dopplers = [item["doppler_hz"] for item in values]
            delay_center = (min(delays) + max(delays)) / 2
            doppler_center = (min(dopplers) + max(dopplers)) / 2
            jobs.append({
                "job_id": f'{event["hex"]}-r{window["revision"]}',
                "event_id": event["id"], "hex": event["hex"],
                "flight": event.get("flight", ""), "outcome": event["outcome"],
                "role": "miss" if event["outcome"] == "no_radar_return" else "positive",
                "revision": window["revision"], "kind": window["kind"],
                "recording": window["file"], "truth_source": source,
                "trajectory": values, "family_trials": family_trials,
                "control_quantile": 1 - .01 / family_trials,
                "cutout": {"delay_km": delay_center, "doppler_hz": doppler_center,
                           "delay_radius_bins": math.ceil((max(delays) - min(delays)) /
                                                          (2 * .12491352416666666)) + 10,
                           "doppler_radius_bins": math.ceil((max(dopplers) - min(dopplers)) /
                                                            4) + 10,
                           "max_frames": len(values) + 4},
            })
    return jobs


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--statistics", required=True, type=Path)
    parser.add_argument("--config-epoch", required=True)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    raw = args.statistics.read_bytes()
    document = json.loads(raw)
    jobs = prepare(document, args.config_epoch)
    output = {"schema": "vectorwarp-current-epoch-spatial-replay-manifest-v1",
              "config_epoch": args.config_epoch,
              "statistics_sha256": hashlib.sha256(raw).hexdigest(),
              "jobs": jobs}
    args.output.write_text(json.dumps(output, separators=(",", ":")) + "\n")
    print(json.dumps({"jobs": len(jobs), "unique_recordings":
                      len({item["recording"] for item in jobs}),
                      "miss_jobs": sum(item["role"] == "miss" for item in jobs),
                      "positive_jobs": sum(item["role"] == "positive" for item in jobs),
                      "max_delay_radius": max(item["cutout"]["delay_radius_bins"] for item in jobs),
                      "max_doppler_radius": max(item["cutout"]["doppler_radius_bins"] for item in jobs)}, indent=2))


if __name__ == "__main__":
    main()
