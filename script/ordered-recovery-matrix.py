#!/usr/bin/env python3
"""Prepare and freeze the ordered, replay-only recovery experiment.

This is deliberately an orchestration boundary, not another detector.  A
backend receives each ``trial`` and may reuse the named cache artifacts; it
must return the raw target/control scores specified by ``RESULT_CONTRACT``.
The freeze operation accepts positive jobs only.  Miss jobs are represented
only by a hashed deferred input, so this program cannot accidentally release
or analyse one while calibration is in progress.
"""

import argparse
import copy
import hashlib
import itertools
import json
import math
import statistics
import subprocess
from pathlib import Path


ALPHA = .01
PHASE_STD_MAX_RAD = .35
AMPLITUDE_CV_MAX = .20

# Every choice is a predeclared member of the family.  Do not collapse an
# ordered pair into one trial: order is the experimental question.
ARMS = (
    ("A", ("spectral_rejection", "loaded_reciprocal", "detector", "tbd_hmm"),
     {"spectral_profile": ("robust_notch_v1", "adaptive_clip_v1"),
      "reciprocal_loading": (.01, .1, 1.0), "tbd_hmm": ("tbd6_hmm_v1", "tbd8_hmm_v1")},
     ("spectral_residual", "loaded_reciprocal_caf")),
    ("B", ("loaded_reciprocal", "spectral_rejection", "detector", "tbd_hmm"),
     {"reciprocal_loading": (.01, .1, 1.0), "spectral_profile": ("robust_notch_v1", "adaptive_clip_v1"),
      "tbd_hmm": ("tbd6_hmm_v1", "tbd8_hmm_v1")},
     ("loaded_reciprocal_caf", "spectral_residual")),
    ("C", ("hdr", "spectral_rejection", "noisy_reference_glrt", "tbd_hmm"),
     {"hdr_mode": ("clip_6db", "clip_12db"), "spectral_profile": ("robust_notch_v1", "adaptive_clip_v1"),
      "glrt_noise": ("known_noise", "unknown_noise"), "tbd_hmm": ("tbd6_hmm_v1", "tbd8_hmm_v1")},
     ("hdr_caf", "spectral_residual", "noisy_reference_glrt")),
    ("D", ("spectral_rejection", "hdr", "noisy_reference_glrt", "tbd_hmm"),
     {"spectral_profile": ("robust_notch_v1", "adaptive_clip_v1"), "hdr_mode": ("clip_6db", "clip_12db"),
      "glrt_noise": ("known_noise", "unknown_noise"), "tbd_hmm": ("tbd6_hmm_v1", "tbd8_hmm_v1")},
     ("spectral_residual", "hdr_caf", "noisy_reference_glrt")),
    ("E", ("inverse_hardware_gain", "spatial_whitening_glrt", "post_doppler_stap", "tbd_hmm"),
     {"whitening": ("diagonal", "shrinkage"), "stap_rank": (1, 2), "tbd_hmm": ("tbd6_hmm_v1", "tbd8_hmm_v1")},
     ("gain_equalized", "spatial_glrt", "post_doppler_stap")),
    ("F", ("inverse_hardware_gain", "trajectory_alignment", "target_protected_spatial_glrt", "hmm"),
     {"alignment": ("linear", "quadratic"), "protected_rank": (1, 2), "hmm": ("hmm_v1", "hmm_v2")},
     ("gain_equalized", "trajectory_aligned", "target_protected_spatial")),
    ("G", ("sparse_beam_channel_subset", "spatial_glrt_stap", "hmm"),
     {"subset": ("beam_top2", "beam_top4", "channels_0_1_2"), "spatial_method": ("glrt", "stap"),
      "hmm": ("hmm_v1", "hmm_v2")}, ("sparse_beam", "spatial_glrt_stap")),
    ("H", ("spatial_glrt_stap", "late_sparse_beam_evidence_fusion", "hmm"),
     {"spatial_method": ("glrt", "stap"), "fusion": ("max_calibrated", "conditional_residual"),
      "hmm": ("hmm_v1", "hmm_v2")}, ("spatial_glrt_stap", "sparse_beam")),
    ("I", ("provenance_time_anchor", "spectral_rejection", "loaded_reciprocal", "local_tbd_hmm"),
     {"spectral_profile": ("robust_notch_v1", "adaptive_clip_v1"),
      "reciprocal_loading": (.01, .1, 1.0), "local_window_cpis": (1, 3)},
     ("capture_anchor", "spectral_residual", "loaded_reciprocal_caf")),
)

