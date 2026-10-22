#!/usr/bin/env python3
"""Run the three frozen spatial/adaptive hypotheses on one complex cutout."""

import argparse
import concurrent.futures
import importlib.util
import json
import math
import multiprocessing
from pathlib import Path


def load(name):
    path = Path(__file__).with_name(name)
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


SPATIAL = load("analyze-spatial-glrt.py")
STAP = load("analyze-post-doppler-stap.py")
SPARSE = load("analyze-sparse-beam-recovery.py")
PHASE = load("analyze-phase-locked-migration.py")
METHODS = ("spatial_glrt", "post_doppler_stap", "sparse_beam")


def compact(result):
    return {key: result[key] for key in (
        "frames", "target_score", "margin_over_control_p99_db",
        "controls_exceeding_target", "control_quantile", "target_fixed_beam",
        "target_beam_votes") if key in result} | {
        "control_p99": result.get("control_p99", result.get("strict_control_p99")),
        "controls": result.get("controls", result.get("trial_accounting", {}).get(
            "whole_search_control_paths")),
    }


def run_method(method, cutouts, config, trajectory, fraction):
    if method == "spatial_glrt":
        return SPATIAL.analyze_trajectory(cutouts, config, trajectory, guard=1,
            training=3, loading=.01, min_doppler=0, beam_policy="fixed",
            control_fraction=fraction)
    if method == "post_doppler_stap":
        return STAP.evaluate(cutouts, config, trajectory,
            {"doppler_radius": 1, "diagonal_loading": .1, "robust_energy_clip": 3.0},
            min_doppler=0, control_fraction=fraction)
    if method == "sparse_beam":
        return SPARSE.analyze(cutouts, config, trajectory, .5, guard=2,
            training=5, loading=.3, control_fraction=fraction)
    raise ValueError(method)


def calibrated_vector(method, treatment, result, job, beams):
    controls = result["control_score_vector"]
    values = [item["raw_score"] for item in controls]

    def record(kind, raw_score, row_offset, column_offset, leave_one_out=False):
        exceedances = sum(value >= raw_score for value in values)
        denominator = len(values) + 1
        if leave_one_out:
            exceedances -= 1
            denominator = len(values)
        probability = (1 + exceedances) / denominator
        return {"kind": kind, "doppler_offset_bins": row_offset,
                "delay_offset_bins": column_offset, "raw_score": raw_score,
                "empirical_survival_p": probability,
                "evidence_log10": -math.log10(probability)}

    return {
        "schema": "vectorwarp-calibrated-trajectory-score-vector-v1",
        "method_id": method, "gain_treatment": treatment, "job_id": job["job_id"],
        "event_id": job["event_id"], "recording": job["recording"],
        "trial_family_count": job["family_trials"],
        "parameter_grid_choices": 1, "internal_search_trials_per_path": beams,
        "target": record("target", result["target_score"], 0, 0),
        "controls": [record("control", item["raw_score"],
                            item["doppler_offset_bins"], item["delay_offset_bins"], True)
                     for item in controls],
    }


def gain_equalized(cutouts, gains):
    common = sum(gains) / len(gains)
    scales = [10 ** ((common - gain) / 20) for gain in gains]
    output = []
    for frame in cutouts:
        changed = dict(frame)
        changed["channels"] = [
            [[[value[0] * scale, value[1] * scale] for value in row]
             for row in channel]
            for channel, scale in zip(frame["channels"], scales)]
        # Express both observations and the measured reference manifold in the
        # same common antenna-voltage coordinates after x' = scale*x.
        changed["reference_weights"] = [
            [value[0] * scale, value[1] * scale]
            for value, scale in zip(frame["reference_weights"], scales)]
        output.append(changed)
    return output, scales


def channel_rms(cutouts):
    sums = [0.0] * len(cutouts[0]["channels"])
    counts = [0] * len(sums)
    for frame in cutouts:
        for channel_index, channel in enumerate(frame["channels"]):
            for row in channel:
                for real, imaginary in row:
                    sums[channel_index] += real * real + imaginary * imaginary
                    counts[channel_index] += 1
    return [math.sqrt(total / count) for total, count in zip(sums, counts)]


def package(treatment, raw_results, job, beams):
    methods = {method: compact(raw_results[method]) for method in METHODS}
    for item in methods.values():
        item["beam_trials"] = beams
        item["path_beam_trials"] = item["controls"] * beams
    vectors = {method: calibrated_vector(method, treatment, raw_results[method],
                                         job, beams) for method in METHODS}
    offset_maps = {
        method: {(item["doppler_offset_bins"], item["delay_offset_bins"]): item
                 for item in vectors[method]["controls"]}
        for method in METHODS}
    aligned = sorted(set.intersection(*(set(items) for items in offset_maps.values())))
    for method in METHODS:
        vector = vectors[method]
        methods[method].update({
            "evidence_log10": vector["target"]["evidence_log10"],
            "control_evidence_log10": [offset_maps[method][offset]["evidence_log10"]
                                       for offset in aligned],
            "parameter_trial_count": vector["parameter_grid_choices"],
            "matched_control_count": len(aligned),
        })
    return {"methods": methods, "score_vectors": vectors,
            "aligned_control_offsets": [
                {"doppler_offset_bins": row, "delay_offset_bins": column}
                for row, column in aligned]}


