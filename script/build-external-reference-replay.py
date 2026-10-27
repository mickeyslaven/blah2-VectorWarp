#!/usr/bin/env python3
"""Build a six-input replay with an independently clocked OWL reference.

The output is BLAH2IQ v1 with channel 0 as the waveform-locked external
reference and the original Kraken channels as channels 1..N.  Host arrival
time is never used as RF timing: a previously qualified affine sample-clock
map and carrier offset are mandatory.
"""

import argparse
import hashlib
import importlib.util
import json
import math
import struct
from pathlib import Path

import numpy as np


MAGIC = b"BLAH2IQ\0"


def load_qualifier(path):
    spec = importlib.util.spec_from_file_location("qualify_external_reference", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def write_blah2iq(path, channels, sample_rate, frequency, blocks):
    written = 0
    with path.open("xb+") as destination:
        header = bytearray(64)
        header[:8] = MAGIC
        struct.pack_into("<IIIIII", header, 8, 1, 64, channels, sample_rate,
                         frequency, 1)
        destination.write(header)
        for block in blocks:
            if block.shape[0] != channels:
                raise ValueError("output channel count changed")
            # Recording.cpp rejects blocks larger than the native streaming
            # bound. Preserve channel-major layout while splitting a CPI.
            for begin in range(0, block.shape[1], 65536):
                part = block[:, begin:begin + 65536]
                count = part.shape[1]
                block_header = bytearray(16)
                struct.pack_into("<IIQ", block_header, 0, count, 0, written)
                destination.write(block_header)
                destination.write(np.asarray(part, dtype="<c8").tobytes(order="C"))
                written += count
        destination.seek(40)
        destination.write(struct.pack("<QQ", written, 1))
        destination.flush()
    return written


def build(hp, owl, qualification, hp_rate, owl_rate, cpi_s,
          residual_tracking=True):
    best = qualification["best"]
    slope = float(best["affine_owl_samples_per_hp_sample"])
    intercept = float(best["affine_intercept_owl_samples"])
    # The short-lock median is intentionally coarse (FFT-bin limited).  A
    # residual near one hertz materially decorrelates a 500 ms CPI, so use the
    # long affine-CPI estimates when the qualifier provides them.
    affine_cfos = [float(row["cfo_hz"]) for row in best.get("affine_cpi_locks", [])
                   if math.isfinite(float(row["cfo_hz"]))]
    cfo = float(np.median(affine_cfos)) if affine_cfos else float(best["median_cfo_hz"])
    pilot_channel = int(best["kraken_channel"])
    if not 0 <= pilot_channel < hp.shape[0]:
        raise ValueError("qualified pilot channel is outside the Kraken capture")
    cpi_samples = int(round(cpi_s * hp_rate))
    if cpi_samples <= 0:
        raise ValueError("CPI must contain samples")
    output = []
    phase_receipts = []
    for begin in range(0, hp.shape[1] - cpi_samples + 1, cpi_samples):
        count = cpi_samples
        external = QUALIFIER.linear_resample(owl, intercept + slope * begin,
                                             slope, count)
        local_time = np.arange(count, dtype=np.float64) / hp_rate
        time = begin / hp_rate + local_time
        external = external * np.exp(2j * np.pi * cfo * time)
        external = external - np.mean(external, dtype=np.complex128)
        pilot = hp[pilot_channel, begin:begin + count]
        residual_cfo = 0.0
        if residual_tracking:
            tracked_cfo, residual_cfo = QUALIFIER.refine_cfo(
                pilot, external, hp_rate, 0.0, max_residual_hz=5.0)
            external *= np.exp(2j * np.pi * tracked_cfo * local_time)
        cross = np.vdot(external, pilot)
        coherence = float(abs(cross) / max(
            np.linalg.norm(external) * np.linalg.norm(pilot), 1e-20))
        if abs(cross):
            external *= np.exp(1j * np.angle(cross))
        external_rms = math.sqrt(float(np.mean(np.abs(external) ** 2)))
        pilot_rms = math.sqrt(float(np.mean(np.abs(pilot) ** 2)))
        if external_rms <= 0 or not math.isfinite(external_rms):
            raise ValueError("external reference has invalid RMS")
        external *= pilot_rms / external_rms
        output.append(np.concatenate((external[None, :].astype(np.complex64),
                                      hp[:, begin:begin + count]), axis=0))
        phase_receipts.append({"first_sample": begin, "samples": count,
                               "pilot_channel": pilot_channel,
                               "coherence": coherence,
                               "nominal_cfo_hz": cfo,
                               "residual_cfo_hz": residual_cfo,
                               "phase_correction_rad": float(np.angle(cross)),
                               "amplitude_scale": pilot_rms / external_rms})
    if not output:
        raise ValueError("capture contains no complete CPI")
    return output, phase_receipts


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--owl-root", type=Path, required=True)
    parser.add_argument("--owl-ledger", type=Path, required=True)
    parser.add_argument("--kraken", type=Path, required=True)
    parser.add_argument("--kraken-metadata", type=Path, required=True)
    parser.add_argument("--qualification", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--receipt", type=Path, required=True)
    parser.add_argument("--cpi", type=float, default=0.5)
    parser.add_argument("--disable-residual-tracking", action="store_true")
    args = parser.parse_args()

    metadata = json.loads(args.kraken_metadata.read_text())
    qualification = json.loads(args.qualification.read_text())
    if qualification.get("qualified") is not True:
        raise ValueError("external reference has not passed waveform qualification")
    hp, frequency = QUALIFIER.read_mchq(args.kraken)
    hp_rate = int(metadata["sampleRate"])
    start_s, end_s = metadata["windowStart"] / 1000, metadata["windowEnd"] / 1000
    owl, owl_rate, _, owl_epoch = QUALIFIER.read_owl(
        args.owl_root, args.owl_ledger, start_s, end_s)
    if frequency != int(metadata["frequency"]) or frequency != int(qualification["frequency_hz"]):
        raise ValueError("RF identity differs between capture and qualification")
    if hp_rate != int(qualification["kraken_rate_hz"]) or owl_rate != int(qualification["owl_rate_hz"]):
        raise ValueError("sample-rate identity differs from qualification")
    if str(owl_epoch) != str(qualification["owl_epoch"]):
        raise ValueError("OWL epoch differs from qualification")

    blocks, phases = build(hp, owl, qualification, hp_rate, owl_rate, args.cpi,
                           residual_tracking=not args.disable_residual_tracking)
    written = write_blah2iq(args.output, hp.shape[0] + 1, hp_rate,
                            frequency, blocks)
    receipt = {
        "schema": "vectorwarp-external-reference-replay-v1",
        "timing_contract": "waveform-derived affine clock and CFO; host arrival is search-only",
        "frequency_hz": frequency,
        "sample_rate_hz": hp_rate,
        "channels": hp.shape[0] + 1,
        "external_reference_channel": 0,
        "kraken_surveillance_channels": list(range(1, hp.shape[0] + 1)),
        "samples_per_channel": written,
        "cpi_s": args.cpi,
        "source": {"kraken": str(args.kraken), "kraken_sha256": sha256(args.kraken),
                   "qualification": str(args.qualification),
                   "qualification_sha256": sha256(args.qualification),
                   "owl_epoch": str(owl_epoch)},
        "alignment": {"slope_owl_samples_per_hp_sample":
                      qualification["best"]["affine_owl_samples_per_hp_sample"],
                      "intercept_owl_samples":
                      qualification["best"]["affine_intercept_owl_samples"],
                      "cfo_hz": (float(np.median([
                          float(row["cfo_hz"]) for row in
                          qualification["best"].get("affine_cpi_locks", [])]))
                          if qualification["best"].get("affine_cpi_locks") else
                          qualification["best"]["median_cfo_hz"]),
                      "residual_tracking": not args.disable_residual_tracking,
                      "cpi_receipts": phases},
        "output": str(args.output),
        "output_sha256": sha256(args.output),
    }
    args.receipt.write_text(json.dumps(receipt, indent=2, sort_keys=True) + "\n")
    print(json.dumps({"output": str(args.output), "samples": written,
                      "sha256": receipt["output_sha256"],
                      "median_cpi_coherence": float(np.median(
                          [row["coherence"] for row in phases]))}, indent=2))


if __name__ == "__main__":
    QUALIFIER = load_qualifier(Path(__file__).with_name("qualify-external-reference.py"))
    main()
