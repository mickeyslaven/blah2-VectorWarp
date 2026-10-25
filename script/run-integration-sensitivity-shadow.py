#!/usr/bin/env python3
"""Run bounded same-IQ dwell/fusion comparisons outside the live processor.

The job is emitted only after strict recorder admission.  Every arm consumes
the same immutable recording and is scored against event-time truth after the
detector decision.  This executable never writes the live configuration and
records its own wall time so an experiment cannot hide its compute cost.
"""

import argparse
import hashlib
import hmac
import importlib.util
import json
import math
import os
import socket
import struct
from pathlib import Path
import tempfile
import time


ROOT = Path(__file__).resolve().parents[1]
REPLAY_SPEC = importlib.util.spec_from_file_location(
    "fusion_replay", ROOT / "script" / "replay-fusion-ab-cass.py")
REPLAY = importlib.util.module_from_spec(REPLAY_SPEC)
REPLAY_SPEC.loader.exec_module(REPLAY)
PHASE_SPEC = importlib.util.spec_from_file_location(
    "phase_locked", ROOT / "script" / "analyze-phase-locked-migration.py")
PHASE = importlib.util.module_from_spec(PHASE_SPEC)
PHASE_SPEC.loader.exec_module(PHASE)
C_KM_S = 299792.458


def canonical(value):
    if isinstance(value, bool) or value is None or isinstance(value, str):
        return json.dumps(value, ensure_ascii=False, separators=(",", ":"))
    if isinstance(value, (int, float)):
        number = float(value)
        if not math.isfinite(number):
            raise ValueError("canonical shadow receipts require finite numbers")
        return '{"$f64":"' + struct.pack(">d", number).hex() + '"}'
    if isinstance(value, list):
        return "[" + ",".join(canonical(item) for item in value) + "]"
    if isinstance(value, dict):
        return "{" + ",".join(json.dumps(key, ensure_ascii=False) + ":" + canonical(value[key])
                              for key in sorted(value)) + "}"
    raise TypeError(f"unsupported canonical value {type(value).__name__}")


def canonical_digest(value):
    return hashlib.sha256(canonical(value).encode("utf-8")).hexdigest()


def load_receipt_key(path):
    stat = path.lstat()
    if path.is_symlink() or not path.is_file() or stat.st_uid != os.geteuid() or stat.st_mode & 0o077:
        raise ValueError("receipt key must be an owner-only regular file")
    key = path.read_bytes()
    if len(key) < 32:
        raise ValueError("receipt key must contain at least 32 bytes")
    return key


def digest(path):
    value = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            value.update(block)
    return value.hexdigest()


def interpolate(samples, timestamp_ms, key):
    ordered = sorted(samples, key=lambda row: row["timestamp_ms"])
    left = max((row for row in ordered if row["timestamp_ms"] <= timestamp_ms),
               key=lambda row: row["timestamp_ms"], default=ordered[0])
    right = min((row for row in ordered if row["timestamp_ms"] >= timestamp_ms),
                key=lambda row: row["timestamp_ms"], default=ordered[-1])
    if left["timestamp_ms"] == right["timestamp_ms"]:
        return left[key]
    fraction = ((timestamp_ms - left["timestamp_ms"]) /
                (right["timestamp_ms"] - left["timestamp_ms"]))
    return left[key] + fraction * (right[key] - left[key])


def inside(delay, doppler, truth, sample_rate, cpi, track=False, uncertainty_s=0, rates=(0, 0)):
    delay_gate = max(C_KM_S / sample_rate * (3 if track else 2), .25)
    doppler_gate = (4 if track else 3) / cpi
    delay_gate += abs(rates[0]) * uncertainty_s
    doppler_gate += abs(rates[1]) * uncertainty_s
    return ((delay - truth[0]) / delay_gate) ** 2 + ((doppler - truth[1]) / doppler_gate) ** 2 <= 1


def longest_run(values):
    best = current = 0
    for value in values:
        current = current + 1 if value else 0
        best = max(best, current)
    return best


