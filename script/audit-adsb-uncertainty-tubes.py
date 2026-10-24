#!/usr/bin/env python3
"""Audit ADS-B replay uncertainty and emit a replay quarantine manifest.

This is a metadata-only tool.  It never opens an RF device, modifies a source
manifest, or removes a recording.  The kinematic bounds are deliberately
explicit: excess-path range is 2-Lipschitz in target position and bistatic
Doppler rate is bounded using a configured aircraft acceleration limit.
Position-only disagreement cannot bound velocity disagreement, so paired-source
frames are marked Doppler-unbounded unless both sources provide velocity.
"""

import argparse
import hashlib
import json
import math
from collections import Counter
from pathlib import Path


C_MPS = 299_792_458.0


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def frame_bound(frame, frequency_hz, sample_rate_hz, acceleration_m_s2,
                speed_bound_m_s=350.0):
    provenance = frame.get("adsb_reconciliation", {})
    age_ms = provenance.get("timing_error_ms")
    disagreement = provenance.get("disagreement", {})
    disagreement_km = disagreement.get("horizontal_km", 0.0) or 0.0
    delay_bin_km = C_MPS / sample_rate_hz / 1000.0
    if age_ms is None:
        return {"bounded": False, "reason": "no_raw_observation",
                "time_offset_radius_ms": None, "delay_radius_bins": None,
                "doppler_radius_hz": None, "disagreement_horizontal_km": disagreement_km}
    age_s = age_ms / 1000.0
    # Acceleration uncertainty changes path-rate by at most 2*a*t.  This is a
    # conditional kinematic bound; the assumption is recorded in the report.
    doppler_age_hz = 2 * frequency_hz / C_MPS * acceleration_m_s2 * age_s
    velocity_bounded = not disagreement or all(
        key in disagreement for key in ("velocity_delta_m_s",))
    velocity_delta = disagreement.get("velocity_delta_m_s", 0.0) or 0.0
    doppler_radius_hz = (doppler_age_hz + 2 * frequency_hz / C_MPS * velocity_delta
                         if velocity_bounded else None)
    # Excess-path range is 2-Lipschitz in target position.  The age term uses a
    # declared speed ceiling because the held source position was not projected.
    delay_radius_km = 2 * (speed_bound_m_s * age_s / 1000 + disagreement_km)
    delay_radius_bins = math.ceil(delay_radius_km / delay_bin_km)
    return {"bounded": velocity_bounded, "reason": None if velocity_bounded else
            "paired_sources_lack_velocity_disagreement",
            "time_offset_radius_ms": age_ms,
            "delay_radius_bins_from_source_disagreement": math.ceil(
                2 * disagreement_km / delay_bin_km),
            "delay_radius_bins": delay_radius_bins,
            "delay_radius_km": delay_radius_km,
            "doppler_radius_hz": doppler_radius_hz,
            "doppler_radius_hz_from_age": doppler_age_hz,
            "disagreement_horizontal_km": disagreement_km}


