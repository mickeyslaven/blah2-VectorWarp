#!/usr/bin/env python3
"""Measure whether an independently clocked external reference can lock.

This is a qualification tool, not a timestamp converter.  Recorder arrival
times select a bounded search region; waveform correlation estimates delay,
sample-clock slope, carrier offset and complex coherence.  A failed waveform
lock never falls back to treating LAN arrival time as RF time.
"""

import argparse
import json
import math
import struct
from pathlib import Path

import numpy as np


def read_mchq(path):
    channels = None
    frequency = None
    blocks = []
    with path.open("rb") as source:
        while True:
            fixed = source.read(32)
            if not fixed:
                break
            if len(fixed) != 32:
                raise ValueError("truncated MCHQ header")
            magic, count_channels, samples, *_ = struct.unpack(">8I", fixed)
            if magic != 0x4D434851 or not 1 <= count_channels <= 8 or not samples:
                raise ValueError("invalid MCHQ frame")
            if channels is None:
                channels = count_channels
            if channels != count_channels:
                raise ValueError("changing MCHQ channel count")
            metadata = source.read(channels * 8)
            if len(metadata) != channels * 8:
                raise ValueError("truncated MCHQ metadata")
            values = struct.unpack(f"<{channels * 2}f", metadata)
            frequencies = np.asarray(values[0::2])
            gains = np.asarray(values[1::2])
            if frequency is None:
                frequency = int(round(float(frequencies[0])))
            if not np.allclose(frequencies, frequency, atol=1, rtol=0):
                raise ValueError("changing MCHQ frequency")
            payload = source.read(samples * channels * 2)
            if len(payload) != samples * channels * 2:
                raise ValueError("truncated MCHQ payload")
            raw = np.frombuffer(payload, np.uint8).reshape(channels, samples, 2)
            decoded = ((raw.astype(np.float32) -
                        raw.mean(axis=1, keepdims=True, dtype=np.float64)) / 127.5)
            decoded *= np.power(10.0, (np.min(gains) - gains) / 20.0)[:, None, None]
            blocks.append(decoded[..., 0] + 1j * decoded[..., 1])
    if not blocks:
        raise ValueError("empty MCHQ recording")
    return np.concatenate(blocks, axis=1), frequency


def read_owl(root, ledger, start_s, end_s):
    rows = [json.loads(line) for line in ledger.read_text().splitlines() if line.strip()]
    chosen = []
    for row in rows:
        duration = row["samples"] / row["sample_rate_hz"]
        frame_end = row["received_unix_ns"] / 1e9
        if frame_end >= start_s - 2.0 and frame_end - duration <= end_s + 2.0:
            path = root / row["segment"]
            if path.is_file():
                chosen.append((row, path))
    chosen.sort(key=lambda value: (value[0]["epoch"], value[0]["first_sample"]))
    if not chosen:
        raise ValueError("no sealed OWL frames overlap the requested host-time window")
    epoch = chosen[0][0]["epoch"]
    selected = []
    expected = None
    for row, path in chosen:
        if row["epoch"] != epoch:
            continue
        if expected is not None and row["first_sample"] != expected:
            raise ValueError("OWL selection contains a source-sample gap")
        with path.open("rb") as source:
            source.seek(row["segment_offset_bytes"])
            payload = source.read(row["payload_bytes"])
        if len(payload) != row["payload_bytes"]:
            raise ValueError("OWL segment is truncated")
        raw = np.frombuffer(payload, dtype="<i2").reshape(-1, 2)
        selected.append(raw[:, 0].astype(np.float32) + 1j * raw[:, 1].astype(np.float32))
        expected = row["end_sample"]
    first = chosen[0][0]
    # Export occurs immediately after a complete CPI is extracted.  This is
    # only a search seed; waveform lock below is the synchronization evidence.
    first_end_s = first["received_unix_ns"] / 1e9
    approximate_start_s = first_end_s - first["samples"] / first["sample_rate_hz"]
    return np.concatenate(selected), first["sample_rate_hz"], approximate_start_s, epoch


def fft_correlation(template, search):
    if len(search) < len(template):
        raise ValueError("correlation search is shorter than template")
    size = 1 << (len(search) + len(template) - 1).bit_length()
    corr = np.fft.ifft(np.fft.fft(search, size) *
                       np.fft.fft(np.conj(template[::-1]), size))
    return corr[len(template) - 1:len(search)]


def block_power(value, block):
    usable = len(value) // block * block
    return (np.abs(value[:usable]).reshape(-1, block) ** 2).mean(axis=1)


