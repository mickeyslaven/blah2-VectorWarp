"""Deterministic OPRS evidence scorer."""

from __future__ import annotations

import math
from collections import defaultdict
from typing import Any

from .common import object_hash, percentile, require, wilson
from .contracts import validate_manifest, validate_plan, validate_trial


DOMAIN_NAMES = ("evaluation_integrity", "acquisition_rf", "synchronization_calibration",
                "reference_quality", "cancellation_target_preservation", "raw_detection",
                "tracking", "bearing_spatial", "recoverability_regression", "realtime_operational")


def _metric(name: str, status: str, value: Any = None, unit: str | None = None,
            numerator: int | float | None = None, denominator: int | float | None = None,
            interval: list[float] | None = None, limitations: list[str] | None = None) -> dict[str, Any]:
    return {"name": name, "status": status, "applicable": status != "N/A", "value": value,
            "unit": unit, "numerator": numerator, "denominator": denominator,
            "interval_95": interval, "limitations": limitations or []}


def _status_from_threshold(value: float, rule: dict[str, Any]) -> str:
    direction = rule.get("direction")
    passed, warned = rule.get("pass"), rule.get("warn")
    if direction == "min":
        return "PASS" if value >= passed else "WARN" if value >= warned else "FAIL"
    if direction == "max":
        return "PASS" if value <= passed else "WARN" if value <= warned else "FAIL"
    return "INVALID"


def _crossing(curve: list[dict[str, Any]], target: float) -> dict[str, Any]:
    """Monotone crossing without extrapolation; lower dB is weaker."""
    monotone = []
    running = 0.0
    for point in sorted(curve, key=lambda item: item["level_db_ref_rms"]):
        running = max(running, point["probability"])
        monotone.append((point["level_db_ref_rms"], running))
    if not monotone or monotone[-1][1] < target:
        return {"estimate_db_ref_rms": None, "bound": f"> {monotone[-1][0]}" if monotone else None}
    if monotone[0][1] >= target:
        return {"estimate_db_ref_rms": None, "bound": f"<= {monotone[0][0]}"}
    for (left_level, left_p), (right_level, right_p) in zip(monotone, monotone[1:]):
        if left_p < target <= right_p:
            if right_p == left_p:
                return {"estimate_db_ref_rms": right_level, "bound": None}
            value = left_level + (target - left_p) * (right_level - left_level) / (right_p - left_p)
            return {"estimate_db_ref_rms": value, "bound": None}
    return {"estimate_db_ref_rms": None, "bound": None}


def _measurement_domains(plan: dict[str, Any], trials: list[dict[str, Any]]) -> dict[str, dict[str, Any]]:
    rules = plan.get("metric_thresholds", {})
    automatic_metrics = {"raw_detection_pass_recall", "tracking_pass_recall",
                         "pass_macro_continuity", "nuisance_tracks_per_hour"}
    values: dict[str, list[dict[str, Any]]] = defaultdict(list)
    for trial in trials:
        for item in trial.get("measurements", []):
            values[item.get("metric_id")].append(item)
    domains: dict[str, dict[str, Any]] = {}
    for domain in DOMAIN_NAMES:
        required = domain in plan.get("required_domains", [])
        metrics = []
        for metric_id, rule in sorted(rules.items()):
            if metric_id in automatic_metrics:
                continue
            if rule.get("domain") != domain:
                continue
            observed = values.get(metric_id, [])
            if not observed:
                metrics.append(_metric(metric_id, "NOT_QUALIFIED" if required else "N/A",
                                       limitations=["No measurement supplied for the frozen metric."]))
                continue
            numeric = [item["value"] for item in observed if isinstance(item.get("value"), (int, float))]
            if len(numeric) != len(observed):
                metrics.append(_metric(metric_id, "INVALID", limitations=["Non-numeric measurement."]))
                continue
            aggregation = rule.get("aggregation", "worst")
            if aggregation == "mean":
                value = sum(numeric) / len(numeric)
            elif rule.get("direction") == "max":
                value = max(numeric)
            else:
                value = min(numeric)
            metrics.append(_metric(metric_id, _status_from_threshold(value, rule), value, rule.get("unit")))
        if not metrics:
            status = "PASS" if domain == "evaluation_integrity" else ("NOT_QUALIFIED" if required else "N/A")
        else:
            states = {item["status"] for item in metrics}
            status = "INVALID" if "INVALID" in states else "FAIL" if "FAIL" in states else \
                "NOT_QUALIFIED" if "NOT_QUALIFIED" in states else "WARN" if "WARN" in states else \
                "PASS" if "PASS" in states else "N/A"
        domains[domain] = {"status": status, "required": required, "metrics": metrics, "limitations": []}
    return domains