def score_arm(arm, truth_samples, sample_rate, cpi, uncertainty_s=0):
    detected, nuisance_frames, nuisance_cells = [], [], 0
    margins = []
    truth_start = min(row["timestamp_ms"] for row in truth_samples)
    truth_end = max(row["timestamp_ms"] for row in truth_samples)
    for frame in arm["detections"]:
        timestamp = frame.get("timestamp")
        if not isinstance(timestamp, (int, float)) or not truth_start <= timestamp <= truth_end:
            continue
        truth = (interpolate(truth_samples, timestamp, "delay_km"),
                 interpolate(truth_samples, timestamp, "doppler_hz"))
        rates = tuple(interpolate(truth_samples, timestamp, key) for key in
                      ("delay_rate_km_s", "doppler_rate_hz_s")) if uncertainty_s else (0, 0)
        vectors = [frame.get("delay", []), frame.get("doppler", []), frame.get("snr", [])]
        if any(not isinstance(values, list) for values in vectors) or len({len(v) for v in vectors}) != 1:
            raise ValueError("detector output vectors have inconsistent lengths")
        cells = list(zip(*vectors))
        candidates = [snr for delay, doppler, snr in cells
                      if inside(delay, doppler, truth, sample_rate, cpi,
                                uncertainty_s=uncertainty_s, rates=rates)]
        nuisance = len(cells) - len(candidates)
        nuisance_cells += nuisance; nuisance_frames.append(nuisance > 0)
        detected.append(bool(candidates))
        if candidates:
            margins.append(max(candidates))
    tracked, track_ids = [], set()
    for frame in arm["tracks"]:
        timestamp = frame.get("timestamp")
        if not isinstance(timestamp, (int, float)) or not truth_start <= timestamp <= truth_end:
            continue
        truth = (interpolate(truth_samples, timestamp, "delay_km"),
                 interpolate(truth_samples, timestamp, "doppler_hz"))
        rates = tuple(interpolate(truth_samples, timestamp, key) for key in
                      ("delay_rate_km_s", "doppler_rate_hz_s")) if uncertainty_s else (0, 0)
        matches = [track for track in frame.get("data", []) if inside(
            track["delay"] * C_KM_S / sample_rate, track["doppler"], truth,
            sample_rate, cpi, True, uncertainty_s, rates)]
        tracked.append(bool(matches))
        track_ids.update(str(item["id"]) for item in matches)
    detection_frames = len(detected)
    track_frames = len(tracked)
    return {"frames": detection_frames, "target_detection_cpis": sum(detected),
            "target_detection_fraction": sum(detected) / detection_frames if detection_frames else None,
            "evaluated_detection_seconds": detection_frames * cpi,
            "target_detection_longest_run": longest_run(detected),
            "nuisance_detection_cpis": sum(nuisance_frames),
            "nuisance_detection_cells": nuisance_cells,
            "track_frames": track_frames, "target_track_cpis": sum(tracked),
            "target_track_fraction": sum(tracked) / track_frames if track_frames else None,
            "evaluated_track_seconds": track_frames * cpi,
            "target_track_ids": sorted(track_ids),
            "target_snr_db_peak": max(margins) if margins else None,
            "all_detection_cells": sum(len(frame.get("delay", []))
                                       for frame in arm["detections"]),
            "all_published_track_ids": len({str(track["id"]) for frame in arm["tracks"]
                                            for track in frame.get("data", [])}),
            "event_time_uncertainty_s": uncertainty_s,
            "nuisance_published_track_ids": len({str(track["id"]) for frame in arm["tracks"]
                                                  for track in frame.get("data", [])} - track_ids)}


def qualified_capture_timing(value):
    cpis = value.get("processor_cpis", [])
    if len(cpis) < 3:
        return False
    for cpi in cpis:
        if cpi.get("exact_event_time") is True:
            continue
        uncertainty = cpi.get("event_time_uncertainty_s")
        if cpi.get("exact_event_time") is not False or \
           not cpi.get("event_time_source") or cpi["event_time_source"] == "unknown" or \
           cpi.get("event_time_uncertainty_model") != "bounded-configured-nuisance-radius" or \
           not isinstance(uncertainty, (float, int)) or not 0 <= uncertainty <= 5:
            return False
        truth = value.get("truth_samples", [])
        if len(truth) < 3 or any(not isinstance(row.get(key), (float, int)) or
            not math.isfinite(row[key]) for row in truth
            for key in ("delay_rate_km_s", "doppler_rate_hz_s")):
            return False
    return True


