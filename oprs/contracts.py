"""Strict semantic validation beyond what JSON Schema can express."""

from __future__ import annotations

from collections import Counter
from typing import Any

from .common import ContractError, LAYERS, PROFILES, finite, object_hash, require


def _ids(rows: list[dict[str, Any]], field: str, where: str) -> None:
    values = [row.get(field) for row in rows]
    require(all(isinstance(v, str) and v for v in values), f"{where}: every {field} is required")
    duplicates = sorted(value for value, count in Counter(values).items() if count > 1)
    require(not duplicates, f"{where}: duplicate {field}: {duplicates[:5]}")


def validate_manifest(value: dict[str, Any]) -> dict[str, Any]:
    require(value.get("schema") == "oprs-system-manifest-v1", "unsupported system manifest schema")
    profile = value.get("capability_profile")
    require(profile in PROFILES, "unknown capability_profile")
    system_id = value.get("system_id")
    require(isinstance(system_id, str) and system_id, "system_id is required")
    channels = value.get("channels")
    require(isinstance(channels, list) and channels, "channels must be non-empty")
    _ids(channels, "channel_id", "manifest channels")
    roles = [role for channel in channels for role in channel.get("roles", [])]
    surveillance = sum("surveillance" in channel.get("roles", []) for channel in channels)
    references = sum("reference" in channel.get("roles", []) for channel in channels)
    require(all(role in {"reference", "surveillance"} for role in roles), "invalid channel role")
    if profile == "REF_SURV_2CH":
        require(len(channels) == 2 and references == 1 and surveillance == 1,
                "REF_SURV_2CH requires exactly one reference and one surveillance channel")
    elif profile == "REF_PLUS_ARRAY":
        require(references >= 1 and surveillance >= 2, "REF_PLUS_ARRAY requires a reference and >=2 surveillance channels")
    elif profile == "SURV_ARRAY_NCH":
        require(surveillance >= 2, "SURV_ARRAY_NCH requires >=2 surveillance channels")
    elif profile == "SINGLE_CHANNEL_REPLAY":
        require(len(channels) == 1, "SINGLE_CHANNEL_REPLAY requires exactly one channel")
    if value.get("bearing_capability") is True:
        require(profile in {"SURV_ARRAY_NCH", "REF_PLUS_ARRAY"}, "bearing requires an array profile")
        require(isinstance(value.get("array_geometry"), dict), "bearing claim requires array_geometry")
    for name in ("sample_rate_hz", "center_frequency_hz", "full_scale"):
        require(finite(value.get(name)) and value[name] > 0, f"{name} must be positive")
    require(value.get("sample_format") in {"blah2iq_cf32le", "cf32le_channel_major", "cs16le_sample_major", "mchq_ci8"},
            "unsupported sample_format")
    return value

def validate_plan(value: dict[str, Any]) -> dict[str, Any]:
    require(value.get("schema") == "oprs-evaluation-plan-v1", "unsupported evaluation plan schema")
    for field in ("plan_id", "site_epoch_id", "receiver_epoch_id", "system_manifest_hash"):
        require(isinstance(value.get(field), str) and value[field], f"{field} is required")
    require(value.get("partition") == "evaluation", "only a frozen evaluation partition can be scored")
    require(value.get("evidence_layer") in LAYERS, "invalid evidence_layer")
    envelope = value.get("opportunity_envelope")
    require(isinstance(envelope, dict) and envelope.get("detector_independent") is True,
            "opportunity envelope must be detector-independent")
    for field in ("delay_min_km", "delay_max_km", "doppler_min_hz", "doppler_max_hz"):
        require(finite(envelope.get(field)), f"opportunity_envelope.{field} is required")
    require(envelope["delay_min_km"] < envelope["delay_max_km"], "invalid delay envelope")
    require(envelope["doppler_min_hz"] < envelope["doppler_max_hz"], "invalid Doppler envelope")
    opportunities = value.get("opportunities")
    require(isinstance(opportunities, list) and opportunities, "plan needs opportunities")
    _ids(opportunities, "opportunity_id", "plan opportunities")
    cpis = [(row.get("pass_id"), row.get("cpi_id")) for row in opportunities]
    require(all(isinstance(a, str) and a and isinstance(b, str) and b for a, b in cpis),
            "pass_id and cpi_id are required")
    require(len(cpis) == len(set(cpis)), "duplicate (pass_id, cpi_id) truth opportunity")
    for row in opportunities:
        require(isinstance(row.get("event_time_ns"), int) and row["event_time_ns"] >= 0, "event_time_ns is required")
        require(isinstance(row.get("truth"), dict), "truth is required")
        truth = row["truth"]
        require(isinstance(truth.get("sample_time_ns"), int), "truth.sample_time_ns is required")
        require(abs(truth["sample_time_ns"] - row["event_time_ns"]) <= value.get("max_truth_offset_ns", 0),
                "truth is not at CPI event time within the frozen tolerance")
        require(truth.get("used_by_inference") is False, "scoring truth must not be used by inference")
        delay, doppler = truth.get("delay_km"), truth.get("doppler_hz")
        require(finite(delay) and finite(doppler), "truth delay_km and doppler_hz are required")
        require(envelope["delay_min_km"] <= delay <= envelope["delay_max_km"], "plan truth outside fixed delay envelope")
        require(envelope["doppler_min_hz"] <= doppler <= envelope["doppler_max_hz"], "plan truth outside fixed Doppler envelope")
    declared = value.get("plan_hash")
    if declared:
        unhashed = dict(value)
        unhashed.pop("plan_hash", None)
        require(declared == object_hash(unhashed), "plan_hash does not match canonical plan")
    return value


