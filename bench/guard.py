#!/usr/bin/env python3
"""Thermally guarded finite benchmark command; stops only its own process group."""
import argparse
import json
import math
import os
from pathlib import Path
import shutil
import signal
import subprocess
import time


def read(path):
    try:
        return Path(path).read_text().strip()
    except OSError:
        return None


def snapshot():
    sensors, gpu = {}, {}
    for hw in Path("/sys/class/hwmon").glob("hwmon*"):
        name = read(hw / "name")
        if name in ("k10temp", "coretemp", "amdgpu", "nouveau"):
            for path in hw.glob("temp*_input"):
                raw = read(path)
                if raw is not None:
                    key = f"{hw.name}:{name}:{path.stem}"
                    sensors[key] = dict(celsius=int(raw) / 1000,
                        label=read(str(path).replace("_input", "_label")),
                        critical_alarm=read(str(path).replace("_input", "_crit_alarm")))
        for path in hw.glob("power*_average"):
            gpu[str(path)] = read(path)
    for card in Path("/sys/class/drm").glob("card[0-9]*"):
        for field in ("gpu_busy_percent", "mem_info_vram_used", "mem_info_vram_total"):
            value = read(card / "device" / field)
            if value is not None:
                gpu[f"{card.name}:{field}"] = value
    if shutil.which("nvidia-smi"):
        try:
            result = subprocess.run(["nvidia-smi", "--query-gpu=temperature.gpu,utilization.gpu,memory.used,power.draw,clocks.sm",
                "--format=csv,noheader,nounits"], capture_output=True, text=True, timeout=2)
            gpu["nvidia"] = result.stdout.strip() if result.returncode == 0 else None
            if result.returncode == 0:
                for index, line in enumerate(result.stdout.splitlines()):
                    sensors[f"nvidia:{index}"] = dict(celsius=float(line.split(",")[0]), critical_alarm=None)
        except (subprocess.TimeoutExpired, ValueError):
            gpu["nvidia"] = None
    memory = dict(line.split(":", 1) for line in Path("/proc/meminfo").read_text().splitlines())
    return dict(utc_ns=time.time_ns(), monotonic=time.monotonic(), sensors=sensors, gpu=gpu,
        available_bytes=int(memory["MemAvailable"].split()[0]) * 1024,
        cpu_stat=read("/proc/stat"), disk_stat=read("/proc/diskstats"), load=read("/proc/loadavg"))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--log", type=Path, required=True)
    parser.add_argument("--max-celsius", type=float, default=82)
    parser.add_argument("--min-memory-gib", type=float, default=2)
    parser.add_argument("--min-disk-gib", type=float, default=8)
    parser.add_argument("--disk", type=Path, default=Path("/"))
    parser.add_argument("--deadline", type=float, default=3600)
    parser.add_argument("command", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    command = args.command[1:] if args.command[:1] == ["--"] else args.command
    if not command:
        parser.error("Missing benchmark command")
    if not all(math.isfinite(value) and value > 0 for value in
               (args.max_celsius, args.min_memory_gib, args.min_disk_gib, args.deadline)):
        parser.error("Temperature, memory, disk and deadline limits must be finite and positive")
    process = None
    start = time.monotonic()
    def check(state):
        if not state["sensors"]:
            raise RuntimeError("No usable CPU/GPU temperature telemetry")
        if any(v["celsius"] >= args.max_celsius or v.get("critical_alarm") == "1"
               for v in state["sensors"].values()):
            raise RuntimeError("Thermal cutoff or critical alarm; benchmark stopped")
        if state["available_bytes"] < args.min_memory_gib * 2**30:
            raise RuntimeError("Available memory fell below the benchmark floor")
        if shutil.disk_usage(args.disk).free < args.min_disk_gib * 2**30:
            raise RuntimeError("Free disk fell below the benchmark floor")
        if time.monotonic() - start > args.deadline:
            raise RuntimeError("Benchmark deadline exceeded")
    def interrupted(signum, frame):
        raise RuntimeError(f"Benchmark interrupted by signal {signum}")
    for sig in (signal.SIGTERM, signal.SIGINT):
        signal.signal(sig, interrupted)
    with args.log.open("x", buffering=1) as log:
        try:
            state = snapshot()
            log.write(json.dumps(dict(event="preflight", command=command, **state)) + "\n")
            check(state)
            process = subprocess.Popen(command, start_new_session=True)
            while process.poll() is None:
                state = snapshot()
                state["process"] = read(f"/proc/{process.pid}/stat")
                state["process_io"] = read(f"/proc/{process.pid}/io")
                log.write(json.dumps(state) + "\n")
                check(state)
                time.sleep(.5)
            log.write(json.dumps(dict(event="exit", returncode=process.returncode, elapsed=time.monotonic()-start)) + "\n")
            return process.returncode
        except BaseException as error:
            if process is not None and process.poll() is None:
                os.killpg(process.pid, signal.SIGKILL)
                process.wait(timeout=5)
            log.write(json.dumps(dict(event="abort", error=str(error))) + "\n")
            raise


if __name__ == "__main__":
    raise SystemExit(main())
