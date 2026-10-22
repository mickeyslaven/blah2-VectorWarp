import importlib.util
from pathlib import Path


SPEC = importlib.util.spec_from_file_location(
    "fresh", Path(__file__).parents[2] / "script" / "build-fresh-postfix-cohort.py")
FRESH = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(FRESH)


POLICY = {"minimum_capture_timestamp_ms": 1000, "required_truth": {
    "association_lag_cpis": 4, "association_lag_ms": 2000,
    "maximum_absolute_raw_observation_offset_s": 1.0,
    "maximum_dual_source_horizontal_disagreement_km": .5,
}}


def sample(timestamp, offset=.2):
    return {"timestamp_ms": timestamp, "position_timestamp_s": timestamp / 1000,
            "ground_speed_kt": 100, "delay_km": timestamp / 1000,
            "doppler_hz": 10, "provenance": {
                "source_id": "strix", "agreeing_sources": ["strix", "pi-zero"],
                "observation_time_offset_s": offset,
                "observation_position_timestamp_s": timestamp / 1000 - offset,
                "projection_model": "constant_velocity_ground_track",
                "maximum_pairwise_projected_disagreement_km": .01,
                "source_observations": [{"accepted": True, "event_time_s": timestamp / 1000},
                                        {"accepted": True, "event_time_s": timestamp / 1000}],
            }}


def event(outcome="tracked", tracked=1, first_track=1250):
    return {"id": "eligibility:e", "pass_id": "e", "hex": "abc123",
            "state": "finalized", "outcome": outcome, "observed_at": 1100,
            "truth_samples": [sample(900), sample(2100)],
            "capture_windows": [{"revision": 7, "state": "acknowledged", "file": "x.mchq",
                                 "window_start": 1000, "window_end": 2000,
                                 "tracked_frames": tracked, "detected_frames": 0,
                                 "first_track_at": first_track}]}


def test_positive_requires_in_window_radar_and_bracketed_truth():
    result = FRESH.build({"eligibility_events": [event()]}, POLICY, 1000, [7], {})
    assert result["summary"] == {"positives": 1, "geometry_eligible_misses": 0, "rejected": 0}
    point = result["admitted"][0]["trajectory"][0]
    assert point["truth_mode"] == "bracketed_event_time_interpolation"
    assert point["maximum_constant_velocity_projection_distance_km"] > 0


def test_rejects_match_outside_window_and_stale_truth():
    outside = event(first_track=2200)
    stale = event()
    stale["id"] = "eligibility:stale"
    stale["truth_samples"][0]["provenance"]["observation_time_offset_s"] = 1.01
    result = FRESH.build({"eligibility_events": [outside, stale]}, POLICY, 1000, [7], {})
    assert result["summary"]["rejected"] == 2
    assert any("do not define" in reason for reason in result["rejected"][0]["reasons"])
    assert any("offset exceeds" in reason for reason in result["rejected"][1]["reasons"])


def test_clean_no_return_is_miss_but_endpoint_hold_is_forbidden():
    miss = event(outcome="no_radar_return", tracked=0, first_track=None)
    accepted = FRESH.build({"eligibility_events": [miss]}, POLICY, 1000, [7], {})
    assert accepted["summary"]["geometry_eligible_misses"] == 1
    miss["truth_samples"] = [sample(1000), sample(1400)]
    rejected = FRESH.build({"eligibility_events": [miss]}, POLICY, 1000, [7], {})
    assert rejected["summary"]["rejected"] == 1
    assert "lacks bracketing" in rejected["rejected"][0]["reasons"][0]
