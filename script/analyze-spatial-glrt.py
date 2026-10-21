#!/usr/bin/env python3
"""Evaluate local covariance-whitened spatial detection on complex CAF cutouts."""

import argparse
import cmath
import importlib.util
import json
import math
from pathlib import Path


SPEC = importlib.util.spec_from_file_location(
    "calibrated", Path(__file__).with_name("analyze-calibrated-complex-replay.py"))
CAL = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(CAL)


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


def cell(frame, row, column):
    return [complex(*channel[row][column]) for channel in frame["channels"]]


def spatial_score(frame, config, row, column, guard, training, loading):
    channels = len(frame["channels"])
    covariance = [[0j] * channels for _ in range(channels)]
    samples = 0
    for row_offset in range(-training, training + 1):
        for column_offset in range(-training, training + 1):
            if abs(row_offset) <= guard and abs(column_offset) <= guard:
                continue
            rr, cc = row + row_offset, column + column_offset
            if not (0 <= rr < len(frame["doppler_hz"]) and
                    0 <= cc < len(frame["delay_km"])):
                continue
            vector = cell(frame, rr, cc)
            for left in range(channels):
                for right in range(channels):
                    covariance[left][right] += vector[left] * vector[right].conjugate()
            samples += 1
    if samples < channels:
        return None
    trace = sum(covariance[index][index].real for index in range(channels)) / samples
    load = max(trace / channels * loading, 1e-12)
    for left in range(channels):
        for right in range(channels):
            covariance[left][right] /= samples
        covariance[left][left] += load
    precision = inverse(covariance)
    observation = cell(frame, row, column)
    precision_observation = multiply(precision, observation)
    reference = [complex(*item) for item in frame["reference_weights"]]
    scores = []
    for combining in CAL.calibrated_weights(config, reference):
        steering = [value.conjugate() for value in combining]
        precision_steering = multiply(precision, steering)
        numerator = abs(dot(steering, precision_observation)) ** 2
        denominator = max(dot(steering, precision_steering).real, 1e-30)
        scores.append(numerator / denominator)
    return scores, scores.index(max(scores)), samples


def percentile(items, fraction):
    ordered = sorted(items)
    return ordered[min(len(ordered) - 1, int(fraction * len(ordered)))]


