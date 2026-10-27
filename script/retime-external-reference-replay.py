#!/usr/bin/env python3
"""Apply waveform-only residual CFO tracking to an existing six-channel replay."""

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


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("input", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("receipt", type=Path)
    parser.add_argument("--pilot-channel", type=int, default=2,
                        help="zero-based Kraken channel, excluding OWL")
    parser.add_argument("--cpi", type=float, default=.5)
    args = parser.parse_args()
    analyzer = load(Path(__file__).with_name(
        "analyze-external-reference-cancellation.py"), "cancellation")
    qualifier = load(Path(__file__).with_name(
        "qualify-external-reference.py"), "qualification")
    builder = load(Path(__file__).with_name(
        "build-external-reference-replay.py"), "builder")
    data, rate, frequency = analyzer.read_blah2iq(args.input)
    if data.shape[0] != 6 or not 0 <= args.pilot_channel < 5:
        raise ValueError("expected OWL plus five Kraken channels")
    cpi = int(round(args.cpi * rate))
    blocks, rows = [], []
    for begin in range(0, data.shape[1] - cpi + 1, cpi):
        block = data[:, begin:begin + cpi].copy()
        external, pilot = block[0], block[args.pilot_channel + 1]
        before = abs(np.vdot(external, pilot)) / max(
            np.linalg.norm(external) * np.linalg.norm(pilot), 1e-30)
        cfo, residual = qualifier.refine_cfo(
            pilot, external, rate, 0.0, max_residual_hz=5.0)
        local_time = np.arange(cpi, dtype=np.float64) / rate
        external *= np.exp(2j * np.pi * cfo * local_time)
        cross = np.vdot(external, pilot)
        if abs(cross):
            external *= np.exp(1j * np.angle(cross))
        external_rms = math.sqrt(float(np.mean(np.abs(external) ** 2)))
        pilot_rms = math.sqrt(float(np.mean(np.abs(pilot) ** 2)))
        external *= pilot_rms / max(external_rms, 1e-30)
        after = abs(np.vdot(external, pilot)) / max(
            np.linalg.norm(external) * np.linalg.norm(pilot), 1e-30)
        block[0] = external
        blocks.append(block)
        rows.append({"first_sample": begin, "residual_cfo_hz": residual,
                     "coherence_before": float(before),
                     "coherence_after": float(after)})
    samples = builder.write_blah2iq(args.output, 6, rate, frequency, blocks)
    receipt = {"schema": "vectorwarp-external-reference-retime-v1",
               "timing_contract": "waveform-only residual CFO; no LAN arrival time",
               "input": str(args.input), "input_sha256": sha256(args.input),
               "output": str(args.output), "output_sha256": sha256(args.output),
               "frequency_hz": frequency, "sample_rate_hz": rate,
               "samples_per_channel": samples, "cpi_s": args.cpi,
               "pilot_channel": args.pilot_channel, "cpis": rows}
    args.receipt.write_text(json.dumps(receipt, indent=2, sort_keys=True) + "\n")
    print(json.dumps({"cpis": len(rows),
                      "median_residual_cfo_hz": float(np.median(
                          [row["residual_cfo_hz"] for row in rows])),
                      "median_coherence_before": float(np.median(
                          [row["coherence_before"] for row in rows])),
                      "median_coherence_after": float(np.median(
                          [row["coherence_after"] for row in rows])),
                      "output_sha256": receipt["output_sha256"]}, indent=2))


if __name__ == "__main__":
    main()
