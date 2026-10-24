#!/usr/bin/env python3
"""Consume verified raw clips exposed by the shared waveform runner barrier."""

import argparse
import importlib.util
import json
import time
from collections import defaultdict
from pathlib import Path


RUNNER_PATH = Path(__file__).with_name("run-current-epoch-spatial-suite.py")
SPEC = importlib.util.spec_from_file_location("spatial_runner", RUNNER_PATH)
RUNNER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(RUNNER)
SUMMARY_PATH = Path(__file__).with_name("summarize-current-epoch-spatial-suite.py")
SUMMARY_SPEC = importlib.util.spec_from_file_location("spatial_summary", SUMMARY_PATH)
SUMMARY = importlib.util.module_from_spec(SUMMARY_SPEC)
SUMMARY_SPEC.loader.exec_module(SUMMARY)
READY_ROOT = "/home/mickeyslaven/wwat563-results/spatial-ready"
ACK_ROOT = "/home/mickeyslaven/wwat563-results/spatial-ack"
FROZEN_COMPLETE = "/home/mickeyslaven/wwat563-results/rolling/frozen-complete"


def write_ack(basename, value):
    path = f"{ACK_ROOT}/{basename}.json"
    payload = json.dumps(value, separators=(",", ":")) + "\n"
    RUNNER.ssh(RUNNER.CASS_HOST,
               f"cat > {path}.part && mv {path}.part {path}", input=payload)


def save(path, state):
    temporary = path.with_suffix(path.suffix + ".part")
    temporary.write_text(json.dumps(state, separators=(",", ":")) + "\n")
    temporary.replace(path)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--manifest", required=True, type=Path)
    parser.add_argument("--state", required=True, type=Path)
    parser.add_argument("--gain-selection", required=True, type=Path)
    parser.add_argument("--poll-seconds", type=float, default=5)
    args = parser.parse_args()
    manifest = json.loads(args.manifest.read_text())
    groups = defaultdict(list)
    for job in manifest["jobs"]:
        groups[job["recording"]].append(job)
    state = json.loads(args.state.read_text()) if args.state.exists() else {
        "schema": "vectorwarp-current-epoch-spatial-run-state-v1",
        "config_epoch": manifest["config_epoch"],
        "statistics_sha256": manifest["statistics_sha256"],
        "results": {}, "recordings": {},
    }
    RUNNER.ssh(RUNNER.CASS_HOST,
               f"mkdir -p {RUNNER.CASS_RESULTS} {READY_ROOT} {ACK_ROOT}")
    expected = {job["job_id"] for job in manifest["jobs"]}
    acknowledged = set()
    warned_missing = set()
    def gain_complete(result):
        return set(result.get("gain_treatments", {})) == {
            "raw_as_acquired", "inverse_hardware_gain"}
    def complete():
        return all(gain_complete(state["results"].get(job_id, {}))
                   for job_id in expected)
    def freeze_selection_if_ready():
        if args.gain_selection.exists():
            return True
        try:
            selection = SUMMARY.select_gain_treatment(manifest, state)
        except ValueError:
            return False
        save(args.gain_selection, selection)
        print(f"froze gain treatment {selection['selected']} from positives", flush=True)
        return True
    while not complete():
        listing = RUNNER.ssh(RUNNER.CASS_HOST,
            f"find {READY_ROOT} -maxdepth 1 -type f -name '*.json' -print",
            capture_output=True).stdout.splitlines()
        waiting = []
        for path in listing:
            basename = Path(path).name.removesuffix(".json")
            if basename not in acknowledged:
                item = json.loads(RUNNER.ssh(
                    RUNNER.CASS_HOST, f"cat {path}", capture_output=True).stdout)
                waiting.append((basename, item))
        if not waiting:
            time.sleep(args.poll_seconds)
            continue
        for basename, item in waiting:
            recording = item["source"]
            jobs = groups.get(recording, [])
            if not jobs:
                write_ack(basename, {"schema": "vectorwarp-wwat563-spatial-ack-v1",
                    "status": "not_in_frozen_cohort", "spatial_jobs": []})
                acknowledged.add(basename)
                print(f"ack {basename}: not in frozen cohort", flush=True)
                continue
            raw_path = item.get("recording", item.get("raw"))
            if not raw_path:
                raise ValueError(f"ready marker lacks recording/raw: {basename}")
            expected_bytes = str(item.get("bytes", ""))
            identity = RUNNER.ssh(RUNNER.CASS_HOST,
                f"if test -f {raw_path} && test \"$(stat -c %s {raw_path})\" = "
                f"\"{expected_bytes}\"; then printf '%s\\n' {expected_bytes}; "
                f"sha256sum {raw_path} | cut -d' ' -f1; fi",
                capture_output=True).stdout.splitlines()
            expected_identity = [expected_bytes, item.get("sha256", "")]
            if identity != expected_identity:
                if basename not in warned_missing:
                    print(f"skip stale/unverified ready marker: {basename}", flush=True)
                    warned_missing.add(basename)
                continue
            if any(job["role"] == "miss" for job in jobs) and not freeze_selection_if_ready():
                print(f"hold miss {basename}: positive-only gain selection not frozen", flush=True)
                continue
            for job in jobs:
                existing = state["results"].get(job["job_id"], {})
                if not gain_complete(existing):
                    print(f"analyze {basename} {job['job_id']}", flush=True)
                    state["results"][job["job_id"]] = RUNNER.process(job, raw_path)
                    save(args.state, state)
            freeze_selection_if_ready()
            state["recordings"][recording] = {
                key: item[key] for key in ("sha256", "bytes") if key in item}
            save(args.state, state)
            write_ack(basename, {"schema": "vectorwarp-wwat563-spatial-ack-v1",
                "status": "complete",
                "spatial_jobs": [job["job_id"] for job in jobs]})
            acknowledged.add(basename)
            print(f"ack {basename}: {len(jobs)} job(s)", flush=True)
    print(json.dumps({"results": len(state["results"]),
                      "recordings": len(state["recordings"])}), flush=True)
    RUNNER.ssh(RUNNER.CASS_HOST,
        f"mkdir -p {Path(FROZEN_COMPLETE).parent} && "
        f"printf complete > {FROZEN_COMPLETE}.part && "
        f"mv {FROZEN_COMPLETE}.part {FROZEN_COMPLETE}")


if __name__ == "__main__":
    main()
