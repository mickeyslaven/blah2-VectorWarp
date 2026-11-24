#!/usr/bin/env python3
"""Export a fully fsynced Timing checkpoint into release-regression runtime-v1 blocks.

This offline adapter retains exact native per-lane evidence. It does not collect
radar data, authorize a release, or replace the independently reviewed scorer.
"""
import argparse
import fcntl
import hashlib
from pathlib import Path
import re
import sys

from vectorwarp_release_regression import (Evidence, IDENTITY_FIELDS, atomic_bytes,
    count, decode, digest, encoded, keys, read_bytes, require, sha256)
from vectorwarp_two_node_soak import load_binding

MAX_LEDGER_BYTES = 512 << 20
LANE_COUNTERS = ("acceptedSamples", "droppedSamples", "discardedSamples")
OUTCOMES = ("completed", "warmup", "rejected", "drain", "final")


def exact(value):
    require(isinstance(value, str) and re.fullmatch(r"0|[1-9][0-9]{0,19}", value),
            "missing exact unsigned decimal counter")
    result = int(value)
    require(count(result), "uint64 overflow")
    return result


def capture_snapshot(value, previous=None):
    keys(value, ("available", "epoch", "discontinuities", "requalifications", "retiredCpis", "retiredSamples", "lanes"), "capture")
    require(value["available"] is True and isinstance(value["lanes"], list) and
            0 < len(value["lanes"]) <= 16, "exact all-lane capture counters unavailable")
    exact(value["epoch"])
    result = {"epoch": value["epoch"], **{key: exact(value[key]) for key in
              ("discontinuities", "requalifications", "retiredCpis", "retiredSamples")}, **{key: 0 for key in LANE_COUNTERS}}
    if previous:
        require(value["epoch"] == previous["epoch"] and len(value["lanes"]) == len(previous["lanes"]),
                "capture epoch/lane reset")
        require(all(exact(value[key]) >= exact(previous[key]) for key in
                    ("discontinuities", "requalifications", "retiredCpis", "retiredSamples")), "capture counter reset")
    for index, lane in enumerate(value["lanes"]):
        keys(lane, LANE_COUNTERS, "capture lane")
        for key in LANE_COUNTERS:
            result[key] += exact(lane[key])
            if previous:
                require(exact(lane[key]) >= exact(previous["lanes"][index][key]), "lane counter reset")
    require(all(count(result[key]) for key in result if key != "epoch"), "aggregate capture counter overflow")
    return result