def audit_manifest(path, sample_rate_hz=2_400_000.0, cpi_s=.5,
                   fresh_age_ms=3000, acceleration_m_s2=10.0,
                   speed_bound_m_s=350.0,
                   fixed_delay_radius_bins=4, fixed_doppler_radius_hz=20.0,
                   processing_max_delay_km=64.0, processing_max_doppler_hz=800.0):
    document = json.loads(path.read_text())
    frequency_hz = document["adsb_reconciliation"]["frequency_hz"]
    output = []
    for row in document.get("rows", []):
        bounds = [frame_bound(frame, frequency_hz, sample_rate_hz, acceleration_m_s2,
                              speed_bound_m_s)
                  for frame in row.get("replay_trajectory", [])]
        ages = [item["time_offset_radius_ms"] for item in bounds
                if item["time_offset_radius_ms"] is not None]
        fallback = any(item["time_offset_radius_ms"] is None for item in bounds)
        non_fresh = fallback or not ages or max(ages) > fresh_age_ms
        unbounded = fallback or any(not item["bounded"] for item in bounds)
        # Current v2 manifests hold the most recent position at the CPI; there
        # is no projection timestamp/method/covariance in the provenance.
        projected = bool(bounds) and all(
            frame.get("adsb_reconciliation", {}).get("projection")
            for frame in row.get("replay_trajectory", []))
        multisource = bool(bounds) and all(
            len(frame.get("adsb_reconciliation", {}).get("candidates", [])) >= 2
            for frame in row.get("replay_trajectory", []))
        max_delay_bins = max((item.get("delay_radius_bins") or 0 for item in bounds), default=0)
        bounded_dopplers = [item["doppler_radius_hz"] for item in bounds
                            if item["doppler_radius_hz"] is not None]
        max_doppler_hz = max(bounded_dopplers, default=None)
        outside_fixed = (unbounded or max_delay_bins > fixed_delay_radius_bins or
                         max_doppler_hz is None or max_doppler_hz > fixed_doppler_radius_hz)
        centers_delay = [float(frame["delay_km"]) for frame in row.get("replay_trajectory", [])]
        centers_doppler = [float(frame["doppler_hz"]) for frame in row.get("replay_trajectory", [])]
        delay_bin_km = C_MPS / sample_rate_hz / 1000.0
        outside_processing = (unbounded or not centers_delay or not centers_doppler or
            min(centers_delay) - max_delay_bins * delay_bin_km < 0 or
            max(centers_delay) + max_delay_bins * delay_bin_km > processing_max_delay_km or
            max_doppler_hz is None or
            min(centers_doppler) - max_doppler_hz < -processing_max_doppler_hz or
            max(centers_doppler) + max_doppler_hz > processing_max_doppler_hz)
        capture = row["capture"]
        source_before_recording = any(
            frame.get("adsb_reconciliation", {}).get("observation_event_time_ms") is not None and
            frame["adsb_reconciliation"]["observation_event_time_ms"] < capture["window_start"]
            for frame in row.get("replay_trajectory", []))
        reasons = []
        if non_fresh: reasons.append("non_fresh_truth")
        if unbounded: reasons.append("unbounded_truth")
        if not projected: reasons.append("held_not_projected_to_cpi")
        if not multisource: reasons.append("not_multisource_every_cpi")
        output.append({
            "event_id": row["event_id"], "hex": row.get("hex"), "flight": row.get("flight"),
            "role": row.get("role"), "outcome": row.get("outcome"),
            "recording": capture["file"], "revision": capture.get("revision"),
            "capture_window_ms": [capture["window_start"], capture["window_end"]],
            "frames": len(bounds), "max_raw_age_ms": max(ages) if ages else None,
            "max_source_disagreement_km": max(
                (item["disagreement_horizontal_km"] for item in bounds), default=0.0),
            "required_time_offset_radius_ms": max(ages) if ages else None,
            "required_delay_radius_bins": None if unbounded else max_delay_bins,
            "required_doppler_radius_hz": None if unbounded else max_doppler_hz,
            "outside_fixed_search": outside_fixed,
            "outside_requested_64km_800hz_processing": outside_processing,
            "source_observation_predates_recording": source_before_recording,
            "projected_to_cpi": projected, "multisource_every_cpi": multisource,
            "quarantined": bool(reasons), "reasons": reasons,
        })
    return output


