#!/usr/bin/env python3
"""Replay-only post-Doppler five-channel STAP / adaptive matched filter."""

import argparse
import cmath
import hashlib
import importlib.util
import json
import math
import statistics
from pathlib import Path


SPEC = importlib.util.spec_from_file_location(
    "calibrated", Path(__file__).with_name("analyze-calibrated-complex-replay.py"))
CAL = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(CAL)

DEFAULT_GRID = tuple(
    {"doppler_radius": radius, "diagonal_loading": loading,
     "robust_energy_clip": clip}
    for radius in (1, 2)
    for loading in (.03, .1)
    for clip in (3.0, 10.0)
)


def inverse(matrix):
    count = len(matrix)
    augmented = [list(row) + [1 if row_index == column else 0
                              for column in range(count)]
                 for row_index, row in enumerate(matrix)]
    for column in range(count):
        pivot = max(range(column, count), key=lambda row: abs(augmented[row][column]))
        if abs(augmented[pivot][column]) < 1e-24:
            raise ValueError("singular covariance")
        augmented[column], augmented[pivot] = augmented[pivot], augmented[column]
        scale = augmented[column][column]
        augmented[column] = [value / scale for value in augmented[column]]
        for row in range(count):
            if row == column:
                continue
            scale = augmented[row][column]
            augmented[row] = [value - scale * pivot_value for value, pivot_value in
                              zip(augmented[row], augmented[column])]
    return [row[count:] for row in augmented]


def multiply(matrix, vector):
    return [sum(value * item for value, item in zip(row, vector)) for row in matrix]


def dot(left, right):
    return sum(value.conjugate() * item for value, item in zip(left, right))


def percentile(items, fraction):
    if not items:
        raise ValueError("empty empirical control population")
    ordered = sorted(items)
    return ordered[max(0, math.ceil(fraction * len(ordered)) - 1)]


def snapshot(frame, row, column, radius):
    return [complex(*frame["channels"][channel][rr][column])
            for rr in range(row - radius, row + radius + 1)
            for channel in range(len(frame["channels"]))]


def robust_covariance(frame, target_row, target_column, radius, exclusion,
                      loading, energy_clip, training=4):
    rows, columns = len(frame["doppler_hz"]), len(frame["delay_km"])
    samples = []
    for row in range(max(radius, target_row - training),
                     min(rows - radius, target_row + training + 1)):
        for column in range(max(0, target_column - training),
                            min(columns, target_column + training + 1)):
            # A training snapshot spans adjacent Doppler bins.  Exclude every
            # centre whose snapshot could contain the target or its guard.
            if (abs(row - target_row) <= exclusion + radius and
                    abs(column - target_column) <= exclusion):
                continue
            samples.append(snapshot(frame, row, column, radius))
    dimension = len(samples[0]) if samples else 0
    if len(samples) < dimension:
        raise ValueError("insufficient target-excluded covariance samples")
    energies = [sum(abs(value) ** 2 for value in item) for item in samples]
    energy_limit = max(statistics.median(energies) * energy_clip, 1e-30)
    covariance = [[0j] * dimension for _ in range(dimension)]
    effective_weight = 0.0
    clipped = 0
    for item, energy in zip(samples, energies):
        weight = min(1.0, energy_limit / max(energy, 1e-30))
        clipped += weight < 1.0
        effective_weight += weight
        for left in range(dimension):
            for right in range(dimension):
                covariance[left][right] += (weight * item[left] *
                                            item[right].conjugate())
    for left in range(dimension):
        for right in range(dimension):
            covariance[left][right] /= effective_weight
    trace = sum(covariance[index][index].real for index in range(dimension))
    load = max(trace / dimension * loading, 1e-15)
    for index in range(dimension):
        covariance[index][index] += load
    return inverse(covariance), {
        "raw_samples": len(samples), "effective_weight": effective_weight,
        "clipped_samples": clipped, "dimension": dimension, "loading_value": load,
    }


def doppler_signature(frame, frequency, center_row, radius, phase_sign):
    axis = frame["doppler_hz"]
    spacing = statistics.median([axis[index + 1] - axis[index]
                                 for index in range(len(axis) - 1)])
    result = []
    for row in range(center_row - radius, center_row + radius + 1):
        delta = (frequency - axis[row]) / spacing
        sinc = 1.0 if delta == 0 else math.sin(math.pi * delta) / (math.pi * delta)
        value = sinc * cmath.exp(phase_sign * 1j * math.pi * delta)
        result.append(value)
    return result


def steering_vectors(frame, config, frequency, center_row, radius):
    reference = [complex(*item) for item in frame["reference_weights"]]
    spatial = [[value.conjugate() for value in weights]
               for weights in CAL.calibrated_weights(config, reference)]
    # Each adjacent post-Doppler filter is a separate look; the loaded
    # covariance jointly adapts all filter/channel samples.  A single look is
    # selected over the complete trajectory, never independently per CPI.
    output = []
    for look_offset in (0,):
        for beam, vector in enumerate(spatial):
            steering = [0j] * ((2 * radius + 1) * len(vector))
            start = (look_offset + radius) * len(vector)
            steering[start:start + len(vector)] = vector
            output.append((beam, look_offset, steering))
    return output


