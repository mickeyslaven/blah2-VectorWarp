#!/usr/bin/env python3
"""Resumable, bounded raw-host to CASS runner for spatial cutout experiments."""

import argparse
import json
import subprocess
from collections import defaultdict
from pathlib import Path


RAW_HOST = "root@100.126.186.65"
CASS_HOST = "mickeyslaven@100.104.10.46"
CASS_SOURCE = "/home/mickeyslaven/wwat563-spatial-src"
CASS_BINARY = "/home/mickeyslaven/.cache/vectorwarp-calibrated-build/bin/blah2"
CASS_CONFIG = "/home/mickeyslaven/wwat-current-config.json"
CASS_RESULTS = "/home/mickeyslaven/wwat563-spatial-results"
CASS_PYTHON = "/home/mickeyslaven/radar-training/venv/bin/python"
CASS_LIBRARY_PATH = ("/home/mickeyslaven/.cache/vectorwarp-calibrated-build/lib:"
                     "/home/mickeyslaven/.cache/vectorwarp-calibrated-build/bin")


def run(argv, **kwargs):
    return subprocess.run(argv, check=True, text=True, **kwargs)


def ssh(host, command, **kwargs):
    return run(["ssh", "-o", "BatchMode=yes", host, command], **kwargs)


def stream(source, target):
    """Copy through this machine without retaining the raw recording locally."""
    reader = subprocess.Popen(
        ["ssh", "-o", "BatchMode=yes", RAW_HOST, "cat", source],
        stdout=subprocess.PIPE,
    )
    writer = subprocess.Popen(
        ["ssh", "-o", "BatchMode=yes", CASS_HOST,
         f"cat > {target}.part && mv {target}.part {target}"],
        stdin=reader.stdout,
    )
    reader.stdout.close()
    writer_status = writer.wait()
    reader_status = reader.wait()
    if reader_status or writer_status:
        raise RuntimeError(f"stream failed ({reader_status}, {writer_status}): {source}")


def sidecar(source):
    result = ssh(RAW_HOST, f"cat {source}.json", capture_output=True)
    return json.loads(result.stdout)


def process(job, raw_path):
    job_id = job["job_id"]
    job_path = f"{CASS_RESULTS}/{job_id}-job.json"
    replay_path = f"{CASS_RESULTS}/{job_id}-replay.json"
    result_path = f"{CASS_RESULTS}/{job_id}-result.json"
    ssh(CASS_HOST, f"cat > {job_path}", input=json.dumps(job) + "\n")
    cutout = job["cutout"]
    replay = [
        CASS_PYTHON, "script/replay-fusion-ab-cass.py",
        "--binary", CASS_BINARY, "--config", CASS_CONFIG,
        "--recording", raw_path,
        "--harness", "test/recording/processor_replay_test.py",
        "--output", replay_path, "--timeout", "900",
        "--modes", "coherent_beam_bank", "--disable-tracker",
        "--complex-cutout-delay-km", str(cutout["delay_km"]),
        "--complex-cutout-doppler-hz", str(cutout["doppler_hz"]),
        "--complex-cutout-delay-radius-bins", str(cutout["delay_radius_bins"]),
        "--complex-cutout-doppler-radius-bins", str(cutout["doppler_radius_bins"]),
        "--complex-cutout-max-frames", str(cutout["max_frames"]),
    ]
    analysis = [
        CASS_PYTHON, "script/evaluate-spatial-cutout-suite.py",
        "--replay", replay_path, "--config", CASS_CONFIG,
        "--job", job_path, "--output", result_path,
    ]
    for command in (replay, analysis):
        remote = ("cd " + CASS_SOURCE + " && export LD_LIBRARY_PATH=" +
                  CASS_LIBRARY_PATH + " && nice -n 10 " + " ".join(command))
        ssh(CASS_HOST, remote, stdout=subprocess.DEVNULL)
    value = json.loads(ssh(CASS_HOST, f"cat {result_path}", capture_output=True).stdout)
    ssh(CASS_HOST, f"rm -f {job_path} {replay_path}")
    return value


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--manifest", required=True, type=Path)
    parser.add_argument("--state", required=True, type=Path)
    args = parser.parse_args()
    manifest = json.loads(args.manifest.read_text())
    groups = defaultdict(list)
    for job in manifest["jobs"]:
        groups[job["recording"]].append(job)
    state = json.loads(args.state.read_text()) if args.state.exists() else {
        "schema": "vectorwarp-current-epoch-spatial-run-state-v1",
        "config_epoch": manifest["config_epoch"],
        "statistics_sha256": manifest["statistics_sha256"],
        "results": {},
        "recordings": {},
    }
    ssh(CASS_HOST, f"mkdir -p {CASS_RESULTS}")
    ordered = sorted(groups, key=lambda path: all(
        job["role"] != "positive" for job in groups[path]))
    for index, source in enumerate(ordered, 1):
        pending = [job for job in groups[source] if job["job_id"] not in state["results"]]
        if not pending:
            continue
        metadata = sidecar(source)
        raw_path = "/tmp/" + Path(source).name
        print(f"[{index}/{len(ordered)}] stream {Path(source).name}", flush=True)
        stream(source, raw_path)
        digest = ssh(CASS_HOST, f"sha256sum {raw_path}", capture_output=True).stdout.split()[0]
        if digest != metadata["dataSha256"]:
            raise RuntimeError(f"SHA mismatch: {source}")
        state["recordings"][source] = {"sha256": digest, "bytes": metadata["dataBytes"]}
        for job in pending:
            print(f"  analyze {job['job_id']} ({job['role']})", flush=True)
            state["results"][job["job_id"]] = process(job, raw_path)
            args.state.write_text(json.dumps(state, separators=(",", ":")) + "\n")
        ssh(CASS_HOST, f"rm -f {raw_path}")
    print(json.dumps({"results": len(state["results"]),
                      "recordings": len(state["recordings"])}))


if __name__ == "__main__":
    main()
