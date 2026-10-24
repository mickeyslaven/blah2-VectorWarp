import importlib.util
from pathlib import Path


SPEC = importlib.util.spec_from_file_location(
    "report", Path(__file__).parents[2] / "script" / "build-aircraft-pass-report.py")
REPORT = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(REPORT)


POLICY = {"required_truth": {"maximum_absolute_raw_observation_offset_s": 1,
    "maximum_dual_source_horizontal_disagreement_km": .5}}


def row(event="e", revision=1, outcome="no_radar_return", role="geometry_eligible_miss"):
    tracked = outcome == "tracked"
    return {"event_id": event, "pass_id": "p", "hex": "abc", "flight": "T",
        "outcome": outcome, "role": role, "config_epoch": "c", "receiver_epoch": "r",
        "radar_window_overlap": {"track": tracked, "detection": tracked},
        "capture": {"revision": revision, "state": "acknowledged", "file": f"r{revision}.mchq",
                    "samples": 10, "tracked_frames": int(tracked),
                    "detected_frames": int(tracked), "window_start": 0, "window_end": 1},
        "provenance_summary": {"truth_mode": "bracketed_event_time_interpolation",
            "nearest_sample_hold": False, "fallback": False,
            "maximum_absolute_raw_observation_offset_s": .2,
            "maximum_dual_source_horizontal_disagreement_km": .1},
        "trajectory": [{"ground_range_km": 7, "elevation_deg": 3, "altitude_geom_ft": 5000,
                        "bearing_deg_true": 90, "delay_km": 2, "doppler_hz": -150}]}


def cohort(rows):
    return {"policy": POLICY, "config_epoch": "c", "receiver_epoch": "r",
            "candidates": {REPORT.candidate_key(value): value for value in rows}}


def test_live_miss_stays_pending_until_complete_offline_matrix():
    value = REPORT.generate(cohort([row()]), "sha", cohort([row()]), {}, {})
    record = value["scopes"][0]["records"][0]
    assert record["classification"] is None
    assert record["classification_status"] == "pending_offline_matrix"
    assert value["scopes"][1]["aggregate"]["passes_total"] == 0


def test_tracked_and_validated_offline_are_distinct():
    miss = row("m", 1); miss["pass_id"] = "pm"
    track = row("t", 2, "tracked", "positive"); track["pass_id"] = "pt"
    source = cohort([miss, track])
    offline = {REPORT.candidate_key(miss): [{"classification": "validated_offline_recovery"}]}
    value = REPORT.generate(source, "sha", source, {}, offline)
    classes = {record["pass_id"]: record["classification"] for record in value["scopes"][0]["records"]}
    assert classes == {"pm": "validated_offline_recovery", "pt": "tracked"}


def test_stale_truth_is_excluded_and_not_in_eligible_denominator():
    bad = row(); bad["provenance_summary"]["maximum_absolute_raw_observation_offset_s"] = 2
    record = REPORT.build_pass("p", [bad], cohort([bad]), "frozen", {}, {})
    assert record["classification"] == "excluded_with_reason"
    aggregate = REPORT.counts([record])
    assert aggregate["passes_eligible"] == 0


def test_offline_join_requires_exact_frozen_sha(tmp_path):
    path = tmp_path / "offline.json"
    path.write_text('{"cohort_sha256":"wrong","results":[]}')
    accepted, rejected = REPORT.load_offline([path], "right")
    assert not accepted and rejected[0]["reason"] == "missing_or_wrong_frozen_cohort_sha256"


def test_quarantined_rows_and_offline_documents_never_count(tmp_path):
    bad = row(); bad["quarantined"] = True
    record = REPORT.build_pass("p", [bad], cohort([bad]), "frozen", {}, {})
    assert record["classification"] == "excluded_with_reason"
    path = tmp_path / "offline.json"
    path.write_text('{"cohort_sha256":"sha","quarantined":true,"results":[]}')
    accepted, rejected = REPORT.load_offline([path], "sha")
    assert not accepted and rejected[0]["reason"] == "quarantined_offline_input"