def evaluate(cutouts, config, trajectory, parameters, exclusion=1, min_doppler=15,
             control_fraction=.99):
    if len(trajectory) < len(cutouts):
        raise ValueError("truth trajectory is shorter than the complex replay")
    if any(len(frame["channels"]) != 5 for frame in cutouts):
        raise ValueError("post-Doppler experiment requires five CAF channels")
    radius = parameters["doppler_radius"]
    target_cells = [(CAL.nearest(frame["doppler_hz"], truth["doppler_hz"]),
                     CAL.nearest(frame["delay_km"], truth["delay_km"]))
                    for frame, truth in zip(cutouts, trajectory)]
    score_maps = []
    covariance_reports = []
    trial_labels = None
    for frame_index, frame in enumerate(cutouts):
        target_row, target_column = target_cells[frame_index]
        trials = steering_vectors(frame, config,
                                  trajectory[frame_index]["doppler_hz"],
                                  target_row, radius)
        labels = [(beam, sign) for beam, sign, _ in trials]
        if trial_labels is None:
            trial_labels = labels
        elif labels != trial_labels:
            raise AssertionError("signal trial ordering changed")
        frame_map = {}
        for row in range(radius, len(frame["doppler_hz"]) - radius):
            if abs(frame["doppler_hz"][row]) < min_doppler:
                continue
            for column in range(len(frame["delay_km"])):
                try:
                    precision, report = robust_covariance(
                        frame, row, column, radius, exclusion,
                        parameters["diagonal_loading"],
                        parameters["robust_energy_clip"])
                except ValueError:
                    continue
                if (row, column) == (target_row, target_column):
                    covariance_reports.append(report)
                precision_steering = [multiply(precision, vector)
                                      for _, _, vector in trials]
                denominators = [max(dot(vector, item).real, 1e-30)
                                for (_, _, vector), item in
                                zip(trials, precision_steering)]
                observation = snapshot(frame, row, column, radius)
                frame_map[(row, column)] = [
                    abs(dot(item, observation)) ** 2 / denominator
                    for item, denominator in zip(precision_steering, denominators)]
        score_maps.append(frame_map)

    row_offsets = range(-len(cutouts[0]["doppler_hz"]) + 1,
                        len(cutouts[0]["doppler_hz"]))
    column_offsets = range(-len(cutouts[0]["delay_km"]) + 1,
                           len(cutouts[0]["delay_km"]))
    paths = {}
    winning_trials = {}
    for row_offset in row_offsets:
        for column_offset in column_offsets:
            totals = [0.0] * len(trial_labels)
            for frame_index, frame_map in enumerate(score_maps):
                target_row, target_column = target_cells[frame_index]
                values = frame_map.get((target_row + row_offset,
                                        target_column + column_offset))
                if values is None:
                    break
                totals = [total + value for total, value in zip(totals, values)]
            else:
                scores = [total / len(cutouts) for total in totals]
                winner = max(range(len(scores)), key=scores.__getitem__)
                paths[(row_offset, column_offset)] = scores[winner]
                winning_trials[(row_offset, column_offset)] = trial_labels[winner]
    if (0, 0) not in paths:
        raise ValueError("exact truth trajectory is outside the scoreable cutout")
    whole_controls = [score for offset, score in paths.items()
                      if abs(offset[0]) > exclusion or abs(offset[1]) > exclusion]
    control_score_vector = [
        {"doppler_offset_bins": offset[0], "delay_offset_bins": offset[1],
         "raw_score": score}
        for offset, score in sorted(paths.items())
        if abs(offset[0]) > exclusion or abs(offset[1]) > exclusion]
    matched_controls = [score for offset, score in paths.items()
                        if offset[0] == 0 and abs(offset[1]) > exclusion]
    target = paths[(0, 0)]
    matched_p99 = percentile(matched_controls, control_fraction)
    whole_p99 = percentile(whole_controls, control_fraction)
    strict_p99 = max(matched_p99, whole_p99)
    beams = config["process"]["surveillance_fusion"]["beams"]
    signal_trials = beams
    return {
        "configuration": {**parameters, "target_exclusion_radius": exclusion,
                          "doppler_look_offsets": [0],
                          "beam_trials": beams, "aggregation": "fixed_trial_mean"},
        "frames": len(cutouts),
        "target_cells": [{"frame": index, "row": row, "column": column,
                          "delay_km": cutouts[index]["delay_km"][column],
                          "doppler_hz": cutouts[index]["doppler_hz"][row]}
                         for index, (row, column) in enumerate(target_cells)],
        "target_score": target,
        "target_winning_trial": {"beam": winning_trials[(0, 0)][0],
                                 "doppler_look_offset": winning_trials[(0, 0)][1]},
        "matched_control_p99": matched_p99,
        "whole_search_control_p99": whole_p99,
        "strict_control_p99": strict_p99,
        "margin_over_control_p99_db": 10 * math.log10(max(target, 1e-300) /
                                                       max(strict_p99, 1e-300)),
        "controls_exceeding_target": sum(value >= target for value in whole_controls),
        "control_score_vector": control_score_vector,
        "control_quantile": control_fraction,
        "whole_search_max": max(whole_controls),
        "trial_accounting": {
            "signal_trials_per_path": signal_trials,
            "matched_control_paths": len(matched_controls),
            "whole_search_control_paths": len(whole_controls),
            "matched_control_signal_trials": len(matched_controls) * signal_trials,
            "whole_search_signal_trials": len(whole_controls) * signal_trials,
        },
        "covariance": covariance_reports,
    }


