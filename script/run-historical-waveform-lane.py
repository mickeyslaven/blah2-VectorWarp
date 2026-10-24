#!/usr/bin/env python3
"""Separate resumable low-priority historical MCHQ waveform lane for Strix.

It intentionally has no 563 ready/ack paths and no live configuration access.
Each epoch receives its own output root/state; positives finish before that
epoch's misses.  At most ``workers`` raw clips exist on Strix at one time.
"""
import argparse
import hashlib
import json
import shlex
import subprocess
import threading
import urllib.parse
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path

STRIX = "strix"
PYTHON = "/root/wwat563-accel/venv/bin/python"
SOURCE = "/root/wwat563-accel/wwat563-spatial-src"


def quote(value): return shlex.quote(str(value))
def run(command, **kwargs): return subprocess.run(command, check=True, text=True, **kwargs)
def ssh(host, command, **kwargs): return run(["ssh", "-o", "BatchMode=yes", host, command], **kwargs)


def unique_rows(document):
    rows = {}
    for row in document["rows"]:
        path = row["capture"]["file"]
        # A tracked attachment is a better calibration representative if a
        # coalesced physical clip belongs to more than one parent event.
        if path not in rows or row["role"] == "positive": rows[path] = row
    return list(rows.values())


def key(row):
    return "r%s-%s" % (row["capture"].get("revision", 0),
                         hashlib.sha256(row["capture"]["file"].encode()).hexdigest()[:12])


def trajectory(row):
    return {"schema": "vectorwarp-clip-truth-trajectory-v1",
            "clip": row["capture"]["file"], "clip_revision": row["capture"].get("revision"),
            "window_start_ms": row["capture"]["window_start"],
            "window_end_ms": row["capture"]["window_end"], "cpi_s": .5,
            "event_id": row["event_id"], "hex": row.get("hex"),
            "flight": row.get("flight"), "outcome": row["outcome"],
            "frequency_hz": row.get("frequency_hz"), "trajectory": row["replay_trajectory"]}


def save(path, value):
    temp = path.with_suffix(path.suffix + ".part")
    temp.write_text(json.dumps(value, separators=(",", ":")) + "\n")
    temp.replace(path)


def recording_url(base_url, source):
    name = Path(source).name
    return f"{base_url.rstrip('/')}/{urllib.parse.quote(name)}"


def fetch_verified_command(raw, source_url):
    """Fetch sidecar and MCHQ on Strix, then atomically publish verified bytes."""
    return f"""set -eu
raw={quote(raw)}
part=\"$raw.part\"
sidecar=$(curl -fsSL --retry 5 --retry-all-errors --connect-timeout 10 {quote(source_url + '.json')})
expected_sha=$(printf '%s' \"$sidecar\" | python3 -c 'import json,sys; print(json.load(sys.stdin)["dataSha256"])')
expected_bytes=$(printf '%s' \"$sidecar\" | python3 -c 'import json,sys; print(json.load(sys.stdin)["dataBytes"])')
case \"$expected_sha\" in [0-9a-f][0-9a-f]*) ;; *) exit 65;; esac
if [ -f \"$raw\" ] && [ \"$(stat -c %s \"$raw\")\" = \"$expected_bytes\" ] && [ \"$(sha256sum \"$raw\" | awk '{{print $1}}')\" = \"$expected_sha\" ]; then
  printf '%s\\n' \"$expected_sha\"
  exit 0
fi
rm -f \"$part\"
curl -fsSL --retry 5 --retry-all-errors --connect-timeout 10 --max-time 900 -o \"$part\" {quote(source_url)}
test \"$(stat -c %s \"$part\")\" = \"$expected_bytes\"
test \"$(sha256sum \"$part\" | awk '{{print $1}}')\" = \"$expected_sha\"
mv -f \"$part\" \"$raw\"
printf '%s\\n' \"$expected_sha\"
"""


def process(row, root, state, state_path, lock, base_url, transfer_gate):
    ident = key(row)
    with lock:
        if state["clips"].get(ident, {}).get("status") == "complete": return
        state["clips"][ident] = {"status": "running", "recording": row["capture"]["file"],
                                  "role": row["role"], "event_id": row["event_id"]}
        save(state_path, state)
    source, raw = row["capture"]["file"], f"{root}/cache/{Path(row['capture']['file']).name}"
    source_url = recording_url(base_url, source)
    # The recorder uplink, not Strix compute, is the limiting resource. Bound
    # fetches independently from analyzer workers and let Strix verify the
    # recorder's sidecar before its atomic cache rename.
    with transfer_gate:
        local_hash = ssh(STRIX, fetch_verified_command(raw, source_url),
                         capture_output=True).stdout.strip().splitlines()[-1]
    trajectory_path = f"{root}/results/{ident}-trajectory.json"
    ssh(STRIX, f"cat > {quote(trajectory_path)}", input=json.dumps(trajectory(row)) + "\n")
    outputs = {}
    for method, script in (("loaded_reciprocal", "analyze-loaded-reciprocal-mchq.py"),
                           ("spectral_interference", "analyze-spectral-interference-rejection.py")):
        output = f"{root}/results/{ident}-{method}.json"
        command = (f"cd {quote(SOURCE)} && nice -n 19 {quote(PYTHON)} script/{script} "
                   f"--recording {quote(raw)} --trajectory {quote(trajectory_path)} --output {quote(output)} "
                   "--fft-size 1024 --max-doppler-hz 800 --max-delay-km 64")
        ssh(STRIX, command)
        outputs[method] = output
    ssh(STRIX, f"rm -f {quote(raw)}")
    with lock:
        state["clips"][ident] = {"status": "complete", "recording": source, "role": row["role"],
                                  "event_id": row["event_id"], "frequency_hz": row.get("frequency_hz"),
                                  "sha256": local_hash, "results": outputs}
        save(state_path, state)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--positive-manifest", required=True, type=Path)
    parser.add_argument("--miss-manifest", required=True, type=Path)
    parser.add_argument("--state", required=True, type=Path)
    parser.add_argument("--root", required=True)
    parser.add_argument("--workers", type=int, default=4)
    parser.add_argument("--transfer-workers", type=int, default=1)
    parser.add_argument("--recording-base-url", required=True)
    args = parser.parse_args()
    if not 1 <= args.workers <= 8: raise ValueError("workers must be 1..8")
    if not 1 <= args.transfer_workers <= args.workers:
        raise ValueError("transfer-workers must be 1..workers")
    state = json.loads(args.state.read_text()) if args.state.exists() else {
        "schema": "vectorwarp-historical-waveform-lane-v1", "clips": {}}
    ssh(STRIX, f"mkdir -p {quote(args.root)}/cache {quote(args.root)}/results")
    for script in ("analyze-loaded-reciprocal-mchq.py", "analyze-spectral-interference-rejection.py"):
        run(["scp", "-q", str(Path(__file__).with_name(script)), f"{STRIX}:{SOURCE}/script/{script}"])
    lock = threading.Lock()
    transfer_gate = threading.Semaphore(args.transfer_workers)
    for manifest in (args.positive_manifest, args.miss_manifest):
        rows = unique_rows(json.loads(manifest.read_text()))
        with ThreadPoolExecutor(max_workers=args.workers) as pool:
            futures = [pool.submit(process, row, args.root, state, args.state, lock,
                                   args.recording_base_url, transfer_gate) for row in rows]
            for future in as_completed(futures): future.result()


if __name__ == "__main__": main()
