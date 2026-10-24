import importlib.util
from pathlib import Path


PATH = Path(__file__).parents[2] / "script" / "score-operational-aircraft-passes.py"
SPEC = importlib.util.spec_from_file_location("score_operational_aircraft_passes", PATH)
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


def truth(timestamp=1000, delay=20, elevation=7):
    return {"timestamp_ms": timestamp, "delay_km": delay, "elevation_deg": elevation,
            "ground_range_km": 12, "doppler_hz": -120, "bearing_deg_true": 90,
            "altitude_geom_ft": 12000,
            "provenance": {"observation_time_offset_s": 0.4, "position_age_s": 0,
                           "projection_model": "constant_velocity_ground_track",
                           "maximum_pairwise_projected_disagreement_km": 0.1}}


def event(pass_id="p1", detected=3, tracked=3, state="finalized"):
    return {"pass_id": pass_id, "hex": "abc123", "flight": "TEST1", "state": state,
            "config_epoch": "epoch-a", "receiver_epoch": "rx-a", "observed_at": 1000,
            "finalized_at": 9000, "eligible_frames": 4, "detected_frames": detected,
            "tracked_frames": tracked, "first_detection_at": 2000 if detected else None,
            "first_track_at": 3000 if tracked else None,
            "truth_samples": [truth(1000 + i * 500) for i in range(4)]}


def test_pass_recall_and_detection_are_separate():
    records = [MODULE.normalize_event(event("tracked", 4, 3)),
               MODULE.normalize_event(event("detected", 3, 0)),
               MODULE.normalize_event(event("miss", 0, 0))]
    summary = MODULE.summarize(records)
    assert summary["passes_admitted"] == 3
    assert summary["detection_passes"] == 2
    assert summary["tracked_passes"] == 1
    assert summary["raw_detection_recall"]["estimate"] == 2 / 3
    assert summary["primary_tracking_recall"]["estimate"] == 1 / 3
    assert summary["detected_to_tracked_conversion"]["estimate"] == 1 / 2


def test_pending_and_stale_truth_are_excluded():
    pending = MODULE.normalize_event(event(state="pending"))
    stale_event = event("stale")
    stale_event["truth_samples"][0]["provenance"]["observation_time_offset_s"] = 1.01
    stale = MODULE.normalize_event(stale_event)
    assert pending["admitted"] is False
    assert "not_finalized" in pending["exclusion_reasons"]
    assert stale["admitted"] is False
    assert "raw_truth_offset_exceeds_1s" in stale["exclusion_reasons"]


def test_epochs_are_never_pooled():
    other = event("p2")
    other["config_epoch"] = "epoch-b"
    result = MODULE.score({"schema": "vectorwarp-evidence-statistics-v2",
                           "eligibilityEvents": [event(), other]}, "live", "input.json")
    assert [x["config_epoch"] for x in result["epochs"]] == ["epoch-a", "epoch-b"]


def test_fixed_geometry_strata():
    record = MODULE.normalize_event(event())
    assert record["strata"]["delay_region"] == "core_le_30km"
    assert record["strata"]["elevation_region"] == "mid_5_15deg"


def test_standardization_uses_same_baseline_geometry_mix():
    easy_base, hard_base = event("easy-base"), event("hard-base", 0, 0)
    for sample in hard_base["truth_samples"]:
        sample["delay_km"] = 45
        sample["elevation_deg"] = 20
    easy_candidate, hard_candidate = event("easy-candidate"), event("hard-candidate", 3, 3)
    for sample in hard_candidate["truth_samples"]:
        sample["delay_km"] = 45
        sample["elevation_deg"] = 20
    baseline = [MODULE.normalize_event(easy_base), MODULE.normalize_event(hard_base)]
    candidate = [MODULE.normalize_event(easy_candidate), MODULE.normalize_event(hard_candidate)]
    result = MODULE.standardized_comparison(baseline, candidate)
    assert result["baseline_coverage"] == 1
    assert result["baseline_standardized_tracking_recall"] == 0.5
    assert result["candidate_standardized_tracking_recall"] == 1
    assert result["standardized_difference_percentage_points"] == 50
