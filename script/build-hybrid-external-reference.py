#!/usr/bin/env python3
"""Build a causal frequency-resolved OWL+Kraken reference replay.

Weights are learned from an initial calibration prefix and then frozen.  The
five Kraken surveillance channels remain byte-for-byte identical in value;
only channel zero is replaced with the hybrid reference.
"""

import argparse
import hashlib
import importlib.util
import json
import math
from pathlib import Path

import numpy as np


def load(path, name):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def learn_weights(inputs, training_samples, frame, hop, loading):
    window = np.sqrt(np.hanning(frame)).astype(np.float32)
    covariance = np.zeros((frame, inputs.shape[0], inputs.shape[0]), np.complex128)
    frames = 0
    for begin in range(0, training_samples - frame + 1, hop):
        spectrum = np.fft.fft(inputs[:, begin:begin + frame] * window, axis=1).T
        covariance += np.einsum("fc,fd->fcd", spectrum, np.conj(spectrum))
        frames += 1
    if frames < 8:
        raise ValueError("calibration prefix has too few spectral frames")
    covariance /= frames
    scale = np.trace(covariance, axis1=1, axis2=2).real / inputs.shape[0]
    covariance += (loading * scale)[:, None, None] * np.eye(inputs.shape[0])
    _, vectors = np.linalg.eigh(covariance)
    weights = vectors[:, :, -1]
    return weights, frames, window


def synthesize(inputs, weights, frame, hop, window, anchor):
    # Fix the otherwise arbitrary eigenvector phase to one physical input at
    # each frequency.  Without this, independently phased FFT bins create a
    # meaningless long impulse response.
    phase = np.angle(weights[:, anchor])
    weights = weights * np.exp(-1j * phase)[:, None]
    output = np.zeros(inputs.shape[1], np.complex128)
    denominator = np.zeros(inputs.shape[1], np.float64)
    for begin in range(0, inputs.shape[1] - frame + 1, hop):
        spectrum = np.fft.fft(inputs[:, begin:begin + frame] * window, axis=1).T
        reference = np.sum(np.conj(weights) * spectrum, axis=1)
        block = np.fft.ifft(reference) * window
        output[begin:begin + frame] += block
        denominator[begin:begin + frame] += window ** 2
    valid = denominator > 1e-6
    output[valid] /= denominator[valid]
    output[~valid] = inputs[anchor, ~valid]
    return output.astype(np.complex64)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("input", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("receipt", type=Path)
    parser.add_argument("--training-cpis", type=int, default=2)
    parser.add_argument("--cpi", type=float, default=.5)
    parser.add_argument("--frame", type=int, default=4096)
    parser.add_argument("--hop", type=int, default=2048)
    parser.add_argument("--diagonal-loading", type=float, default=1e-3)
    args = parser.parse_args()
    analyzer = load(Path(__file__).with_name(
        "analyze-external-reference-cancellation.py"), "cancellation")
    builder = load(Path(__file__).with_name(
        "build-external-reference-replay.py"), "builder")
    data, rate, frequency = analyzer.read_blah2iq(args.input)
    if data.shape[0] != 6:
        raise ValueError("expected OWL plus five Kraken channels")
    training_samples = int(round(args.training_cpis * args.cpi * rate))
    if training_samples >= data.shape[1] - int(round(args.cpi * rate)):
        raise ValueError("calibration prefix leaves no evaluation CPI")
    weights, training_frames, window = learn_weights(
        data, training_samples, args.frame, args.hop, args.diagonal_loading)
    hybrid = synthesize(data, weights, args.frame, args.hop, window, anchor=0)
    cpi_samples = int(round(args.cpi * rate))
    blocks, rms_scales = [], []
    for begin in range(0, data.shape[1] - cpi_samples + 1, cpi_samples):
        block = data[:, begin:begin + cpi_samples].copy()
        reference = hybrid[begin:begin + cpi_samples]
        target_rms = math.sqrt(float(np.mean(np.abs(block[3]) ** 2)))
        reference_rms = math.sqrt(float(np.mean(np.abs(reference) ** 2)))
        scale = target_rms / max(reference_rms, 1e-30)
        block[0] = reference * scale
        blocks.append(block)
        rms_scales.append(scale)
    written = builder.write_blah2iq(args.output, 6, rate, frequency, blocks)
    receipt = {
        "schema": "vectorwarp-hybrid-external-reference-v1",
        "input": str(args.input), "input_sha256": sha256(args.input),
        "output": str(args.output), "output_sha256": sha256(args.output),
        "frequency_hz": frequency, "sample_rate_hz": rate,
        "samples_per_channel": written, "cpi_s": args.cpi,
        "training_cpis": args.training_cpis,
        "evaluation_first_sample": training_samples,
        "spectral_frame": args.frame, "spectral_hop": args.hop,
        "diagonal_loading": args.diagonal_loading,
        "training_frames": training_frames,
        "inputs": ["owl_external", "kraken_ch0", "kraken_ch1",
                   "kraken_ch2", "kraken_ch3", "kraken_ch4"],
        "phase_anchor": "owl_external",
        "causality": "weights learned from calibration prefix and frozen",
        "rms_scales": rms_scales,
    }
    args.receipt.write_text(json.dumps(receipt, indent=2, sort_keys=True) + "\n")
    print(json.dumps({key: receipt[key] for key in (
        "output_sha256", "training_frames", "evaluation_first_sample")}, indent=2))


if __name__ == "__main__":
    main()
