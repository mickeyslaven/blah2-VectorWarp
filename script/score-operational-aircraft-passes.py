#!/usr/bin/env python3
"""Score live VectorWarp performance at the independent-aircraft-pass level.

This scorer intentionally does not use frame count as the headline metric.  A
configuration is compared by the fraction of finalized, truth-valid aircraft
passes that form an ADS-B-associated radar track, with raw detection and track
formation reported separately.  Records are never pooled across config epochs.
"""

import argparse
import hashlib
import json
import math
import statistics
from collections import Counter, defaultdict
from pathlib import Path


SCHEMA = "vectorwarp-operational-scorecard-v1"
MIN_OPPORTUNITY_FRAMES = 3
MIN_SUCCESS_FRAMES = 3
MAX_RAW_TRUTH_OFFSET_S = 1.0
MAX_SOURCE_DISAGREEMENT_KM = 0.5


def wilson(successes, total, z=1.959963984540054):
    if total <= 0:
        return {"estimate": None, "lower_95": None, "upper_95": None}
    p = successes / total
    denominator = 1 + z * z / total
    centre = (p + z * z / (2 * total)) / denominator
    radius = z * math.sqrt(p * (1 - p) / total + z * z / (4 * total * total)) / denominator
    lower = 0.0 if successes == 0 else max(0.0, centre - radius)
    upper = 1.0 if successes == total else min(1.0, centre + radius)
    return {"estimate": p, "lower_95": lower, "upper_95": upper}


def finite_number(value):
    return isinstance(value, (int, float)) and math.isfinite(value)


def truth_errors(event):
    errors = []
    samples = event.get("truth_samples") or []
    if len(samples) < MIN_OPPORTUNITY_FRAMES:
        errors.append("insufficient_truth_samples")
        return errors
    for sample in samples:
        provenance = sample.get("provenance") or {}
        offset = provenance.get("observation_time_offset_s")
        if not finite_number(offset) or abs(offset) > MAX_RAW_TRUTH_OFFSET_S:
            errors.append("raw_truth_offset_exceeds_1s")
        if provenance.get("projection_model") != "constant_velocity_ground_track":
            errors.append("missing_event_time_projection")
        position_age = provenance.get("position_age_s")
        if not finite_number(position_age) or abs(position_age) > 1e-6:
            errors.append("truth_not_projected_to_event_time")
        disagreement = provenance.get("maximum_pairwise_projected_disagreement_km")
        if finite_number(disagreement) and disagreement > MAX_SOURCE_DISAGREEMENT_KM:
            errors.append("source_disagreement_exceeds_0_5km")
    return sorted(set(errors))


def event_geometry(event):
    samples = event.get("truth_samples") or []
    def med(field):
        values = [x.get(field) for x in samples if finite_number(x.get(field))]
        return statistics.median(values) if values else None
    def med_abs(field):
        values = [abs(x.get(field)) for x in samples if finite_number(x.get(field))]
        return statistics.median(values) if values else None
    return {
        "delay_km": med("delay_km"),
        "elevation_deg": med("elevation_deg"),
        "ground_range_km": med("ground_range_km"),
        "abs_doppler_hz": med_abs("doppler_hz"),
        "azimuth_deg": med("bearing_deg_true"),
        "altitude_ft": med("altitude_geom_ft"),
    }


def delay_region(delay):
    if not finite_number(delay):
        return "unknown"
    if delay <= 30:
        return "core_le_30km"
    if delay <= 60:
        return "extended_30_60km"
    return "outside_60km"


def elevation_region(elevation):
    if not finite_number(elevation):
        return "unknown"
    if elevation < 5:
        return "low_lt_5deg"
    if elevation < 15:
        return "mid_5_15deg"
    return "high_ge_15deg"


def doppler_region(doppler):
    if not finite_number(doppler):
        return "unknown"
    value = abs(doppler)
    if value < 15:
        return "clutter_band_lt_15hz"
    if value < 100:
        return "near_15_100hz"
    return "clear_ge_100hz"


