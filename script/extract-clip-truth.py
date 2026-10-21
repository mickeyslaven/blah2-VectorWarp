#!/usr/bin/env python3
"""Extract signal-time truth trajectories for recorded IQ clip windows."""

import argparse
import json
from pathlib import Path


FIELDS = ("delay_km", "doppler_hz", "bearing_deg_true", "elevation_deg",
          "latitude", "longitude", "altitude_geom_ft")


def event_index(statistics):
    return {item["id"]: item for item in statistics.get("eligibilityEvents", [])}


def unique_positions(samples):
    output = {}
    for sample in samples:
        timestamp = sample.get("position_timestamp_s")
        if timestamp is not None:
            output[float(timestamp) * 1000] = sample
    return sorted(output.items())


def interpolate(samples, timestamp):
    if not samples:
        raise ValueError("eligibility event has no positioned truth samples")
    if timestamp <= samples[0][0]:
        return {key: samples[0][1].get(key) for key in FIELDS}
    if timestamp >= samples[-1][0]:
        return {key: samples[-1][1].get(key) for key in FIELDS}
    for index in range(1, len(samples)):
        right_time, right = samples[index]
        if right_time < timestamp:
            continue
        left_time, left = samples[index - 1]
        fraction = (timestamp - left_time) / (right_time - left_time)
        result = {}
        for key in FIELDS:
            a, b = left.get(key), right.get(key)
            result[key] = (a + (b - a) * fraction
                           if isinstance(a, (int, float)) and isinstance(b, (int, float))
                           else a if fraction < .5 else b)
        return result
    raise AssertionError("truth interpolation failed")


def extract(statistics, sidecar, cpi):
    events = event_index(statistics)
    identifiers = sidecar.get("eventIds", [])
    candidates = [events[item] for item in identifiers if item in events]
    if len(candidates) != 1:
        raise ValueError("clip must resolve to exactly one retained eligibility event")
    event = candidates[0]
    positions = unique_positions(event.get("truth_samples", []))
    midpoint = (sidecar["windowStart"] + sidecar["windowEnd"]) / 2
    if (not positions or min(abs(timestamp - midpoint) for timestamp, _ in positions) > 10000):
        predicted = event.get("predicted", {})
        positions = [(midpoint, predicted)]
        truth_source = "event_prediction_fallback"
    else:
        truth_source = "interpolated_position_samples"
    frames = round((sidecar["windowEnd"] - sidecar["windowStart"]) / (cpi * 1000))
    trajectory = []
    for frame in range(frames):
        timestamp = sidecar["windowStart"] + (frame + .5) * cpi * 1000
        trajectory.append({"frame": frame, "timestamp_ms": timestamp,
                           **interpolate(positions, timestamp)})
    return {
        "schema": "vectorwarp-clip-truth-trajectory-v1",
        "clip_revision": sidecar["clipRevision"],
        "clip": sidecar.get("file"),
        "window_start_ms": sidecar["windowStart"],
        "window_end_ms": sidecar["windowEnd"],
        "cpi_s": cpi,
        "event_id": event["id"],
        "hex": event["hex"],
        "flight": event.get("flight", ""),
        "outcome": event.get("outcome"),
        "truth_source": truth_source,
        "trajectory": trajectory,
    }


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--statistics", required=True, type=Path)
    parser.add_argument("--sidecar", required=True, type=Path)
    parser.add_argument("--cpi", type=float, default=.5)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    result = extract(json.loads(args.statistics.read_text()),
                     json.loads(args.sidecar.read_text()), args.cpi)
    args.output.write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps({"hex": result["hex"], "frames": len(result["trajectory"]),
                      "first": result["trajectory"][0],
                      "last": result["trajectory"][-1]}, indent=2))


if __name__ == "__main__":
    main()
