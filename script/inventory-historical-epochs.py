#!/usr/bin/env python3
"""Join finalized historical events to retained MCHQ sidecars by event id.

Frequency is deliberately taken from the recording sidecar, not guessed from a
current receiver configuration.  Each output trajectory is interpolated at the
midpoint of every complete 0.5-s capture CPI.
"""
import argparse
import importlib.util
import json
from collections import Counter, defaultdict
from pathlib import Path


SPEC = importlib.util.spec_from_file_location(
    "truth", Path(__file__).with_name("extract-clip-truth.py"))
TRUTH = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(TRUTH)


def clip_index(sidecars):
    output = {}
    for item in sidecars:
        for event_id in item.get("eventIds", []):
            output.setdefault(event_id, []).append(item)
    return output


def trajectory(event, window, cpi_ms=500):
    positions = TRUTH.unique_positions(event.get("truth_samples", []))
    midpoint = (window["window_start"] + window["window_end"]) / 2
    if not positions or min(abs(timestamp - midpoint) for timestamp, _ in positions) > 10000:
        positions, source = [(midpoint, event.get("predicted", {}))], "event_prediction_fallback"
    else:
        source = "interpolated_position_samples"
    values = []
    for frame, timestamp in enumerate(range(window["window_start"] + cpi_ms // 2,
                                            window["window_end"], cpi_ms)):
        values.append({"frame": frame, "timestamp_ms": timestamp,
                       **TRUTH.interpolate(positions, timestamp)})
    return source, values


def inventory(statistics, sidecars):
    indexed = clip_index(sidecars)
    rows, missing = [], []
    for event in statistics.get("eligibility_events", []):
        if event.get("state") != "finalized" or event.get("outcome") not in (
                "no_radar_return", "tracked"):
            continue
        for window in event.get("capture_windows", []):
            if window.get("state") != "acknowledged" or not window.get("file"):
                continue
            clips = [item for item in indexed.get(event["id"], [])
                     if item.get("path") == window["file"]]
            if not clips:
                missing.append({"event_id": event["id"], "config_epoch": event["config_epoch"],
                                "file": window["file"], "reason": "missing_sidecar_or_raw"})
                continue
            source, values = trajectory(event, window)
            for clip in clips:
                rows.append({"event_id": event["id"], "hex": event.get("hex"),
                             "flight": event.get("flight", ""), "outcome": event["outcome"],
                             "role": "positive" if event["outcome"] == "tracked" else "miss",
                             "config_epoch": event["config_epoch"],
                             "receiver_epoch": event.get("receiver_epoch"),
                             "frequency_hz": clip.get("frequency"),
                             "sample_rate_hz": clip.get("sampleRate"),
                             "channel_gains_db": clip.get("channelGains"),
                             "availability": "available" if clip.get("raw_exists") else "sidecar_only",
                             "truth_source": source, "replay_trajectory": values,
                             "capture": {"capture_id": window.get("capture_id"),
                               "kind": window.get("kind"), "revision": window.get("revision"),
                               "window_start": window["window_start"], "window_end": window["window_end"],
                               "file": window["file"], "samples": window.get("samples"),
                               "sha256": clip.get("dataSha256"), "bytes": clip.get("dataBytes")}})
    summaries = defaultdict(Counter)
    for row in rows:
        bucket = summaries[(row["config_epoch"], row["frequency_hz"])]
        bucket["mappings"] += 1; bucket[row["role"]] += 1; bucket[row["availability"]] += 1
    return {"schema": "vectorwarp-historical-epoch-manifest-v1", "rows": rows,
            "availability_gaps": missing,
            "epochs": [{"config_epoch": epoch, "frequency_hz": frequency, **counts}
                       for (epoch, frequency), counts in sorted(summaries.items())]}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--statistics", required=True, type=Path)
    parser.add_argument("--sidecars", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    output = inventory(json.loads(args.statistics.read_text()),
                       json.loads(args.sidecars.read_text()))
    args.output.write_text(json.dumps(output, separators=(",", ":")) + "\n")


if __name__ == "__main__":
    main()
