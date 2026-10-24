#!/usr/bin/env python3
"""Build a strict fresh-only IQ cohort from evidence-statistics output.

This intentionally refuses endpoint holds and event-prediction fallbacks.  A
truth point for every CPI must be bracketed by two event-time projected ADS-B
samples that independently satisfy the configured provenance limits.
"""

import argparse
import json
from pathlib import Path


KINEMATIC_FIELDS = (
    "latitude", "longitude", "altitude_geom_ft", "ground_speed_kt",
    "track_deg_true", "bearing_deg_true", "elevation_deg", "ground_range_km",
    "slant_range_km", "transmitter_range_km", "delay_km", "doppler_hz",
    "delay_rate_km_s", "doppler_rate_hz_s",
)


def provenance_error(sample, policy):
    provenance = sample.get("provenance") or {}
    offset = provenance.get("observation_time_offset_s")
    if not isinstance(offset, (int, float)):
        return "missing raw observation offset"
    if abs(offset) > policy["maximum_absolute_raw_observation_offset_s"]:
        return "raw observation offset exceeds policy"
    if provenance.get("projection_model") != "constant_velocity_ground_track":
        return "non-event-time projection model"
    if provenance.get("observation_position_timestamp_s") is None:
        return "missing observation event time"
    observations = [item for item in provenance.get("source_observations", [])
                    if item.get("accepted") is True]
    if not observations or any(item.get("event_time_s") is None for item in observations):
        return "missing accepted source event time"
    if len(observations) > 1:
        disagreement = provenance.get("maximum_pairwise_projected_disagreement_km")
        if not isinstance(disagreement, (int, float)):
            return "missing dual-source disagreement"
        if disagreement > policy["maximum_dual_source_horizontal_disagreement_km"]:
            return "dual-source disagreement exceeds policy"
    return None


def interpolate(left, right, timestamp):
    a, b = left["timestamp_ms"], right["timestamp_ms"]
    if not a < timestamp < b:
        raise ValueError("CPI midpoint is not strictly bracketed")
    fraction = (timestamp - a) / (b - a)
    result = {}
    for field in KINEMATIC_FIELDS:
        av, bv = left.get(field), right.get(field)
        result[field] = (av + (bv - av) * fraction
                         if isinstance(av, (int, float)) and isinstance(bv, (int, float))
                         else None)
    result.update({
        "timestamp_ms": timestamp,
        "truth_mode": "bracketed_event_time_interpolation",
        "bracket_timestamp_ms": [a, b],
        "source_ids": sorted(set((left["provenance"].get("agreeing_sources") or []) +
                                 (right["provenance"].get("agreeing_sources") or []) +
                                 [left["provenance"]["source_id"],
                                  right["provenance"]["source_id"]])),
        "raw_observation_offsets_s": [
            left["provenance"]["observation_time_offset_s"],
            right["provenance"]["observation_time_offset_s"],
        ],
        "maximum_pairwise_projected_disagreement_km": max(
            left["provenance"].get("maximum_pairwise_projected_disagreement_km") or 0,
            right["provenance"].get("maximum_pairwise_projected_disagreement_km") or 0),
    })
    speed = result.get("ground_speed_kt")
    offsets = result["raw_observation_offsets_s"]
    result["maximum_constant_velocity_projection_distance_km"] = (
        speed * 0.000514444 * max(abs(value) for value in offsets)
        if isinstance(speed, (int, float)) else None)
    return result


def trajectory(event, window, cpi_ms, policy):
    samples = sorted(event.get("truth_samples", []), key=lambda item: item.get("timestamp_ms", 0))
    errors = sorted(set(error for sample in samples
                        if (error := provenance_error(sample, policy))))
    if errors:
        return None, errors
    output = []
    frames = (window["window_end"] - window["window_start"]) // cpi_ms
    for frame in range(frames):
        timestamp = window["window_start"] + frame * cpi_ms + cpi_ms / 2
        left = next((sample for sample in reversed(samples)
                     if sample["timestamp_ms"] < timestamp), None)
        right = next((sample for sample in samples
                      if sample["timestamp_ms"] > timestamp), None)
        if left is None or right is None:
            return None, [f"CPI {frame} lacks bracketing event-time truth"]
        maximum_span_s = policy.get("maximum_truth_bracket_span_s")
        span_s = (right["timestamp_ms"] - left["timestamp_ms"]) / 1000
        if isinstance(maximum_span_s, (int, float)) and span_s > maximum_span_s:
            return None, [
                f"CPI {frame} truth bracket spans {span_s:.3f}s, exceeding "
                f"{maximum_span_s:.3f}s policy"
            ]
        point = interpolate(left, right, timestamp)
        point["frame"] = frame
        output.append(point)
    return output, []