def build(paths, spatial_paths=(), **kwargs):
    cutouts = {}
    for spatial_path in spatial_paths:
        spatial = json.loads(spatial_path.read_text())
        for job in spatial.get("jobs", []):
            cutouts[(job["event_id"], job["revision"])] = job["cutout"]
    rows = []
    inputs = []
    for path in paths:
        manifest_rows = audit_manifest(path, **kwargs)
        inputs.append({"path": str(path), "sha256": sha256(path), "rows": len(manifest_rows)})
        for row in manifest_rows:
            row["source_manifest"] = path.name
            cutout = cutouts.get((row["event_id"], row["revision"]))
            row["declared_spatial_cutout"] = cutout
            row["fits_declared_spatial_cutout"] = bool(cutout) and (
                row["required_delay_radius_bins"] is not None and
                row["required_doppler_radius_hz"] is not None and
                row["required_delay_radius_bins"] <= cutout["delay_radius_bins"] and
                # Existing cutout preparation used a 4 Hz output-bin spacing.
                row["required_doppler_radius_hz"] <= 4 * cutout["doppler_radius_bins"])
        rows.extend(manifest_rows)
    counts = Counter()
    for row in rows:
        counts["rows"] += 1
        counts["unique_recordings"] = len({item["recording"] for item in rows})
        counts["quarantined_rows"] += row["quarantined"]
        counts["outside_fixed_search_rows"] += row["outside_fixed_search"]
        counts["outside_requested_processing_rows"] += row["outside_requested_64km_800hz_processing"]
        counts["source_observation_predates_recording_rows"] += row["source_observation_predates_recording"]
        counts["rows_with_declared_spatial_cutout"] += row["declared_spatial_cutout"] is not None
        counts["rows_fitting_declared_spatial_cutout"] += row["fits_declared_spatial_cutout"]
        for reason in row["reasons"]: counts[f"reason:{reason}"] += 1
    return {
        "schema": "vectorwarp-adsb-replay-quarantine-v1", "replay_only": True,
        "live_rf_or_config_changed": False, "recordings_deleted": False,
        "inputs": inputs,
        "cutout_inventory": {"manifests": [str(path) for path in spatial_paths],
            "note": "These are requested cutout extents. Replay JSON cutouts were deleted by the runner after scoring, so retained result files do not preserve actual complex cutout cells."},
        "assumptions": {"sample_rate_hz": kwargs.get("sample_rate_hz", 2_400_000.0),
            "cpi_s": kwargs.get("cpi_s", .5), "delay_bin_km": C_MPS / kwargs.get("sample_rate_hz", 2_400_000.0) / 1000,
            "doppler_bin_hz": 1 / kwargs.get("cpi_s", .5),
            "aircraft_acceleration_bound_m_s2": kwargs.get("acceleration_m_s2", 10.0),
            "aircraft_speed_bound_m_s": kwargs.get("speed_bound_m_s", 350.0),
            "fixed_search": {"delay_radius_bins": 4, "doppler_radius_hz": 20.0},
            "requested_replay_processing": {"delay_km": [0, 64], "doppler_hz": [-800, 800]},
            "note": "Raw MCHQ contains the CPI samples; 64 km/800 Hz are requested replay-processing limits, not RF recording limits."},
        "fresh_only_acceptance": {
            "max_raw_observation_age_ms": kwargs.get("fresh_age_ms", 3000),
            "requirements": [
                "event-time timestamp for every source observation",
                "at least two independent sources at every held CPI",
                "position and velocity projected from each source event time to the CPI midpoint",
                "projection method, source identifiers, source event times, raw ages, and covariance retained",
                "maximum pairwise projected horizontal disagreement <= 0.5 km",
                "maximum pairwise projected velocity disagreement <= 10 m/s",
                "uncertainty tube fits both the predeclared search and retained cutout on every CPI",
            ],
            "validation_rule": "Select/rank every tube cell; apply the tube-size trial factor only in validation against identically maximized translated controls."
        },
        "counts": dict(counts), "rows": rows,
    }


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--manifests", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--spatial-manifests", type=Path)
    args = parser.parse_args()
    paths = sorted(args.manifests.glob("*manifest.adsb-reconciled-v2.json"))
    spatial_paths = (sorted(args.spatial_manifests.glob("*spatial-manifest.json"))
                     if args.spatial_manifests else [])
    result = build(paths, spatial_paths=spatial_paths)
    args.output.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n")
    print(json.dumps(result["counts"], indent=2, sort_keys=True))


if __name__ == "__main__":
    main()
