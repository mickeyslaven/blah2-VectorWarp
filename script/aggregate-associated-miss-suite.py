#!/usr/bin/env python3
"""Aggregate target-specific production spatial results without validation claims."""
import argparse
import hashlib
import json
from pathlib import Path


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def method_rows(result):
    rows = []
    for treatment, contents in result.get("gain_treatments", {}).items():
        for method, value in contents.get("methods", {}).items():
            exceed = value.get("controls_exceeding_target")
            rows.append({
                "gain_treatment": treatment,
                "method": method,
                "target_score": value.get("target_score"),
                "margin_over_control_p99_db": value.get("margin_over_control_p99_db"),
                "controls": value.get("controls"),
                "controls_exceeding_target": exceed,
                "raw_rank": None if exceed is None else exceed + 1,
                "family_trials": result.get("family_trials"),
            })
    return rows


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--cohort", required=True, type=Path)
    parser.add_argument("--results", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--positive-results", type=Path)
    args = parser.parse_args()
    cohort = json.loads(args.cohort.read_text())
    positive_files = {item["capture"]["file"] for item in cohort["positives"]}
    labels, missing = [], []
    for job in cohort["misses"]:
        path = args.results / f'{job["job_id"]}-result.json'
        if not path.exists():
            missing.append(job["job_id"])
            continue
        result = json.loads(path.read_text())
        rows = method_rows(result)
        labels.append({
            "job_id": job["job_id"], "event_id": job["event_id"],
            "pass_id": job["pass_id"], "hex": job["hex"], "flight": job.get("flight"),
            "recording": job["capture"]["file"],
            "scope": "overlap_flagged" if job["capture"]["file"] in positive_files else "clean",
            "raw_methods": rows,
            "best_raw_margin_db": max((row["margin_over_control_p99_db"] for row in rows
                                        if row["margin_over_control_p99_db"] is not None), default=None),
            "classification": "exploratory_candidate_unvalidated",
        })
    labels.sort(key=lambda item: (item["best_raw_margin_db"] is None,
                                  -(item["best_raw_margin_db"] or 0)))
    positive = []
    if args.positive_results:
        for path in sorted(args.positive_results.glob("*-result.json")):
            result = json.loads(path.read_text())
            positive.append({"job_id": result["job_id"], "pass_id": result.get("hex"),
                             "raw_methods": method_rows(result)})
    output = {
        "schema": "vectorwarp-associated-production-spatial-exploratory-v1",
        "cohort_sha256": digest(args.cohort),
        "cohort_counts": cohort["summary"],
        "completed_labels": len(labels), "missing_labels": missing,
        "validation": {
            "status": "not_validated",
            "reason": "two positive passes fail the production spatial positive-margin gate; no held-out recovery claim is permitted",
            "trial_corrected_labels": False,
            "plus_3db_validated": False,
        },
        "positive_regression": positive,
        "labels": labels,
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(output, indent=2) + "\n")
    if missing:
        raise SystemExit(f"missing {len(missing)} result(s): {', '.join(missing)}")


if __name__ == "__main__":
    main()