def score(manifest: dict[str, Any], plan: dict[str, Any], trials: list[dict[str, Any]]) -> dict[str, Any]:
    validate_manifest(manifest)
    validate_plan(plan)
    require(plan["system_manifest_hash"] == object_hash(manifest), "plan references a different system manifest")
    for trial in trials:
        validate_trial(trial, plan)
    if len({trial["trial_id"] for trial in trials}) != len(trials):
        raise ValueError("duplicate trial_id")
    planned = {row["opportunity_id"]: row for row in plan["opportunities"]}
    rows = []
    missing = 0
    for trial in trials:
        by_id = {row["opportunity_id"]: row for row in trial["opportunity_results"]}
        for opportunity_id, truth in planned.items():
            result = by_id.get(opportunity_id)
            if result is None:
                missing += 1
                result = {"raw_detection": False, "track_present": False, "track_confirmed": False,
                          "measurement_supported": False, "bearing_deg": None, "track_id": None}
            rows.append((truth, result, trial["trial_id"]))

    pass_rows: dict[tuple[str, str], list[tuple[dict[str, Any], dict[str, Any]]]] = defaultdict(list)
    for truth, result, trial_id in rows:
        pass_rows[(trial_id, truth["pass_id"])].append((truth, result))
    min_frames = plan.get("pass_success_min_cpis", 1)
    detection_successes = tracking_successes = 0
    continuities: list[float] = []
    fragments: list[int] = []
    bearing_errors: list[float] = []
    bearing_pass_success: dict[tuple[str, str], list[float]] = defaultdict(list)
    for key, items in pass_rows.items():
        detections = sum(result["raw_detection"] for _, result in items)
        supported_tracks = sum(result["track_present"] and result["track_confirmed"] and
                               result["measurement_supported"] for _, result in items)
        detection_successes += detections >= min_frames
        tracking_successes += supported_tracks >= min_frames
        continuities.append(supported_tracks / len(items))
        fragments.append(len({result["track_id"] for _, result in items
                              if result["track_present"] and result["track_confirmed"] and result["measurement_supported"]}))
        for truth, result in items:
            if result.get("bearing_deg") is not None and truth["truth"].get("bearing_deg") is not None:
                delta = abs((result["bearing_deg"] - truth["truth"]["bearing_deg"] + 180) % 360 - 180)
                bearing_errors.append(delta)
                bearing_pass_success[key].append(delta)
    passes = len(pass_rows)
    detection_ci, tracking_ci = wilson(detection_successes, passes), wilson(tracking_successes, passes)
    nuisance = sum(trial["nuisance_new_tracks"] for trial in trials)
    hours = sum(trial["valid_radar_seconds"] for trial in trials) / 3600
    nuisance_rate = nuisance / hours
    domains = _measurement_domains(plan, trials)
    domains["evaluation_integrity"] = {"status": "PASS", "required": True, "metrics": [
        _metric("frozen_opportunity_contract", "PASS", True),
        _metric("missing_opportunity_results_counted_as_misses", "PASS", missing, "count")], "limitations": []}
    rules = plan.get("metric_thresholds", {})
    def automatic(metric_id, value):
        return _status_from_threshold(value, rules[metric_id]) if metric_id in rules else "NOT_QUALIFIED"
    detection_metric = _metric(
        "raw_detection_pass_recall", automatic("raw_detection_pass_recall", detection_ci["estimate"]),
        detection_ci["estimate"], "fraction", detection_successes, passes,
        [detection_ci["lower_95"], detection_ci["upper_95"]])
    domains["raw_detection"]["metrics"].append(detection_metric)
    tracking_metrics = [
        _metric("tracking_pass_recall", automatic("tracking_pass_recall", tracking_ci["estimate"]),
                tracking_ci["estimate"], "fraction", tracking_successes, passes,
                [tracking_ci["lower_95"], tracking_ci["upper_95"]]),
        _metric("pass_macro_continuity", automatic("pass_macro_continuity", sum(continuities) / len(continuities)),
                sum(continuities) / len(continuities), "fraction"),
        _metric("median_track_fragments_per_pass", "PASS", percentile([float(v) for v in fragments], .5), "count"),
        _metric("nuisance_tracks_per_hour", automatic("nuisance_tracks_per_hour", nuisance_rate),
                nuisance_rate, "tracks/hour", nuisance, hours)]
    domains["tracking"]["metrics"].extend(tracking_metrics)
    for domain_name in ("raw_detection", "tracking"):
        states = {metric["status"] for metric in domains[domain_name]["metrics"]}
        domains[domain_name]["status"] = "INVALID" if "INVALID" in states else "FAIL" if "FAIL" in states else \
            "NOT_QUALIFIED" if "NOT_QUALIFIED" in states else "WARN" if "WARN" in states else "PASS"
    trial_by_id = {trial["trial_id"]: trial for trial in trials}
    curves = {}
    for label, predicate in (("detection", lambda result: result["raw_detection"]),
                             ("tracking", lambda result: result["track_present"] and result["track_confirmed"] and result["measurement_supported"])):
        grouped: dict[float, list[bool]] = defaultdict(list)
        for (trial_id, _pass_id), items in pass_rows.items():
            level = trial_by_id[trial_id].get("injection_level_db_ref_rms")
            if isinstance(level, (int, float)):
                grouped[float(level)].append(sum(predicate(result) for _, result in items) >= min_frames)
        if grouped:
            curve = [{"level_db_ref_rms": level, "successes": sum(outcomes), "trials": len(outcomes),
                      "probability": sum(outcomes) / len(outcomes)} for level, outcomes in sorted(grouped.items())]
            curves[label] = {"probability_curve": curve, "L50": _crossing(curve, .5), "L90": _crossing(curve, .9)}
            domain = "raw_detection" if label == "detection" else "tracking"
            domains[domain]["metrics"].append(_metric(f"{label}_sensitivity", "PASS", curves[label], "dB ref RMS"))
    recovery_rows = [row for trial in trials for row in trial.get("recovery_trials", [])]
    if recovery_rows:
        ordered = sorted(enumerate(recovery_rows), key=lambda pair: pair[1].get("p_value", 2.0))
        rejected = set()
        for rank, (index, item) in enumerate(ordered):
            p_value = item.get("p_value")
            if not isinstance(p_value, (int, float)) or not 0 <= p_value <= 1:
                continue
            if p_value <= .05 / (len(ordered) - rank):
                rejected.add(index)
            else:
                break
        confirmed = sum(index in rejected and item.get("recovered_miss") is True and
                        item.get("positive_controls_retained") == item.get("positive_controls_total")
                        for index, item in enumerate(recovery_rows))
        domains["recoverability_regression"]["metrics"].append(_metric(
            "holm_familywise_confirmed_recoveries", "PASS", confirmed, "count", confirmed, len(recovery_rows),
            limitations=["Family-wise alpha=0.05; all declared positive controls must be retained."]))
    bearing_claimed = manifest.get("bearing_capability") is True
    if bearing_claimed:
        if bearing_errors:
            within = {angle: sum(all(error <= angle for error in errors) for errors in bearing_pass_success.values())
                      for angle in (15, 30, 45)}
            metrics = [_metric(f"pass_macro_within_{angle}deg", "PASS", within[angle] / len(pass_rows), "fraction",
                               within[angle], len(pass_rows)) for angle in (15, 30, 45)]
            metrics += [_metric("median_absolute_circular_error", "PASS", percentile(bearing_errors, .5), "degree"),
                        _metric("p90_absolute_circular_error", "PASS", percentile(bearing_errors, .9), "degree")]
            domains["bearing_spatial"].update(status="PASS", metrics=metrics)
        else:
            domains["bearing_spatial"].update(status="NOT_QUALIFIED", metrics=[
                _metric("bearing_evidence", "NOT_QUALIFIED", limitations=["Bearing is claimed but no event-time bearing estimates were supplied."])])
    else:
        domains["bearing_spatial"] = {"status": "N/A", "required": False, "metrics": [
            _metric("bearing_capability", "N/A", limitations=["System does not claim bearing capability."])], "limitations": []}
    required_states = [domain["status"] for domain in domains.values() if domain["required"]]
    overall = "INVALID" if "INVALID" in required_states else "FAIL" if "FAIL" in required_states else \
        "NOT_QUALIFIED" if any(state in {"NOT_QUALIFIED", "N/A"} for state in required_states) else "QUALIFIED"
    bearing30 = next((m for m in domains["bearing_spatial"]["metrics"] if m["name"] == "pass_macro_within_30deg"),
                     _metric("pass_macro_within_30deg", "N/A"))
    return {"schema": "open-passive-radar-scorecard-v1", "oprs_version": "0.2.0",
            "claim_scope": plan["evidence_layer"], "overall_status": overall,
            "system_id": manifest["system_id"], "capability_profile": manifest["capability_profile"],
            "plan_id": plan["plan_id"], "plan_hash": plan["plan_hash"],
            "mission_tuple": {"tracking_pass_recall": next(m for m in tracking_metrics if m["name"] == "tracking_pass_recall"),
                              "raw_detection_pass_recall": detection_metric,
                              "nuisance_tracks_per_hour": next(m for m in tracking_metrics if m["name"] == "nuisance_tracks_per_hour"),
                              "bearing_within_30deg": bearing30},
            "domains": domains,
            "evidence_summary": {"trials": len(trials), "planned_opportunities_per_trial": len(planned),
                                 "missing_results_counted_as_misses": missing, "passes": passes,
                                 "sensitivity": curves},
            "prohibitions": ["This scorecard does not combine evidence layers.",
                             "Scoring truth is validation-only and must not be used during inference."]}


def invalid_scorecard(manifest: dict[str, Any] | None, plan: dict[str, Any] | None, errors: list[str]) -> dict[str, Any]:
    return {"schema": "open-passive-radar-scorecard-v1", "oprs_version": "0.2.0", "overall_status": "INVALID",
            "system_id": (manifest or {}).get("system_id"), "claim_scope": (plan or {}).get("evidence_layer"),
            "mission_tuple": None, "domains": {"evaluation_integrity": {"status": "INVALID", "required": True,
                "metrics": [], "limitations": errors}}, "integrity_errors": errors}