def analyze(cutouts, config, trajectory, grid):
    candidates = []
    for parameters in grid:
        result = evaluate(cutouts, config, trajectory, parameters)
        candidates.append({"parameters": parameters,
                           "margin_over_control_p99_db":
                               result["margin_over_control_p99_db"],
                           "target_score": result["target_score"],
                           "strict_control_p99": result["strict_control_p99"]})
    winner = max(range(len(candidates)),
                 key=lambda index: candidates[index]["margin_over_control_p99_db"])
    result = evaluate(cutouts, config, trajectory, grid[winner])
    result.update({
        "schema": "vectorwarp-post-doppler-stap-v1",
        "method": "robust-loaded post-Doppler STAP adaptive matched filter",
        "candidate_only": True,
        "parameter_selection": {"policy": "maximize r277 strict-control-p99 margin",
                                "grid_trials": len(grid), "selected_index": winner,
                                "candidates": candidates},
    })
    result["trial_accounting"]["parameter_grid_trials"] = len(grid)
    return result


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--replay", required=True, type=Path)
    parser.add_argument("--config", required=True, type=Path)
    parser.add_argument("--trajectory", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--select-grid", action="store_true",
                        help="Run the predeclared eight-point grid (r277 only)")
    parser.add_argument("--doppler-radius", type=int)
    parser.add_argument("--loading", type=float)
    parser.add_argument("--energy-clip", type=float)
    parser.add_argument("--delay-bias-km", type=float, default=.02578255)
    parser.add_argument("--doppler-bias-hz", type=float, default=7.053549)
    args = parser.parse_args()
    replay_bytes = args.replay.read_bytes()
    replay = json.loads(replay_bytes)
    config = json.loads(args.config.read_text())
    trajectory_document = json.loads(args.trajectory.read_text())
    trajectory = [{**item, "delay_km": item["delay_km"] + args.delay_bias_km,
                   "doppler_hz": item["doppler_hz"] + args.doppler_bias_hz}
                  for item in trajectory_document["trajectory"]]
    cutouts = replay["arms"]["coherent_beam_bank"]["complex_cutouts"]
    if args.select_grid:
        if any(value is not None for value in
               (args.doppler_radius, args.loading, args.energy_clip)):
            parser.error("explicit parameters cannot be combined with --select-grid")
        result = analyze(cutouts, config, trajectory, DEFAULT_GRID)
    else:
        if any(value is None for value in
               (args.doppler_radius, args.loading, args.energy_clip)):
            parser.error("frozen --doppler-radius, --loading, and --energy-clip required")
        parameters = {"doppler_radius": args.doppler_radius,
                      "diagonal_loading": args.loading,
                      "robust_energy_clip": args.energy_clip}
        result = evaluate(cutouts, config, trajectory, parameters)
        result.update({"schema": "vectorwarp-post-doppler-stap-v1",
                       "method": "robust-loaded post-Doppler STAP adaptive matched filter",
                       "candidate_only": True,
                       "parameter_selection": {"policy": "frozen_on_r277",
                                               "grid_trials": 0}})
        result["trial_accounting"]["parameter_grid_trials"] = 0
    result.update({"trajectory": str(args.trajectory), "replay": str(args.replay),
                   "replay_sha256": hashlib.sha256(replay_bytes).hexdigest(),
                   "recording": replay.get("recording"),
                   "trajectory_schema": trajectory_document.get("schema"),
                   "calibrated_signal_time_bias": {
                       "delay_km": args.delay_bias_km,
                       "doppler_hz": args.doppler_bias_hz}})
    args.output.write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps({"output": str(args.output), "frames": result["frames"],
                      "parameters": result["configuration"],
                      "margin_db": result["margin_over_control_p99_db"],
                      "controls": result["trial_accounting"]}, indent=2))


if __name__ == "__main__":
    main()
