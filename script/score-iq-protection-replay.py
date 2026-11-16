#!/usr/bin/env python3
"""Score paired replay arms against admission truth without feeding truth to DSP."""

import argparse
import glob
import hashlib
import json
import math
from pathlib import Path
import statistics


def load(path):
    return json.loads(Path(path).read_text())


SPEED_OF_LIGHT_M_S = 299792458.0


def admitted_truth(admission):
    points = {}
    for capture in admission.get("captures", {}).values():
        if not capture.get("valid"):
            continue
        admitted = {row.get("timestamp_ms") for row in capture.get("cpis", [])}
        if None in admitted:
            raise ValueError("admitted CPI has no timestamp")
        for row in capture.get("truth_samples", []):
            delay, doppler = row.get("delay_km"), row.get("doppler_hz")
            if all(isinstance(value, (int, float)) and math.isfinite(value)
                   for value in (delay, doppler)):
                timestamp = row.get("timestamp_ms")
                if not isinstance(timestamp, int):
                    raise ValueError("admitted truth sample has no integer timestamp")
                if timestamp not in admitted:
                    continue
                value = (float(delay), float(doppler))
                if timestamp in points and points[timestamp] != value:
                    raise ValueError("conflicting admitted truth at one CPI endpoint")
                points[timestamp] = value
        missing = admitted - points.keys()
        if missing:
            raise ValueError("admitted CPI has no exact evaluation truth sample")
    if not points:
        raise ValueError("admission contains no valid truth samples")
    return points