def azimuth_region(azimuth):
    if not finite_number(azimuth):
        return "unknown"
    sector = int((azimuth % 360) // 45) * 45
    return f"azimuth_{sector:03d}_{sector + 45:03d}deg"


def normalize_event(event):
    errors = []
    if event.get("state") != "finalized":
        errors.append("not_finalized")
    eligible = event.get("eligible_frames")
    if not isinstance(eligible, int) or eligible < MIN_OPPORTUNITY_FRAMES:
        errors.append("insufficient_eligible_frames")
    errors.extend(truth_errors(event))
    detected = event.get("detected_frames") or 0
    tracked = event.get("tracked_frames") or 0
    geometry = event_geometry(event)
    return {
        "pass_id": event.get("pass_id"), "hex": event.get("hex"),
        "flight": event.get("flight"), "config_epoch": event.get("config_epoch"),
        "receiver_epoch": event.get("receiver_epoch"), "observed_at": event.get("observed_at"),
        "finalized_at": event.get("finalized_at"), "eligible_frames": eligible or 0,
        "detected_frames": detected, "tracked_frames": tracked,
        "detection_success": detected >= MIN_SUCCESS_FRAMES,
        "tracking_success": tracked >= MIN_SUCCESS_FRAMES,
        "continuity": tracked / eligible if isinstance(eligible, int) and eligible > 0 else None,
        "detection_latency_s": latency(event.get("observed_at"), event.get("first_detection_at")),
        "tracking_latency_s": latency(event.get("observed_at"), event.get("first_track_at")),
        "geometry": geometry,
        "strata": {"delay_region": delay_region(geometry["delay_km"]),
                   "elevation_region": elevation_region(geometry["elevation_deg"]),
                   "doppler_region": doppler_region(geometry["abs_doppler_hz"]),
                   "azimuth_region": azimuth_region(geometry["azimuth_deg"])},
        "admitted": not errors, "exclusion_reasons": sorted(set(errors)),
    }


def latency(start_ms, event_ms):
    if not finite_number(start_ms) or not finite_number(event_ms):
        return None
    return max(0.0, (event_ms - start_ms) / 1000)


def median_or_none(values):
    values = [x for x in values if finite_number(x)]
    return statistics.median(values) if values else None


def summarize(records):
    admitted = [x for x in records if x["admitted"]]
    detected = sum(x["detection_success"] for x in admitted)
    tracked = sum(x["tracking_success"] for x in admitted)
    tracked_records = [x for x in admitted if x["tracking_success"]]
    detected_records = [x for x in admitted if x["detection_success"]]
    exclusions = Counter(reason for x in records for reason in x["exclusion_reasons"])
    return {
        "passes_seen": len(records), "passes_admitted": len(admitted),
        "passes_excluded": len(records) - len(admitted),
        "detection_passes": detected, "tracked_passes": tracked,
        "raw_detection_recall": wilson(detected, len(admitted)),
        "primary_tracking_recall": wilson(tracked, len(admitted)),
        "detected_to_tracked_conversion": wilson(
            sum(x["tracking_success"] for x in detected_records), len(detected_records)),
        "median_tracking_continuity": median_or_none(x["continuity"] for x in tracked_records),
        "median_detection_latency_s": median_or_none(x["detection_latency_s"] for x in detected_records),
        "median_tracking_latency_s": median_or_none(x["tracking_latency_s"] for x in tracked_records),
        "exclusion_reasons": dict(sorted(exclusions.items())),
    }


def stratify(records, field):
    groups = defaultdict(list)
    for record in records:
        groups[record["strata"][field]].append(record)
    return {name: summarize(rows) for name, rows in sorted(groups.items())}


def joint_geometry_key(record):
    """Coarse joint cells stay interpretable and usable with modest samples."""
    strata = record["strata"]
    return f'{strata["delay_region"]}|{strata["elevation_region"]}'


def standardized_comparison(baseline, candidate):
    """Directly standardize both arms to baseline delay/elevation traffic.

    Only common-support cells contribute. Coverage makes any loss of difficult
    baseline geometry explicit instead of silently dropping it.
    """
    baseline = [x for x in baseline if x["admitted"]]
    candidate = [x for x in candidate if x["admitted"]]
    base_cells, candidate_cells = defaultdict(list), defaultdict(list)
    for row in baseline:
        base_cells[joint_geometry_key(row)].append(row)
    for row in candidate:
        candidate_cells[joint_geometry_key(row)].append(row)
    common = sorted(set(base_cells) & set(candidate_cells))
    base_total = len(baseline)
    supported = sum(len(base_cells[key]) for key in common)
    coverage = supported / base_total if base_total else 0
    if not common or not supported:
        return {"status": "insufficient_common_support", "baseline_coverage": coverage}
    weights = {key: len(base_cells[key]) / supported for key in common}
    def standardized(cells):
        return sum(weights[key] *
                   sum(row["tracking_success"] for row in cells[key]) / len(cells[key])
                   for key in common)
    base_rate, candidate_rate = standardized(base_cells), standardized(candidate_cells)
    return {
        "status": "descriptive" if min(len(baseline), len(candidate)) < 30 else "preliminary",
        "method": "direct standardization to baseline delay/elevation mix on common support",
        "baseline_passes": len(baseline), "candidate_passes": len(candidate),
        "baseline_coverage": coverage, "common_cells": common,
        "reference_weights": weights,
        "baseline_standardized_tracking_recall": base_rate,
        "candidate_standardized_tracking_recall": candidate_rate,
        "standardized_difference_percentage_points": (candidate_rate - base_rate) * 100,
        "warning": None if coverage >= 0.8 else "less_than_80_percent_baseline_geometry_coverage",
    }


def score(document, label, source, source_sha256=None):
    records = [normalize_event(x) for x in document.get("eligibilityEvents", [])]
    epochs = defaultdict(list)
    for record in records:
        epochs[record.get("config_epoch") or "unknown"].append(record)
    return {
        "label": label, "source": source, "source_sha256": source_sha256,
        "input_schema": document.get("schema"),
        "input_created_at": document.get("createdAt"), "input_updated_at": document.get("updatedAt"),
        "epochs": [{
            "config_epoch": epoch,
            "summary": summarize(rows),
            "stratified": {"delay_region": stratify(rows, "delay_region"),
                           "elevation_region": stratify(rows, "elevation_region"),
                           "doppler_region": stratify(rows, "doppler_region"),
                           "azimuth_region": stratify(rows, "azimuth_region")},
            "records": rows,
        } for epoch, rows in sorted(epochs.items())],
    }


def parse_input(value):
    if "=" not in value:
        path = Path(value)
        return path.stem, path
    label, path = value.split("=", 1)
    return label, Path(path)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--input", action="append", required=True,
                        help="LABEL=PATH to evidence-statistics.json; repeatable")
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    inputs = []
    for value in args.input:
        label, path = parse_input(value)
        payload = path.read_bytes()
        inputs.append(score(json.loads(payload), label, str(path),
                            hashlib.sha256(payload).hexdigest()))
    output = {
        "schema": SCHEMA,
        "metric_contract": {
            "independent_unit": "finalized aircraft pass",
            "minimum_opportunity_frames": MIN_OPPORTUNITY_FRAMES,
            "success_frames": MIN_SUCCESS_FRAMES,
            "primary_metric": "tracking pass recall",
            "success_definition": "at least 3 ADS-B-associated tracked CPIs in an admitted pass",
            "detection_definition": "at least 3 ADS-B-associated raw-detection CPIs in an admitted pass",
            "truth": {"maximum_raw_observation_offset_s": MAX_RAW_TRUTH_OFFSET_S,
                      "event_time_projection_required": True,
                      "maximum_source_disagreement_km": MAX_SOURCE_DISAGREEMENT_KM},
            "pool_config_epochs": False,
            "interval": "two-sided Wilson 95%",
            "interpretation": "Recall is primary only at a non-inferior nuisance-track burden; nuisance data is reported separately and is not available in this evidence file.",
        },
        "inputs": inputs,
    }
    all_epochs = [(item["label"], epoch) for item in inputs for epoch in item["epochs"]]
    output["geometry_adjusted_comparisons"] = []
    if len(all_epochs) >= 2:
        baseline_label, baseline = all_epochs[0]
        for candidate_label, candidate in all_epochs[1:]:
            comparison = standardized_comparison(baseline["records"], candidate["records"])
            comparison.update({"baseline": {"label": baseline_label,
                                             "config_epoch": baseline["config_epoch"]},
                               "candidate": {"label": candidate_label,
                                             "config_epoch": candidate["config_epoch"]}})
            output["geometry_adjusted_comparisons"].append(comparison)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(output, indent=2, allow_nan=False) + "\n")
    compact = {}
    for item in inputs:
        compact[item["label"]] = {epoch["config_epoch"]: epoch["summary"]
                                  for epoch in item["epochs"]}
    print(json.dumps(compact, sort_keys=True))


if __name__ == "__main__":
    main()
