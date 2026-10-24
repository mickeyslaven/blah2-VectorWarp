#!/usr/bin/env python3
"""Score replayed fusion arms against exact-provenance positive trajectories."""

import argparse
import json
import math
from pathlib import Path
import statistics


C_KM_S = 299792.458


def inside(delay, doppler, truth, delay_gate, doppler_gate):
    return ((delay - truth["delay_km"]) / delay_gate) ** 2 + (
        (doppler - truth["doppler_hz"]) / doppler_gate) ** 2 <= 1


def gates(truth, sample_rate, cpi, track=False):
    km_per_bin = C_KM_S / sample_rate
    horizon = cpi * (3 if track else 1)
    delay = max(km_per_bin * (3 if track else 2),
                .25 + abs(truth.get("delay_rate_km_s", 0)) * horizon)
    doppler = max((1 / cpi) * (4 if track else 3),
                  5 + abs(truth.get("doppler_rate_hz_s", 0)) * horizon)
    return delay, doppler


def score(path, positive, arm_name, arm, sample_rate, cpi):
    trajectory = positive["trajectory"]
    target_detection_frames = []
    target_snrs = []
    for frame_index, frame in enumerate(arm["detections"]):
        # The first published replay frame is pipeline warm-up. Frame 1 aligns
        # with the first CPI in the admitted truth trajectory.
        truth = trajectory[max(0, min(len(trajectory) - 1, frame_index - 1))]
        delay_gate, doppler_gate = gates(truth, sample_rate, cpi)
        matches = [(snr, delay, doppler) for delay, doppler, snr in zip(
            frame["delay"], frame["doppler"], frame["snr"])
            if inside(delay, doppler, truth, delay_gate, doppler_gate)]
        if matches:
            target_detection_frames.append(frame_index)
            target_snrs.append(max(matches)[0])

    km_per_bin = C_KM_S / sample_rate
    target_track_frames = []
    target_track_ids = set()
    all_track_ids = set()
    for frame_index, frame in enumerate(arm["tracks"]):
        truth = trajectory[max(0, min(len(trajectory) - 1, frame_index - 1))]
        delay_gate, doppler_gate = gates(truth, sample_rate, cpi, track=True)
        for track in frame["data"]:
            all_track_ids.add(track["id"])
            if inside(track["delay"] * km_per_bin, track["doppler"], truth,
                      delay_gate, doppler_gate):
                target_track_frames.append(frame_index)
                target_track_ids.add(track["id"])
                break

    return {
        "file": path.name,
        "arm": arm_name,
        "job_id": positive["job_id"],
        "pass_id": positive["pass_id"],
        "flight": positive["flight"],
        "hex": positive["hex"],
        "target_detection_frame_count": len(target_detection_frames),
        "target_detection_frames": target_detection_frames,
        "target_snr_db_median": statistics.median(target_snrs) if target_snrs else None,
        "target_snr_db_peak": max(target_snrs) if target_snrs else None,
        "target_track_frame_count": len(target_track_frames),
        "target_track_ids": sorted(target_track_ids),
        "all_detection_cell_count": sum(len(frame["delay"]) for frame in arm["detections"]),
        "all_track_id_count": len(all_track_ids),
    }


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--cohort", required=True, type=Path)
    parser.add_argument("--input", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--sample-rate", type=float, default=2_400_000)
    parser.add_argument("--cpi", type=float, default=.5)
    args = parser.parse_args()
    cohort = json.loads(args.cohort.read_text())
    positives = {Path(row["capture"]["file"]).stem: row for row in cohort["positives"]}
    rows = []
    for path in sorted(args.input.glob("*.json")):
        match = next((row for stem, row in positives.items() if stem in path.name), None)
        if match is not None:
            document = json.loads(path.read_text())
            for arm_name, arm in document.get("arms", {}).items():
                rows.append(score(path, match, arm_name, arm, args.sample_rate, args.cpi))
    aggregates = {}
    for arm in sorted({row["arm"] for row in rows}):
        selected = [row for row in rows if row["arm"] == arm]
        aggregates[arm] = {
            "clips": len(selected),
            "independent_passes": len({row["pass_id"] for row in selected}),
            "clips_with_target_detection": sum(row["target_detection_frame_count"] > 0
                                                for row in selected),
            "clips_with_target_track": sum(row["target_track_frame_count"] > 0
                                            for row in selected),
            "target_detection_frames": sum(row["target_detection_frame_count"]
                                           for row in selected),
            "all_detection_cells": sum(row["all_detection_cell_count"] for row in selected),
            "all_track_ids": sum(row["all_track_id_count"] for row in selected),
        }
    result = {
        "schema": "vectorwarp-positive-fusion-ab-analysis-v1",
        "limitations": [
            "Three clips represent only two independent aircraft passes.",
            "Replay frame 1 is aligned to admitted trajectory frame 0 after one warm-up frame.",
            "Target gates are causal resolution-and-motion gates; ADS-B is used only for scoring.",
        ],
        "rows": rows,
        "aggregates": aggregates,
    }
    args.output.write_text(json.dumps(result, indent=2) + "\n")


if __name__ == "__main__":
    main()