RESULT_CONTRACT = {
    "required": ("raw_target_score", "control_scores", "rank"),
    "optional": ("direct_path_phasors", "coherent_before_spatial", "coherent_after_spatial"),
    "notes": "Retain raw scores and rank. validated_plus_3db is a post-freeze label, never a selector.",
}


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def trial_registry():
    trials = []
    for arm, order, parameters, caches in ARMS:
        names = tuple(parameters)
        for values in itertools.product(*(parameters[name] for name in names)):
            parameter = dict(zip(names, values))
            key = arm + ":" + ",".join(f"{name}={parameter[name]}" for name in names)
            # Fusion receives two CAF-derived scores.  It is explicitly a
            # single calibrated statistic, never a sum of correlated evidence.
            correlation = "same_caf_family" if arm == "H" else None
            trials.append({"trial_id": key, "arm": arm, "order": list(order),
                           "parameters": parameter, "cache_inputs": list(caches),
                           "evidence_policy": ("max_or_conditional_residual_calibrated_once"
                                               if arm == "H" else "single_pipeline_score"),
                           "correlation_group": correlation})
    return trials


def build(positive_manifest, spatial_manifest, deferred_miss_manifest):
    positives = positive_manifest["rows"]
    if len(positives) < 3 or any(row["role"] != "positive" for row in positives):
        raise ValueError("ordered calibration requires an immutable all-positive cohort of at least three rows")
    # One aircraft pass can legitimately contribute more than one clip.  Event
    # ID alone is therefore not a unique join key; include the recorder
    # revision so neither clip silently aliases the other.
    by_capture = {(job["event_id"], job["revision"]): job
                  for job in spatial_manifest["jobs"] if job["role"] == "positive"}
    jobs = []
    for row in positives:
        revision = row.get("capture", {}).get("revision")
        spatial = by_capture.get((row["event_id"], revision))
        if not spatial:
            raise ValueError(f"no positive spatial job for {row['event_id']} revision {revision}")
        jobs.append({"job_id": spatial["job_id"], "event_id": row["event_id"], "revision": spatial["revision"],
                     "recording": spatial["recording"], "trajectory": spatial["trajectory"],
                     "role": "positive", "cache_namespace": "ordered-recovery-v1/" + spatial["job_id"],
                     "timing": timing_metadata(row)})
    trials = trial_registry()
    return {"schema": "vectorwarp-ordered-recovery-matrix-v1", "status": "calibration_pending",
            "replay_only": True, "live_rf_or_config_mutation": False,
            "calibration": {"roles_allowed": ["positive"], "positive_job_count": len(jobs),
                            "selection_rule": "leave-one-pass-out; select on remaining passes, score held-out pass",
                            "validation_label": "+3dB validated", "alpha": ALPHA},
            "deferred_misses": {"status": "blocked_until_frozen", "manifest_sha256": sha(deferred_miss_manifest)},
            "jobs": jobs, "trials": trials, "familywise_trial_count": len(trials),
            "control_quantile": 1 - ALPHA / len(trials),
            "direct_path_phasor_gate": {"phase_circular_std_max_rad": PHASE_STD_MAX_RAD,
                "amplitude_cv_max": AMPLITUDE_CV_MAX,
                "on_unavailable": "coherent_comparison_unavailable",
                "comparison": "coherent trajectory accumulation before vs after spatial suppression"},
            "result_contract": RESULT_CONTRACT}


