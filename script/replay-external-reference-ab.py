#!/usr/bin/env python3
"""Compare OWL dedicated reference with Kraken array synthesis on identical IQ."""

import argparse
import hashlib
import importlib.util
import json
import math
import statistics
from pathlib import Path


def load(path, name):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def digest(path):
    value = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            value.update(block)
    return value.hexdigest()


def six_channel_config(source, frequency):
    config = json.loads(json.dumps(source))
    config["capture"]["fc"] = frequency
    device = config["capture"]["device"]
    device["channel_count"] = 6
    device["reference_channel"] = 0
    device["surveillance_channels"] = [1, 2, 3, 4, 5]
    heimdall = device.get("heimdall", {})
    gains = heimdall.get("channel_gains")
    if isinstance(gains, list) and len(gains) == 5:
        heimdall["channel_gains"] = [float(gains[0]), *map(float, gains)]
    geometry = device.get("array_geometry", {})
    for element in geometry.get("elements", []):
        element["daq_channel"] = int(element["daq_channel"]) + 1
    if isinstance(geometry.get("bearing_channels"), list):
        geometry["bearing_channels"] = [value + 1 for value in geometry["bearing_channels"]]
    config["process"]["reference_synthesis"]["mode"] = "array_eigenbeam"
    config["process"]["reference_synthesis"]["channels"] = [1, 2, 3, 4, 5]
    return config


def cutout_summary(cutouts):
    rows = []
    for frame in cutouts:
        row = {key: frame.get(key) for key in (
            "frame", "timestamp", "center_delay_km", "center_doppler_hz")}
        channels = frame.get("channels", [])
        if channels and channels[0] and channels[0][0]:
            height, width = len(channels[0]), len(channels[0][0])
            cy, cx = height // 2, width // 2
            cell = lambda value: float(value[0]) ** 2 + float(value[1]) ** 2
            target = sum(cell(channel[cy][cx]) for channel in channels)
            nuisance = [sum(cell(channel[y][x]) for channel in channels)
                        for y in range(height) for x in range(width)
                        if (y, x) != (cy, cx)]
            row["fused_target_contrast_db"] = 10 * math.log10(
                max(target, 1e-30) / max(statistics.median(nuisance), 1e-30))
        rows.append(row)
    return rows


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--config", type=Path, required=True)
    parser.add_argument("--recording", type=Path, required=True)
    parser.add_argument("--receipt", type=Path, required=True)
    parser.add_argument("--harness", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--delay-km", type=float, required=True)
    parser.add_argument("--doppler-hz", type=float, required=True)
    parser.add_argument("--timeout", type=float, default=600)
    args = parser.parse_args()

    replay = load(Path(__file__).with_name("replay-fusion-ab-cass.py"), "fusion_replay")
    harness = replay.load_harness(args.harness)
    source = replay.load_config(args.config)
    build_receipt = json.loads(args.receipt.read_text())
    source = six_channel_config(source, int(build_receipt["frequency_hz"]))
    cutout = {"delay_km": args.delay_km, "doppler_hz": args.doppler_hz,
              "delay_radius_bins": 3, "doppler_radius_bins": 3,
              "max_frames": 64}

    arms = {}
    arms["kraken_synthesized_reference"] = replay.run_arm(
        harness, args.binary.resolve(), source, args.recording.resolve(),
        "noncoherent", args.timeout, None, None,
        "kraken_synthesized_reference", 0, False, False, None, None, None,
        "cpu", cutout, None, None, None, True, None)
    arms["owl_external_reference"] = replay.run_arm(
        harness, args.binary.resolve(), source, args.recording.resolve(),
        "noncoherent", args.timeout, 0, None, "owl_external_reference", 0,
        False, False, None, None, None, "cpu", cutout, None, None, None,
        True, None)

    result = {"schema": "vectorwarp-external-reference-ab-v1",
              "recording": str(args.recording),
              "recording_sha256": digest(args.recording),
              "build_receipt_sha256": digest(args.receipt),
              "truth": {"delay_km": args.delay_km,
                        "doppler_hz": args.doppler_hz},
              "comparison_contract": "same six-channel IQ, same five surveillance channels, noncoherent map fusion, fixed detector configuration",
              "arms": arms}
    args.output.write_text(json.dumps(result, separators=(",", ":")) + "\n")
    print(json.dumps({name: {"detection_frames": len(value["detections"]),
                                   "cutouts": cutout_summary(value["complex_cutouts"])}
                      for name, value in arms.items()}, indent=2))


if __name__ == "__main__":
    main()
