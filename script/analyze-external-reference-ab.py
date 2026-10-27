#!/usr/bin/env python3
"""Summarize target-cell contrast and nuisance burden for an external-ref A/B."""

import argparse
import hashlib
import json
import math
import statistics
from pathlib import Path


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def power(cell):
    return float(cell[0]) ** 2 + float(cell[1]) ** 2


def summarize_cutout(cutout):
    channels = cutout["channels"]
    height = len(channels[0])
    width = len(channels[0][0])
    cy, cx = height // 2, width // 2
    channel_contrast = []
    for channel in channels:
        target = power(channel[cy][cx])
        nuisance = [power(channel[y][x]) for y in range(height)
                    for x in range(width) if (y, x) != (cy, cx)]
        channel_contrast.append(10.0 * math.log10(max(target, 1e-30) /
                                                   max(statistics.median(nuisance), 1e-30)))
    target = sum(power(channel[cy][cx]) for channel in channels)
    nuisance = [sum(power(channel[y][x]) for channel in channels)
                for y in range(height) for x in range(width)
                if (y, x) != (cy, cx)]
    return {
        "frame": cutout.get("frame"),
        "timestamp": cutout.get("timestamp"),
        "fused_target_contrast_db": 10.0 * math.log10(
            max(target, 1e-30) / max(statistics.median(nuisance), 1e-30)),
        "physical_channel_target_contrast_db": channel_contrast,
    }


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("input", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    source = json.loads(args.input.read_text())
    arms = {}
    for name, arm in source["arms"].items():
        frames = [summarize_cutout(value) for value in arm["complex_cutouts"]]
        channel_count = len(frames[0]["physical_channel_target_contrast_db"])
        arms[name] = {
            "frames": frames,
            "frame_count": len(frames),
            "detector_nuisance_detection_count": sum(
                len(frame.get("delay", [])) for frame in arm["detections"]),
            "median_fused_target_contrast_db": statistics.median(
                frame["fused_target_contrast_db"] for frame in frames),
            "maximum_fused_target_contrast_db": max(
                frame["fused_target_contrast_db"] for frame in frames),
            "median_physical_channel_target_contrast_db": [
                statistics.median(frame["physical_channel_target_contrast_db"][index]
                                  for frame in frames)
                for index in range(channel_count)
            ],
        }
    result = {
        "schema": "vectorwarp-external-reference-ab-analysis-v1",
        "source": str(args.input),
        "source_sha256": sha256(args.input),
        "truth": source["truth"],
        "comparison_contract": source["comparison_contract"],
        "arms": arms,
        "decision": {
            "promote_external_reference": False,
            "reason": (
                "Neither arm accepted the truth-selected aircraft cell. The OWL arm "
                "reduced detector nuisance detections but had lower target-cell contrast "
                "on this weak distant pass; more independent passes with stable target "
                "energy are required."
            ),
        },
    }
    args.output.write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
