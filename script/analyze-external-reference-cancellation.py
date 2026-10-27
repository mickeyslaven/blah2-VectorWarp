#!/usr/bin/env python3
"""Measure held-out direct-path cancellation for external/internal references.

The input is the six-channel replay produced by build-external-reference-replay:
channel 0 is OWL and channels 1..5 are the unchanged Kraken channels.  Transfer
functions are learned only on the first half of each CPI and scored on the
second half.  Results therefore measure predictive reference quality rather
than same-sample subtraction or self-inclusion.
"""

import argparse
import hashlib
import json
import math
import statistics
import struct
from pathlib import Path

import numpy as np


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def read_blah2iq(path):
    with path.open("rb") as source:
        header = source.read(64)
        if len(header) != 64 or header[:8] != b"BLAH2IQ\0":
            raise ValueError("invalid BLAH2IQ header")
        _, _, channels, sample_rate, frequency, _ = struct.unpack_from(
            "<IIIIII", header, 8)
        blocks = []
        while True:
            block = source.read(16)
            if not block:
                break
            if len(block) != 16:
                raise ValueError("truncated BLAH2IQ block header")
            samples, _, _ = struct.unpack("<IIQ", block)
            payload = source.read(samples * channels * 8)
            if len(payload) != samples * channels * 8:
                raise ValueError("truncated BLAH2IQ payload")
            blocks.append(np.frombuffer(payload, "<c8").reshape(channels, samples))
    if not blocks:
        raise ValueError("empty BLAH2IQ recording")
    return np.concatenate(blocks, axis=1), sample_rate, frequency


