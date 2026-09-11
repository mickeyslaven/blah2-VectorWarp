#!/usr/bin/env python3
"""Summarize completed runs only; retain aborts and numerical disagreements."""
import argparse
import csv
import json
import math
from pathlib import Path
import statistics


def percentile(values, fraction):
    values = sorted(values)
    position = (len(values)-1)*fraction
    lo, hi = math.floor(position), math.ceil(position)
    return values[lo]+(values[hi]-values[lo])*(position-lo)


def numeric_difference(left, right):
    """JSON output has 0.01-unit rounding. Track IDs are excluded separately."""
    if isinstance(left, dict) and isinstance(right, dict):
        keys = set(left) | set(right)
        keys -= {"id", "uuid", "trackId"}
        if any(key not in left or key not in right for key in keys):
            return math.inf
        return max((numeric_difference(left[key], right[key]) for key in keys), default=0)
    if isinstance(left, list) and isinstance(right, list):
        if len(left) != len(right):
            return math.inf
        return max((numeric_difference(a,b) for a,b in zip(left,right)), default=0)
    if isinstance(left, (float,int)) and isinstance(right, (float,int)):
        return abs(left-right)
    return 0 if left == right else math.inf


def summarize(directory):
    manifest = json.loads((directory/"manifest.json").read_text())
    config = manifest["config"]
    runs = []
    outputs_cache = {}
    for path in sorted(directory.glob("*.summary.json")):
        label = path.name.removesuffix(".summary.json")
        summary = json.loads(path.read_text())
        with (directory/f"{label}.frames.csv").open() as stream:
            rows = list(csv.DictReader(stream))
        if len(rows) != summary["frames"]:
            raise RuntimeError(f"Frame CSV count differs: {label}")
        if not rows:
            raise RuntimeError(f"Completed run has no frames: {label}")
        times = [float(row["pipeline_ms"]) for row in rows]
        steady = [float(row["pipeline_ms"]) for row in rows[3:]]
        read = [float(row["read_ms"]) for row in rows]
        duration = summary["samples_per_channel"] / config["sample_rate"]
        temperature = []
        min_memory = []
        gpu_memory = []
        nvidia_memory, gpu_utilization = [], []
        with (directory/f"{label}.thermal.jsonl").open() as stream:
            for line in stream:
                state = json.loads(line)
                temperature += [value["celsius"] for value in state.get("sensors", {}).values()]
                if "available_bytes" in state:
                    min_memory.append(state["available_bytes"])
                for key,value in state.get("gpu", {}).items():
                    if key.endswith(":mem_info_vram_used"):
                        gpu_memory.append(int(value))
                    elif key.endswith(":gpu_busy_percent"):
                        gpu_utilization.append(float(value))
                    elif key == "nvidia" and value:
                        for device in value.splitlines():
                            fields = device.split(",")
                            try:
                                gpu_utilization.append(float(fields[1]))
                                nvidia_memory.append(float(fields[2])*2**20)
                            except (IndexError, ValueError):
                                pass  # Unsupported telemetry is not a zero reading.
        profile = label.split("-")[0]
        baseline = "pair-upstream-cpu-auto-r1" if profile == "pair" else "array-fast-cpu-auto-r1"
        if baseline not in outputs_cache:
            baseline_path=directory/f"{baseline}.outputs.jsonl"
            outputs_cache[baseline]=[json.loads(line) for line in baseline_path.read_text().splitlines()] if baseline_path.exists() else []
        actual = [json.loads(line) for line in (directory/f"{label}.outputs.jsonl").read_text().splitlines()]
        expected = outputs_cache[baseline]
        if len(actual) != len(rows):
            raise RuntimeError(f"Radar output count differs: {label}")
        if not expected or len(expected) != len(actual):
            raise RuntimeError(f"Complete correctness baseline is missing or has different length: {label}")
        detection_mismatch = track_mismatch = rounded_differences = 0
        for a,b in zip(actual, expected):
            d = numeric_difference(a["detections"],b["detections"])
            t = numeric_difference(a["tracks"],b["tracks"])
            detection_mismatch += d > .0100001
            track_mismatch += t > .0100001
            rounded_differences += 0 < max(d,t) <= .0100001
        runs.append(dict(label=label,host=manifest["host"],profile=profile,**summary,
            recorded_seconds=duration,pipeline_realtime_multiplier=duration/(sum(times)/1000),
            replay_realtime_multiplier=duration/((summary["startup_ms"]+sum(times)+sum(read))/1000),
            pipeline_mean_ms=statistics.mean(times),pipeline_median_ms=statistics.median(times),
            pipeline_p95_ms=percentile(times,.95),pipeline_p99_ms=percentile(times,.99),pipeline_max_ms=max(times),
            steady_mean_ms=statistics.mean(steady) if steady else None,
            startup_inclusive_replay_ms=summary["startup_ms"]+sum(times)+sum(read),
            pipeline_deadline_misses=sum(value>1000*config["cpi"] for value in times),
            replay_deadline_misses=sum(value+io>1000*config["cpi"] for value,io in zip(times,read)),
            peak_temperature_c=max(temperature) if temperature else None,
            minimum_available_memory_bytes=min(min_memory) if min_memory else None,
            peak_host_amd_vram_bytes=max(gpu_memory) if gpu_memory else None,
            peak_host_nvidia_vram_bytes=max(nvidia_memory) if nvidia_memory else None,
            mean_host_gpu_utilization_percent=statistics.mean(gpu_utilization) if gpu_utilization else None,
            output_frames_compared=min(len(actual),len(expected)),
            detection_disagreement_frames=detection_mismatch,track_disagreement_frames=track_mismatch,
            rounding_only_difference_frames=rounded_differences,
            **{f"mean_{key}":statistics.mean(float(row[key]) for row in rows)
               for key in ("read_ms","extract_ms","reference_ms","spectrum_ms","clutter_ms","ambiguity_ms","detection_ms","tracker_ms","json_ms")}))
    return runs


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("directories", type=Path, nargs="+")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    runs = [run for directory in args.directories for run in summarize(directory)]
    if not runs:
        raise RuntimeError("No completed benchmark results")
    with args.output.with_suffix(".json").open("x") as stream:
        json.dump(runs,stream,indent=2,allow_nan=False)
    with args.output.with_suffix(".csv").open("x",newline="") as stream:
        writer=csv.DictWriter(stream,fieldnames=list(runs[0]))
        writer.writeheader(); writer.writerows(runs)
    print(json.dumps(dict(completed_runs=len(runs), hosts=sorted({run["host"] for run in runs}))))


if __name__ == "__main__":
    main()