def build(statistics, policy_document, cutoff_ms, revisions, rf):
    truth_policy = policy_document["required_truth"]
    cpi_ms = truth_policy["association_lag_ms"] // truth_policy["association_lag_cpis"]
    admitted, rejected = [], []
    selected = set(revisions)
    for event in statistics.get("eligibility_events", []):
        for window in event.get("capture_windows", []):
            if selected and window.get("revision") not in selected:
                continue
            reasons = []
            minimum_capture_ms = max(
                cutoff_ms, policy_document["minimum_capture_timestamp_ms"])
            if window.get("window_start", 0) < minimum_capture_ms:
                reasons.append("capture predates minimum capture timestamp")
            if event.get("state") != "finalized": reasons.append("event not finalized")
            if window.get("state") != "acknowledged" or not window.get("file"):
                reasons.append("capture not acknowledged")
            first_track = window.get("first_track_at")
            first_detection = window.get("first_detection_at")
            overlap_track = (window.get("tracked_frames", 0) > 0 and
                             isinstance(first_track, (int, float)) and
                             window["window_start"] <= first_track <= window["window_end"])
            overlap_detection = (window.get("detected_frames", 0) > 0 and
                                 isinstance(first_detection, (int, float)) and
                                 window["window_start"] <= first_detection <= window["window_end"])
            role = None
            # A target-specific detector association is a radar catch even if
            # the tracker did not acquire enough consecutive cells inside this
            # particular recording window.  Requiring overlap_track alone
            # silently discarded valid detector-positive controls.
            if event.get("outcome") == "tracked" and (overlap_track or overlap_detection):
                role = "positive"
            elif (event.get("outcome") == "no_radar_return" and
                  not overlap_track and not overlap_detection):
                role = "geometry_eligible_miss"
            else:
                reasons.append("event outcome and in-window radar evidence do not define a cohort role")
            points, truth_errors = trajectory(event, window, cpi_ms, truth_policy)
            reasons.extend(truth_errors)
            item = {
                "event_id": event.get("id"), "pass_id": event.get("pass_id"),
                "hex": event.get("hex"), "flight": event.get("flight"),
                "outcome": event.get("outcome"), "role": role,
                "config_epoch": event.get("config_epoch"),
                "receiver_epoch": event.get("receiver_epoch"),
                "capture": window, "radar_window_overlap": {
                    "track": overlap_track, "detection": overlap_detection,
                },
            }
            if points:
                item["trajectory"] = points
                item["provenance_summary"] = {
                    "truth_mode": "bracketed_event_time_interpolation",
                    "nearest_sample_hold": False, "fallback": False,
                    "maximum_absolute_raw_observation_offset_s": max(
                        abs(value) for point in points
                        for value in point["raw_observation_offsets_s"]),
                    "maximum_dual_source_horizontal_disagreement_km": max(
                        point["maximum_pairwise_projected_disagreement_km"] for point in points),
                    "maximum_constant_velocity_projection_distance_km": max(
                        point["maximum_constant_velocity_projection_distance_km"] or 0
                        for point in points),
                }
            if reasons:
                item["reasons"] = reasons
                rejected.append(item)
            else:
                admitted.append(item)
    return {
        "schema": "vectorwarp-postfix-fresh-cohort-v1",
        "cutoff_ms": cutoff_ms, "policy": policy_document,
        "association_lag_cpis": truth_policy["association_lag_cpis"],
        "cpi_ms": cpi_ms, "rf": rf,
        "admitted": admitted, "rejected": rejected,
        "summary": {
            "positives": sum(item["role"] == "positive" for item in admitted),
            "geometry_eligible_misses": sum(item["role"] == "geometry_eligible_miss"
                                             for item in admitted),
            "rejected": len(rejected),
        },
    }


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--statistics", required=True, type=Path)
    parser.add_argument("--policy", required=True, type=Path)
    parser.add_argument("--cutoff-ms", required=True, type=int)
    parser.add_argument("--include-revision", action="append", type=int, default=[])
    parser.add_argument("--rf-metadata", type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    result = build(json.loads(args.statistics.read_text()),
                   json.loads(args.policy.read_text()), args.cutoff_ms,
                   args.include_revision,
                   json.loads(args.rf_metadata.read_text()) if args.rf_metadata else None)
    args.output.write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result["summary"], sort_keys=True))


if __name__ == "__main__":
    main()
