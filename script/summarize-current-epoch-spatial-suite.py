#!/usr/bin/env python3
"""Create the compact event matrix for the frozen current-epoch cohort."""

import argparse
import importlib.util
import json
from collections import defaultdict
from pathlib import Path


METHODS = ("spatial_glrt", "post_doppler_stap", "sparse_beam")
THRESHOLD_DB = 3.0
STACK_PATH = Path(__file__).with_name("stack-current-epoch-evidence.py")
STACK_SPEC = importlib.util.spec_from_file_location("stack_evidence", STACK_PATH)
STACK = importlib.util.module_from_spec(STACK_SPEC)
STACK_SPEC.loader.exec_module(STACK)


def stacks(clip):
    source = {method: {
        "target": clip["methods"][method]["evidence_log10"],
        "controls": clip["methods"][method]["control_evidence_log10"],
    } for method in METHODS}
    result = STACK.stack(source)
    return {"+".join(row["combination"]): {
                "margin_evidence": row["margin"],
                "margin_db_equivalent": 10 * row["margin"],
                "recovered": 10 * row["margin"] >= THRESHOLD_DB,
                "target": row["target"],
                "familywise_control_p99": row["control_p99_all_trials"],
            } for row in result["rows"] if not row["single"]}


def summarize(manifest, state):
    expected = {job["job_id"] for job in manifest["jobs"]}
    present = set(state["results"])
    if missing := sorted(expected - present):
        raise ValueError(f"incomplete run: {len(missing)} jobs missing; first={missing[0]}")
    events = defaultdict(list)
    jobs = {job["job_id"]: job for job in manifest["jobs"]}
    for job_id in sorted(expected):
        events[jobs[job_id]["event_id"]].append(state["results"][job_id])
    rows = []
    for event_id, clips in sorted(events.items()):
        first = clips[0]
        methods = {}
        for method in METHODS:
            ranked = sorted((clip["methods"][method]["margin_over_control_p99_db"],
                             clip["job_id"]) for clip in clips)
            margin, job_id = ranked[-1]
            methods[method] = {"best_margin_db": margin, "best_job_id": job_id,
                               "recovered": margin >= THRESHOLD_DB}
        clip_stacks = {clip["job_id"]: stacks(clip) for clip in clips}
        stack_names = sorted(next(iter(clip_stacks.values())))
        stacked = {}
        for name in stack_names:
            job_id, best = max(((job_id, values[name])
                                for job_id, values in clip_stacks.items()),
                               key=lambda item: item[1]["margin_db_equivalent"])
            stacked[name] = {"best_margin_db_equivalent":
                             best["margin_db_equivalent"],
                             "best_job_id": job_id,
                             "recovered": best["recovered"]}
        rows.append({
            "event_id": event_id, "hex": first["hex"], "flight": first["flight"],
            "role": first["role"], "outcome": first["outcome"],
            "clip_count": len(clips), "family_trials": first["family_trials"],
            "control_quantile": first["control_quantile"], "methods": methods,
            "stacks": stacked,
            "recovered_any": any(item["recovered"] for item in methods.values()) or
                             any(item["recovered"] for item in stacked.values()),
            "clips": [{"job_id": clip["job_id"], "revision": clip["revision"],
                       "margins_db": {method: clip["methods"][method]
                                      ["margin_over_control_p99_db"]
                                      for method in METHODS},
                       "stack_margins_db_equivalent": {
                           name: item["margin_db_equivalent"]
                           for name, item in clip_stacks[clip["job_id"]].items()}}
                      for clip in sorted(clips, key=lambda item: item["job_id"])],
        })
    misses = [row for row in rows if row["role"] == "miss"]
    positives = [row for row in rows if row["role"] == "positive"]
    return {
        "schema": "vectorwarp-current-epoch-spatial-adaptive-matrix-v1",
        "config_epoch": manifest["config_epoch"],
        "statistics_sha256": manifest["statistics_sha256"],
        "scope": {"events": len(rows), "jobs": len(expected),
                  "unique_recordings": len(state["recordings"]),
                  "miss_events": len(misses),
                  "miss_jobs": sum(row["clip_count"] for row in misses),
                  "positive_events": len(positives),
                  "positive_jobs": sum(row["clip_count"] for row in positives)},
        "decision": {"minimum_margin_db": THRESHOLD_DB,
                     "event_rule": "any clip at or above threshold",
                     "familywise_alpha": .01,
                     "correction": "Bonferroni across 3 singles + 3 pairs + all-three stack times event clip count",
                     "empirical_controls": "target-excluded translated exact-time trajectories; all paths counted"},
        "methods": {
            "spatial_glrt": {"beam_policy": "fixed", "guard": 1,
                             "training": 3, "diagonal_loading": .01,
                             "minimum_abs_doppler_hz": 0},
            "post_doppler_stap": {"doppler_radius": 1, "dimensions": 15,
                                  "diagonal_loading": .1,
                                  "robust_energy_clip": 3.0,
                                  "minimum_abs_doppler_hz": 0},
            "sparse_beam": {"lasso_penalty": .5, "guard": 2,
                            "training": 5, "diagonal_loading": .3},
        },
        "summary": {
            "misses_recovered_any": sum(row["recovered_any"] for row in misses),
            "positive_events_retained_any": sum(row["recovered_any"] for row in positives),
            "by_method": {method: {
                "misses_recovered": sum(row["methods"][method]["recovered"] for row in misses),
                "positive_events_retained": sum(row["methods"][method]["recovered"] for row in positives),
            } for method in METHODS},
            "by_stack": {name: {
                "misses_recovered": sum(row["stacks"][name]["recovered"] for row in misses),
                "positive_events_retained": sum(row["stacks"][name]["recovered"]
                                                for row in positives),
            } for name in (sorted(rows[0]["stacks"]) if rows else [])},
        },
        "events": rows,
    }


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--manifest", required=True, type=Path)
    parser.add_argument("--state", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    result = summarize(json.loads(args.manifest.read_text()),
                       json.loads(args.state.read_text()))
    args.output.write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result["summary"], indent=2))


if __name__ == "__main__":
    main()
