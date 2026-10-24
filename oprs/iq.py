"""Portable IQ inspection and replay-only echo injection.

The initial adapter supports VectorWarp's self-describing BLAH2IQ v1 format.
It intentionally writes a new file with O_EXCL semantics and never touches live
receiver settings.
"""

from __future__ import annotations

import cmath
import math
import os
import random
import struct
from pathlib import Path
from typing import Any

from .common import ContractError, file_hash, percentile, require
from .contracts import validate_manifest


MAGIC = b"BLAH2IQ\0"
LIGHT_SPEED_MPS = 299_792_458.0
MAX_SAMPLES = 20_000_000


def read_blah2iq(path: Path, max_samples: int = MAX_SAMPLES) -> tuple[dict[str, int], list[list[complex]]]:
    with path.open("rb") as stream:
        header = stream.read(64)
        require(len(header) == 64 and header[:8] == MAGIC, "not a BLAH2IQ v1 recording")
        version, header_size, channels, sample_rate, frequency, encoding = struct.unpack_from("<IIIIII", header, 8)
        samples, complete = struct.unpack_from("<QQ", header, 40)
        require((version, header_size, encoding, complete) == (1, 64, 1, 1), "unsupported or incomplete BLAH2IQ recording")
        require(1 <= channels <= 8 and 0 < samples <= max_samples, "invalid or excessive recording dimensions")
        output = [[] for _ in range(channels)]
        position = 0
        while position < samples:
            block = stream.read(16)
            require(len(block) == 16, "truncated BLAH2IQ block header")
            count, reserved, first = struct.unpack("<IIQ", block)
            require(count and count <= 262144 and reserved == 0 and first == position, "invalid BLAH2IQ block sequence")
            payload = stream.read(count * channels * 8)
            require(len(payload) == count * channels * 8, "truncated BLAH2IQ payload")
            offset = 0
            for channel in range(channels):
                values = output[channel]
                for real, imag in struct.iter_unpack("<ff", payload[offset:offset + count * 8]):
                    require(math.isfinite(real) and math.isfinite(imag), "non-finite IQ sample")
                    values.append(complex(real, imag))
                offset += count * 8
            position += count
        require(stream.read(1) == b"", "trailing data in BLAH2IQ recording")
    return {"channels": channels, "sample_rate_hz": sample_rate, "center_frequency_hz": frequency,
            "samples_per_channel": samples}, output


def write_blah2iq(path: Path, metadata: dict[str, int], channels: list[list[complex]]) -> None:
    count = len(channels[0])
    require(channels and all(len(row) == count for row in channels), "IQ channels are not aligned")
    flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL
    descriptor = os.open(path, flags, 0o600)
    try:
        with os.fdopen(descriptor, "wb") as stream:
            header = bytearray(64)
            header[:8] = MAGIC
            struct.pack_into("<IIIIII", header, 8, 1, 64, len(channels), metadata["sample_rate_hz"],
                             metadata["center_frequency_hz"], 1)
            struct.pack_into("<QQ", header, 40, count, 1)
            stream.write(header)
            for first in range(0, count, 262144):
                size = min(262144, count - first)
                stream.write(struct.pack("<IIQ", size, 0, first))
                for row in channels:
                    for value in row[first:first + size]:
                        stream.write(struct.pack("<ff", value.real, value.imag))
    except Exception:
        path.unlink(missing_ok=True)
        raise


def _rms(values: list[complex]) -> float:
    return math.sqrt(sum(value.real * value.real + value.imag * value.imag for value in values) / len(values))


def analyze_iq(path: Path, manifest: dict[str, Any]) -> dict[str, Any]:
    validate_manifest(manifest)
    meta, channels = read_blah2iq(path)
    require(meta["channels"] == len(manifest["channels"]), "recording/manifest channel-count mismatch")
    require(meta["sample_rate_hz"] == manifest["sample_rate_hz"], "recording/manifest sample-rate mismatch")
    require(meta["center_frequency_hz"] == manifest["center_frequency_hz"], "recording/manifest frequency mismatch")
    full_scale = manifest["full_scale"]
    metrics = []
    rms_values = []
    for descriptor, values in zip(manifest["channels"], channels):
        magnitudes = [abs(value) / full_scale for value in values]
        rms = _rms(values) / full_scale
        rms_values.append(rms)
        metrics.append({"channel_id": descriptor["channel_id"], "rms_fraction_full_scale": rms,
                        "p99_9_magnitude_fraction_full_scale": percentile(magnitudes, .999),
                        "peak_magnitude_fraction_full_scale": max(magnitudes),
                        "near_rail_fraction": sum(value >= .98 for value in magnitudes) / len(values),
                        "rail_fraction": sum(value >= 1.0 for value in magnitudes) / len(values)})
    imbalance = 20 * math.log10(max(rms_values) / min(rms_values)) if min(rms_values) > 0 else None
    coherence = None
    if len(channels) >= 2:
        a, b = channels[0], channels[1]
        cross = sum(x.conjugate() * y for x, y in zip(a, b))
        denominator = math.sqrt(sum(abs(x) ** 2 for x in a) * sum(abs(y) ** 2 for y in b))
        coherence = abs(cross) / denominator if denominator else None
    return {"schema": "oprs-iq-health-v1", "source": str(path), "source_sha256": file_hash(path),
            "metadata": meta, "valid_iq_seconds": meta["samples_per_channel"] / meta["sample_rate_hz"],
            "dropped_samples": None, "frame_gaps": None, "channels": metrics,
            "channel_rms_imbalance_db": imbalance, "zero_lag_channel_0_1_coherence": coherence,
            "limitations": ["Dropped samples and frame gaps require capture-side sequence telemetry; BLAH2IQ stores only completed contiguous samples."]}


