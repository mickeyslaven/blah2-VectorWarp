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


def compact(result):
    return {key: result[key] for key in (
        "frames", "target_score", "margin_over_control_p99_db",
        "controls_exceeding_target", "control_quantile") if key in result} | {
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


def calibrated_vector(method, result, job, beams):
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
        "method_id": method, "job_id": job["job_id"],
        "event_id": job["event_id"], "recording": job["recording"],
        "trial_family_count": job["family_trials"],
        "parameter_grid_choices": 1, "internal_search_trials_per_path": beams,
        "target": record("target", result["target_score"], 0, 0),
        "controls": [record("control", item["raw_score"],
                            item["doppler_offset_bins"], item["delay_offset_bins"], True)
                     for item in controls],
    }


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
    methods = ("spatial_glrt", "post_doppler_stap", "sparse_beam")
    with concurrent.futures.ProcessPoolExecutor(
            max_workers=3, mp_context=multiprocessing.get_context("fork")) as executor:
        futures = {method: executor.submit(run_method, method, cutouts, config,
                                           trajectory, fraction) for method in methods}
        raw_results = {method: future.result() for method, future in futures.items()}
    output = {key: job[key] for key in ("job_id", "event_id", "hex", "flight", "outcome",
              "role", "revision", "kind", "recording", "truth_source", "family_trials",
              "control_quantile")}
    output["methods"] = {method: compact(raw_results[method]) for method in methods}
    for item in output["methods"].values():
        item["beam_trials"] = config["process"]["surveillance_fusion"]["beams"]
        item["path_beam_trials"] = item["controls"] * item["beam_trials"]
    output["score_vectors"] = {
        method: calibrated_vector(method, raw_results[method], job,
                                  config["process"]["surveillance_fusion"]["beams"])
        for method in methods}
    offset_maps = {
        method: {(item["doppler_offset_bins"], item["delay_offset_bins"]): item
                 for item in output["score_vectors"][method]["controls"]}
        for method in methods}
    aligned_offsets = sorted(set.intersection(*(set(items) for items in offset_maps.values())))
    output["aligned_control_offsets"] = [
        {"doppler_offset_bins": row, "delay_offset_bins": column}
        for row, column in aligned_offsets]
    for method in methods:
        vector = output["score_vectors"][method]
        output["methods"][method].update({
            "evidence_log10": vector["target"]["evidence_log10"],
            "control_evidence_log10": [offset_maps[method][offset]["evidence_log10"]
                                       for offset in aligned_offsets],
            "parameter_trial_count": vector["parameter_grid_choices"],
            "matched_control_count": len(aligned_offsets),
        })
    args.output.write_text(json.dumps(output, separators=(",", ":")) + "\n")
    print(json.dumps({"job_id": job["job_id"], "methods": output["methods"]}, indent=2))


if __name__ == "__main__":
    main()