def valid_captures(admission):
    if admission.get("state") != "admitted" or admission.get("integrity", {}).get("valid") is not True:
        raise ValueError("integration shadow requires a strictly admitted recording")
    candidates = [(capture_id, value) for capture_id, value in
                  admission.get("captures", {}).items()
                  if value.get("valid") is True and value.get("admission_state") == "admitted"
                  and qualified_capture_timing(value)]
    if not candidates:
        raise ValueError("admission has no complete three-CPI truth trajectory")
    return candidates


def integration_diagnostic(cutouts, config, truth_samples):
    """Describe fixed-cell, trajectory, and phase-corrected accumulation."""
    if len(cutouts) < 2:
        raise ValueError("integration diagnostic needs at least two complex CPI cutouts")
    pilots, phase_source = PHASE.pilot_series(cutouts)
    phase_coherence = PHASE.phase_increment_coherence(pilots)
    trajectory = [(interpolate(truth_samples, frame["timestamp"], "delay_km"),
                   interpolate(truth_samples, frame["timestamp"], "doppler_hz"))
                  for frame in cutouts]
    middle = trajectory[len(trajectory) // 2]
    beam_count = int(config["process"]["surveillance_fusion"]["beams"])
    fixed = [[] for _ in range(beam_count)]
    moving = [[] for _ in range(beam_count)]
    for frame, truth, pilot in zip(cutouts, trajectory, pilots):
        fixed_row, fixed_column = PHASE.nearest(frame["doppler_hz"], middle[1]), PHASE.nearest(
            frame["delay_km"], middle[0])
        moving_row, moving_column = PHASE.nearest(frame["doppler_hz"], truth[1]), PHASE.nearest(
            frame["delay_km"], truth[0])
        for beam, value in enumerate(PHASE.beam_values(frame, config, fixed_row, fixed_column)):
            fixed[beam].append(value)
        for beam, value in enumerate(PHASE.beam_values(frame, config, moving_row, moving_column)):
            moving[beam].append(value * pilot.conjugate())

    def noncoherent(values):
        return sum(abs(value) ** 2 for value in values)

    fixed_power = max(noncoherent(values) for values in fixed)
    trajectory_power = max(noncoherent(values) for values in moving)
    coherent_power = max(abs(sum(values)) ** 2 / len(values) for values in moving)
    coherence_ratio = max(abs(sum(values)) / max(1e-30, sum(abs(value) for value in values))
                          for values in moving)
    return {"frames": len(cutouts), "phase_source": phase_source,
            "phase_increment_coherence": phase_coherence,
            "fixed_cell_noncoherent_power": fixed_power,
            "trajectory_noncoherent_power": trajectory_power,
            "trajectory_coherent_power": coherent_power,
            "trajectory_over_fixed_db": 10 * math.log10(max(trajectory_power, 1e-300) /
                                                          max(fixed_power, 1e-300)),
            "coherent_over_trajectory_noncoherent_db": 10 * math.log10(
                max(coherent_power, 1e-300) / max(trajectory_power, 1e-300)),
            "coherent_phasor_concentration": coherence_ratio,
            "limitation": "reference_weight_proxy is diagnostic until native cutouts export a direct/pilot phasor"
            if phase_source == "reference_weight_proxy" else None}


def arm_plan(config, requested):
    production = config["process"]["surveillance_fusion"]["mode"]
    plan = []
    for name in requested:
        if name == "production":
            plan.append(("production", production, None))
        elif name == "physical_channels":
            channels = int(config["capture"]["device"].get("channel_count", 2))
            plan.extend((f"physical_ch{channel}", "noncoherent", channel)
                        for channel in range(channels))
        else:
            plan.append((name, name, None))
    seen, unique = set(), []
    for item in plan:
        if item[0] not in seen:
            seen.add(item[0]); unique.append(item)
    return unique


def validate_job(job):
    if job.get("schema") != "vectorwarp-integration-shadow-job-v1":
        raise ValueError("unsupported integration shadow job schema")
    binding = job.get("job_binding_sha256")
    unsigned = dict(job); unsigned.pop("job_binding_sha256", None)
    if not isinstance(binding, str) or canonical_digest(unsigned) != binding:
        raise ValueError("integration shadow job binding mismatch")
    identity = job.get("identity", {})
    if job.get("job_id") != canonical_digest(identity):
        raise ValueError("integration shadow job identity mismatch")
    experiment = job.get("experiment", {})
    modes = experiment.get("fusion_modes", [])
    if experiment.get("same_iq") is not True or experiment.get("shadow_only") is not True or \
       experiment.get("require_production_arm") is not True or modes.count("production") != 1:
        raise ValueError("integration shadow requires exactly one production baseline")
    if experiment.get("receipt_key_id") != identity.get("receipt_key_id") or \
       not isinstance(identity.get("receipt_key_id"), str) or len(identity["receipt_key_id"]) != 64:
        raise ValueError("integration shadow receipt-key identity mismatch")
    if experiment.get("physical_channel_reference_policy") != "leave-one-out":
        raise ValueError("unsupported physical-channel reference policy")
    if experiment.get("execution_host_policy") != "strix-only":
        raise ValueError("integration shadow heavy replay is restricted to Strix")
    return identity, experiment


def run(job, config_path, binary, harness_path, recording=None, admission=None,
        acceleration="gpu-resident", receipt_key=None):
    identity, experiment = validate_job(job)
    config_bytes = config_path.read_bytes()
    config_file_sha = hashlib.sha256(config_bytes).hexdigest()
    config = REPLAY.load_config(config_path)
    config_sha = canonical_digest(config)
    if config_sha != identity.get("config_sha256"):
        raise ValueError("worker configuration does not match the queued configuration")
    admission_path = Path(admission or job["admission_sidecar"])
    recording_path = Path(recording or job["recording"])
    admission = json.loads(admission_path.read_text())
    admission_sha = canonical_digest(admission)
    if admission_sha != identity.get("admission_sha256"):
        raise ValueError("admission sidecar does not match the queued immutable receipt")
    if admission.get("schema") != "vectorwarp-capture-admission-v1" or \
       admission.get("revision") != identity.get("revision") or \
       admission.get("config_epoch") != identity.get("config_epoch") or \
       admission.get("receiver_epoch") != identity.get("receiver_epoch"):
        raise ValueError("admission identity does not match the queued capture epochs")
    captures = valid_captures(admission)
    exact_timing = all(cpi.get("exact_event_time") is True
                       for _, capture in captures for cpi in capture["processor_cpis"])
    timing_policy = "exact" if exact_timing else "provisional"
    if job.get("timing_policy") != timing_policy or job.get("decision_grade") is not exact_timing:
        raise ValueError("job timing policy differs from admitted CPI provenance")
    uncertainty_by_capture = {capture_id: max(cpi.get("event_time_uncertainty_s", 0)
        for cpi in capture["processor_cpis"]) for capture_id, capture in captures}
    primary_capture_id, primary_capture = max(
        captures, key=lambda item: len(item[1]["processor_cpis"]))
    recording_sha = digest(recording_path)
    expected_sha = identity.get("recording_sha256")
    if not isinstance(expected_sha, str) or len(expected_sha) != 64 or recording_sha != expected_sha:
        raise ValueError("staged recording does not match the admitted SHA-256")
    if admission.get("integrity", {}).get("data_sha256") != recording_sha:
        raise ValueError("admission receipt does not bind the staged recording SHA-256")
    native_sidecar = Path(f"{recording_path}.json")
    expected_native_sha = admission.get("integrity", {}).get("sidecar_sha256")
    if expected_native_sha != identity.get("native_sidecar_sha256"):
        raise ValueError("admission native-metadata digest does not match the queued identity")
    if not native_sidecar.is_file() or digest(native_sidecar) != expected_native_sha:
        raise ValueError("native recording metadata does not match its admitted digest")
    native_metadata = json.loads(native_sidecar.read_text())
    if not exact_timing:
        for _, capture in captures:
            for cpi in capture["processor_cpis"]:
                if cpi.get("epoch") != native_metadata.get("epoch") or not (
                    isinstance(cpi.get("first_epoch_sample"), int) and
                    isinstance(cpi.get("end_epoch_sample"), int) and
                    native_metadata.get("firstEpochSample", math.inf) <= cpi["first_epoch_sample"] <
                    cpi["end_epoch_sample"] <= native_metadata.get("endEpochSample", -math.inf)):
                    raise ValueError("provisional CPI is not contained in native recording")
                if not isinstance(cpi.get("max_raw_observation_offset_s"), (int, float)) or \
                   not 0 <= cpi["max_raw_observation_offset_s"] <= 1 or \
                   not isinstance(cpi.get("truth_bracket_span_ms"), (int, float)) or \
                   not 0 <= cpi["truth_bracket_span_ms"] <= 3000:
                    raise ValueError("provisional CPI lacks fresh projected truth")
    channel_count = int(config["capture"]["device"].get("channel_count", 2))
    gains = native_metadata.get("channelGains")
    if native_metadata.get("schema") != "vectorwarp-kraken-iq-clip-v1" or \
       native_metadata.get("frequency") != config["capture"]["fc"] or \
       native_metadata.get("sampleRate") != config["capture"]["fs"] or \
       native_metadata.get("channels") != channel_count or \
       not isinstance(gains, list) or len(gains) != channel_count or \
       any(not isinstance(value, (int, float)) or not math.isfinite(value) for value in gains):
        raise ValueError("native recording RF/gain identity does not match queued configuration")
    native_identity = {"frequency_hz": native_metadata["frequency"],
                       "sample_rate_hz": native_metadata["sampleRate"],
                       "channels": channel_count,
                       "actual_channel_gains_db": gains}
    if (identity.get("native_identity") != native_identity or
            identity.get("native_identity_sha256") != canonical_digest(native_identity)):
        raise ValueError("native recording RF/gain identity does not match queued receipt")
    truth_by_capture = {capture_id: value["truth_samples"]
                        for capture_id, value in captures}
    truth = primary_capture["truth_samples"]
    sample_rate = float(config["capture"]["fs"])
    deadline = time.monotonic() + float(experiment["max_wall_seconds_per_clip"])
    harness = REPLAY.load_harness(harness_path)
    arms, truth_selected_exports, started = {}, [], time.monotonic()
    for dwell in experiment["dwell_seconds"]:
        dwell_key = f"{float(dwell):g}s"
        arms[dwell_key] = {}
        diagnostic_emitted = False
        for label, mode, physical in arm_plan(config, experiment["fusion_modes"]):
            remaining = deadline - time.monotonic()
            if remaining <= 1:
                raise TimeoutError("integration shadow exhausted its per-clip wall budget")
            source = json.loads(json.dumps(config))
            source["process"]["data"]["cpi"] = float(dwell)
            arm_started = time.monotonic()
            replay = REPLAY.run_arm(harness, binary, source, recording_path, mode,
                                    min(remaining, 900), label=f"{dwell_key}-{label}",
                                    acceleration=acceleration, surveillance_channel=physical,
                                    complex_cutout=None)
            elapsed = time.monotonic() - arm_started
            target_scores = {capture_id: score_arm(replay, target_truth, sample_rate, float(dwell),
                                                  uncertainty_by_capture[capture_id])
                             for capture_id, target_truth in truth_by_capture.items()}
            output_cpis = len(replay["detections"])
            arms[dwell_key][label] = {"timing": {"wall_seconds": elapsed,
                "output_cpis": output_cpis,
                "wall_ms_per_output_cpi": elapsed * 1000 / output_cpis if output_cpis else None,
                "live_processor_work_added_ms": 0},
                "targets": target_scores}
            # A geometry-eligible truth pass gets a bounded native complex
            # record even when CA-CFAR rejects it.  The truth coordinate may
            # select a CUT but may not select a beam, a detection, or an arm.
            # This is a separate replay process and never feeds live output.
            if not diagnostic_emitted and label == "production":
                input_fingerprint = canonical_digest({"recording_sha256": recording_sha,
                    "admission_sha256": admission_sha, "config_sha256": config_sha,
                    "native_identity_sha256": identity["native_identity_sha256"]})
                for capture_id, capture_truth in truth_by_capture.items():
                    centre_time = sorted(row["timestamp_ms"] for row in capture_truth)[len(capture_truth) // 2]
                    cutout_request = {"delay_km": interpolate(capture_truth, centre_time, "delay_km"),
                        "doppler_hz": interpolate(capture_truth, centre_time, "doppler_hz"),
                        "delay_radius_bins": 16, "doppler_radius_bins": 24, "max_frames": 32}
                    remaining = deadline - time.monotonic()
                    if remaining <= 1:
                        raise TimeoutError("integration shadow exhausted its per-clip wall budget")
                    diagnostic_replay = REPLAY.run_arm(
                        harness, binary, source, recording_path, mode, min(remaining, 900),
                        label=f"{dwell_key}-{label}-truth-{capture_id}", acceleration=acceleration,
                        surveillance_channel=physical, complex_cutout=cutout_request)
                    truth_selected_exports.append(truth_selected_export(
                        diagnostic_replay["complex_cutouts"], source, capture_truth, capture_id,
                        f"{dwell_key}-{label}", job["job_binding_sha256"], input_fingerprint))
                    if capture_id == primary_capture_id:
                        arms[dwell_key][label]["integration"] = integration_diagnostic(
                            diagnostic_replay["complex_cutouts"], source, capture_truth)
                        arms[dwell_key][label]["integration"].update({
                            "truth_conditioned": True,
                            "selection_basis": "geometry-eligible-truth-cut-only",
                            "excluded_from_arm_scoring": True})
                diagnostic_emitted = True
    wall = time.monotonic() - started
    if hashlib.sha256(config_path.read_bytes()).hexdigest() != config_file_sha:
        raise RuntimeError("configuration changed while the shadow was running")
    result = {"schema": "vectorwarp-integration-shadow-result-v1", "job_id": job["job_id"],
            "job_binding_sha256": job["job_binding_sha256"],
            "state": "complete", "completed_at": time.time(), "shadow_only": True,
            "timing_policy": timing_policy, "decision_grade": exact_timing,
            "production_promotion_allowed": False,
            "authoritative_output": False, "production_unchanged": True,
            "input": {"recording": str(recording_path), "recording_sha256": recording_sha,
                      "admission": str(admission_path), "admission_sha256": admission_sha,
                      "native_sidecar_sha256": expected_native_sha,
                      "applied_channel_gains_db": gains,
                      "native_identity": native_identity,
                      "native_identity_sha256": canonical_digest(native_identity),
                      "config_sha256": config_sha, "config_file_sha256": config_file_sha,
                      "config_epoch": identity.get("config_epoch"),
                      "receiver_epoch": identity.get("receiver_epoch"),
                      "producer_host": identity.get("producer_host"),
                      "worker_host": socket.gethostname(),
                      "worker_acceleration": acceleration},
            "timing": {"wall_seconds": wall, "configured_limit_seconds":
                       experiment["max_wall_seconds_per_clip"],
                       "live_processor_work_added_ms": 0},
            "trial_accounting": {"configured_pfa": config["process"]["detection"]["pfa"],
                "configured_effective_beam_trials": config["process"]["surveillance_fusion"].get(
                    "effective_beam_trials", config["process"]["surveillance_fusion"].get("beams", 1)),
                "beam_count": config["process"]["surveillance_fusion"].get("beams", 1),
                "delay_bins": config["process"]["ambiguity"]["delayMax"] -
                    config["process"]["ambiguity"]["delayMin"] + 1,
                "doppler_span_hz": config["process"]["ambiguity"]["dopplerMax"] -
                    config["process"]["ambiguity"]["dopplerMin"],
                "empirical_familywise_acceptance": False,
                "reason": "requires a frozen target-free multi-clip holdout, never one aircraft clip"},
            "arms": arms,
            "truth_selected_exports": truth_selected_exports,
            "summary": {"winner": None, "selection_performed": False,
                        "selection_reason": "requires normalized pass-disjoint holdout scoring",
                        "comparison_unit": "same immutable IQ; fractions include explicit denominators",
                        "target_capture_count": len(captures),
                        "integration_diagnostic_target_capture_id": primary_capture_id}}
    result["result_binding_sha256"] = canonical_digest(result)
    if receipt_key is not None:
        key_id = hashlib.sha256(receipt_key).hexdigest()
        if key_id != experiment.get("receipt_key_id"):
            raise ValueError("worker receipt key does not match the queued verifier identity")
        result["result_authentication"] = {"algorithm": "hmac-sha256", "key_id": key_id,
            "tag": hmac.new(receipt_key, canonical(result).encode("utf-8"), hashlib.sha256).hexdigest()}
    return result


def _complex_pair(value):
    """JSON-safe complex value used only by the shadow evidence envelope."""
    if not isinstance(value, complex) or not (math.isfinite(value.real) and
                                              math.isfinite(value.imag)):
        raise ValueError("truth-selected export contains a non-finite complex sample")
    return [value.real, value.imag]


def truth_selected_export(cutouts, config, truth_samples, capture_id, arm_label,
                          job_binding_sha256, input_fingerprint):
    """Export bounded, pre-decision replay evidence for an eligible truth pass.

    This deliberately does *not* consume a detection, CFAR result, or winning
    beam.  The target coordinate selects a CUT; every physical channel and all
    calibrated beam responses at that same CUT are retained, together with the
    surrounding complex nuisance cells.  The replay recorder itself is native
    and replay-only; this envelope gives its bounded output a stable contract.
    """
    if len(cutouts) < 3:
        raise ValueError("truth-selected export requires a three-CPI native history")
    frames, history = [], []
    covariance = None
    for frame in cutouts:
        timestamp = frame.get("timestamp")
        channels = frame.get("channels")
        if not isinstance(timestamp, (int, float)) or not isinstance(channels, list) or not channels:
            raise ValueError("truth-selected export native cutout is incomplete")
        truth = {"delay_km": interpolate(truth_samples, timestamp, "delay_km"),
                 "doppler_hz": interpolate(truth_samples, timestamp, "doppler_hz")}
        row = PHASE.nearest(frame.get("doppler_hz", []), truth["doppler_hz"])
        column = PHASE.nearest(frame.get("delay_km", []), truth["delay_km"])
        if any(not isinstance(channel, list) or row >= len(channel) or
               not isinstance(channel[row], list) or column >= len(channel[row])
               for channel in channels):
            raise ValueError("truth-selected export channel CUT is incomplete")
        physical = [complex(*channel[row][column]) for channel in channels]
        beams = PHASE.beam_values(frame, config, row, column)
        nuisance = []
        for nuisance_row, doppler in enumerate(frame["doppler_hz"]):
            for nuisance_column, delay in enumerate(frame["delay_km"]):
                if (nuisance_row, nuisance_column) == (row, column):
                    continue
                nuisance.append({"delay_km": delay, "doppler_hz": doppler,
                    "physical_channels": [_complex_pair(complex(*channel[nuisance_row][nuisance_column]))
                                          for channel in channels]})
        frames.append({"timestamp_ms": timestamp, "truth": truth,
                       "target_cut": {"physical_channels": [_complex_pair(value) for value in physical],
                                      "beam_responses": [_complex_pair(value) for value in beams]},
                       "nuisance_cells": nuisance})
        history.append(physical)
    channels = len(history[0])
    if any(len(row) != channels for row in history):
        raise ValueError("truth-selected export physical channel count changed")
    covariance = [[_complex_pair(sum(row[left] * row[right].conjugate() for row in history) /
                                  len(history))
                   for right in range(channels)] for left in range(channels)]
    whitening = {"mode": config["process"]["surveillance_fusion"]["mode"],
                 "covariance": covariance,
                 "state": "shadow-observed-pre-decision",
                 "decision_grade": False}
    value = {"schema": "vectorwarp-truth-selected-shadow-export-v1",
             "shadow_only": True, "decision_grade": False,
             "production_promotion_allowed": False, "production_unchanged": True,
             "selection": {"basis": "geometry-eligible-truth-cut-only",
                           "cfar_or_beam_selection_used": False,
                           "capture_id": capture_id, "arm": arm_label},
             "provenance": {"native_recorder": "vectorwarp-complex-caf-cutout-v1",
                            "job_binding_sha256": job_binding_sha256,
                            "input_fingerprint": input_fingerprint},
             "multi_cpi_history": frames, "covariance_whitening": whitening}
    value["export_sha256"] = canonical_digest(value)
    return value


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--job", required=True, type=Path)
    parser.add_argument("--config", required=True, type=Path)
    parser.add_argument("--binary", required=True, type=Path)
    parser.add_argument("--harness", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--recording", type=Path,
                        help="Local staged path for the immutable recording")
    parser.add_argument("--admission", type=Path,
                        help="Local staged path for the admission sidecar")
    parser.add_argument("--acceleration", choices=("cpu", "gpu-resident"),
                        default="gpu-resident")
    parser.add_argument("--receipt-key", required=True, type=Path,
                        help="Owner-only key shared with the live receipt verifier")
    parser.add_argument("--allow-same-host-for-test", action="store_true",
                        help="Test-only override; production shadows must run off the live receiver")
    args = parser.parse_args()
    job = json.loads(args.job.read_text())
    validate_job(job)
    claim = Path(f"{args.job}.claim")
    if claim.exists():
        try:
            previous = json.loads(claim.read_text())
            same_host = previous.get("host") == socket.gethostname()
            pid = int(previous.get("pid", -1))
            try:
                if pid <= 1: raise ProcessLookupError
                os.kill(pid, 0); alive = True
            except (ProcessLookupError, ValueError): alive = False
            except PermissionError: alive = True
            stale_after = float(job["experiment"]["max_wall_seconds_per_clip"]) + 300
            stale = time.time() - float(previous.get("started_at", time.time())) > stale_after
            if same_host and not alive and stale and \
               previous.get("job_binding_sha256") == job.get("job_binding_sha256"):
                claim.unlink()
        except (OSError, ValueError, TypeError, json.JSONDecodeError):
            pass
    claim_fd = os.open(claim, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    try:
        claim_receipt = {"schema": "vectorwarp-integration-shadow-claim-v1",
                         "pid": os.getpid(), "host": socket.gethostname(),
                         "started_at": time.time(),
                         "job_binding_sha256": job["job_binding_sha256"]}
        os.write(claim_fd, (json.dumps(claim_receipt) + "\n").encode())
        os.close(claim_fd); claim_fd = None
        if args.output.exists():
            raise FileExistsError("integration shadow result already exists")
        if not args.allow_same_host_for_test and \
           job.get("identity", {}).get("producer_host") == socket.gethostname():
            raise RuntimeError("integration shadow worker must not run on the live producer host")
        if not args.allow_same_host_for_test and "strix" not in socket.gethostname().lower():
            raise RuntimeError("integration shadow heavy replay must run on Strix")
        receipt_key = load_receipt_key(args.receipt_key.resolve())
        result = run(job, args.config.resolve(), args.binary.resolve(), args.harness.resolve(),
                     args.recording.resolve() if args.recording else None,
                     args.admission.resolve() if args.admission else None, args.acceleration,
                     receipt_key)
        args.output.parent.mkdir(parents=True, exist_ok=True)
        with tempfile.NamedTemporaryFile("w", dir=args.output.parent, delete=False) as output:
            json.dump(result, output, indent=2); output.write("\n")
            temporary = output.name
        os.link(temporary, args.output)
        os.unlink(temporary)
    finally:
        if claim_fd is not None: os.close(claim_fd)
        claim.unlink(missing_ok=True)
    print(json.dumps({"job_id": result["job_id"], "state": result["state"],
                      "wall_seconds": result["timing"]["wall_seconds"],
                      "winner": result["summary"]["winner"]}))


if __name__ == "__main__":
    main()