def analyze_trajectory(cutouts, config, trajectory, guard=1, training=3,
                       loading=.01, min_doppler=15, beam_policy="max_per_frame"):
    if len(trajectory) < len(cutouts):
        raise ValueError("truth trajectory is shorter than the complex replay")
    target_cells = [(CAL.nearest(frame["doppler_hz"], truth["doppler_hz"]),
                     CAL.nearest(frame["delay_km"], truth["delay_km"]))
                    for frame, truth in zip(cutouts, trajectory)]
    accumulated = {}
    beam_votes = {}
    covariance_samples = {}
    row_offsets = range(-len(cutouts[0]["doppler_hz"]) + 1,
                        len(cutouts[0]["doppler_hz"]))
    column_offsets = range(-len(cutouts[0]["delay_km"]) + 1,
                           len(cutouts[0]["delay_km"]))
    for frame_index, frame in enumerate(cutouts):
        target_row, target_column = target_cells[frame_index]
        for row_offset in row_offsets:
            row = target_row + row_offset
            if not 0 <= row < len(frame["doppler_hz"]):
                continue
            if abs(frame["doppler_hz"][row]) < min_doppler:
                continue
            for column_offset in column_offsets:
                column = target_column + column_offset
                if not 0 <= column < len(frame["delay_km"]):
                    continue
                result = spatial_score(frame, config, row, column, guard, training, loading)
                if result is not None:
                    scores, beam, samples = result
                    key = row_offset, column_offset
                    if key not in accumulated:
                        accumulated[key] = [0.0] * len(scores)
                    for index, score in enumerate(scores):
                        accumulated[key][index] += score
                    beam_votes.setdefault(key, []).append(beam)
                    covariance_samples[key] = samples
    fixed_beams = {}
    valid = {key: values for key, values in accumulated.items()
             if len(beam_votes[key]) == len(cutouts)}
    if beam_policy == "fixed":
        for key, scores in valid.items():
            fixed_beams[key] = max(range(len(scores)), key=lambda beam: scores[beam])
        accumulated = {key: max(scores) / len(cutouts) for key, scores in valid.items()}
    elif beam_policy == "max_per_frame":
        # Recompute the previous statistic from the recorded winning-beam scores.
        accumulated = {}
        for key in valid:
            total = 0.0
            for frame_index, frame in enumerate(cutouts):
                target_row, target_column = target_cells[frame_index]
                row, column = target_row + key[0], target_column + key[1]
                scores, _, _ = spatial_score(frame, config, row, column, guard,
                                              training, loading)
                total += max(scores)
            accumulated[key] = total / len(cutouts)
    else:
        raise ValueError("unknown beam policy")
    target_key = 0, 0
    if target_key not in accumulated:
        raise ValueError("target cell was not scoreable")
    controls = [value for (row_offset, column_offset), value in accumulated.items()
                if abs(row_offset) > guard or abs(column_offset) > guard]
    p99 = percentile(controls, .99)
    target = accumulated[target_key]
    return {
        "schema": "vectorwarp-spatial-glrt-v1",
        "frames": len(cutouts),
        "target_cells": [{"frame": frame, "row": row, "column": column,
                          "delay_km": cutouts[frame]["delay_km"][column],
                          "doppler_hz": cutouts[frame]["doppler_hz"][row]}
                         for frame, (row, column) in enumerate(target_cells)],
        "configuration": {"guard_radius": guard, "training_radius": training,
                          "diagonal_loading": loading, "beam_trials":
                          config["process"]["surveillance_fusion"]["beams"],
                          "beam_policy": beam_policy},
        "target_score": target,
        "control_p99": p99,
        "margin_over_control_p99_db": 10 * math.log10(max(target, 1e-300) /
                                                       max(p99, 1e-300)),
        "controls": len(controls),
        "controls_exceeding_target": sum(value >= target for value in controls),
        "target_beam_votes": beam_votes[target_key],
        "target_fixed_beam": fixed_beams.get(target_key),
        "target_covariance_samples_per_frame": covariance_samples[target_key],
    }


def analyze(cutouts, config, delay, doppler, guard=1, training=3,
            loading=.01, min_doppler=15, beam_policy="max_per_frame"):
    trajectory = [{"delay_km": delay, "doppler_hz": doppler} for _ in cutouts]
    return analyze_trajectory(cutouts, config, trajectory, guard, training,
                              loading, min_doppler, beam_policy)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--replay", required=True, type=Path)
    parser.add_argument("--config", required=True, type=Path)
    parser.add_argument("--truth", required=True, type=Path)
    parser.add_argument("--trajectory", type=Path,
                        help="Per-frame clip truth from extract-clip-truth.py")
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--guard", type=int, default=1)
    parser.add_argument("--training", type=int, default=3)
    parser.add_argument("--loading", type=float, default=.01)
    parser.add_argument("--beam-policy", choices=("max_per_frame", "fixed"),
                        default="max_per_frame")
    parser.add_argument("--delay-bias-km", type=float, default=.02578255)
    parser.add_argument("--doppler-bias-hz", type=float, default=7.053549)
    args = parser.parse_args()
    replay = json.loads(args.replay.read_text())
    config = json.loads(args.config.read_text())
    ident, truth = CAL.load_truth(args.truth)
    cutouts = replay["arms"]["coherent_beam_bank"]["complex_cutouts"]
    if args.trajectory:
        trajectory_document = json.loads(args.trajectory.read_text())
        trajectory = trajectory_document["trajectory"]
        result = analyze_trajectory(cutouts, config, trajectory, args.guard,
                                    args.training, args.loading,
                                    beam_policy=args.beam_policy)
        result["trajectory"] = str(args.trajectory)
    else:
        result = analyze(cutouts, config, truth["delay"] + args.delay_bias_km,
                         truth["doppler"] + args.doppler_bias_hz,
                         args.guard, args.training, args.loading,
                         beam_policy=args.beam_policy)
    result.update({"hex": ident, "truth": truth, "candidate_only": True})
    args.output.write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
