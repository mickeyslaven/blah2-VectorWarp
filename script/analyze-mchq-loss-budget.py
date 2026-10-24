#!/usr/bin/env python3
"""Measure acquisition and array-coherence losses in Kraken MCHQ recordings."""

import argparse
import json
import math
from pathlib import Path
import struct

import numpy as np


MAGIC = 0x4D434851
SAMPLE_RATE = 2_400_000
ATSC_CHANNEL_BANDWIDTH = 6_000_000


def db(value):
    return 10 * math.log10(max(float(value), 1e-30))


def circular_std(phases):
    if len(phases) < 2:
        return 0.0
    concentration = abs(np.mean(np.exp(1j * np.asarray(phases))))
    return math.sqrt(max(0.0, -2 * math.log(max(concentration, 1e-15))))


def best_lag(reference, value, radius=4):
    best = (0.0, 0, 0j)
    for lag in range(-radius, radius + 1):
        if lag < 0:
            left, right = reference[-lag:], value[:lag]
        elif lag > 0:
            left, right = reference[:-lag], value[lag:]
        else:
            left, right = reference, value
        cross = np.vdot(left, right)
        coherence = abs(cross) / math.sqrt(max(
            float(np.vdot(left, left).real * np.vdot(right, right).real), 1e-30))
        if coherence > best[0]:
            best = (float(coherence), lag, complex(cross))
    return best


def read_exact(stream, size):
    value = stream.read(size)
    if len(value) != size:
        raise ValueError("truncated MCHQ recording")
    return value


def iter_mchq(path):
    with path.open("rb") as stream:
        while True:
            header = stream.read(32)
            if not header:
                return
            if len(header) != 32:
                raise ValueError("truncated MCHQ header")
            magic, channels, samples, phase, noise, counter, group, retune = struct.unpack(
                ">8I", header)
            if magic != MAGIC or not 1 <= channels <= 16 or not samples:
                raise ValueError("malformed MCHQ header")
            metadata = struct.unpack("<" + "f" * (channels * 2),
                                     read_exact(stream, channels * 8))
            frequencies = metadata[::2]
            gains = metadata[1::2]
            raw = np.frombuffer(read_exact(stream, channels * samples * 2), dtype=np.uint8)
            raw = raw.reshape(channels, samples, 2)
            yield ({"channels": channels, "samples": samples, "phase": phase,
                    "noise": noise, "frequency_change_counter": counter,
                    "group": group, "retune": retune,
                    "frequencies": frequencies, "gains": gains}, raw)


def spectral_metrics(reference):
    count = min(reference.size, 262144)
    if count < 1024:
        return {}
    value = reference[:count] - np.mean(reference[:count])
    window = np.hanning(count)
    power = np.abs(np.fft.fftshift(np.fft.fft(value * window))) ** 2
    power /= max(float(np.max(power)), 1e-30)
    occupied_20db = float(np.mean(power >= .01))
    occupied_10db = float(np.mean(power >= .1))
    return {"occupied_fraction_above_minus_20db": occupied_20db,
            "occupied_fraction_above_minus_10db": occupied_10db,
            "spectral_p90_over_median_db": db(np.quantile(power, .9) /
                                               max(np.median(power), 1e-30))}


def held_out_cancellation(raw):
    frame = 4096
    blocks = raw.shape[1] // frame
    if blocks < 8 or raw.shape[0] < 2:
        return []
    raw = raw[:, :blocks * frame]
    result = []
    window = np.hanning(frame)
    train = blocks // 2
    for channel in range(raw.shape[0]):
        others = np.delete(raw, channel, axis=0)
        covariance = others @ np.conj(others.T) / others.shape[1]
        _, vectors = np.linalg.eigh(covariance)
        reference = np.conj(vectors[:, -1]) @ others
        x = reference.reshape(blocks, frame)
        y = raw[channel].reshape(blocks, frame)
        xf = np.fft.fft(x * window, axis=1)
        yf = np.fft.fft(y * window, axis=1)
        denominator = np.sum(np.abs(xf[:train]) ** 2, axis=0) + 1e-12
        transfer = np.sum(yf[:train] * np.conj(xf[:train]), axis=0) / denominator
        prediction = transfer[None, :] * xf[train:]
        input_power = np.mean(np.abs(yf[train:]) ** 2)
        residual_power = np.mean(np.abs(yf[train:] - prediction) ** 2)
        result.append({"channel": channel,
                       "predictable_common_signal_db": db(input_power / residual_power)})
    return result