def eigen_reference(channels):
    centered = channels - channels.mean(axis=1, keepdims=True)
    stride = max(1, centered.shape[1] // 32768)
    sample = centered[:, ::stride]
    covariance = sample @ sample.conj().T / sample.shape[1]
    _, vectors = np.linalg.eigh(covariance)
    weight = vectors[:, -1]
    return np.asarray(weight.conj() @ centered, np.complex64)


def scalar_cancellation(target, reference):
    count = min(len(target), len(reference))
    split = count // 2
    x0, y0 = reference[:split], target[:split]
    coefficient = np.vdot(x0, y0) / max(np.vdot(x0, x0).real, 1e-30)
    x1, y1 = reference[split:count], target[split:count]
    residual = y1 - coefficient * x1
    return 10 * math.log10(max(np.mean(np.abs(y1) ** 2), 1e-30) /
                             max(np.mean(np.abs(residual) ** 2), 1e-30))


def frequency_selective_cancellation(target, reference, frame=4096):
    blocks = min(len(target), len(reference)) // frame
    if blocks < 8:
        raise ValueError("insufficient samples for held-out cancellation")
    used = blocks * frame
    x = reference[:used].reshape(blocks, frame)
    y = target[:used].reshape(blocks, frame)
    window = np.hanning(frame)
    xf = np.fft.fft(x * window, axis=1)
    yf = np.fft.fft(y * window, axis=1)
    train = blocks // 2
    transfer = (np.sum(yf[:train] * np.conj(xf[:train]), axis=0) /
                (np.sum(np.abs(xf[:train]) ** 2, axis=0) + 1e-9))
    held_y, held_x = yf[train:], xf[train:]
    residual = held_y - transfer[None, :] * held_x
    before = np.mean(np.abs(held_y) ** 2)
    after = np.mean(np.abs(residual) ** 2)
    return 10 * math.log10(max(before, 1e-30) / max(after, 1e-30))


def multi_reference_cancellation(target, references, frame=4096,
                                 diagonal_loading=1e-3):
    """Held-out MISO cancellation showing incremental reference information."""
    count = min([len(target), *(len(value) for value in references)])
    blocks = count // frame
    if blocks < 8:
        raise ValueError("insufficient samples for held-out cancellation")
    used = blocks * frame
    window = np.hanning(frame)
    yf = np.fft.fft(target[:used].reshape(blocks, frame) * window, axis=1)
    xf = np.stack([
        np.fft.fft(value[:used].reshape(blocks, frame) * window, axis=1)
        for value in references], axis=1)  # blocks, references, frequency
    train = blocks // 2
    x0 = np.moveaxis(xf[:train], 2, 0)  # frequency, blocks, references
    y0 = np.moveaxis(yf[:train], 1, 0)  # frequency, blocks
    gram = np.einsum("fbr,fbs->frs", np.conj(x0), x0)
    scale = np.trace(gram, axis1=1, axis2=2).real / len(references)
    gram += (diagonal_loading * scale)[:, None, None] * np.eye(len(references))
    cross = np.einsum("fbr,fb->fr", np.conj(x0), y0)
    weights = np.linalg.solve(gram, cross[..., None])[..., 0]
    held_x = np.moveaxis(xf[train:], 2, 0)
    estimate = np.einsum("fbr,fr->fb", held_x, weights)
    held_y = np.moveaxis(yf[train:], 1, 0)
    residual = held_y - estimate
    before = np.mean(np.abs(held_y) ** 2)
    after = np.mean(np.abs(residual) ** 2)
    return 10 * math.log10(max(before, 1e-30) / max(after, 1e-30))


def normalized_coherence(target, reference):
    cross = np.vdot(reference, target)
    return float(abs(cross) / max(np.linalg.norm(reference) *
                                  np.linalg.norm(target), 1e-30))


def summarize(values):
    return {"median": float(statistics.median(values)),
            "minimum": float(min(values)), "maximum": float(max(values))}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("recording", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--cpi", type=float, default=.5)
    parser.add_argument("--max-cpis", type=int, default=8)
    args = parser.parse_args()
    data, rate, frequency = read_blah2iq(args.recording)
    if data.shape[0] != 6:
        raise ValueError("expected OWL plus five Kraken channels")
    cpi = int(round(args.cpi * rate))
    rows = []
    for begin in list(range(0, data.shape[1] - cpi + 1, cpi))[:args.max_cpis]:
        external = data[0, begin:begin + cpi]
        physical = data[1:, begin:begin + cpi]
        production = eigen_reference(physical)
        for channel in range(5):
            target = physical[channel]
            leave_one_out = eigen_reference(np.delete(physical, channel, axis=0))
            for name, reference, fair in (
                    ("owl_external", external, True),
                    ("kraken_leave_one_out", leave_one_out, True),
                    ("kraken_production_self_inclusive", production, False)):
                rows.append({"cpi_first_sample": begin, "channel": channel,
                             "reference": name, "fair_held_out": fair,
                             "coherence": normalized_coherence(target, reference),
                             "scalar_cancellation_db": scalar_cancellation(target, reference),
                             "frequency_selective_cancellation_db":
                             frequency_selective_cancellation(target, reference)})
            rows.append({"cpi_first_sample": begin, "channel": channel,
                         "reference": "owl_plus_kraken_leave_one_out",
                         "fair_held_out": True,
                         "coherence": normalized_coherence(target, leave_one_out),
                         "scalar_cancellation_db": scalar_cancellation(
                             target, leave_one_out),
                         "frequency_selective_cancellation_db":
                         multi_reference_cancellation(
                             target, [leave_one_out, external])})
    summaries = {}
    for name in sorted(set(row["reference"] for row in rows)):
        selected = [row for row in rows if row["reference"] == name]
        summaries[name] = {
            "fair_held_out": selected[0]["fair_held_out"],
            "coherence": summarize([row["coherence"] for row in selected]),
            "scalar_cancellation_db": summarize(
                [row["scalar_cancellation_db"] for row in selected]),
            "frequency_selective_cancellation_db": summarize(
                [row["frequency_selective_cancellation_db"] for row in selected]),
            "per_channel_frequency_selective_median_db": [
                float(statistics.median(row["frequency_selective_cancellation_db"]
                                        for row in selected if row["channel"] == channel))
                for channel in range(5)],
        }
    result = {"schema": "vectorwarp-external-reference-cancellation-v1",
              "recording": str(args.recording),
              "recording_sha256": sha256(args.recording),
              "frequency_hz": frequency, "sample_rate_hz": rate,
              "cpi_s": args.cpi,
              "contract": "first-half transfer fit, second-half evaluation; self-inclusive result diagnostic only",
              "summaries": summaries, "rows": rows}
    args.output.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n")
    print(json.dumps(summaries, indent=2, sort_keys=True))


if __name__ == "__main__":
    main()
