import importlib.util
from pathlib import Path


PATH = Path(__file__).parents[2] / "script" / "summarize-current-epoch-spatial-suite.py"
SPEC = importlib.util.spec_from_file_location("summary", PATH)
SUMMARY = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(SUMMARY)


def result(job_id, event_id, role, margins):
    return {
        "job_id": job_id, "event_id": event_id, "hex": event_id,
        "flight": "TST", "role": role,
        "outcome": "tracked" if role == "positive" else "no_radar_return",
        "revision": 1, "family_trials": 6, "control_quantile": .998,
        "methods": {method: {"margin_over_control_p99_db": margin,
                             "evidence_log10": max(0.0, margin / 10),
                             "control_evidence_log10": [0.0, .1, .2, .3]}
                    for method, margin in zip(SUMMARY.METHODS, margins)},
    }


def test_summary_uses_best_clip_and_three_db_threshold():
    manifest = {"config_epoch": "epoch", "statistics_sha256": "hash", "jobs": [
        {"job_id": "m1", "event_id": "miss"},
        {"job_id": "m2", "event_id": "miss"},
        {"job_id": "p1", "event_id": "positive"},
    ]}
    state = {"recordings": {"a": {}}, "results": {
        "m1": result("m1", "miss", "miss", (2.9, -1, 0)),
        "m2": result("m2", "miss", "miss", (3.0, -2, 1)),
        "p1": result("p1", "positive", "positive", (4, 2, -1)),
    }}
    output = SUMMARY.summarize(manifest, state)
    assert output["scope"]["miss_events"] == 1
    assert output["summary"]["misses_recovered_any"] == 1
    assert output["summary"]["positive_events_retained_any"] == 1
    miss = next(row for row in output["events"] if row["role"] == "miss")
    assert miss["methods"]["spatial_glrt"] == {
        "best_margin_db": 3.0, "best_job_id": "m2", "recovered": True}
    assert len(miss["stacks"]) == 4


def test_summary_rejects_incomplete_state():
    manifest = {"config_epoch": "epoch", "statistics_sha256": "hash",
                "jobs": [{"job_id": "missing", "event_id": "miss"}]}
    try:
        SUMMARY.summarize(manifest, {"recordings": {}, "results": {}})
    except ValueError as error:
        assert "1 jobs missing" in str(error)
    else:
        raise AssertionError("incomplete state accepted")