def timing_metadata(row):
    capture = row.get("capture", {})
    start, end = capture.get("window_start"), capture.get("window_end")
    anchor = capture.get("first_detection_at") or capture.get("first_track_at")
    if start is None or end is None:
        return {"status": "unavailable", "reason": "capture window absent"}
    if row.get("role") == "positive":
        if anchor is None:
            return {"status": "unavailable", "reason": "positive capture lacks first_detection_at/first_track_at"}
        kind = "first_detection_at" if capture.get("first_detection_at") is not None else "first_track_at"
    else:
        anchor, kind = capture.get("requested_event_time"), "requested_event_time"
        if anchor is None:
            return {"status": "unavailable", "reason": "miss capture lacks requested_event_time"}
    if not start <= anchor <= end:
        return {"status": "unavailable", "reason": "anchor outside capture window"}
    return {"status": "available", "anchor_kind": kind, "anchor_ms": anchor,
            "window_start_ms": start, "window_end_ms": end, "cpi_ms": 500,
            "policy": "fixed center CPI plus radius 0 or 1; never score-selected"}


def initial_state(manifest):
    """The durable handoff state deliberately has no runnable miss queue."""
    return {"schema": "vectorwarp-ordered-recovery-matrix-state-v1",
            "matrix_schema": manifest["schema"], "matrix_status": manifest["status"],
            "calibration_job_ids": [job["job_id"] for job in manifest["jobs"]],
            "deferred_misses": {"status": "blocked_until_frozen",
                                "manifest_sha256": manifest["deferred_misses"]["manifest_sha256"]},
            "cache": {"policy": "replay-only named intermediates; cache keys are job namespace plus cache input"},
            "results": {}}


def merge_states(manifest, states):
    """Combine disjoint clip-worker states without weakening completeness checks."""
    allowed = {job["job_id"] for job in manifest["jobs"]}
    expected = {trial["trial_id"] for trial in manifest["trials"]}
    merged = initial_state(manifest)
    merged["runtime_seconds"] = {}
    for state in states:
        for job_id, result in state.get("results", {}).items():
            if job_id not in allowed:
                raise ValueError(f"worker state contains unknown job {job_id}")
            existing = merged["results"].get(job_id)
            if existing is not None and existing != result:
                raise ValueError(f"conflicting worker results for {job_id}")
            merged["results"][job_id] = result
        for job_id, seconds in state.get("runtime_seconds", {}).items():
            if job_id in allowed:
                merged["runtime_seconds"][job_id] = seconds
    complete = set(merged["results"]) == allowed and all(
        set(result.get("trials", {})) == expected for result in merged["results"].values())
    merged["status"] = "complete_positive_only" if complete else "running_positive_only"
    return merged


def subset(manifest, included_arms):
    """Derive a separately corrected family subset without changing its source cohort."""
    included = tuple(dict.fromkeys(included_arms))
    known = {trial["arm"] for trial in manifest["trials"]}
    if not included or any(arm not in known for arm in included):
        raise ValueError("subset arms must be non-empty members of the source matrix")
    result = copy.deepcopy(manifest)
    result["trials"] = [trial for trial in result["trials"] if trial["arm"] in included]
    result["familywise_trial_count"] = len(result["trials"])
    result["control_quantile"] = 1 - ALPHA / len(result["trials"])
    result["method_family_subset"] = {
        "included_arms": list(included),
        "excluded_arms": sorted(known - set(included)),
        "reason": "manifold-independent detection calibration while installed-array manifold qualification is pending",
        "spatial_or_bearing_results_deferred": True,
    }
    return result