def file_sha256(path):
    digest = hashlib.sha256()
    with Path(path).open("rb") as source:
        for block in iter(lambda: source.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def comparable_config(config):
    value = json.loads(json.dumps(config))
    value.pop("network", None)
    reference = value.get("process", {}).get("reference_synthesis", {})
    reference.pop("target_protection_beams", None)
    reference.pop("target_protection_beam_index", None)
    return value


def validate_provenance(replays, admission_path, timeline_path):
    expected = replays[0]
    identity = ("recording_sha256", "timeline_sha256", "metadata_sha256",
                "profile_sha256", "admission_sha256", "binary_sha256",
                "runner_sha256")
    for replay in replays:
        for key in identity:
            if replay.get(key) != expected.get(key) or not replay.get(key):
                raise ValueError(f"replay provenance differs or is absent: {key}")
        if comparable_config(replay.get("resolved_config", {})) != comparable_config(
                expected.get("resolved_config", {})):
            raise ValueError("resolved replay configs differ outside protection and ports")
    actual = {
        "recording_sha256": file_sha256(expected["recording"]),
        "timeline_sha256": file_sha256(timeline_path),
        "metadata_sha256": file_sha256(expected["metadata"]),
        "profile_sha256": file_sha256(expected["profile"]),
        "admission_sha256": file_sha256(admission_path),
        "binary_sha256": file_sha256(expected["binary"]),
        "runner_sha256": file_sha256(expected["runner"]),
    }
    for key, digest in actual.items():
        if expected.get(key) != digest:
            raise ValueError(f"replay provenance hash is stale or forged: {key}")


def near_truth(delay, doppler, truth, delay_gate, doppler_gate):
    expected_delay, expected_doppler = truth
    return (abs(delay - expected_delay) <= delay_gate and
            abs(doppler - expected_doppler) <= doppler_gate)


def summarize(path, truth, endpoints, sample_rate_hz, delay_gate, doppler_gate):
    replay = load(path)
    detection_frames = replay["frames"]["detection"]
    track_frames = replay["frames"]["track"]
    matched_snr = []
    matched_detection_frames = 0
    total_detections = 0
    if len(detection_frames) != len(endpoints) or len(track_frames) != len(endpoints):
        raise ValueError("replay stream/timeline endpoint count differs")
    matched_endpoint_snrs = {}
    scorable_frames = 0
    for index, frame in enumerate(detection_frames):
        timestamp = endpoints[index].get("timestamp_ms")
        if timestamp not in truth:
            continue
        scorable_frames += 1
        found = False
        delays = frame.get("delay", [])
        dopplers = frame.get("doppler", [])
        snrs = frame.get("snr", [])
        total_detections += len(delays)
        for delay, doppler, snr in zip(delays, dopplers, snrs):
            if near_truth(delay, doppler, truth[timestamp], delay_gate, doppler_gate):
                found = True
                matched_snr.append(float(snr))
                matched_endpoint_snrs.setdefault(str(timestamp), []).append(float(snr))
        matched_detection_frames += int(found)
    matched_track_frames = 0
    matched_active_frames = 0
    total_track_entities = 0
    delay_bin_km = SPEED_OF_LIGHT_M_S / sample_rate_hz / 1000.0
    for index, frame in enumerate(track_frames):
        timestamp = endpoints[index].get("timestamp_ms")
        if timestamp not in truth:
            continue
        matched = []
        for track in frame.get("data", []):
            total_track_entities += 1
            delay_km = float(track.get("delay", math.inf)) * delay_bin_km
            if near_truth(delay_km, float(track.get("doppler", math.inf)),
                          truth[timestamp],
                          delay_gate, doppler_gate):
                matched.append(track)
        matched_track_frames += int(bool(matched))
        matched_active_frames += int(any(track.get("state") == "ACTIVE"
                                         for track in matched))
    arm = replay["arm"]
    return {
        "path": str(path),
        "arm": arm,
        "detection_frames": len(detection_frames),
        "scorable_frames": scorable_frames,
        "truth_detection_frames": matched_detection_frames,
        "truth_detection_samples": len(matched_snr),
        "truth_snr_mean_db": statistics.fmean(matched_snr) if matched_snr else None,
        "truth_snr_median_db": statistics.median(matched_snr) if matched_snr else None,
        "truth_snr_max_db": max(matched_snr) if matched_snr else None,
        "total_detections": total_detections,
        "track_frames": len(track_frames),
        "truth_track_frames": matched_track_frames,
        "truth_measured_track_frames": None,
        "truth_measured_track_frames_reason": "track JSON has no validated current-measurement field",
        "truth_active_track_frames": matched_active_frames,
        "total_track_entities": total_track_entities,
        "matched_endpoint_snrs_db": matched_endpoint_snrs,
        "track_delay_bin_km": delay_bin_km,
    }


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--admission", type=Path, required=True)
    parser.add_argument("--timeline", type=Path, required=True)
    parser.add_argument("--baseline", type=Path, required=True)
    parser.add_argument("--arms", required=True)
    parser.add_argument("--delay-gate-km", type=float, default=.25)
    parser.add_argument("--doppler-gate-hz", type=float, default=15.0)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    admission = load(args.admission)
    timeline = load(args.timeline)
    truth = admitted_truth(admission)
    endpoints = timeline.get("frames", [])
    sample_rate_hz = float(timeline["sample_rate_hz"])
    paths = [args.baseline] + [Path(path) for path in sorted(glob.glob(args.arms))]
    replays = [load(path) for path in paths]
    validate_provenance(replays, args.admission, args.timeline)
    rows = [summarize(path, truth, endpoints, sample_rate_hz,
                      args.delay_gate_km, args.doppler_gate_hz)
            for path in paths]
    baseline = rows[0]
    for row in rows:
        for name in ("truth_detection_frames", "truth_detection_samples",
                     "total_detections", "truth_track_frames",
                     "truth_active_track_frames",
                     "total_track_entities"):
            row[f"delta_{name}"] = row[name] - baseline[name]
        row["delta_truth_snr_mean_db"] = (
            row["truth_snr_mean_db"] - baseline["truth_snr_mean_db"]
            if row["truth_snr_mean_db"] is not None and
            baseline["truth_snr_mean_db"] is not None else None)
        common = sorted(set(baseline["matched_endpoint_snrs_db"]) &
                        set(row["matched_endpoint_snrs_db"]))
        paired = [statistics.fmean(row["matched_endpoint_snrs_db"][key]) -
                  statistics.fmean(baseline["matched_endpoint_snrs_db"][key])
                  for key in common]
        row["paired_common_detection_endpoints"] = len(common)
        row["paired_common_endpoint_snr_delta_mean_db"] = (
            statistics.fmean(paired) if paired else None)
    result = {
        "schema": "vectorwarp-iq-protection-score-v1",
        "admission": str(args.admission),
        "truth_points": len(truth),
        "timeline": str(args.timeline),
        "gate": {"delay_km": args.delay_gate_km,
                 "doppler_hz": args.doppler_gate_hz},
        "qualification": "development diagnostic; same-IQ CPU coherent beam bank; resident noise whitening/TBD disabled identically in every arm",
        "scorer_sha256": file_sha256(Path(__file__)),
        "rows": rows,
    }
    args.output.write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result))


if __name__ == "__main__":
    main()
