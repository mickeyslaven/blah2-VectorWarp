import importlib.util
from pathlib import Path


PATH = Path(__file__).parents[2] / "script" / "summarize-current-epoch-spatial-suite.py"
SPEC = importlib.util.spec_from_file_location("summary", PATH)
SUMMARY = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(SUMMARY)


def result(job_id, event_id, role, margins):
    base = {
        "job_id": job_id, "event_id": event_id, "hex": event_id,
        "flight": "TST", "role": role,
        "outcome": "tracked" if role == "positive" else "no_radar_return",
        "revision": 1, "family_trials": 6, "control_quantile": .998,
    }
    package = {"methods": {method: {"margin_over_control_p99_db": margin,
                             "evidence_log10": max(0.0, margin / 10),
                             "control_evidence_log10": [0.0, .1, .2, .3]}
                    for method, margin in zip(SUMMARY.METHODS, margins)},
    }
    base["gain_treatments"] = {
        "raw_as_acquired": package,
        "inverse_hardware_gain": {"methods": {
            method: dict(value) for method, value in package["methods"].items()}},
    }
    return base


def test_summary_uses_best_clip_and_three_db_threshold():
    manifest = {"config_epoch": "epoch", "statistics_sha256": "hash", "jobs": [
        {"job_id": "m1", "event_id": "miss", "role": "miss"},
        {"job_id": "m2", "event_id": "miss", "role": "miss"},
        {"job_id": "p1", "event_id": "positive", "role": "positive"},
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
    assert output["gain_selection"]["selected"] == "raw_as_acquired"


def test_gain_selection_uses_positive_only_and_can_choose_equalized():
    manifest = {"config_epoch": "epoch", "statistics_sha256": "hash", "jobs": [
        {"job_id": "p1", "event_id": "positive", "role": "positive"},
        {"job_id": "m1", "event_id": "miss", "role": "miss"},
    ]}
    positive = result("p1", "positive", "positive", (0, 0, 0))
    positive["gain_treatments"]["inverse_hardware_gain"] = result(
        "x", "x", "positive", (4, 0, 0))["gain_treatments"]["raw_as_acquired"]
    miss = result("m1", "miss", "miss", (100, 100, 100))
    selection = SUMMARY.select_gain_treatment(
        manifest, {"results": {"p1": positive, "m1": miss}})
    assert selection["selected"] == "inverse_hardware_gain"


def test_gain_diagnostics_report_margin_beam_and_noise_effects():
    manifest = {"jobs": [
        {"job_id": "p1", "event_id": "positive", "role": "positive"},
        {"job_id": "m1", "event_id": "miss", "role": "miss"},
    ]}
    positive = result("p1", "positive", "positive", (1, 2, 3))
    miss = result("m1", "miss", "miss", (-1, -2, -3))
    for clip, delta, beams in ((positive, 1.5, (2, 4)), (miss, -.5, (3, 3))):
        for method in SUMMARY.METHODS:
            clip["gain_treatments"]["inverse_hardware_gain"]["methods"][method][
                "margin_over_control_p99_db"] += delta
        clip["gain_treatments"]["raw_as_acquired"]["methods"]["spatial_glrt"][
            "target_fixed_beam"] = beams[0]
        clip["gain_treatments"]["inverse_hardware_gain"]["methods"]["spatial_glrt"][
            "target_fixed_beam"] = beams[1]
        clip["gain_accounting"] = {
            "raw_channel_rms": [1, 2], "equalized_channel_rms": [2, 1],
            "noise_power_change_db": [6.020599913, -6.020599913],
        }
    diagnostics = SUMMARY.gain_diagnostics(
        manifest, {"results": {"p1": positive, "m1": miss}})
    assert diagnostics["equalized_minus_raw_margin_db"]["positive"][
        "post_doppler_stap"]["median"] == 1.5
    assert diagnostics["spatial_glrt_fixed_beam"]["positive"]["changed"] == 1
    assert diagnostics["spatial_glrt_fixed_beam"]["miss"]["changed"] == 0
    assert diagnostics["channel_rms_change_db"]["median_observed"] == [
        6.020599913279624, -6.020599913279624]


def test_summary_rejects_incomplete_state():
    manifest = {"config_epoch": "epoch", "statistics_sha256": "hash",
                "jobs": [{"job_id": "missing", "event_id": "miss"}]}
    try:
        SUMMARY.summarize(manifest, {"recordings": {}, "results": {}})
    except ValueError as error:
        assert "1 jobs missing" in str(error)
    else:
        raise AssertionError("incomplete state accepted")