def execute(manifest, state, backend, cache_root):
    """Execute the bounded positive-only matrix through a replay backend.

    ``backend`` is an argv prefix.  For each trial it is called as
    ``backend --request request.json --output result.json``.  The request is
    intentionally self-contained and tells the backend which intermediates it
    may populate/reuse under ``cache_dir``.  This keeps raw IQ on the compute
    host and lets the current spatial runner be the backend implementation.
    """
    if manifest.get("status") != "calibration_pending":
        raise ValueError("execute only accepts an unfrozen matrix")
    if any(job["role"] != "positive" for job in manifest["jobs"]):
        raise ValueError("matrix execution accepts positives only")
    cache_root = Path(cache_root); cache_root.mkdir(parents=True, exist_ok=True)
    expected = {trial["trial_id"] for trial in manifest["trials"]}
    for job in manifest["jobs"]:
        result = state.setdefault("results", {}).setdefault(job["job_id"], {"role": "positive", "trials": {}})
        if result.get("role") != "positive":
            raise ValueError("state contains non-positive result")
        for trial in manifest["trials"]:
            if trial["trial_id"] in result["trials"]:
                continue
            namespace = cache_root / job["cache_namespace"]
            namespace.mkdir(parents=True, exist_ok=True)
            request = {"schema": "vectorwarp-ordered-recovery-backend-request-v1", "job": job,
                       "trial": trial, "cache_dir": str(namespace),
                       "required_result": list(RESULT_CONTRACT["required"]),
                       "control_quantile": manifest["control_quantile"]}
            request_path = namespace / (hashlib.sha256(trial["trial_id"].encode()).hexdigest() + ".request.json")
            output_path = request_path.with_suffix(".result.json")
            request_path.write_text(json.dumps(request, separators=(",", ":")) + "\n")
            subprocess.run([*backend, "--request", str(request_path), "--output", str(output_path)], check=True)
            score = json.loads(output_path.read_text())
            if any(key not in score for key in RESULT_CONTRACT["required"]):
                raise ValueError(f"backend returned incomplete score for {job['job_id']}/{trial['trial_id']}")
            result["trials"][trial["trial_id"]] = score
            # These measurements are recording-level diagnostics.  Preserve
            # them outside the per-trial score so freeze can gate an actual
            # before/after comparison rather than infer it from labels.
            for key in ("direct_path_phasors", "coherent_before_spatial",
                        "coherent_after_spatial"):
                if key in score:
                    if key in result and result[key] != score[key]:
                        raise ValueError(f"backend changed {key} within {job['job_id']}")
                    result[key] = score[key]
    if set(state["results"]) != {job["job_id"] for job in manifest["jobs"]} or any(
            set(value["trials"]) != expected for value in state["results"].values()):
        raise ValueError("backend did not complete the bounded matrix")
    return state


def circular_std(phases):
    resultant = abs(sum(complex(math.cos(x), math.sin(x)) for x in phases)) / len(phases)
    return math.sqrt(max(0., -2 * math.log(max(resultant, 1e-300))))


def phasor_gate(phasors):
    if not phasors or len(phasors) < 2:
        return {"status": "unavailable", "reason": "fewer than two measured direct-path phasors"}
    values = [complex(item[0], item[1]) if isinstance(item, list) else complex(item) for item in phasors]
    amplitudes, phases = [abs(x) for x in values], [math.atan2(x.imag, x.real) for x in values]
    mean = statistics.fmean(amplitudes)
    phase_std, amplitude_cv = circular_std(phases), statistics.pstdev(amplitudes) / mean if mean else math.inf
    passed = phase_std <= PHASE_STD_MAX_RAD and amplitude_cv <= AMPLITUDE_CV_MAX
    return {"status": "passed" if passed else "failed", "phase_circular_std_rad": phase_std,
            "amplitude_cv": amplitude_cv, "sample_count": len(values)}


def corrected_margin_db(score, threshold):
    """Express either power scores or natural-log likelihood scores in dB."""
    domain = score.get("score_domain")
    if domain is None and score.get("provenance", {}).get("arm") in tuple("ABCDI"):
        domain = "natural_log_likelihood"
    if domain == "natural_log_likelihood":
        return (10 / math.log(10)) * (float(score["raw_target_score"]) - float(threshold))
    return 10 * math.log10(max(float(score["raw_target_score"]), 1e-300) /
                           max(float(threshold), 1e-300))


