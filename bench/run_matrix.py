#!/usr/bin/env python3
"""Three alternating full-recording runs per mode, with raw artifacts and guards.

Run this within a dedicated memory-bounded scope. No CPU quota is set here;
existing affinity/power/cgroup limits are recorded, never changed. An aborted
run stops the matrix and is never retried automatically.
"""
import argparse
import csv
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import subprocess
import sys
import time


def command_output(command):
    try:
        result = subprocess.run(command, capture_output=True, text=True, timeout=10)
        return dict(returncode=result.returncode, stdout=result.stdout, stderr=result.stderr)
    except (OSError, subprocess.TimeoutExpired) as error:
        return dict(unavailable=str(error))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--recording", type=Path, required=True)
    parser.add_argument("--profile", type=Path, required=True)
    parser.add_argument("--sha256", required=True, help="Verified input recording checksum")
    parser.add_argument("--devices", nargs="*", default=[])
    parser.add_argument("--repeats", type=int, default=3)
    parser.add_argument("--expected-frames", type=int)
    parser.add_argument("--deadline", type=float, default=2400)
    parser.add_argument("--max-celsius", type=float, default=82)
    parser.add_argument("--min-memory-gib", type=float, default=2)
    parser.add_argument("--min-disk-gib", type=float, default=8)
    parser.add_argument("--disk", type=Path, default=Path("/"))
    args = parser.parse_args()
    if not re.fullmatch(r"[a-fA-F0-9]{64}", args.sha256):
        parser.error("--sha256 must be a complete SHA-256 checksum")
    if any(not re.fullmatch(r"[0-9]+:[0-9]+:[0-9]+", device) for device in args.devices):
        parser.error("--devices must contain numeric IDs from testAcceleration --list")
    if args.repeats < 1 or args.repeats > 20 or (args.expected_frames is not None and args.expected_frames < 1):
        parser.error("Use 1–20 repeats and a positive expected frame count")
    recording_path, profile_path = args.recording.resolve(), args.profile.resolve()
    root = args.root.resolve()
    args.output.mkdir(exist_ok=False)
    output = args.output.resolve()
    environment = dict(os.environ, LD_LIBRARY_PATH=str(root / "lib"), OPENBLAS_NUM_THREADS="1", OMP_NUM_THREADS="1")
    manifest = dict(start_utc_ns=time.time_ns(), host=platform.node(), platform=platform.platform(),
        affinity=sorted(os.sched_getaffinity(0)), devices=args.devices,
        note="Signal-processing pipeline; capture/network/browser excluded. Validation is separately timed.",
        lscpu=command_output(["lscpu", "-J"]), uname=command_output(["uname", "-a"]),
        gpu=command_output(["lspci", "-nnk"]), power=command_output(["powerprofilesctl", "get"]),
        cgroup=Path("/proc/self/cgroup").read_text(), mountinfo=Path("/proc/self/mountinfo").read_text(),
        package=json.loads((root / "package-sha256.json").read_text()),
        config=json.loads(profile_path.read_text()), repeats=args.repeats,
        guards=dict(max_celsius=args.max_celsius, deadline=args.deadline,
            min_memory_gib=args.min_memory_gib, min_disk_gib=args.min_disk_gib))
    digest = hashlib.sha256()
    with recording_path.open("rb") as recording:
        for block in iter(lambda: recording.read(8 << 20), b""):
            digest.update(block)
    manifest["recording_sha256"] = digest.hexdigest()
    if digest.hexdigest() != args.sha256.lower():
        raise RuntimeError("Benchmark dataset does not match verified recording")
    for name, expected in manifest["package"].items():
        path = root / name if name.startswith("lib/") else root / "bin" / name
        if hashlib.sha256(path.read_bytes()).hexdigest() != expected:
            raise RuntimeError(f"Benchmark binary/library hash mismatch: {name}")
    (output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    cases = [("pair", "upstream", "cpu", "auto"), ("pair", "fast", "cpu", "auto"),
             ("pair", "fast", "auto", "auto")]
    cases += [("pair", "fast", "gpu", device) for device in args.devices]
    cases += [("array", "fast", "cpu", "auto"), ("array", "fast", "auto", "auto")]
    cases += [("array", "fast", "gpu", device) for device in args.devices]
    expected_samples = None
    expected_frames = args.expected_frames
    with (output / "events.jsonl").open("x", buffering=1) as events:
        for repeat in range(args.repeats):
            order = cases if repeat % 2 == 0 else list(reversed(cases))
            for profile, engine, mode, device in order:
                label = f"{profile}-{engine}-{mode}-{device.replace(':', '_')}-r{repeat+1}"
                golden = output / f"{profile}.maps"
                golden_mode = "compare" if golden.exists() else "write"
                if golden_mode == "write" and mode != "cpu":
                    raise RuntimeError("CPU correctness reference must run first")
                guard = [sys.executable, str(root / "guard.py"), "--log", str(output / f"{label}.thermal.jsonl"),
                    "--deadline", str(args.deadline), "--max-celsius", str(args.max_celsius),
                    "--min-memory-gib", str(args.min_memory_gib), "--min-disk-gib", str(args.min_disk_gib),
                    "--disk", str(args.disk.resolve())]
                cmd = guard + ["--", str(root / "bin" / f"bench-{engine}"), str(recording_path),
                    str(profile_path), str(output / label), profile, mode, "0", golden_mode, str(golden)]
                event = dict(event="start", label=label, utc_ns=time.time_ns(), command=cmd)
                events.write(json.dumps(event)+"\n")
                print(json.dumps(event), flush=True)
                with (output / f"{label}.log").open("x") as log:
                    result = subprocess.run(cmd, env=dict(environment, BLAH2_GPU_DEVICE=device), stdout=log, stderr=subprocess.STDOUT)
                events.write(json.dumps(dict(event="end", label=label, utc_ns=time.time_ns(), returncode=result.returncode))+"\n")
                if result.returncode:
                    raise RuntimeError(f"{label} failed; matrix stopped, no automatic retry")
                summary = json.loads((output / f"{label}.summary.json").read_text())
                if expected_frames is None:
                    expected_frames = summary["frames"]
                if expected_samples is None:
                    expected_samples = summary["samples_per_channel"]
                if summary["frames"] != expected_frames or summary["samples_per_channel"] != expected_samples:
                    raise RuntimeError("Benchmark runs did not consume identical complete frames")
                print(json.dumps(dict(event="complete", label=label, summary=summary)), flush=True)
        events.write(json.dumps(dict(event="matrix_complete", utc_ns=time.time_ns()))+"\n")
    print("All repeated full-recording runs completed", flush=True)


if __name__ == "__main__":
    main()