def export_ledger(directory, candidate, archive, block_size=128):
    keys(candidate, IDENTITY_FIELDS, "candidate identity")
    require(re.fullmatch(r"[0-9a-f]{40}", candidate["sourceCommit"]) and
            all(digest(candidate[key]) for key in IDENTITY_FIELDS[1:]), "invalid candidate identity")
    require(type(block_size) is int and 1 <= block_size <= 1024, "invalid export block size")
    require(directory.is_dir() and not directory.is_symlink(), "ledger directory must be real")
    seal_raw = read_bytes(directory / "status.json")
    seal = decode(seal_raw)
    keys(seal, ("schema", "state", "fault", "anchorMonoNs", "anchorUtcNs", "generation", "records",
                "throughSequence", "bytes", "sha256"), "ledger seal")
    require(seal["schema"] == "vectorwarp-cycle-ledger-v1" and seal["state"] in ("checkpoint", "complete") and
            seal["fault"] == "" and count(seal["records"]) and seal["records"] > 0 and
            count(seal["generation"]) and seal["generation"] > 0 and
            count(seal["bytes"]) and 0 < seal["bytes"] <= MAX_LEDGER_BYTES and digest(seal["sha256"]),
            "ledger is not cleanly sealed")
    mono, utc = exact(seal["anchorMonoNs"]), exact(seal["anchorUtcNs"])
    require(mono > 0 and utc > 0, "invalid clock anchors")
    filename = directory / "cycles.jsonl"
    require(filename.is_file() and not filename.is_symlink() and filename.stat().st_size >= seal["bytes"],
            "ledger checkpoint prefix missing/truncated")
    seal_sha = archive.retain(seal_raw)
    measured = hashlib.sha256()
    previous_capture = None
    previous_end = None
    sequence = 0
    references, retained, cycles = [], [], []
    boundary_capture = boundary_epoch = first_epoch = None

    def epoch(nanoseconds):
        mapped = utc + nanoseconds - mono
        require(count(mapped), "mapped clock overflow")
        return mapped / 1_000_000_000

    def flush():
        nonlocal boundary_capture, boundary_epoch, retained, cycles
        if not cycles:
            return
        capture_end = capture_snapshot(retained[-1]["captureAfter"])
        require(capture_end["acceptedSamples"] > boundary_capture["acceptedSamples"],
                "runtime block has no accepted sample advance")
        raw_sha = archive.result({"schema": "vectorwarp-retained-cycle-block-v1",
            "anchorMonoNs": seal["anchorMonoNs"],
            "anchorUtcNs": seal["anchorUtcNs"], "records": retained})
        references.append(archive.result({"schema": "vectorwarp-release-runtime-v1", **candidate,
            "startEpoch": boundary_epoch, "endEpoch": cycles[-1]["completedEpoch"],
            "captureStart": boundary_capture, "captureEnd": capture_end,
            "completedCpisBefore": cycles[0]["sequence"] - 1,
            "completedCpisThrough": cycles[-1]["sequence"], "cycles": cycles,
            "rawEvidenceSha256": raw_sha}))
        boundary_capture, boundary_epoch = capture_end, cycles[-1]["completedEpoch"]
        retained, cycles = [], []

    # Pin the already-fsynced prefix. A concurrent writer may revoke/replace
    # status and append a newer tail; neither changes the bytes this seal owns.
    remaining = seal["bytes"]
    with filename.open("rb") as stream:
        while remaining:
            line = stream.readline(min(32769, remaining))
            require(len(line) <= 32768 and line.endswith(b"\n"), "oversized/truncated cycle record")
            remaining -= len(line)
            measured.update(line)
            record = decode(line)
            keys(record, ("sequence", "startMonoNs", "endMonoNs", "drainBatches", "outcome",
                          "captureBefore", "captureAfter"), "cycle")
            sequence += 1
            require(exact(record["sequence"]) == sequence, "missing/duplicate/reset cycle sequence")
            start, end = exact(record["startMonoNs"]), exact(record["endMonoNs"])
            exact(record["drainBatches"])
            require(start >= mono and end > start and (previous_end is None or start >= previous_end),
                    "cycle clock regression/overlap")
            require(record["outcome"] in OUTCOMES, "unknown processing outcome")
            before = capture_snapshot(record["captureBefore"], previous_capture)
            after = capture_snapshot(record["captureAfter"], record["captureBefore"])
            previous_capture, previous_end = record["captureAfter"], end
            if boundary_capture is None:
                boundary_capture, boundary_epoch = before, epoch(start)
                first_epoch = boundary_epoch
            # Keep the final advancing block pending until another advancing
            # cycle starts its successor. Drain/final-only tails then remain in
            # their predecessor, independent of the requested block alignment.
            if len(cycles) >= block_size:
                pending_end = capture_snapshot(retained[-1]["captureAfter"])
                if (pending_end["acceptedSamples"] > boundary_capture["acceptedSamples"] and
                        after["acceptedSamples"] > pending_end["acceptedSamples"]):
                    flush()
            retained.append(record)
            cycles.append({"sequence": sequence, "startedEpoch": epoch(start), "completedEpoch": epoch(end)})
    require(sequence == seal["records"] == exact(seal["throughSequence"]) and
            measured.hexdigest() == seal["sha256"], "ledger count/hash differs from seal")
    flush()
    return {"schema": "vectorwarp-exported-runtime-v1", **candidate, "startEpoch": first_epoch,
            "endEpoch": boundary_epoch, "runtimeEvidence": references, "ledgerSealSha256": seal_sha}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for flag in ("ledger", "binding", "node", "evidence-dir", "output"):
        parser.add_argument("--" + flag, required=True)
    args = parser.parse_args()
    output = Path(args.output).resolve()
    try:
        ledger = Path(args.ledger).resolve()
        require(output.parent != ledger and output != Path(args.binding).resolve(), "output overlaps source evidence")
        with output.with_suffix(output.suffix + ".lock").open("a+b") as lock:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            atomic_bytes(output, encoded({"schema": "vectorwarp-exported-runtime-v1", "error": "export-incomplete"}))
            binding = load_binding(args.binding)
            node = binding["nodes"][args.node]
            candidate = {"sourceCommit": binding["sourceCommit"], **{key: node[key] for key in IDENTITY_FIELDS[1:]}}
            archive = Evidence(Path(args.evidence_dir), binding["monitorMinimumFreeBytes"])
            report = export_ledger(ledger, candidate, archive)
            report["bindingSha256"] = archive.retain(read_bytes(Path(args.binding)))
            report["exporterSha256"] = archive.retain(Path(__file__).read_bytes())
            atomic_bytes(output, encoded(report))
        return 0
    except (ValueError, OSError, KeyError, TypeError, OverflowError) as error:
        print(f"cycle ledger export refused: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
