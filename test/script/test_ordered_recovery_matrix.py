import importlib.util
import json
from pathlib import Path


PATH = Path(__file__).parents[2] / "script" / "ordered-recovery-matrix.py"
SPEC = importlib.util.spec_from_file_location("ordered", PATH)
ORDERED = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(ORDERED)


def manifests(tmp_path):
    rows = [{"event_id": f"e{i}", "role": "positive", "capture": {"revision": 118 + i}}
            for i in range(3)]
    spatial = {"jobs": [{"event_id": f"e{i}", "job_id": f"p{i}", "revision": 118 + i,
                           "recording": f"r{i}", "trajectory": [], "role": "positive"} for i in range(3)]}
    miss = tmp_path / "miss.json"; miss.write_text("{}")
    return ORDERED.build({"rows": rows}, spatial, miss)


def test_build_keeps_two_clips_from_the_same_pass_distinct(tmp_path):
    rows = [{"event_id": "same-pass", "role": "positive", "capture": {"revision": revision}}
            for revision in (1610, 1612)]
    rows.append({"event_id": "other-pass", "role": "positive", "capture": {"revision": 118}})
    spatial = {"jobs": [{"event_id": row["event_id"], "job_id": f"p{row['capture']['revision']}",
                           "revision": row["capture"]["revision"], "recording": "r", "trajectory": [],
                           "role": "positive"} for row in rows]}
    miss = tmp_path / "miss.json"; miss.write_text("{}")
    manifest = ORDERED.build({"rows": rows}, spatial, miss)
    assert [job["job_id"] for job in manifest["jobs"]] == ["p1610", "p1612", "p118"]


def full_state(manifest, phasors=None):
    return {"results": {job["job_id"]: {"role": "positive", "direct_path_phasors": phasors or [],
        "coherent_before_spatial": 1.0, "coherent_after_spatial": 1.1,
        "trials": {trial["trial_id"]: {"raw_target_score": 2.0 if trial["arm"] == "A" else 1.0,
          "control_scores": [1.0] * 20, "rank": 1} for trial in manifest["trials"]}}
        for job in manifest["jobs"]}}


def test_all_orders_parameters_and_fusion_policy_are_counted(tmp_path):
    manifest = manifests(tmp_path)
    assert manifest["familywise_trial_count"] == len(manifest["trials"]) == 104
    assert {trial["arm"] for trial in manifest["trials"]} == set("ABCDEFGHI")
    assert manifest["control_quantile"] == 1 - .01 / 104
    h = [x for x in manifest["trials"] if x["arm"] == "H"]
    assert h and all(x["evidence_policy"] == "max_or_conditional_residual_calibrated_once" for x in h)
    state = ORDERED.initial_state(manifest)
    assert state["deferred_misses"]["status"] == "blocked_until_frozen"
    assert state["results"] == {}


def test_freeze_is_positive_only_and_preserves_raw_labels(tmp_path):
    manifest = manifests(tmp_path)
    frozen = ORDERED.freeze(manifest, full_state(manifest, [[1, 0], [1, .01], [.99, -.01]]))
    assert frozen["status"] == "frozen"
    assert frozen["selected_trial"].startswith("A:")
    assert frozen["leave_one_pass_out"]["useful_detection_discrimination"] is True
    assert frozen["leave_one_pass_out"]["positive_margin_passes"] == 3
    assert frozen["direct_path_phasor_gate"]["status"] == "passed"
    assert frozen["coherent_trajectory_comparison"]["status"] == "measured"
    label = next(iter(next(iter(frozen["raw_score_rank_and_validation_labels"].values())).values()))
    assert {"raw_target_score", "rank", "margin_db", "validated_plus_3db"} <= set(label)


def test_freeze_rejects_miss_or_incomplete_trial_and_unavailable_gate(tmp_path):
    manifest = manifests(tmp_path); state = full_state(manifest)
    state["results"]["p0"]["role"] = "miss"
    try:
        ORDERED.freeze(manifest, state)
    except ValueError as error:
        assert "non-positive" in str(error)
    else: assert False
    assert ORDERED.phasor_gate([])["status"] == "unavailable"


def test_execute_calls_backend_once_per_positive_trial_and_caches(tmp_path):
    manifest = manifests(tmp_path); state = ORDERED.initial_state(manifest)
    backend = tmp_path / "backend.py"
    backend.write_text("""import argparse, json\np=argparse.ArgumentParser(); p.add_argument('--request'); p.add_argument('--output'); a=p.parse_args()\njson.dump({'raw_target_score': 1, 'control_scores': [1], 'rank': 1, 'direct_path_phasors': [[1,0],[1,.01]], 'coherent_before_spatial': 1, 'coherent_after_spatial': 1}, open(a.output, 'w'))\n""")
    result = ORDERED.execute(manifest, state, ["python3", str(backend)], tmp_path / "cache")
    assert len(result["results"]) == 3
    assert all(len(row["trials"]) == 104 for row in result["results"].values())
    assert all(row["direct_path_phasors"] for row in result["results"].values())


def test_time_anchor_is_provenance_fixed_and_never_score_selected():
    row = {"role": "positive", "capture": {"window_start": 1000, "window_end": 11000,
           "first_detection_at": 3500, "first_track_at": 4000}}
    timing = ORDERED.timing_metadata(row)
    assert timing["status"] == "available"
    assert timing["anchor_kind"] == "first_detection_at"
    assert timing["anchor_ms"] == 3500
    assert "score-selected" in timing["policy"]
    missing = ORDERED.timing_metadata({"role": "positive", "capture": {"window_start": 0, "window_end": 1}})
    assert missing["status"] == "unavailable"


def test_subset_keeps_jobs_and_recorrects_selected_method_families(tmp_path):
    manifest = manifests(tmp_path)
    selected = ORDERED.subset(manifest, ["A", "B", "C", "D", "I"])
    assert selected["jobs"] == manifest["jobs"]
    assert {trial["arm"] for trial in selected["trials"]} == set("ABCDI")
    assert selected["familywise_trial_count"] == len(selected["trials"]) == 68
    assert selected["control_quantile"] == 1 - .01 / 68
    assert selected["method_family_subset"]["excluded_arms"] == list("EFGH")


def test_subset_freeze_does_not_require_spatial_comparison(tmp_path):
    manifest = ORDERED.subset(manifests(tmp_path), ["A"])
    state = full_state(manifest, [[1, 0], [1, .01], [.99, -.01]])
    for output in state["results"].values():
        output.pop("coherent_before_spatial")
        output.pop("coherent_after_spatial")
    frozen = ORDERED.freeze(manifest, state)
    assert frozen["direct_path_phasor_gate"]["status"] == "passed"
    assert frozen["coherent_trajectory_comparison"]["status"] == "unavailable"


def test_merge_states_requires_every_unique_job(tmp_path):
    manifest = ORDERED.subset(manifests(tmp_path), ["A"])
    complete = full_state(manifest)
    parts = []
    for job_id, result in complete["results"].items():
        parts.append({"results": {job_id: result}, "runtime_seconds": {job_id: 1.0}})
    merged = ORDERED.merge_states(manifest, parts)
    assert merged["status"] == "complete_positive_only"
    assert set(merged["results"]) == {job["job_id"] for job in manifest["jobs"]}


def test_negative_log_likelihood_margin_is_not_ratio_clamped():
    score = {"raw_target_score": -2.0, "score_domain": "natural_log_likelihood"}
    assert ORDERED.corrected_margin_db(score, -3.0) > 4.3