def freeze(manifest, state):
    if manifest.get("status") != "calibration_pending":
        raise ValueError("only an unfrozen matrix can be frozen")
    results = state.get("results", {})
    allowed = {job["job_id"] for job in manifest["jobs"]}
    if set(results) != allowed:
        raise ValueError("freeze requires results for every immutable positive job")
    expected = {trial["trial_id"] for trial in manifest["trials"]}
    aggregate = {trial: [] for trial in expected}
    direct = []
    labels = {}
    for job_id, output in results.items():
        if output.get("role", "positive") != "positive":
            raise ValueError("non-positive result supplied to calibration")
        if set(output.get("trials", {})) != expected:
            raise ValueError(f"{job_id} does not contain every predeclared trial")
        labels[job_id] = {}
        for trial_id, score in output["trials"].items():
            if any(key not in score for key in RESULT_CONTRACT["required"]):
                raise ValueError(f"{job_id}/{trial_id} lacks raw score, controls, or rank")
            controls = score["control_scores"]
            if not controls:
                raise ValueError(f"{job_id}/{trial_id} has no controls")
            threshold = sorted(controls)[min(len(controls) - 1, int(manifest["control_quantile"] * len(controls)))]
            margin = corrected_margin_db(score, threshold)
            aggregate[trial_id].append((margin, float(score["rank"])))
            labels[job_id][trial_id] = {"raw_target_score": score["raw_target_score"], "rank": score["rank"],
                                        "margin_db": margin, "validated_plus_3db": margin >= 3}
        direct.extend(output.get("direct_path_phasors", []))
    # Score selection and validation by aircraft pass, not clip.  This avoids
    # overweighting the one pass that contributed two recordings and gives a
    # genuinely held-out estimate despite the small positive cohort.
    jobs_by_id = {job["job_id"]: job for job in manifest["jobs"]}
    pass_jobs = {}
    for job_id in results:
        pass_jobs.setdefault(jobs_by_id[job_id]["event_id"], []).append(job_id)

    def pass_macro(trial_id, selected_passes):
        margins, ranks = [], []
        for pass_id in selected_passes:
            clip_labels = [labels[job_id][trial_id] for job_id in pass_jobs[pass_id]]
            margins.append(statistics.fmean(row["margin_db"] for row in clip_labels))
            ranks.append(statistics.median(row["rank"] for row in clip_labels))
        return statistics.fmean(margins), statistics.median(ranks)

    pass_ids = sorted(pass_jobs)
    ranked = sorted((*pass_macro(trial, pass_ids), trial) for trial in expected)
    ranked.sort(key=lambda x: (-x[0], x[1], x[2]))
    held_out = []
    for held_out_pass in pass_ids:
        training = [pass_id for pass_id in pass_ids if pass_id != held_out_pass]
        fold = sorted((*pass_macro(trial, training), trial) for trial in expected)
        fold.sort(key=lambda x: (-x[0], x[1], x[2]))
        selected = fold[0][2]
        margin, rank = pass_macro(selected, [held_out_pass])
        held_out.append({"pass_id": held_out_pass, "clip_count": len(pass_jobs[held_out_pass]),
                         "selected_trial": selected, "held_out_margin_db": margin,
                         "held_out_median_rank": rank, "validated_plus_3db": margin >= 3})
    held_out_margins = [fold["held_out_margin_db"] for fold in held_out]
    positive_passes = sum(value > 0 for value in held_out_margins)
    useful = statistics.median(held_out_margins) > 0 and positive_passes >= math.ceil(.6 * len(held_out))
    gate = phasor_gate(direct)
    comparison = "unavailable"
    if gate["status"] == "passed":
        pairs = []
        for job_id, output in results.items():
            if "coherent_before_spatial" not in output or "coherent_after_spatial" not in output:
                comparison = {"status": "unavailable",
                              "reason": "spatial comparison was intentionally not evaluated for this method-family subset"}
                break
            pairs.append({"job_id": job_id, "before": output["coherent_before_spatial"],
                          "after": output["coherent_after_spatial"],
                          "after_minus_before": output["coherent_after_spatial"] - output["coherent_before_spatial"]})
        else:
            comparison = {"status": "measured", "per_job": pairs}
    return {**manifest, "status": "frozen", "selected_trial": ranked[0][2],
            "selection": {"pass_macro_mean_positive_margin_db": ranked[0][0],
                          "pass_macro_median_positive_rank": ranked[0][1],
                          "positive_jobs": sorted(allowed), "positive_pass_count": len(pass_ids)},
            "leave_one_pass_out": {"folds": held_out,
                "median_held_out_margin_db": statistics.median(held_out_margins),
                "positive_margin_passes": positive_passes, "pass_count": len(held_out),
                "useful_detection_discrimination": useful,
                "release_rule": "median held-out margin > 0 dB and >=60% of held-out passes > 0 dB",
                "plus_3db_passes": sum(value >= 3 for value in held_out_margins)},
            "direct_path_phasor_gate": gate,
            "coherent_trajectory_comparison": comparison,
            "raw_score_rank_and_validation_labels": labels}


