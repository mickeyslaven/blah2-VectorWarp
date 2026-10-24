#!/usr/bin/env python3
"""Create the compact event matrix for the frozen current-epoch cohort."""

import argparse
import importlib.util
import json
import math
import statistics
from collections import defaultdict
from pathlib import Path


METHODS = ("spatial_glrt", "post_doppler_stap", "sparse_beam")
TREATMENTS = ("raw_as_acquired", "inverse_hardware_gain")
THRESHOLD_DB = 3.0
STACK_PATH = Path(__file__).with_name("stack-current-epoch-evidence.py")
STACK_SPEC = importlib.util.spec_from_file_location("stack_evidence", STACK_PATH)
STACK = importlib.util.module_from_spec(STACK_SPEC)
STACK_SPEC.loader.exec_module(STACK)


def treatment(clip, name):
    return clip.get("gain_treatments", {}).get(name)


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


def best_event_margin(clips):
    candidates = []
    for clip in clips:
        candidates.extend(item["margin_over_control_p99_db"]
                          for item in clip["methods"].values())
        candidates.extend(item["margin_db_equivalent"]
                          for item in stacks(clip).values())
    return max(candidates)


def gain_diagnostics(manifest, state):
    """Describe treatment effects without feeding miss outcomes into selection."""
    jobs = {job["job_id"]: job for job in manifest["jobs"]}
    margin_deltas = {role: {method: [] for method in METHODS}
                     for role in ("positive", "miss")}
    fixed_beam_changes = {role: 0 for role in ("positive", "miss")}
    fixed_beam_comparisons = {role: 0 for role in ("positive", "miss")}
    rms_changes = []
    theoretical_noise_changes = []
    for job_id, job in jobs.items():
        clip = state["results"].get(job_id, {})
        raw = treatment(clip, "raw_as_acquired")
        equalized = treatment(clip, "inverse_hardware_gain")
        if raw is None or equalized is None:
            continue
        role = job["role"]
        for method in METHODS:
            margin_deltas[role][method].append(
                equalized["methods"][method]["margin_over_control_p99_db"] -
                raw["methods"][method]["margin_over_control_p99_db"])
        raw_beam = raw["methods"]["spatial_glrt"].get("target_fixed_beam")
        equalized_beam = equalized["methods"]["spatial_glrt"].get("target_fixed_beam")
        if raw_beam is not None and equalized_beam is not None:
            fixed_beam_comparisons[role] += 1
            fixed_beam_changes[role] += raw_beam != equalized_beam
        accounting = clip.get("gain_accounting", {})
        raw_rms = accounting.get("raw_channel_rms", [])
        equalized_rms = accounting.get("equalized_channel_rms", [])
        if len(raw_rms) == len(equalized_rms) and raw_rms:
            rms_changes.append([20 * math.log10(max(after, 1e-300) /
                                                max(before, 1e-300))
                                for before, after in zip(raw_rms, equalized_rms)])
        if accounting.get("noise_power_change_db"):
            theoretical_noise_changes.append(accounting["noise_power_change_db"])
    channel_count = len(rms_changes[0]) if rms_changes else 0
    observed = [statistics.median(row[channel] for row in rms_changes)
                for channel in range(channel_count)]
    theoretical = ([statistics.median(row[channel]
                                      for row in theoretical_noise_changes)
                    for channel in range(len(theoretical_noise_changes[0]))]
                   if theoretical_noise_changes else [])
    return {
        "scope": "descriptive only; excluded from positive-only treatment selection",
        "equalized_minus_raw_margin_db": {
            role: {method: {
                "clips": len(values),
                "median": statistics.median(values) if values else None,
                "minimum": min(values) if values else None,
                "maximum": max(values) if values else None,
            } for method, values in methods.items()}
            for role, methods in margin_deltas.items()
        },
        "spatial_glrt_fixed_beam": {
            role: {"comparisons": fixed_beam_comparisons[role],
                   "changed": fixed_beam_changes[role]}
            for role in fixed_beam_changes
        },
        "channel_rms_change_db": {
            "clip_count": len(rms_changes),
            "median_observed": observed,
            "theoretical_from_hardware_gain_scaling": theoretical,
            "interpretation": (
                "Positive values amplify both antenna voltage and receiver noise; "
                "the low-gain channels receive the largest upward scaling."
            ),
        },
    }


