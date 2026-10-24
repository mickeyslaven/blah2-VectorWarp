#!/usr/bin/env python3
"""Make a compact, exact current-config WWAT-CD replay manifest."""

import argparse
import json
from pathlib import Path


def walk(value):
    if isinstance(value, dict):
        yield value
        for item in value.values():
            yield from walk(item)
    elif isinstance(value, list):
        for item in value:
            yield from walk(item)


def inventory(document, epoch, outcomes):
    events_by_id = {item["id"]: item for item in walk(document)
                    if item.get("config_epoch") == epoch and item.get("state") == "finalized"
                    and item.get("outcome") in outcomes and item.get("id")}
    events = list(events_by_id.values())
    rows = []
    for event in events:
        predicted = event.get("predicted", {})
        for window in event.get("capture_windows", []):
            if window.get("state") != "acknowledged" or not window.get("file"):
                continue
            trajectory = [{key: sample.get(key) for key in
                           ("timestamp_ms", "delay_km", "doppler_hz", "bearing_deg_true")}
                          for sample in event.get("truth_samples", [])
                          if window["window_start"] <= sample.get("timestamp_ms", -1) <= window["window_end"]]
            # One target state per complete 0.5 s replay CPI, nearest in time
            # to its midpoint.  Preserve the raw event samples above too.
            cpis = []
            for frame, midpoint in enumerate(range(window["window_start"] + 250,
                                                   window["window_end"], 500)):
                if trajectory:
                    state = min(trajectory, key=lambda item: abs(item["timestamp_ms"] - midpoint))
                    cpis.append({"frame": frame, "timestamp_ms": midpoint,
                                 "delay_km": state["delay_km"],
                                 "doppler_hz": state["doppler_hz"],
                                 "bearing_deg_true": state["bearing_deg_true"]})
            rows.append({"event_id": event["id"], "hex": event.get("hex"),
                         "flight": event.get("flight"), "observed_at": event.get("observed_at"),
                         "outcome": event.get("outcome"),
                         "role": "miss" if event.get("outcome") == "no_radar_return" else "positive",
                         "config_epoch": epoch,
                         "predicted": {key: predicted.get(key) for key in
                                       ("delay_km", "doppler_hz", "bearing_deg_true")},
                         "truth_samples_count": len(trajectory),
                         "replay_trajectory": cpis,
                         "capture": {key: window.get(key) for key in
                                     ("capture_id", "kind", "revision", "window_start", "window_end",
                                      "file", "samples")}})
    unique = sorted({row["capture"]["file"] for row in rows})
    return {"schema": "vectorwarp-wwat563-miss-manifest-v1", "config_epoch": epoch,
            "finalized_events": len(events), "parent_capture_mappings": len(rows),
            "unique_mchq_clips": len(unique), "rows": rows}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--statistics", required=True, type=Path)
    parser.add_argument("--epoch", required=True)
    parser.add_argument("--outcomes", default="no_radar_return")
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    result = inventory(json.loads(args.statistics.read_text()), args.epoch,
                       set(args.outcomes.split(",")))
    args.output.write_text(json.dumps(result, separators=(",", ":")) + "\n")


if __name__ == "__main__":
    main()