def merge_phase_vector(output, phase):
    """Add a real-pilot vector and realign all methods for the stacker."""
    method_id = "phase_locked_migration"
    methods, vectors = output["methods"], output["score_vectors"]
    methods[method_id] = dict(phase["methods"][method_id])
    vectors[method_id] = phase["score_vectors"][method_id]
    offset_maps = {
        name: {(item["doppler_offset_bins"], item["delay_offset_bins"]): item
               for item in vector["controls"]}
        for name, vector in vectors.items()}
    aligned = sorted(set.intersection(*(set(items) for items in offset_maps.values())))
    if not aligned:
        raise ValueError("phase vector has no controls aligned with spatial methods")
    for name, items in offset_maps.items():
        methods[name]["control_evidence_log10"] = [
            items[offset]["evidence_log10"] for offset in aligned]
        methods[name]["matched_control_count"] = len(aligned)
    output["aligned_control_offsets"] = [
        {"doppler_offset_bins": row, "delay_offset_bins": column}
        for row, column in aligned]


def phase_vector(cutouts, config, job):
    """Only a serialized direct pilot can join the acceptance stack."""
    try:
        result = PHASE.analyze(cutouts, config, job, phase_source="direct",
                               phase_gates=(.6,), delay_grid=(0,), doppler_grid=(0,))
    except ValueError as error:
        return {"status": "diagnostic_proxy_not_qualified", "reason": str(error)}
    return {"status": "qualified_direct_pilot", "result": result}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--replay", required=True, type=Path)
    parser.add_argument("--config", required=True, type=Path)
    parser.add_argument("--job", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    replay = json.loads(args.replay.read_text())
    config = json.loads(args.config.read_text())
    job = json.loads(args.job.read_text())
    cutouts = replay["arms"]["coherent_beam_bank"]["complex_cutouts"]
    trajectory = job["trajectory"]
    fraction = job["control_quantile"]
    gains = config["capture"]["device"]["heimdall"]["channel_gains"]
    equalized, scales = gain_equalized(cutouts, gains)
    treatments = {"raw_as_acquired": cutouts,
                  "inverse_hardware_gain": equalized}
    with concurrent.futures.ProcessPoolExecutor(
            max_workers=6, mp_context=multiprocessing.get_context("fork")) as executor:
        futures = {(treatment, method): executor.submit(
            run_method, method, values, config, trajectory, fraction)
            for treatment, values in treatments.items() for method in METHODS}
        results = {key: future.result() for key, future in futures.items()}
    output = {key: job[key] for key in ("job_id", "event_id", "hex", "flight", "outcome",
              "role", "revision", "kind", "recording", "truth_source", "family_trials",
              "control_quantile")}
    beams = config["process"]["surveillance_fusion"]["beams"]
    output["gain_treatments"] = {
        treatment: package(treatment,
                           {method: results[(treatment, method)] for method in METHODS},
                           job, beams) for treatment in treatments}
    # Compatibility aliases are explicitly the raw-as-acquired treatment.
    output.update(output["gain_treatments"]["raw_as_acquired"])
    # This executes before the runner removes replay JSON.  Existing v1
    # cutouts have no real pilot and remain explicitly diagnostic-only.
    output["phase_locked_migration"] = phase_vector(cutouts, config, job)
    if output["phase_locked_migration"]["status"] == "qualified_direct_pilot":
        merge_phase_vector(output, output["phase_locked_migration"]["result"])
    raw_rms, equalized_rms = channel_rms(cutouts), channel_rms(equalized)
    output["gain_accounting"] = {
        "channel_gains_db": gains, "common_voltage_scale_gain_db": sum(gains) / len(gains),
        "inverse_gain_amplitude_scales": scales,
        "raw_channel_rms": raw_rms, "equalized_channel_rms": equalized_rms,
        "noise_power_change_db": [20 * math.log10(scale) for scale in scales],
        "covariance_whitening": {
            method: "intrinsic local full-covariance whitening; no extra preprocessing to avoid double whitening"
            for method in METHODS},
    }
    args.output.write_text(json.dumps(output, separators=(",", ":")) + "\n")
    print(json.dumps({"job_id": job["job_id"], "methods": output["methods"]}, indent=2))


if __name__ == "__main__":
    main()