def linear_resample(value, source_start, source_per_output, count):
    position = source_start + np.arange(count, dtype=np.float64) * source_per_output
    left = np.floor(position).astype(np.int64)
    fraction = position - left
    if left[0] < 0 or left[-1] + 1 >= len(value):
        raise ValueError("resample request is outside the OWL recording")
    return (value[left] * (1.0 - fraction) + value[left + 1] * fraction).astype(np.complex64)


def coarse_offset(hp, owl, hp_rate, owl_rate, seed_samples, radius_s=1.0):
    common = 10_000
    hp_block = hp_rate // common
    owl_block = owl_rate // common
    duration = min(2.0, len(hp) / hp_rate - 0.1)
    template = block_power(hp[:int(duration * hp_rate)], hp_block)
    template = (template - template.mean()) / max(template.std(), 1e-12)
    seed = int(seed_samples / owl_block)
    radius = int(radius_s * common)
    begin = max(0, seed - radius)
    end = min(len(owl) // owl_block, seed + radius + len(template))
    search = block_power(owl[begin * owl_block:end * owl_block], owl_block)
    search = (search - search.mean()) / max(search.std(), 1e-12)
    corr = fft_correlation(template.astype(np.complex64), search.astype(np.complex64)).real
    index = int(np.argmax(corr))
    coefficient = float(corr[index] / max(len(template), 1))
    return (begin + index) * owl_block, coefficient


def fine_lock(hp, owl, hp_rate, owl_rate, hp_start, owl_start, duration_s=0.08):
    count = int(duration_s * hp_rate)
    margin = int(0.002 * hp_rate)
    template = hp[hp_start:hp_start + count].astype(np.complex64)
    source_per_output = owl_rate / hp_rate
    search = linear_resample(owl, owl_start - margin * source_per_output,
                             source_per_output, count + 2 * margin)
    template_envelope = np.abs(template)
    search_envelope = np.abs(search)
    template_envelope -= template_envelope.mean()
    search_envelope -= search_envelope.mean()
    envelope_corr = fft_correlation(template_envelope.astype(np.complex64),
                                    search_envelope.astype(np.complex64))
    delay = int(np.argmax(np.abs(envelope_corr)))
    center = search[delay:delay + count]
    product = template * np.conj(center)
    fft_size = 1 << (len(product) - 1).bit_length()
    spectrum = np.fft.fftshift(np.fft.fft(product, fft_size))
    peak = int(np.argmax(np.abs(spectrum)))
    cfo = (peak - fft_size // 2) * hp_rate / fft_size
    time = np.arange(count, dtype=np.float64) / hp_rate
    aligned = center * np.exp(2j * np.pi * cfo * time)
    cross = np.vdot(aligned, template)
    coherence = float(abs(cross) / max(np.linalg.norm(aligned) * np.linalg.norm(template), 1e-20))
    owl_index = owl_start + (delay - margin) * source_per_output
    return {"hp_sample": hp_start, "owl_sample": float(owl_index),
            "cfo_hz": float(cfo), "coherence": coherence,
            "fine_delay_hp_samples": delay - margin}


def affine_lock(hp, owl, hp_rate, hp_start, owl_start,
                owl_samples_per_hp_sample, duration_s=0.45):
    """Score one long CPI after applying the waveform-derived clock map."""
    count = min(int(duration_s * hp_rate), len(hp) - hp_start)
    if count <= 0:
        raise ValueError("empty affine lock window")
    template = hp[hp_start:hp_start + count].astype(np.complex64)
    aligned_clock = linear_resample(owl, owl_start,
                                    owl_samples_per_hp_sample, count)
    product = template * np.conj(aligned_clock)
    fft_size = 1 << (len(product) - 1).bit_length()
    spectrum = np.fft.fftshift(np.fft.fft(product, fft_size))
    peak = int(np.argmax(np.abs(spectrum)))
    cfo = (peak - fft_size // 2) * hp_rate / fft_size
    time = np.arange(count, dtype=np.float64) / hp_rate
    aligned = aligned_clock * np.exp(2j * np.pi * cfo * time)
    cross = np.vdot(aligned, template)
    coherence = float(abs(cross) /
                      max(np.linalg.norm(aligned) * np.linalg.norm(template), 1e-20))
    return {"hp_sample": hp_start, "owl_sample": float(owl_start),
            "duration_s": count / hp_rate, "cfo_hz": float(cfo),
            "coherence": coherence}


def qualify(hp_channels, owl, hp_rate, owl_rate, seed_samples):
    candidates = []
    for channel, hp in enumerate(hp_channels):
        coarse, envelope = coarse_offset(hp, owl, hp_rate, owl_rate, seed_samples)
        locks = []
        for seconds in np.arange(0.25, min(len(hp) / hp_rate - 0.2, 3.75), 0.5):
            hp_start = int(seconds * hp_rate)
            owl_start = coarse + seconds * owl_rate
            try:
                locks.append(fine_lock(hp, owl, hp_rate, owl_rate,
                                       hp_start, owl_start))
            except ValueError:
                pass
        accepted = [row for row in locks if row["coherence"] >= 0.10]
        drift_ppm = None
        if len(accepted) >= 3:
            slope, intercept = np.polyfit([row["hp_sample"] for row in accepted],
                                          [row["owl_sample"] for row in accepted], 1)
            drift_ppm = float((slope / (owl_rate / hp_rate) - 1.0) * 1e6)
        candidates.append({"kraken_channel": channel,
                           "coarse_owl_sample": int(coarse),
                           "envelope_correlation": envelope,
                           "locks": locks, "accepted_locks": len(accepted),
                           "sample_clock_offset_ppm": drift_ppm,
                           "affine_owl_samples_per_hp_sample": (
                               float(slope) if len(accepted) >= 3 else None),
                           "affine_intercept_owl_samples": (
                               float(intercept) if len(accepted) >= 3 else None),
                           "median_coherence": (float(np.median([x["coherence"] for x in accepted]))
                                                if accepted else 0.0),
                           "median_cfo_hz": (float(np.median([x["cfo_hz"] for x in accepted]))
                                             if accepted else None)})
    candidates.sort(key=lambda row: (row["accepted_locks"], row["median_coherence"]),
                    reverse=True)
    best = candidates[0]
    best["affine_cpi_locks"] = []
    if best["affine_owl_samples_per_hp_sample"] is not None:
        slope = best["affine_owl_samples_per_hp_sample"]
        intercept = best["affine_intercept_owl_samples"]
        for seconds in np.arange(0.25, min(len(hp_channels[best["kraken_channel"]]) /
                                           hp_rate - 0.45, 3.75), 0.5):
            hp_start = int(seconds * hp_rate)
            try:
                best["affine_cpi_locks"].append(affine_lock(
                    hp_channels[best["kraken_channel"]], owl, hp_rate, hp_start,
                    intercept + slope * hp_start, slope))
            except ValueError:
                pass
    best["median_affine_cpi_coherence"] = (
        float(np.median([row["coherence"] for row in best["affine_cpi_locks"]]))
        if best["affine_cpi_locks"] else 0.0)
    qualified = (best["accepted_locks"] >= 3 and
                 best["median_coherence"] >= 0.15 and
                 len(best["affine_cpi_locks"]) >= 3 and
                 best["median_affine_cpi_coherence"] >= 0.15 and
                 best["sample_clock_offset_ppm"] is not None and
                 abs(best["sample_clock_offset_ppm"]) <= 200)
    return qualified, candidates


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--owl-root", type=Path, required=True)
    parser.add_argument("--owl-ledger", type=Path, required=True)
    parser.add_argument("--kraken", type=Path, required=True)
    parser.add_argument("--kraken-metadata", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    metadata = json.loads(args.kraken_metadata.read_text())
    hp, frequency = read_mchq(args.kraken)
    if frequency != int(metadata["frequency"]):
        raise ValueError("MCHQ and sidecar frequency differ")
    hp_rate = int(metadata["sampleRate"])
    start_s, end_s = metadata["windowStart"] / 1000, metadata["windowEnd"] / 1000
    owl, owl_rate, owl_start_s, epoch = read_owl(args.owl_root, args.owl_ledger,
                                                 start_s, end_s)
    seed = (start_s - owl_start_s) * owl_rate
    qualified, candidates = qualify(hp, owl, hp_rate, owl_rate, seed)
    output = {"schema": "vectorwarp-external-reference-lock-v1",
              "qualified": qualified, "frequency_hz": frequency,
              "kraken_rate_hz": hp_rate, "owl_rate_hz": owl_rate,
              "owl_epoch": str(epoch),
              "timing_contract": "waveform-derived; LAN arrival used only as a bounded search seed",
              "acceptance": {"minimum_locks": 3, "minimum_median_coherence": 0.15,
                             "minimum_affine_cpi_locks": 3,
                             "minimum_median_affine_cpi_coherence": 0.15,
                             "maximum_absolute_clock_offset_ppm": 200},
              "best": candidates[0], "candidates": candidates}
    args.output.write_text(json.dumps(output, indent=2, sort_keys=True) + "\n")
    print(json.dumps({"qualified": qualified, "best": candidates[0]}, indent=2))


if __name__ == "__main__":
    main()