def analyze(path, analysis_stride=16, contiguous_samples=262144):
    histogram = None
    covariance = None
    sums = None
    sum_power = None
    observations = 0
    total_samples = 0
    blocks = 0
    first = None
    fixed = None
    contiguous = []
    contiguous_count = 0
    pair_observations = None
    state_counts = {}
    metadata_stable = True
    for header, raw in iter_mchq(path):
        if first is None:
            first = header
            channels = header["channels"]
            histogram = np.zeros((channels, 256), dtype=np.int64)
            covariance = np.zeros((channels, channels), dtype=np.complex128)
            sums = np.zeros(channels, dtype=np.complex128)
            sum_power = np.zeros(channels, dtype=np.float64)
            pair_observations = [[] for _ in range(channels)]
            fixed = {key: header[key] for key in ("channels", "frequencies", "gains",
                                                  "frequency_change_counter", "group")}
        if header["channels"] != first["channels"]:
            raise ValueError("channel count changed inside recording")
        metadata_stable = metadata_stable and all(
            header[key] == fixed[key] for key in fixed)
        for channel in range(raw.shape[0]):
            histogram[channel] += np.bincount(raw[channel].ravel(), minlength=256)
        values = ((raw[:, ::analysis_stride, 0].astype(np.float64) - 127.5) +
                  1j * (raw[:, ::analysis_stride, 1].astype(np.float64) - 127.5)) / 127.5
        covariance += values @ np.conj(values.T)
        sums += np.sum(values, axis=1)
        sum_power += np.sum(np.abs(values) ** 2, axis=1)
        observations += values.shape[1]
        lag_count = min(raw.shape[1], 32768)
        lag_values = ((raw[:, :lag_count, 0].astype(np.float64) - 127.5) +
                      1j * (raw[:, :lag_count, 1].astype(np.float64) - 127.5)) / 127.5
        lag_values -= np.mean(lag_values, axis=1, keepdims=True)
        for channel in range(1, raw.shape[0]):
            coherence, lag, cross = best_lag(lag_values[0], lag_values[channel])
            pair_observations[channel].append((coherence, lag, float(np.angle(cross))))
        if contiguous_count < contiguous_samples:
            used = min(raw.shape[1], contiguous_samples - contiguous_count)
            contiguous.append(((raw[:, :used, 0].astype(np.float64) - 127.5) +
                               1j * (raw[:, :used, 1].astype(np.float64) - 127.5)) / 127.5)
            contiguous_count += used
        state = f"phase_{header['phase']:x}_noise_{header['noise']}_retune_{header['retune']}"
        state_counts[state] = state_counts.get(state, 0) + 1
        total_samples += raw.shape[1]
        blocks += 1

    if first is None:
        raise ValueError("empty MCHQ recording")
    covariance /= observations
    means = sums / observations
    centered = covariance - means[:, None] * np.conj(means[None, :])
    eigenvalues, eigenvectors = np.linalg.eigh(centered)
    eigenvalues = np.maximum(eigenvalues.real, 0)
    trace = max(float(np.sum(eigenvalues)), 1e-30)
    principal = eigenvectors[:, -1]
    coherent_fraction = float(eigenvalues[-1] / trace)
    coherent_gain = db(eigenvalues[-1] / max(trace / len(eigenvalues), 1e-30))
    correlation = centered / np.sqrt(np.maximum(
        np.outer(np.diag(centered).real, np.diag(centered).real), 1e-30))
    channel_metrics = []
    component_total = histogram.shape[1] and int(np.sum(histogram[0]))
    component_axis = (np.arange(256) - 127.5) / 127.5
    for channel in range(first["channels"]):
        cumulative = np.cumsum(histogram[channel])
        q999_index = int(np.searchsorted(cumulative, .999 * component_total))
        channel_metrics.append({
            "channel": channel,
            "gain_db": first["gains"][channel],
            "dc_magnitude_fraction": float(abs(means[channel])),
            "rms_fraction": math.sqrt(max(float(sum_power[channel] / observations), 0)),
            "component_q999_fraction": float(abs(component_axis[q999_index])),
            "near_rail_fraction": float(np.sum(histogram[channel, :2]) +
                                        np.sum(histogram[channel, -2:])) / component_total,
            "rail_fraction": float(histogram[channel, 0] + histogram[channel, -1]) /
                             component_total,
            "principal_weight_magnitude": float(abs(principal[channel])),
            "principal_weight_phase_rad": float(np.angle(principal[channel])),
        })
    pairs = []
    for channel in range(1, first["channels"]):
        values = pair_observations[channel]
        coherences = [item[0] for item in values]
        lags = [item[1] for item in values]
        phases = [item[2] for item in values]
        pairs.append({"channels": [0, channel],
                      "normalized_correlation_median": float(np.median(coherences)),
                      "normalized_correlation_min": float(np.min(coherences)),
                      "integer_lag_median_samples": float(np.median(lags)),
                      "integer_lag_span_samples": int(max(lags) - min(lags)),
                      "relative_phase_circular_std_rad": circular_std(phases)})
    contiguous_raw = np.concatenate(contiguous, axis=1)
    contiguous_centered = contiguous_raw - np.mean(contiguous_raw, axis=1, keepdims=True)
    reference = np.conj(principal) @ contiguous_centered
    return {
        "path": str(path),
        "format": "mchq",
        "sample_rate_hz": SAMPLE_RATE,
        "center_frequency_hz": float(np.median(first["frequencies"])),
        "channels": first["channels"],
        "blocks": blocks,
        "samples_per_channel": total_samples,
        "duration_s": total_samples / SAMPLE_RATE,
        "state_counts": state_counts,
        "metadata_stable": metadata_stable,
        "channel_metrics": channel_metrics,
        "array": {
            "covariance_eigenvalues": [float(value) for value in eigenvalues],
            "principal_coherent_fraction": coherent_fraction,
            "principal_coherent_gain_db": coherent_gain,
            "zero_lag_correlation_magnitude": np.abs(correlation).tolist(),
            "block_pair_stability": pairs,
        },
        "spectrum": spectral_metrics(reference),
        "held_out_cancellation": held_out_cancellation(contiguous_centered),
        "waveform_reconstruction": {
            "assumed_broadcast_channel_bandwidth_hz": ATSC_CHANNEL_BANDWIDTH,
            "captured_bandwidth_fraction": SAMPLE_RATE / ATSC_CHANNEL_BANDWIDTH,
            "full_channel_reconstruction_possible": SAMPLE_RATE >= ATSC_CHANNEL_BANDWIDTH,
            "reason": "capture sample rate is narrower than the broadcast channel"
                      if SAMPLE_RATE < ATSC_CHANNEL_BANDWIDTH else "bandwidth is sufficient",
        },
    }


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--recording", action="append", required=True,
                        help="LABEL=PATH; may be repeated")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--analysis-stride", type=int, default=16)
    args = parser.parse_args()
    if args.analysis_stride < 1:
        parser.error("--analysis-stride must be positive")
    recordings = []
    for item in args.recording:
        if "=" not in item:
            parser.error("--recording must be LABEL=PATH")
        label, value = item.split("=", 1)
        result = analyze(Path(value), args.analysis_stride)
        result["label"] = label
        recordings.append(result)
    document = {"schema": "vectorwarp-mchq-loss-budget-v1",
                "recordings": recordings}
    args.output.write_text(json.dumps(document, indent=2) + "\n")
    print(json.dumps(document, indent=2))


if __name__ == "__main__":
    main()
