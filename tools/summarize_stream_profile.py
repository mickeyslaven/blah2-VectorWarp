#!/usr/bin/env python3
"""Summarize bounded STREAM_PROFILE records from a saved journal (no mutations)."""
import argparse
import json
import math
import re
from collections import defaultdict
from pathlib import Path


def percentile(values, fraction):
    values = sorted(values)
    return values[max(0, math.ceil(len(values) * fraction) - 1)] if values else None


def metrics(values):
    return {"n": len(values), "mean": sum(values) / len(values) if values else None,
            "p50": percentile(values, .5), "p95": percentile(values, .95),
            "p99": percentile(values, .99), "max": max(values, default=None),
            "over1": sum(v > 1 for v in values), "over10": sum(v > 10 for v in values),
            "over100": sum(v > 100 for v in values), "over500": sum(v > 500 for v in values)}


def activity_totals(rows):
    return {key: sum(row.get(key, 0) for row in rows)
            for key in rows[0] if key.endswith(("_minor", "_major", "_nvcsw", "_nivcsw", "_endpoint_migration"))}


def summarize(text):
    groups = defaultdict(list)
    for line in text.splitlines():
        match = re.search(r"STREAM_PROFILE_(CYCLE|WORK|CLUTTER|RECEIVE|HIST|RECEIVE_META) (.*)", line)
        if not match:
            continue
        row = dict(re.findall(r"([a-zA-Z0-9_]+)=([^\s]+)", match[2]))
        for key, value in row.copy().items():
            try:
                row[key] = float(value) if "." in value else int(value)
            except ValueError:
                pass
        pid = re.search(r"\[(\d+)\]:", line[:match.start()])
        row["journal_pid"] = str(row.get("pid", pid[1] if pid else "unknown"))
        groups[match[1]].append(row)
    result = {"scope": "bounded opt-in observations; no cross-restart aggregation",
              "receiver_histograms": groups["HIST"],
              "receiver_sampling": groups["RECEIVE_META"], "runs": []}
    runs = defaultdict(list)
    for row in groups["CYCLE"]:
        runs[(row["journal_pid"], row["epoch"])].append(row)
    for (pid, epoch), rows in runs.items():
        # A repeated sequence starts a new process when a journal format omitted PID.
        rows.sort(key=lambda r: r["start_us"])
        if len({r["sequence"] for r in rows}) != len(rows):
            raise ValueError("Repeated cycle sequence: supply a journal containing explicit process IDs")
        run = {"pid": pid, "epoch": epoch, "cycles": len(rows), "metrics_ms": {},
               "activity_totals": activity_totals(rows)}
        for key in rows[0]:
            if key.endswith("_ms"):
                values = [r[key] for r in rows]
                if key in ("completion_ms", "idle_ms"):
                    values = values[1:]
                run["metrics_ms"][key] = metrics(values)
        backlog = [r["producer_queued"] + r["pending_queued"] for r in rows]
        elapsed = (rows[-1]["start_us"] - rows[0]["start_us"]) / 1e6
        run["queue"] = {"first_samples": backlog[0], "last_samples": backlog[-1],
                        "peak_samples": max(backlog), "elapsed_s": elapsed,
                        "endpoint_slope_samples_per_s": (backlog[-1] - backlog[0]) / elapsed if elapsed else None}
        rate = rows[0].get("sample_rate", 0)
        if rate:
            run["queue"].update(first_seconds=backlog[0] / rate, last_seconds=backlog[-1] / rate,
                                peak_seconds=max(backlog) / rate)
        phases = ("queue_wait_ms", "queue_locked_ms", "capture_install_ms", "pre_spectrum_ms",
                  "spectrum_ms", "capture_staging_ms", "pre_pipeline_ms", "pre_radar_ms",
                  "radar_ms", "serialize_ms", "output_ms", "finish_next_ms", "publication_ms", "destruction_ms")
        if all(key in row for row in rows for key in phases):
            run["maximum_phase_sum_error_ms"] = max(abs(sum(row[key] for key in phases) - row["processing_ms"])
                                                     for row in rows)
        run["largest_cycles"] = []
        for row in sorted(rows, key=lambda r: r["processing_ms"], reverse=True)[:8]:
            lo, hi = row["start_us"], row["retired_us"]
            work = [w for w in groups["WORK"] if w["start_us"] < hi and w["end_us"] > lo]
            receive = [r for r in groups["RECEIVE"] if lo <= r["mono_us"] <= hi]
            run["largest_cycles"].append({"cycle": row, "overlapping_work": work,
                                          "overlapping_receiver_tails": receive,
                                          "overlapping_clutter": [c for c in groups["CLUTTER"]
                                                                  if c["start_us"] < hi and c["end_us"] > lo]})
        result["runs"].append(run)
    workers = defaultdict(list)
    for row in groups["WORK"]:
        workers[(row["journal_pid"], row["role"], row["identity"])].append(row)
    result["workers"] = [{"pid": pid, "role": role, "identity": identity,
                           "activity_totals": activity_totals(rows),
                           "metrics_ms": {key: metrics([r[key] for r in rows])
                                          for key in rows[0] if key.endswith("_ms")}}
                         for (pid, role, identity), rows in workers.items()]
    clutter = defaultdict(list)
    for row in groups["CLUTTER"]:
        clutter[row["journal_pid"]].append(row)
    result["clutter_profiles"] = [
        {"pid": pid, "frames": len(rows), "activity_totals": activity_totals(rows),
         "metrics_ms": {key: metrics([r[key] for r in rows if key in r])
                        for key in sorted({k for r in rows for k in r if k.endswith("_ms")})}}
        for pid, rows in clutter.items()]
    # PIDs and transport sequence/op, not independent frame counters, identify
    # transactions. Same-host monotonic clocks also bind the module's internal
    # clutter frame to its enclosing worker transaction without a wire change.
    child = {}
    for row in groups["WORK"]:
        if row["role"] == "portable-worker" and row.get("peer_pid"):
            key = (row["journal_pid"], str(row["peer_pid"]), row["sequence"], row["identity"])
            if key in child:
                raise ValueError("Repeated portable worker sequence: ambiguous process identity")
            child[key] = row
    transactions, unmatched = [], 0
    for parent in groups["WORK"]:
        if parent["role"] != "portable-parent" or not parent.get("peer_pid"):
            continue
        worker = child.get((str(parent["peer_pid"]), parent["journal_pid"],
                            parent["sequence"], parent["identity"]))
        if not worker:
            unmatched += 1
            continue
        item = {"parent": parent, "worker": worker,
                "clutter": [c for c in clutter[worker["journal_pid"]]
                            if worker["start_us"] <= c["start_us"] and c["end_us"] <= worker["end_us"]]}
        if all(key in row for row in (parent, worker) for key in ("phase1_us", "phase2_us")):
            item["metrics_ms"] = {
                # Includes IPC send/receipt/scheduling: not a pure runqueue wait.
                "dispatch_to_receipt_ms": (worker["start_us"] - parent["start_us"]) / 1000,
                "worker_compute_ms": (worker["phase1_us"] - worker["start_us"]) / 1000,
                "worker_response_ms": (worker["phase2_us"] - worker["phase1_us"]) / 1000,
                "response_to_parent_ms": (parent["phase2_us"] - worker["phase2_us"]) / 1000,
                "parent_validation_ms": (parent["end_us"] - parent["phase2_us"]) / 1000}
        transactions.append(item)
    result["portable_transactions"] = {
        "note": "same-host monotonic; IPC/scheduling intervals are not pure queue wait; response/send endpoint races may be negative",
        "matched": len(transactions), "unmatched_parent": unmatched,
        "metrics_ms": {key: metrics([t["metrics_ms"][key] for t in transactions if "metrics_ms" in t])
                       for key in sorted({k for t in transactions for k in t.get("metrics_ms", {})})},
        "records": transactions}
    return result


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("journal", type=Path)
    args = parser.parse_args()
    print(json.dumps(summarize(args.journal.read_text()), indent=2, allow_nan=False))