def validate_trial(value: dict[str, Any], plan: dict[str, Any]) -> dict[str, Any]:
    require(value.get("schema") == "oprs-trial-output-v1", "unsupported trial output schema")
    for field in ("trial_id", "plan_hash", "site_epoch_id", "receiver_epoch_id", "config_epoch_id"):
        require(isinstance(value.get(field), str) and value[field], f"{field} is required")
    require(value["plan_hash"] == plan["plan_hash"], "trial references a different evaluation plan")
    require(value.get("evidence_layer") == plan["evidence_layer"], "trial evidence layer differs from plan")
    require(value.get("partition") == "evaluation", "trial is not held-out evaluation evidence")
    require(value.get("inference_truth_access") is False, "validation truth was available during inference")
    require(value["site_epoch_id"] == plan["site_epoch_id"], "site epoch mismatch")
    require(value["receiver_epoch_id"] == plan["receiver_epoch_id"], "receiver epoch mismatch")
    results = value.get("opportunity_results")
    require(isinstance(results, list), "opportunity_results must be an array")
    _ids(results, "opportunity_id", "trial opportunity_results")
    planned = {row["opportunity_id"]: row for row in plan["opportunities"]}
    require(not (set(row["opportunity_id"] for row in results) - planned.keys()), "trial contains opportunities absent from plan")
    for row in results:
        truth = planned[row["opportunity_id"]]
        require(row.get("pass_id") == truth["pass_id"] and row.get("cpi_id") == truth["cpi_id"],
                "trial identity does not match frozen truth")
        require(row.get("event_time_ns") == truth["event_time_ns"], "trial CPI timestamp differs from plan")
        for field in ("raw_detection", "track_present", "track_confirmed", "measurement_supported"):
            require(isinstance(row.get(field), bool), f"{field} must be boolean")
        require(not row["measurement_supported"] or row["track_present"], "measurement support without a track")
        require(not row["track_confirmed"] or row["track_present"], "confirmed state without a track")
        if row["measurement_supported"]:
            require(isinstance(row.get("measurement_id"), str) and row["measurement_id"],
                    "measurement_id required for a measurement-supported track")
        if row["track_present"]:
            require(isinstance(row.get("track_id"), str) and row["track_id"], "track_id required when track_present")
        if row.get("bearing_deg") is not None:
            require(finite(row["bearing_deg"]), "bearing_deg must be finite")
    require(finite(value.get("valid_radar_seconds")) and value["valid_radar_seconds"] > 0,
            "valid_radar_seconds must be positive")
    require(isinstance(value.get("nuisance_new_tracks"), int) and value["nuisance_new_tracks"] >= 0,
            "nuisance_new_tracks must be a non-negative integer")
    hashes = value.get("evidence_hashes")
    require(isinstance(hashes, dict) and hashes, "evidence_hashes must be non-empty")
    require(all(isinstance(item, str) and item.startswith("sha256:") and len(item) == 71 for item in hashes.values()),
            "every evidence hash must be sha256:<64 hex>")
    return value
