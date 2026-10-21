#!/usr/bin/env python3
"""Replay-only sparse beam decomposition of multichannel complex CAF cutouts."""

import argparse
import importlib.util
import json
import math
from pathlib import Path

import numpy as np


ROOT = Path(__file__).resolve().parent
CAL_SPEC = importlib.util.spec_from_file_location(
    "calibrated", ROOT / "analyze-calibrated-complex-replay.py")
CAL = importlib.util.module_from_spec(CAL_SPEC)
CAL_SPEC.loader.exec_module(CAL)


def percentile(values, fraction):
    ordered = sorted(values)
    return ordered[min(len(ordered) - 1, int(fraction * len(ordered)))]


def complex_cell(frame, row, column):
    return np.asarray([complex(*channel[row][column])
                       for channel in frame["channels"]], dtype=np.complex128)


def covariance(frame, row, column, guard, training, loading):
    vectors = []
    for rr in range(max(0, row - training),
                    min(len(frame["doppler_hz"]), row + training + 1)):
        for cc in range(max(0, column - training),
                        min(len(frame["delay_km"]), column + training + 1)):
            if abs(rr - row) <= guard and abs(cc - column) <= guard:
                continue
            vectors.append(complex_cell(frame, rr, cc))
    if len(vectors) < len(frame["channels"]):
        return None
    matrix = np.stack(vectors)
    result = matrix.T @ matrix.conj() / len(vectors)
    scale = max(float(np.trace(result).real / result.shape[0]), 1e-12)
    result += np.eye(result.shape[0]) * scale * loading
    return result


def whiten(matrix, values):
    eigenvalues, eigenvectors = np.linalg.eigh(matrix)
    floor = max(float(eigenvalues.max()) * 1e-9, 1e-12)
    inverse_root = (eigenvectors * (1 / np.sqrt(np.maximum(eigenvalues, floor)))) @ \
        eigenvectors.conj().T
    return inverse_root @ values


def soft(values, threshold):
    magnitude = np.abs(values)
    return np.where(magnitude > threshold,
                    values * (1 - threshold / np.maximum(magnitude, 1e-300)), 0)


def lasso(dictionary, observation, penalty, iterations=80):
    # Complex FISTA for 0.5 ||Az-y||^2 + penalty ||z||_1.
    spectral = max(float(np.linalg.norm(dictionary, 2) ** 2), 1e-12)
    coefficients = np.zeros(dictionary.shape[1], dtype=np.complex128)
    accelerated = coefficients.copy()
    momentum = 1.0
    for _ in range(iterations):
        gradient = dictionary.conj().T @ (dictionary @ accelerated - observation)
        updated = soft(accelerated - gradient / spectral, penalty / spectral)
        next_momentum = (1 + math.sqrt(1 + 4 * momentum * momentum)) / 2
        accelerated = updated + (momentum - 1) / next_momentum * (updated - coefficients)
        coefficients, momentum = updated, next_momentum
    return coefficients


def score(frame, config, row, column, penalty, guard, training, loading):
    noise = covariance(frame, row, column, guard, training, loading)
    if noise is None:
        return None
    reference = [complex(*value) for value in frame["reference_weights"]]
    beams = np.asarray(CAL.calibrated_weights(config, reference),
                       dtype=np.complex128).conj().T
    beams = whiten(noise, beams)
    beams /= np.maximum(np.linalg.norm(beams, axis=0, keepdims=True), 1e-12)
    observation = whiten(noise, complex_cell(frame, row, column))
    coefficients = lasso(beams, observation, penalty)
    return np.abs(coefficients) ** 2


def analyze(cutouts, config, trajectory, penalty, guard=1, training=3, loading=.01):
    target_cells = [(CAL.nearest(frame["doppler_hz"], truth["doppler_hz"]),
                     CAL.nearest(frame["delay_km"], truth["delay_km"]))
                    for frame, truth in zip(cutouts, trajectory)]
    row_span = range(-len(cutouts[0]["doppler_hz"]) + 1,
                     len(cutouts[0]["doppler_hz"]))
    column_span = range(-len(cutouts[0]["delay_km"]) + 1,
                        len(cutouts[0]["delay_km"]))
    accumulated = {}
    counts = {}
    for frame_index, frame in enumerate(cutouts):
        target_row, target_column = target_cells[frame_index]
        for row_offset in row_span:
            row = target_row + row_offset
            if not 0 <= row < len(frame["doppler_hz"]):
                continue
            for column_offset in column_span:
                column = target_column + column_offset
                if not 0 <= column < len(frame["delay_km"]):
                    continue
                powers = score(frame, config, row, column, penalty, guard,
                               training, loading)
                if powers is None:
                    continue
                key = row_offset, column_offset
                accumulated[key] = accumulated.get(key, np.zeros_like(powers)) + powers
                counts[key] = counts.get(key, 0) + 1
    valid = {key: float(value.max() / counts[key]) for key, value in accumulated.items()
             if counts[key] == len(cutouts)}
    target = valid[(0, 0)]
    controls = [value for key, value in valid.items()
                if abs(key[0]) > guard or abs(key[1]) > guard]
    control_p99 = percentile(controls, .99)
    return {
        "schema": "vectorwarp-sparse-beam-recovery-v1",
        "candidate_only": True,
        "frames": len(cutouts),
        "configuration": {"lasso_penalty": penalty, "guard_radius": guard,
                          "training_radius": training, "diagonal_loading": loading,
                          "beam_trials": config["process"]["surveillance_fusion"]["beams"]},
        "target_score": target,
        "control_p99": control_p99,
        "margin_over_control_p99_db": 10 * math.log10(max(target, 1e-300) /
                                                       max(control_p99, 1e-300)),
        "controls": len(controls),
        "controls_exceeding_target": sum(value >= target for value in controls),
        "target_cells": [{"frame": index, "row": row, "column": column,
                          "delay_km": cutouts[index]["delay_km"][column],
                          "doppler_hz": cutouts[index]["doppler_hz"][row]}
                         for index, (row, column) in enumerate(target_cells)],
    }


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--replay", required=True, type=Path)
    parser.add_argument("--config", required=True, type=Path)
    parser.add_argument("--trajectory", required=True, type=Path)
    parser.add_argument("--penalty", required=True, type=float)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--guard", type=int, default=1)
    parser.add_argument("--training", type=int, default=3)
    parser.add_argument("--loading", type=float, default=.01)
    parser.add_argument("--delay-bias-km", type=float, default=0)
    parser.add_argument("--doppler-bias-hz", type=float, default=0)
    args = parser.parse_args()
    replay = json.loads(args.replay.read_text())
    config = json.loads(args.config.read_text())
    trajectory_document = json.loads(args.trajectory.read_text())
    cutouts = replay["arms"]["coherent_beam_bank"]["complex_cutouts"]
    trajectory = [{**item,
                   "delay_km": item["delay_km"] + args.delay_bias_km,
                   "doppler_hz": item["doppler_hz"] + args.doppler_bias_hz}
                  for item in trajectory_document["trajectory"]]
    result = analyze(cutouts, config, trajectory, args.penalty, args.guard,
                     args.training, args.loading)
    result["replay"] = str(args.replay)
    result["trajectory"] = str(args.trajectory)
    result["hex"] = trajectory_document.get("hex")
    args.output.write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
