#!/usr/bin/env python3
"""Reject processor/GPU artifacts assembled from different native builds."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import subprocess
import sys


def fail(message: str) -> None:
    raise SystemExit(f"processing-cohort: {message}")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--artifact", required=True, type=Path)
    parser.add_argument("--gpu-mode", required=True, choices=("AUTO", "ON", "OFF"))
    args = parser.parse_args()

    bindir = args.artifact / "bin"
    processor = bindir / "blah2"
    if not processor.is_file() or not processor.stat().st_mode & 0o111:
        fail("processor binary is missing or not executable")
    try:
        completed = subprocess.run(
            [str(processor), "--processing-capabilities"],
            check=True, text=True, capture_output=True, timeout=10,
        )
        report = json.loads(completed.stdout)
    except (OSError, subprocess.SubprocessError, json.JSONDecodeError) as error:
        fail(f"processor capability report failed: {error}")

    if report.get("schema") != 1:
        fail("processor capability schema is not 1")
    resident = report.get("residentGpu")
    wideband = report.get("widebandGpu")
    if not isinstance(resident, bool) or not isinstance(wideband, bool):
        fail("processor GPU capabilities are not booleans")
    if resident != wideband:
        fail("processor exposes only one member of the resident/wideband GPU pair")

    members = (
        bindir / "blah2-gpu-worker",
        bindir / "blah2-gpu-resident-vulkan.so",
        bindir / "blah2-gpu-wideband-vulkan.so",
    )
    present = tuple(member.is_file() for member in members)
    if len(set(present)) != 1:
        fail("resident/wideband GPU artifact set is incomplete")
    modules_present = present[0]
    if resident != modules_present:
        fail("processor capability and resident/wideband GPU artifacts disagree")
    if args.gpu_mode == "ON" and not resident:
        fail("GPU=ON build lacks the Linux resident/wideband GPU cohort")
    if args.gpu_mode == "OFF" and resident:
        fail("GPU=OFF build unexpectedly contains the resident/wideband GPU cohort")


if __name__ == "__main__":
    main()
