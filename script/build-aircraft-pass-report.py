#!/usr/bin/env python3
"""Build the durable aircraft/pass report for frozen and rolling fresh cohorts.

The immutable matrix cohort and post-freeze rolling admissions are emitted as
separate scopes.  Offline results may enrich a pass but cannot alter its live
outcome or admit a row that fails the original fresh-data gates.
"""

import argparse
import hashlib
import json
import math
import statistics
from collections import Counter, defaultdict
from pathlib import Path


CLASSIFICATIONS = (
    "tracked", "detected_not_tracked", "miss_unrecoverable",
    "offline_candidate", "validated_offline_recovery", "excluded_with_reason",
)
GEOMETRY_BINS = {
    "range_km": [0, 5, 10, 20, 40, math.inf],
    "elevation_deg": [0, 2, 5, 10, 20, 45, 90],
    "altitude_ft": [0, 3000, 10000, 18000, 30000, 50000, math.inf],
    "azimuth_deg": [0, 45, 90, 135, 180, 225, 270, 315, 360],
    "delay_km": [0, 1, 2, 5, 10, 20, 40, math.inf],
    "doppler_hz": [-math.inf, -500, -250, -100, 0, 100, 250, 500, math.inf],
}


def sha256(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def candidate_key(row):
    return f'{row["event_id"]}|r{row["capture"]["revision"]}'


def median(points, field):
    values = [point.get(field) for point in points if isinstance(point.get(field), (int, float))]
    return statistics.median(values) if values else None


def bin_label(value, edges):
    if not isinstance(value, (int, float)):
        return "unknown"
    for left, right in zip(edges, edges[1:]):
        if left <= value < right:
            lo = "-inf" if left == -math.inf else f"{left:g}"
            hi = "inf" if right == math.inf else f"{right:g}"
            return f"[{lo},{hi})"
    return "out_of_range"


def strict_errors(row, cohort):
    policy = cohort["policy"]["required_truth"]
    provenance = row.get("provenance_summary") or {}
    errors = []
    if row.get("quarantined") is True or capture_quarantined(row):
        errors.append("quarantined_input")
    if row.get("config_epoch") != cohort.get("config_epoch"):
        errors.append("config_epoch_mismatch")
    if row.get("receiver_epoch") != cohort.get("receiver_epoch"):
        errors.append("receiver_epoch_mismatch")
    if provenance.get("truth_mode") != "bracketed_event_time_interpolation":
        errors.append("not_bracketed_event_time_truth")
    if provenance.get("nearest_sample_hold") is not False:
        errors.append("nearest_sample_hold_not_forbidden")
    if provenance.get("fallback") is not False:
        errors.append("fallback_not_forbidden")
    if provenance.get("maximum_absolute_raw_observation_offset_s", math.inf) > \
            policy["maximum_absolute_raw_observation_offset_s"]:
        errors.append("raw_observation_offset_exceeds_policy")
    if provenance.get("maximum_dual_source_horizontal_disagreement_km", math.inf) > \
            policy["maximum_dual_source_horizontal_disagreement_km"]:
        errors.append("source_disagreement_exceeds_policy")
    capture = row.get("capture") or {}
    if capture.get("state") != "acknowledged" or not capture.get("file"):
        errors.append("capture_not_acknowledged")
    if not row.get("trajectory"):
        errors.append("trajectory_missing")
    return errors


def capture_quarantined(row):
    capture = row.get("capture") or {}
    return capture.get("quarantined") is True or row.get("status") == "quarantined"


def load_iq(path):
    if not path:
        return {}
    document = json.loads(Path(path).read_text())
    return {item["path"]: item for item in document.get("recordings", [])}


def walk(value):
    if isinstance(value, dict):
        yield value
        for child in value.values():
            yield from walk(child)
    elif isinstance(value, list):
        for child in value:
            yield from walk(child)


def load_offline(paths, frozen_sha):
    """Index only explicitly classified, cohort-bound offline results."""
    index = defaultdict(list)
    rejected = []
    for path in paths:
        document = json.loads(Path(path).read_text())
        if document.get("quarantined") is True or document.get("status") == "quarantined":
            rejected.append({"path": str(path), "reason": "quarantined_offline_input"})
            continue
        declared = document.get("cohort_sha256") or document.get("snapshot_sha256")
        if declared != frozen_sha:
            rejected.append({"path": str(path), "reason": "missing_or_wrong_frozen_cohort_sha256"})
            continue
        for item in walk(document):
            if item.get("quarantined") is True or item.get("status") == "quarantined":
                continue
            classification = item.get("classification")
            if classification not in ("offline_candidate", "validated_offline_recovery",
                                       "miss_unrecoverable"):
                continue
            keys = []
            if item.get("candidate_key"): keys.append(item["candidate_key"])
            if item.get("event_id") and item.get("revision") is not None:
                keys.append(f'{item["event_id"]}|r{item["revision"]}')
            for key in keys:
                index[key].append({"classification": classification,
                    "method": item.get("method") or item.get("selected_method"),
                    "raw_score": item.get("raw_score"), "rank": item.get("rank"),
                    "margin_db": item.get("margin_db"),
                    "trial_corrected": item.get("trial_corrected"),
                    "validated_plus_3db": item.get("validated_plus_3db"),
                    "source": str(path)})
    return index, rejected


def association(capture):
    detection = capture.get("detection_associations")
    tracker = capture.get("tracker_associations")
    available = isinstance(detection, list) or isinstance(tracker, list)
    return {"status": "available" if available else "unavailable_historical",
            "detection": detection or [], "tracker": tracker or []}


def iq_integrity(capture, iq):
    record = iq.get(capture.get("file"))
    if not record:
        return {"status": "pending", "expected_samples_per_channel": capture.get("samples")}
    channels = record.get("channel_metrics", [])
    passed = (record.get("metadata_stable") is True and
              record.get("samples_per_channel") == capture.get("samples") and
              all(value.get("rail_fraction") == 0 and value.get("near_rail_fraction") == 0
                  for value in channels))
    return {"status": "passed" if passed else "failed", "format": record.get("format"),
            "sample_rate_hz": record.get("sample_rate_hz"),
            "center_frequency_hz": record.get("center_frequency_hz"),
            "channels": record.get("channels"), "duration_s": record.get("duration_s"),
            "samples_per_channel": record.get("samples_per_channel"),
            "metadata_stable": record.get("metadata_stable"),
            "maximum_rail_fraction": max((x.get("rail_fraction", math.inf) for x in channels), default=None),
            "maximum_near_rail_fraction": max((x.get("near_rail_fraction", math.inf) for x in channels), default=None),
            "principal_coherent_fraction": (record.get("array") or {}).get("principal_coherent_fraction")}


def live_outcome(rows):
    if any(row.get("outcome") == "tracked" and row.get("radar_window_overlap", {}).get("track")
           for row in rows):
        return "tracked"
    if any((row.get("capture", {}).get("detected_frames") or 0) > 0 for row in rows):
        return "detected_not_tracked"
    return "no_radar_return"


def offline_classification(results):
    values = {item["classification"] for item in results}
    for value in ("validated_offline_recovery", "offline_candidate", "miss_unrecoverable"):
        if value in values:
            return value
    return None


def build_pass(pass_id, rows, cohort, scope, iq, offline):
    rows = sorted(rows, key=lambda row: row["capture"]["revision"])
    errors = sorted(set(error for row in rows for error in strict_errors(row, cohort)))
    offline_rows = [item for row in rows for item in offline.get(candidate_key(row), [])]
    outcome = live_outcome(rows)
    if errors:
        classification, final = "excluded_with_reason", True
    elif outcome == "tracked":
        classification, final = "tracked", True
    elif outcome == "detected_not_tracked":
        classification, final = "detected_not_tracked", True
    else:
        classification = offline_classification(offline_rows)
        final = classification in ("validated_offline_recovery", "miss_unrecoverable")
    points = [point for row in rows for point in row.get("trajectory", [])]
    geometry = {"range_km": median(points, "ground_range_km"),
        "elevation_deg": median(points, "elevation_deg"),
        "altitude_ft": median(points, "altitude_geom_ft"),
        "azimuth_deg": median(points, "bearing_deg_true"),
        "delay_km": median(points, "delay_km"), "doppler_hz": median(points, "doppler_hz")}
    geometry["bins"] = {name: bin_label(value, GEOMETRY_BINS[name])
                        for name, value in geometry.items() if name in GEOMETRY_BINS}
    clips = []
    for row in rows:
        capture = row["capture"]
        clips.append({"candidate_key": candidate_key(row), "revision": capture["revision"],
            "file": capture["file"], "kind": capture.get("kind"),
            "window_start_ms": capture.get("window_start"),
            "window_end_ms": capture.get("window_end"), "samples": capture.get("samples"),
            "detected_frames": capture.get("detected_frames", 0),
            "tracked_frames": capture.get("tracked_frames", 0),
            "truth_provenance": row.get("provenance_summary"),
            "association_provenance": association(capture),
            "iq_integrity": iq_integrity(capture, iq),
            "offline_results": offline.get(candidate_key(row), [])})
    arrays = sorted({str(item.get("array_id") or item.get("array"))
                     for clip in clips for group in ("detection", "tracker")
                     for item in clip["association_provenance"][group]
                     if item.get("array_id") is not None or item.get("array") is not None})
    return {"scope": scope, "pass_id": pass_id, "hex": rows[0].get("hex"),
        "flight": rows[0].get("flight"), "live_outcome": outcome,
        "classification": classification, "classification_final": final,
        "classification_status": "final" if final else "pending_offline_matrix",
        "excluded_reasons": errors, "config_epoch": rows[0].get("config_epoch"),
        "receiver_epoch": rows[0].get("receiver_epoch"), "arrays": arrays or ["unknown"],
        "geometry": geometry, "clips": clips, "offline_results": offline_rows}


def counts(records):
    eligible = [row for row in records if row["classification"] != "excluded_with_reason"]
    final = [row for row in eligible if row["classification_final"]]
    by_class = Counter(row["classification"] or "pending_offline_matrix" for row in records)
    aircraft = defaultdict(list)
    for row in records:
        aircraft[row["hex"]].append(row)
    aircraft_class = Counter()
    precedence = ("validated_offline_recovery", "offline_candidate", "tracked",
                  "detected_not_tracked", "miss_unrecoverable", "excluded_with_reason")
    for rows in aircraft.values():
        values = {row["classification"] for row in rows}
        value = next((name for name in precedence if name in values), "pending_offline_matrix")
        aircraft_class[value] += 1
    final_class = Counter(row["classification"] for row in final)
    iq_status = Counter(clip["iq_integrity"]["status"] for row in records for clip in row["clips"])
    association_status = Counter(clip["association_provenance"]["status"]
                                 for row in records for clip in row["clips"])
    return {"passes_total": len(records), "passes_eligible": len(eligible),
        "passes_classification_final": len(final), "unique_aircraft": len(aircraft),
        "passes_by_classification": dict(sorted(by_class.items())),
        "aircraft_by_classification": dict(sorted(aircraft_class.items())),
        "clip_iq_integrity": dict(sorted(iq_status.items())),
        "clip_association_provenance": dict(sorted(association_status.items())),
        "pass_rates_all_eligible": {key: value / len(eligible) if eligible else None
                                    for key, value in sorted(by_class.items()) if key != "excluded_with_reason"},
        "pass_rates_final_only": {key: value / len(final) if final else None
                                  for key, value in sorted(final_class.items())},
        "aircraft_rates": {key: value / len(aircraft) if aircraft else None
                           for key, value in sorted(aircraft_class.items())}}


def stratify(records):
    output = {}
    for dimension in GEOMETRY_BINS:
        groups = defaultdict(list)
        for row in records:
            groups[row["geometry"]["bins"][dimension]].append(row)
        output[dimension] = {name: counts(rows) for name, rows in sorted(groups.items())}
    arrays = defaultdict(list)
    for row in records:
        for array in row["arrays"]:
            arrays[array].append(row)
    output["array"] = {name: counts(rows) for name, rows in sorted(arrays.items())}
    return output


def generate(frozen, frozen_sha, rolling, iq, offline):
    frozen_candidates = frozen["candidates"]
    rolling_candidates = {key: row for key, row in rolling["candidates"].items()
                          if key not in frozen_candidates}
    scopes = []
    for name, source, candidates in (("frozen_matrix_cohort", frozen, frozen_candidates),
                                     ("post_freeze_rolling", rolling, rolling_candidates)):
        grouped = defaultdict(list)
        for row in candidates.values(): grouped[row["pass_id"]].append(row)
        records = [build_pass(key, rows, source, name, iq, offline)
                   for key, rows in sorted(grouped.items())]
        scopes.append({"scope": name, "immutable": name == "frozen_matrix_cohort",
                       "records": records, "aggregate": counts(records),
                       "stratified": stratify(records)})
    return {"schema": "vectorwarp-aircraft-pass-report-v1",
        "classification_enum": list(CLASSIFICATIONS),
        "classification_semantics": {
            "null": "strict live miss awaiting the complete offline matrix",
            "offline_candidate": "unthresholded candidate; not a recovery",
            "validated_offline_recovery": "positive-calibrated, trial-corrected recovery",
            "miss_unrecoverable": "complete frozen matrix found no validated recovery"},
        "frozen_cohort": {"sha256": frozen_sha, "candidate_clips": len(frozen_candidates)},
        "rolling_post_freeze": {"source_candidate_clips": len(rolling["candidates"]),
                                "new_candidate_clips": len(rolling_candidates)},
        "offline_join": {"accepted_candidate_keys": len(offline)},
        "bin_edges": {key: [("-inf" if x == -math.inf else "inf" if x == math.inf else x)
                            for x in values] for key, values in GEOMETRY_BINS.items()},
        "scopes": scopes}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--frozen", required=True, type=Path)
    parser.add_argument("--frozen-sha256", required=True)
    parser.add_argument("--rolling", required=True, type=Path)
    parser.add_argument("--iq-audit", type=Path)
    parser.add_argument("--offline-result", action="append", type=Path, default=[])
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    actual = sha256(args.frozen)
    if actual != args.frozen_sha256:
        raise ValueError(f"frozen cohort SHA mismatch: expected {args.frozen_sha256}, got {actual}")
    frozen, rolling = json.loads(args.frozen.read_text()), json.loads(args.rolling.read_text())
    offline, rejected = load_offline(args.offline_result, actual)
    report = generate(frozen, actual, rolling, load_iq(args.iq_audit), offline)
    report["offline_join"]["rejected_inputs"] = rejected
    args.output.write_text(json.dumps(report, indent=2, allow_nan=False) + "\n")
    print(json.dumps({scope["scope"]: scope["aggregate"] for scope in report["scopes"]}, sort_keys=True))


if __name__ == "__main__":
    main()
