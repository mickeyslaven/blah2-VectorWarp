#!/usr/bin/env python3
"""Exploratory signed low-tail test for the sparse spatial statistic."""

import argparse
import json
import math
from collections import defaultdict
from pathlib import Path


METHOD = "sparse_beam"
VARIANT_TRIALS = 15  # seven combinations under two gain treatments + inversion
ALPHA = .01
MINIMUM_MARGIN_DB = 3.0


def percentile(values, fraction):
    ordered = sorted(values)
    return ordered[max(0, min(len(ordered) - 1,
                              math.ceil(fraction * len(ordered)) - 1))]


def fisher_probability(probabilities):
    """Upper-tail chi-square probability for Fisher's even degrees of freedom."""
    half = -sum(math.log(max(value, 1e-300)) for value in probabilities)
    return min(1.0, math.exp(-half) *
               sum(half ** term / math.factorial(term)
                   for term in range(len(probabilities))))


def score(job, result, event_clip_count):
    vector = result["score_vectors"][METHOD]
    target = vector["target"]["raw_score"]
    controls = [item["raw_score"] for item in vector["controls"]]
    below = sum(value <= target for value in controls)
    probability = (1 + below) / (1 + len(controls))
    quantile = ALPHA / (VARIANT_TRIALS * event_clip_count)
    threshold = percentile(controls, quantile)
    margin = 10 * math.log10(max(threshold, 1e-300) / max(target, 1e-300))
    return {"job_id": job["job_id"], "target_score": target,
            "controls": len(controls), "controls_at_or_below_target": below,
            "lower_tail_p": probability, "family_lower_quantile": quantile,
            "family_lower_control": threshold, "notch_margin_db": margin,
            "clears_three_db": margin >= MINIMUM_MARGIN_DB}


def cohort(name, event_rows):
    event_results = []
    for event_id, rows in event_rows:
        clip_count = len(rows)
        clip_scores = [score(job, result, clip_count) for job, result in rows]
        best = min(clip_scores, key=lambda item: item["lower_tail_p"])
        event_p = min(1.0, best["lower_tail_p"] * clip_count)
        event_results.append({"event_id": event_id, "hex": rows[0][0]["hex"],
                              "event_lower_tail_p": event_p,
                              "best_notch_margin_db": max(item["notch_margin_db"]
                                                          for item in clip_scores),
                              "clips": clip_scores})
    combined = fisher_probability([item["event_lower_tail_p"] for item in event_results])
    corrected = min(1.0, combined * VARIANT_TRIALS)
    consistent = all(item["event_lower_tail_p"] <= .5 for item in event_results)
    clears_margin = all(item["best_notch_margin_db"] >= MINIMUM_MARGIN_DB
                        for item in event_results)
    return {"name": name, "events": event_results,
            "fisher_p": combined, "variant_corrected_fisher_p": corrected,
            "directionally_consistent": consistent,
            "all_events_clear_three_db": clears_margin,
            "passes": corrected <= ALPHA and consistent and clears_margin}


def evaluate(manifest, state, hypothesis_job):
    jobs = {job["job_id"]: job for job in manifest["jobs"]}
    if hypothesis_job not in jobs:
        raise ValueError("hypothesis job is not in manifest")
    hypothesis_event = jobs[hypothesis_job]["event_id"]
    if hypothesis_job not in state["results"] or "score_vectors" not in state["results"][hypothesis_job]:
        raise ValueError("hypothesis job has not been scored")
    hypothesis = score(jobs[hypothesis_job], state["results"][hypothesis_job], 1)
    positive_events = defaultdict(list)
    for job in manifest["jobs"]:
        if job["role"] != "positive" or job["event_id"] == hypothesis_event:
            continue
        result = state["results"].get(job["job_id"])
        if result is None or "score_vectors" not in result:
            raise ValueError(f'positive job is incomplete: {job["job_id"]}')
        positive_events[job["event_id"]].append((job, result))
    ordered = sorted(positive_events.items(), key=lambda item:
                     min(row[0]["trajectory"][0]["timestamp_ms"] for row in item[1]))
    midpoint = (len(ordered) + 1) // 2
    qualification = cohort("chronological_first_half", ordered[:midpoint])
    validation = cohort("chronological_held_out_half", ordered[midpoint:])
    frozen = qualification["passes"] and validation["passes"]
    output = {
        "schema": "vectorwarp-inverted-spatial-tail-v1",
        "exploratory": True, "method_id": METHOD,
        "hypothesis_job": hypothesis_job,
        "hypothesis_event_excluded": hypothesis_event,
        "hypothesis_generation": hypothesis,
        "trial_accounting": {"variant_trials": VARIANT_TRIALS,
                             "alpha": ALPHA,
                             "minimum_notch_margin_db": MINIMUM_MARGIN_DB},
        "qualification": qualification, "validation": validation,
        "frozen_for_misses": frozen, "misses_scored": frozen,
    }
    if frozen:
        misses = []
        grouped = defaultdict(list)
        for job in manifest["jobs"]:
            if job["role"] == "miss":
                result = state["results"].get(job["job_id"])
                if result is None or "score_vectors" not in result:
                    raise ValueError(f'miss job is incomplete: {job["job_id"]}')
                grouped[job["event_id"]].append((job, result))
        for event_id, rows in sorted(grouped.items()):
            misses.append(cohort(event_id, [(event_id, rows)])["events"][0])
        output["misses"] = misses
    else:
        output["rejection_reason"] = (
            "The predeclared positive-control qualification and held-out validation "
            "did not both support a corrected target-induced low-tail notch.")
    return output


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--manifest", required=True, type=Path)
    parser.add_argument("--state", required=True, type=Path)
    parser.add_argument("--hypothesis-job", default="a68a06-r110")
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    result = evaluate(json.loads(args.manifest.read_text()),
                      json.loads(args.state.read_text()), args.hypothesis_job)
    args.output.write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps({key: result[key] for key in
                      ("frozen_for_misses", "misses_scored")}, indent=2))


if __name__ == "__main__":
    main()