def _interpolate(points: list[dict[str, Any]], time_s: float, field: str, default: float = 0.0) -> float:
    if not points:
        return default
    if time_s <= points[0]["time_s"]:
        return float(points[0].get(field, default))
    if time_s >= points[-1]["time_s"]:
        return float(points[-1].get(field, default))
    for left, right in zip(points, points[1:]):
        if left["time_s"] <= time_s <= right["time_s"]:
            fraction = (time_s - left["time_s"]) / (right["time_s"] - left["time_s"])
            return float(left.get(field, default)) * (1 - fraction) + float(right.get(field, default)) * fraction
    return default


def inject_blah2iq(source: Path, destination: Path, manifest: dict[str, Any], scenario: dict[str, Any]) -> dict[str, Any]:
    validate_manifest(manifest)
    require(scenario.get("schema") == "oprs-benchmark-scenario-v1", "unsupported scenario schema")
    require(scenario.get("replay_only") is True, "injection scenarios must be replay-only")
    meta, channels = read_blah2iq(source)
    require(meta["channels"] == len(manifest["channels"]), "source/manifest channel-count mismatch")
    by_id = {row["channel_id"]: i for i, row in enumerate(manifest["channels"])}
    source_id = scenario["source_channel_id"]
    require(source_id in by_id, "scenario source channel absent from manifest")
    source_index = by_id[source_id]
    if manifest["capability_profile"] == "REF_SURV_2CH":
        require("reference" in manifest["channels"][source_index]["roles"], "two-channel injection source must be reference")
    target_indices = [by_id[item] for item in scenario["inject_into_channel_ids"]]
    require(source_index not in target_indices, "reference/source channel must remain unchanged")
    if manifest["capability_profile"] == "REF_SURV_2CH":
        expected = [i for i, row in enumerate(manifest["channels"]) if "surveillance" in row["roles"]]
        require(target_indices == expected, "REF_SURV_2CH injection must target only its surveillance channel")
    points = sorted(scenario["trajectory"], key=lambda row: row["time_s"])
    require(points and all(points[i]["time_s"] < points[i + 1]["time_s"] for i in range(len(points) - 1)),
            "trajectory times must be strictly increasing")
    sample_rate = meta["sample_rate_hz"]
    phase = 0.0
    output = [list(row) for row in channels]
    geometry = (manifest.get("array_geometry") or {}).get("elements", {})
    frequency = meta["center_frequency_hz"]
    for sample in range(meta["samples_per_channel"]):
        time_s = sample / sample_rate
        delay_km = _interpolate(points, time_s, "delay_km")
        delay = round(delay_km * 1000 * sample_rate / LIGHT_SPEED_MPS)
        if delay < 0 or sample < delay:
            continue
        doppler = _interpolate(points, time_s, "doppler_hz")
        phase += 2 * math.pi * doppler / sample_rate
        level_db = _interpolate(points, time_s, "level_db_ref_rms", -160)
        amplitude = 10 ** (level_db / 20)
        azimuth = math.radians(_interpolate(points, time_s, "azimuth_deg"))
        base = channels[source_index][sample - delay] * amplitude * cmath.exp(1j * phase)
        for target in target_indices:
            channel_id = manifest["channels"][target]["channel_id"]
            position = geometry.get(channel_id, [0.0, 0.0, 0.0])
            spatial = -2 * math.pi * frequency / LIGHT_SPEED_MPS * (position[0] * math.cos(azimuth) + position[1] * math.sin(azimuth))
            output[target][sample] += base * cmath.exp(1j * spatial)
            for echo in scenario.get("multipath", []):
                extra = round(echo["extra_delay_km"] * 1000 * sample_rate / LIGHT_SPEED_MPS)
                if sample >= delay + extra:
                    output[target][sample] += channels[source_index][sample - delay - extra] * amplitude * \
                        10 ** (echo["relative_level_db"] / 20) * cmath.exp(1j * (phase + spatial + math.radians(echo.get("phase_deg", 0))))
    write_blah2iq(destination, meta, output)
    unchanged = all(output[source_index][i] == channels[source_index][i] for i in range(len(channels[source_index])))
    return {"schema": "oprs-injection-receipt-v1", "source_sha256": file_hash(source),
            "output_sha256": file_hash(destination), "scenario_id": scenario["scenario_id"],
            "source_channel_id": source_id, "source_channel_unchanged": unchanged,
            "normalization": "complex voltage dB relative to measured reference waveform RMS",
            "samples_per_channel": meta["samples_per_channel"]}


def make_fixture(path: Path, channels: int = 2, sample_rate: int = 20_000, seconds: float = 1.0,
                 frequency: int = 100_000_000, seed: int = 20261022) -> dict[str, Any]:
    require(channels >= 2 and sample_rate > 0 and seconds > 0, "invalid fixture dimensions")
    count = round(sample_rate * seconds)
    randomizer = random.Random(seed)
    reference = []
    for sample in range(count):
        carrier = .25 * cmath.exp(1j * 2 * math.pi * 1100 * sample / sample_rate)
        reference.append(carrier + complex(randomizer.gauss(0, .05), randomizer.gauss(0, .05)))
    rows = [reference]
    for channel in range(1, channels):
        rows.append([.7 * value + complex(randomizer.gauss(0, .08), randomizer.gauss(0, .08)) for value in reference])
    meta = {"sample_rate_hz": sample_rate, "center_frequency_hz": frequency}
    write_blah2iq(path, meta, rows)
    return {"path": str(path), "sha256": file_hash(path), "seed": seed, "samples_per_channel": count}
