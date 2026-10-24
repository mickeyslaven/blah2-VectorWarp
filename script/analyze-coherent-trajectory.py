#!/usr/bin/env python3
"""Search complex CAF cutouts for a fixed-beam, migration-corrected coherent path."""

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
TAU = 2 * math.pi


def values(specification):
    start, stop, step = map(float, specification.split(":"))
    output = []
    current = start
    while current <= stop + abs(step) * 1e-9:
        output.append(current)
        current += step
    return output


def beam_cube(cutouts, config):
    cube = []
    for frame in cutouts:
        weights = CAL.calibrated_weights(
            config, [complex(*item) for item in frame["reference_weights"]])
        beams = []
        for beam_weights in weights:
            rows = []
            for row in range(len(frame["doppler_hz"])):
                cells = []
                for column in range(len(frame["delay_km"])):
                    channels = [complex(*channel[row][column])
                                for channel in frame["channels"]]
                    cells.append(sum(weight * sample for weight, sample in
                                     zip(beam_weights, channels)))
                rows.append(cells)
            beams.append(rows)
        cube.append(beams)
    return cube


def nearest(axis, value):
    return min(range(len(axis)), key=lambda index: abs(axis[index] - value))


def score_anchor(cube, cutouts, anchor_row, anchor_column, cpi,
                 delay_rates, doppler_rates, residual_rates):
    delay0 = cutouts[0]["delay_km"][anchor_column]
    doppler0 = cutouts[0]["doppler_hz"][anchor_row]
    best = None
    for delay_rate in delay_rates:
        for doppler_rate in doppler_rates:
            cells = []
            valid = True
            for frame in range(len(cutouts)):
                time = frame * cpi
                desired_doppler = doppler0 + doppler_rate * time
                desired_delay = delay0 + delay_rate * time
                doppler_axis = cutouts[frame]["doppler_hz"]
                delay_axis = cutouts[frame]["delay_km"]
                doppler_half_bin = abs(doppler_axis[1] - doppler_axis[0]) / 2
                delay_half_bin = abs(delay_axis[1] - delay_axis[0]) / 2
                if (desired_doppler < min(doppler_axis) - doppler_half_bin or
                    desired_doppler > max(doppler_axis) + doppler_half_bin or
                    desired_delay < min(delay_axis) - delay_half_bin or
                    desired_delay > max(delay_axis) + delay_half_bin):
                    valid = False
                    break
                row = nearest(doppler_axis, desired_doppler)
                column = nearest(delay_axis, desired_delay)
                cells.append((row, column))
            if not valid:
                continue
            for beam in range(len(cube[0])):
                samples = [cube[frame][beam][row][column]
                           for frame, (row, column) in enumerate(cells)]
                noncoherent = sum(abs(sample) ** 2 for sample in samples) / len(samples)
                for residual in residual_rates:
                    for sign in (-1, 1):
                        aligned = []
                        for frame, sample in enumerate(samples):
                            time = frame * cpi
                            phase_cycles = (doppler0 + residual) * time + .5 * doppler_rate * time ** 2
                            aligned.append(sample * cmath.exp(sign * 1j * TAU * phase_cycles))
                        coherent = abs(sum(aligned)) ** 2 / len(aligned)
                        item = {"score": coherent, "noncoherent": noncoherent,
                                "coherence": coherent / max(sum(abs(value) ** 2
                                for value in aligned), 1e-300),
                                "beam": beam, "delay_rate_km_s": delay_rate,
                                "doppler_rate_hz_s": doppler_rate,
                                "residual_phase_rate_hz": residual,
                                "phase_sign": sign, "cells": cells}
                        if best is None or item["score"] > best["score"]:
                            best = item
    return best


def p99(items):
    ordered = sorted(items)
    return ordered[min(len(ordered) - 1, int(.99 * len(ordered)))]


def analyze(cutouts, config, delay, doppler, delay_rates, doppler_rates,
            residual_rates, guard=2, min_doppler=15):
    if len(cutouts) < 3:
        raise ValueError("at least three complex cutout frames are required")
    cpi = config["process"]["data"]["cpi"]
    cube = beam_cube(cutouts, config)
    target_row = nearest(cutouts[0]["doppler_hz"], doppler)
    target_column = nearest(cutouts[0]["delay_km"], delay)
    target = score_anchor(cube, cutouts, target_row, target_column, cpi,
                          delay_rates, doppler_rates, residual_rates)
    controls = []
    for row, frequency in enumerate(cutouts[0]["doppler_hz"]):
        if abs(frequency) < min_doppler:
            continue
        for column in range(len(cutouts[0]["delay_km"])):
            if abs(row - target_row) <= guard and abs(column - target_column) <= guard:
                continue
            item = score_anchor(cube, cutouts, row, column, cpi,
                                delay_rates, doppler_rates, residual_rates)
            if item is not None:
                controls.append(item)
    threshold = p99([item["score"] for item in controls])
    noncoherent_threshold = p99([item["noncoherent"] for item in controls])
    def margin(value, reference):
        return 10 * math.log10(max(value, 1e-300) / max(reference, 1e-300))
    return {
        "schema": "vectorwarp-coherent-trajectory-v1",
        "frames": len(cutouts),
        "cpi_s": cpi,
        "target_cell": {"row": target_row, "column": target_column,
                        "delay_km": cutouts[0]["delay_km"][target_column],
                        "doppler_hz": cutouts[0]["doppler_hz"][target_row]},
        "search": {"beams": len(cube[0]), "delay_rates_km_s": delay_rates,
                   "doppler_rates_hz_s": doppler_rates,
                   "residual_phase_rates_hz": residual_rates,
                   "phase_signs": [-1, 1], "control_anchors": len(controls)},
        "target_best": target,
        "control_p99_score": threshold,
        "coherent_margin_over_control_p99_db": margin(target["score"], threshold),
        "noncoherent_margin_over_control_p99_db": margin(
            target["noncoherent"], noncoherent_threshold),
        "controls_exceeding_target": sum(item["score"] >= target["score"]
                                         for item in controls),
    }


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--replay", required=True, type=Path)
    parser.add_argument("--config", required=True, type=Path)
    parser.add_argument("--truth", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--delay-rate", default="-0.10:0.10:0.05")
    parser.add_argument("--doppler-rate", default="-8:8:2")
    parser.add_argument("--residual-phase-rate", default="-1:1:1")
    parser.add_argument("--delay-bias-km", type=float, default=.02578255)
    parser.add_argument("--doppler-bias-hz", type=float, default=7.053549)
    args = parser.parse_args()
    replay = json.loads(args.replay.read_text())
    config = json.loads(args.config.read_text())
    ident, truth = CAL.load_truth(args.truth)
    cutouts = replay["arms"]["coherent_beam_bank"]["complex_cutouts"]
    result = analyze(cutouts, config, truth["delay"] + args.delay_bias_km,
                     truth["doppler"] + args.doppler_bias_hz,
                     values(args.delay_rate), values(args.doppler_rate),
                     values(args.residual_phase_rate))
    result.update({"hex": ident, "truth": truth, "candidate_only": True})
    args.output.write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
