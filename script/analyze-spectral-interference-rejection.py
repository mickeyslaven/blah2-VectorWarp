#!/usr/bin/env python3
"""Blind subband interference rejection for retained MCHQ replays.

The detector estimates disturbance power without target truth.  For every CPI,
it regresses each surveillance FFT bin on the direct-path reference across slow
time, measures the residual in 24 fixed subbands, and downweights noisy
subbands before the ordinary matched-filter CAF.  Candidate strength is scored
against translated whole-map trajectories, including all loading trials.
"""
import argparse
import importlib.util
import json
import math
from pathlib import Path

import numpy as np


BASE = Path(__file__).with_name("analyze-loaded-reciprocal-mchq.py")
SPEC = importlib.util.spec_from_file_location("loaded_reciprocal", BASE)
COMMON = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(COMMON)


def spectral_caf(iq, fft_size, max_delay_samples, max_doppler_hz,
                 loadings, subbands):
    blocks = iq.shape[1] // fft_size
    iq = iq[:, :blocks * fft_size].reshape(iq.shape[0], blocks, fft_size)
    reference = iq[0] - iq[0].mean(axis=1, keepdims=True)
    surveillance = iq[1:] - iq[1:].mean(axis=1, keepdims=True)
    x = np.fft.fft(reference, axis=1)
    y = np.fft.fft(surveillance, axis=2)

    # Remove the stationary direct-path component before estimating disturbance.
    denominator = np.maximum(np.sum(np.abs(x) ** 2, axis=0), 1e-18)
    alpha = np.sum(y * np.conj(x)[None, :, :], axis=1) / denominator[None, :]
    residual = y - alpha[:, None, :] * x[None, :, :]
    bin_power = np.mean(np.abs(residual) ** 2, axis=(0, 1))

    shifted = np.fft.fftshift(bin_power)
    groups = np.array_split(np.arange(fft_size), subbands)
    band_power = np.asarray([np.median(shifted[group]) for group in groups])
    floor = max(float(np.median(band_power)), 1e-18)
    prf = COMMON.SAMPLE_RATE / fft_size
    doppler = np.fft.fftshift(np.fft.fftfreq(blocks, d=1 / prf))
    rows = np.flatnonzero(np.abs(doppler) <= max_doppler_hz)
    output, diagnostics = {}, {}
    parameters = ["baseline"] + loadings
    for parameter in parameters:
        if parameter == "baseline":
            band_weight = np.ones(subbands)
        else:
            band_weight = floor / (band_power + parameter * floor)
            band_weight /= np.mean(band_weight)
        shifted_weight = np.empty(fft_size)
        for group, value in zip(groups, band_weight):
            shifted_weight[group] = value
        weight = np.fft.ifftshift(shifted_weight)
        correlations = np.fft.ifft(y * np.conj(x)[None, :, :] * weight[None, None, :],
                                   axis=2)[:, :, :max_delay_samples]
        ambiguity = np.fft.fftshift(np.fft.fft(correlations, axis=1), axes=1)
        output[parameter] = np.sum(np.abs(ambiguity[:, rows, :]) ** 2,
                                   axis=0).astype(np.float64)
        effective = float(np.sum(band_weight) ** 2 /
                          (subbands * np.sum(band_weight ** 2)))
        diagnostics[str(parameter)] = {
            "subband_residual_db_relative_median": [
                COMMON.db(value / floor) for value in band_power],
            "minimum_weight": float(np.min(band_weight)),
            "maximum_weight": float(np.max(band_weight)),
            "effective_bandwidth_fraction": effective,
        }
    return doppler[rows], output, diagnostics


def analyze(recording, trajectory_file, loadings, subbands, cpi, fft_size,
            max_delay_km, max_doppler_hz, guard, training, controls_stride):
    trajectory = json.loads(Path(trajectory_file).read_text())["trajectory"]
    cpi_samples = int(round(cpi * COMMON.SAMPLE_RATE))
    max_delay_samples = int(math.ceil(max_delay_km * COMMON.SAMPLE_RATE /
                                      COMMON.C_KM_S)) + 1
    parameters = ["baseline"] + loadings
    maps = {parameter: [] for parameter in parameters}
    diagnostics = {str(parameter): [] for parameter in parameters}
    doppler_axis = None
    for index, iq in enumerate(COMMON.frames(recording, cpi_samples)):
        if index >= len(trajectory):
            break
        axis, surfaces, frame_diagnostics = spectral_caf(
            iq, fft_size, max_delay_samples, max_doppler_hz, loadings, subbands)
        doppler_axis = axis
        for parameter in parameters:
            maps[parameter].append(surfaces[parameter])
            diagnostics[str(parameter)].append(frame_diagnostics[str(parameter)])
    if not maps["baseline"]:
        raise ValueError("recording has no complete CPI")
    scores, controls = COMMON.score(maps, doppler_axis, trajectory, parameters,
                                    guard, training, controls_stride)
    for parameter in parameters:
        score = scores[str(parameter)]
        score["margin_over_parameter_trial_control_p99_db"] = COMMON.db(
            score["target"] / controls["parameter_trial_control_p99"])
    candidates = [str(value) for value in loadings]
    selected = max(candidates,
                   key=lambda value: scores[value]["margin_over_parameter_trial_control_p99_db"])
    return {
        "schema": "vectorwarp-spectral-interference-rejection-v1",
        "candidate_only": True,
        "recording": str(recording),
        "trajectory": str(trajectory_file),
        "frames": len(maps["baseline"]),
        "sample_rate_hz": COMMON.SAMPLE_RATE,
        "cpi_s": cpi,
        "filter": {
            "method": "blind direct-path-regression residual subband weighting",
            "truth_used_for_weights": False,
            "fft_size": fft_size,
            "subbands": subbands,
            "loadings": loadings,
            "diagnostics": diagnostics,
        },
        "controls": controls,
        "scores": scores,
        "selected_loading": float(selected),
        "selected_margin_db": scores[selected]["margin_over_parameter_trial_control_p99_db"],
        "positive_gate_pass": scores[selected]["margin_over_parameter_trial_control_p99_db"] >= 3.0,
        "deployment_note": "Offline replay experiment only; no live detector was changed.",
    }


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--recording", required=True, type=Path)
    parser.add_argument("--trajectory", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--loadings", default="0.25,1.0,4.0")
    parser.add_argument("--subbands", type=int, default=24)
    parser.add_argument("--cpi", type=float, default=.5)
    parser.add_argument("--fft-size", type=int, default=2048)
    parser.add_argument("--max-delay-km", type=float, default=64)
    parser.add_argument("--max-doppler-hz", type=float, default=400)
    parser.add_argument("--guard", type=int, default=2)
    parser.add_argument("--training", type=int, default=8)
    parser.add_argument("--controls-stride", type=int, default=2)
    args = parser.parse_args()
    loadings = [float(value) for value in args.loadings.split(",")]
    if not loadings or any(value <= 0 for value in loadings):
        parser.error("loadings must be positive")
    if args.subbands < 4 or args.subbands > args.fft_size:
        parser.error("subbands must be between 4 and fft-size")
    result = analyze(args.recording, args.trajectory, loadings, args.subbands,
                     args.cpi, args.fft_size, args.max_delay_km,
                     args.max_doppler_hz, args.guard, args.training,
                     args.controls_stride)
    args.output.write_text(json.dumps(result, indent=2) + "\n")


if __name__ == "__main__":
    main()