def select_gain_treatment(manifest, state):
    """Freeze one treatment using positive controls only."""
    positive_jobs = [job for job in manifest["jobs"] if job["role"] == "positive"]
    missing = [job["job_id"] for job in positive_jobs
               if not all(treatment(state["results"].get(job["job_id"], {}), name)
                          for name in TREATMENTS)]
    if missing:
        raise ValueError(f"positive gain evaluation incomplete: {len(missing)} jobs")
    events = defaultdict(list)
    for job in positive_jobs:
        events[job["event_id"]].append(state["results"][job["job_id"]])
    rows = {}
    for name in TREATMENTS:
        margins = [best_event_margin([treatment(clip, name) for clip in clips])
                   for clips in events.values()]
        rows[name] = {
            "positive_events_retained": sum(value >= THRESHOLD_DB for value in margins),
            "median_best_margin_db": statistics.median(margins),
            "event_best_margins_db": margins,
        }
    # Stable final tie-break deliberately favors current raw behavior.
    selected = max(TREATMENTS, key=lambda name: (
        rows[name]["positive_events_retained"], rows[name]["median_best_margin_db"],
        name == "raw_as_acquired"))
    return {
        "schema": "vectorwarp-current-epoch-gain-selection-v1",
        "config_epoch": manifest["config_epoch"],
        "selection_scope": "positive controls only; miss results were not inspected",
        "rule": "maximize retained positive events at +3 dB; tie by median best event margin; final tie raw",
        "selected": selected, "treatments": rows,
    }


def summarize(manifest, state, selection=None):
    expected = {job["job_id"] for job in manifest["jobs"]}
    present = set(state["results"])
    if missing := sorted(expected - present):
        raise ValueError(f"incomplete run: {len(missing)} jobs missing; first={missing[0]}")
    selection = selection or select_gain_treatment(manifest, state)
    selected_treatment = selection["selected"]
    events = defaultdict(list)
    jobs = {job["job_id"]: job for job in manifest["jobs"]}
    for job_id in sorted(expected):
        raw = state["results"][job_id]
        selected = treatment(raw, selected_treatment)
        if selected is None:
            raise ValueError(f"job lacks selected gain treatment: {job_id}")
        events[jobs[job_id]["event_id"]].append({**raw, **selected})
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
        "gain_selection": selection,
        "scope": {"events": len(rows), "jobs": len(expected),
                  "unique_recordings": len(state["recordings"]),
                  "miss_events": len(misses),
                  "miss_jobs": sum(row["clip_count"] for row in misses),
                  "positive_events": len(positives),
                  "positive_jobs": sum(row["clip_count"] for row in positives)},
        "decision": {"minimum_margin_db": THRESHOLD_DB,
                     "event_rule": "any clip at or above threshold",
                     "familywise_alpha": .01,
                     "correction": "Bonferroni across 7 combinations under each of 2 gain treatments plus signed inversion, times event clip count",
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
        "gain_accounting": {
            "evaluated": list(TREATMENTS),
            "selected_for_miss_decisions": selected_treatment,
            "covariance_whitening": "intrinsic in all three methods; no extra preprocessing was applied to avoid double whitening",
            "diagnostics": gain_diagnostics(manifest, state),
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
    parser.add_argument("--gain-selection", type=Path)
    args = parser.parse_args()
    selection = json.loads(args.gain_selection.read_text()) if args.gain_selection else None
    result = summarize(json.loads(args.manifest.read_text()),
                       json.loads(args.state.read_text()), selection)
    args.output.write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result["summary"], indent=2))


if __name__ == "__main__":
    main()
