#!/usr/bin/env python3
"""Maintain a strict rolling fresh-IQ ledger and atomically freeze a run.

Updating admission never launches replay.  Freezing writes a content-addressed,
one-clip-per-pass snapshot; later admissions remain in the rolling ledger and
therefore belong to a subsequent run.
"""

import argparse
import hashlib
import importlib.util
import json
import os
import tempfile
import time
from pathlib import Path


ROOT = Path(__file__).parent


def load_module(name, filename):
    spec = importlib.util.spec_from_file_location(name, ROOT / filename)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


FRESH = load_module("fresh_builder", "build-fresh-postfix-cohort.py")
FINAL = load_module("fresh_finalizer", "finalize-fresh-cohort-snapshot.py")


def atomic_json(path, value):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary = tempfile.mkstemp(prefix=path.name + ".", dir=path.parent)
    try:
        with os.fdopen(descriptor, "w") as stream:
            json.dump(value, stream, indent=2)
            stream.write("\n")
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


def candidate_key(row):
    capture = row["capture"]
    return f'{row["event_id"]}|r{capture["revision"]}'


def summary(candidates):
    rows = list(candidates.values())
    return {
        "candidate_clips": len(rows),
        "positive_clips": sum(row["role"] == "positive" for row in rows),
        "miss_clips": sum(row["role"] == "geometry_eligible_miss" for row in rows),
        "positive_passes": len({row["pass_id"] for row in rows if row["role"] == "positive"}),
        "miss_passes": len({row["pass_id"] for row in rows
                            if row["role"] == "geometry_eligible_miss"}),
        "unique_aircraft": len({row["hex"] for row in rows}),
    }


def update(existing, statistics, policy, cutoff_ms, rf, config_epoch, receiver_epoch):
    if existing:
        if existing["status"] != "rolling_admission_open":
            raise ValueError("ledger is not open for rolling admission")
        if existing["cutoff_ms"] != cutoff_ms:
            raise ValueError("cutoff change requires a new rolling ledger")
        if existing["rf"] != rf:
            raise ValueError("RF metadata changed; refuse to mix epochs")
        if existing["config_epoch"] != config_epoch or existing["receiver_epoch"] != receiver_epoch:
            raise ValueError("configured epoch changed; refuse to mix epochs")
        candidates = dict(existing["candidates"])
        created_at_ms = existing["created_at_ms"]
    else:
        candidates = {}
        created_at_ms = int(time.time() * 1000)
    fresh = FRESH.build(statistics, policy, cutoff_ms, [], rf)
    rejected_epoch = []
    for row in fresh["admitted"]:
        if row.get("config_epoch") != config_epoch or row.get("receiver_epoch") != receiver_epoch:
            rejected_epoch.append({"event_id": row.get("event_id"),
                                   "revision": row.get("capture", {}).get("revision"),
                                   "reason": "config_or_receiver_epoch_mismatch"})
            continue
        candidates[candidate_key(row)] = row
    value = {
        "schema": "vectorwarp-strict-fresh-rolling-cohort-v1",
        "status": "rolling_admission_open",
        "replay_execution": "blocked_until_native_backends_and_atomic_freeze",
        "created_at_ms": created_at_ms,
        "updated_at_ms": int(time.time() * 1000),
        "source_statistics_updated_at_ms": statistics.get("updated_at"),
        "cutoff_ms": cutoff_ms,
        "policy": policy,
        "rf": rf,
        "config_epoch": config_epoch,
        "receiver_epoch": receiver_epoch,
        "admission": {
            "truth": "strict_bracketed_event_time_interpolation",
            "nearest_sample_hold": False,
            "fallback": False,
            "deduplication_key": "event_id|capture_revision",
            "all_valid_positives_retained": True,
        },
        "candidates": dict(sorted(candidates.items())),
        "last_refresh": {
            "valid_rows_seen": len(fresh["admitted"]),
            "rejected_rows_seen": len(fresh["rejected"]),
            "epoch_rejections": rejected_epoch,
        },
    }
    value["summary"] = summary(value["candidates"])
    return value


def freeze(ledger):
    if ledger.get("status") != "rolling_admission_open":
        raise ValueError("only an open rolling ledger can be frozen")
    groups = {}
    for row in ledger["candidates"].values():
        groups.setdefault(row["pass_id"], []).append(row)
    selected = [FINAL.choose(rows) for rows in groups.values()]
    selected.sort(key=lambda row: (row["role"] != "positive", row["pass_id"]))
    ledger_bytes = (json.dumps(ledger, sort_keys=True, separators=(",", ":")) + "\n").encode()
    value = {
        "schema": "vectorwarp-atomic-fresh-matrix-cohort-v1",
        "status": "frozen_for_full_matrix_launch",
        "frozen_at_ms": int(time.time() * 1000),
        "rolling_ledger_canonical_sha256": hashlib.sha256(ledger_bytes).hexdigest(),
        "cutoff_ms": ledger["cutoff_ms"],
        "policy": ledger["policy"],
        "rf": ledger["rf"],
        "config_epoch": ledger["config_epoch"],
        "receiver_epoch": ledger["receiver_epoch"],
        "selection_policy": {
            "clips_per_pass": 1,
            "positives": "maximize in-window radar evidence, then truth freshness",
            "misses": "minimize raw offset/disagreement, then central-window distance",
        },
        "selected": selected,
        "summary": {
            **summary({candidate_key(row): row for row in selected}),
            "valid_windows_before_dedup": len(ledger["candidates"]),
        },
    }
    payload = json.dumps(value, sort_keys=True, separators=(",", ":")).encode()
    value["snapshot_sha256"] = hashlib.sha256(payload).hexdigest()
    return value


def main():
    parser = argparse.ArgumentParser()
    sub = parser.add_subparsers(dest="command", required=True)
    refresh = sub.add_parser("update")
    refresh.add_argument("--statistics", required=True, type=Path)
    refresh.add_argument("--policy", required=True, type=Path)
    refresh.add_argument("--rf-metadata", required=True, type=Path)
    refresh.add_argument("--cutoff-ms", required=True, type=int)
    refresh.add_argument("--config-epoch", required=True)
    refresh.add_argument("--receiver-epoch", required=True)
    refresh.add_argument("--ledger", required=True, type=Path)
    seal = sub.add_parser("freeze")
    seal.add_argument("--ledger", required=True, type=Path)
    seal.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    if args.command == "update":
        existing = json.loads(args.ledger.read_text()) if args.ledger.exists() else None
        value = update(existing, json.loads(args.statistics.read_text()),
                       json.loads(args.policy.read_text()), args.cutoff_ms,
                       json.loads(args.rf_metadata.read_text()),
                       args.config_epoch, args.receiver_epoch)
        atomic_json(args.ledger, value)
    else:
        value = freeze(json.loads(args.ledger.read_text()))
        atomic_json(args.output, value)
    print(json.dumps(value["summary"], sort_keys=True))


if __name__ == "__main__":
    main()
