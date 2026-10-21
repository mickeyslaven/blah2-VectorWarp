#!/usr/bin/env python3
"""Score a fixed target-protected-reference beam bank without truth-tuning it.

Each replay is a predeclared geometric reference null.  For the reported
post-hoc truth cell, the empirical control population pools every other
delay/Doppler cell from every bank member, so its p99 includes the reference
bank trial factor.  This is deliberately a replay-analysis tool, not live DSP.
"""

import argparse
import json
import math
from pathlib import Path


def nearest(axis, value):
    return min(range(len(axis)), key=lambda index: abs(axis[index] - value))


def p99(values):
    ordered = sorted(values)
    return ordered[min(len(ordered) - 1, int(.99 * len(ordered)))]


def load_truth(path):
    document = json.loads(path.read_text().splitlines()[0])
    _, truth = next(iter(document["truth"].items()))
    return truth


def value(frame, row, column):
    # The raw complex per-surveillance-channel CAF cutout is retained before
    # map fusion.  A conservative per-cell reference-bank score is the sum of
    # channel powers; it cannot create coherent gain that the replay lacks.
    return sum(abs(complex(*channel[row][column])) ** 2
               for channel in frame["channels"])


def score(replays, truth, guard=1):
    targets, controls, per_beam = [], [], []
    for path in replays:
        replay = json.loads(path.read_text())
        cutouts = replay["arms"]["coherent_beam_bank"].get("complex_cutouts", [])
        target_values, control_values = [], []
        for frame in cutouts:
            row = nearest(frame["doppler_hz"], truth["doppler"])
            column = nearest(frame["delay_km"], truth["delay"])
            target_values.append(value(frame, row, column))
            for rr in range(len(frame["doppler_hz"])):
                for cc in range(len(frame["delay_km"])):
                    if abs(rr - row) > guard or abs(cc - column) > guard:
                        control_values.append(value(frame, rr, cc))
        if target_values:
            item = {"path": str(path), "frames": len(target_values),
                    "target_mean_power": sum(target_values) / len(target_values),
                    "control_cells": len(control_values)}
            per_beam.append(item)
            targets.append(item["target_mean_power"])
            controls.extend(control_values)
    if not targets or not controls:
        raise ValueError("bank lacks usable complex cutouts")
    threshold = p99(controls)
    best = max(targets)
    return {"schema": "vectorwarp-target-protected-reference-v1",
            "assumption": "pooled non-target cells across all fixed reference-null beams are the empirical whole-search control population",
            "beam_trials": len(per_beam), "per_beam": per_beam,
            "pooled_control_cells": len(controls), "pooled_control_p99_power": threshold,
            "best_truth_cell_mean_power": best,
            "best_truth_margin_over_pooled_p99_db": 10 * math.log10(max(best, 1e-300) / max(threshold, 1e-300)),
            "accepted": 10 * math.log10(max(best, 1e-300) / max(threshold, 1e-300)) >= 3}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--replays", required=True, nargs="+", type=Path)
    parser.add_argument("--truth", required=True, type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    result = score(args.replays, load_truth(args.truth))
    encoded = json.dumps(result, indent=2) + "\n"
    if args.output:
        args.output.write_text(encoded)
    else:
        print(encoded, end="")


if __name__ == "__main__":
    main()