def main():
    parser = argparse.ArgumentParser()
    sub = parser.add_subparsers(dest="command", required=True)
    prepare = sub.add_parser("prepare")
    prepare.add_argument("--positive-manifest", required=True, type=Path); prepare.add_argument("--spatial-manifest", required=True, type=Path)
    prepare.add_argument("--miss-manifest", required=True, type=Path); prepare.add_argument("--output", required=True, type=Path)
    prepare.add_argument("--state-output", type=Path)
    seal = sub.add_parser("freeze")
    seal.add_argument("--manifest", required=True, type=Path); seal.add_argument("--state", required=True, type=Path); seal.add_argument("--output", required=True, type=Path)
    execute_parser = sub.add_parser("execute")
    execute_parser.add_argument("--manifest", required=True, type=Path); execute_parser.add_argument("--state", required=True, type=Path)
    execute_parser.add_argument("--output", required=True, type=Path)
    execute_parser.add_argument("--cache-root", required=True, type=Path)
    execute_parser.add_argument("--backend", required=True, nargs="+")
    subset_parser = sub.add_parser("subset")
    subset_parser.add_argument("--manifest", required=True, type=Path)
    subset_parser.add_argument("--include-arm", required=True, action="append")
    subset_parser.add_argument("--output", required=True, type=Path)
    subset_parser.add_argument("--state-output", type=Path)
    merge_parser = sub.add_parser("merge")
    merge_parser.add_argument("--manifest", required=True, type=Path)
    merge_parser.add_argument("--state", required=True, action="append", type=Path)
    merge_parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    if args.command == "prepare":
        value = build(json.loads(args.positive_manifest.read_text()), json.loads(args.spatial_manifest.read_text()), args.miss_manifest)
        if args.state_output:
            args.state_output.write_text(json.dumps(initial_state(value), indent=2) + "\n")
    elif args.command == "freeze":
        value = freeze(json.loads(args.manifest.read_text()), json.loads(args.state.read_text()))
    elif args.command == "execute":
        value = execute(json.loads(args.manifest.read_text()), json.loads(args.state.read_text()), args.backend, args.cache_root)
    elif args.command == "subset":
        value = subset(json.loads(args.manifest.read_text()), args.include_arm)
        if args.state_output:
            args.state_output.write_text(json.dumps(initial_state(value), indent=2) + "\n")
    else:
        value = merge_states(json.loads(args.manifest.read_text()),
                             [json.loads(path.read_text()) for path in args.state])
    args.output.write_text(json.dumps(value, indent=2) + "\n")
    print(json.dumps({"status": value.get("status", value.get("matrix_status")),
                      "familywise_trial_count": value.get("familywise_trial_count", 0)}, sort_keys=True))


if __name__ == "__main__":
    main()
