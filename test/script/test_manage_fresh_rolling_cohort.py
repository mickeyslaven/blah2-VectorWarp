import importlib.util
import json
from pathlib import Path


PATH = Path(__file__).parents[2] / "script" / "manage-fresh-rolling-cohort.py"
SPEC = importlib.util.spec_from_file_location("rolling", PATH)
ROLLING = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(ROLLING)


def item(event, revision, role, pass_id):
    return {"event_id": event, "pass_id": pass_id, "hex": event, "role": role,
            "capture": {"revision": revision, "window_start": revision * 1000,
                        "window_end": revision * 1000 + 1000,
                        "tracked_frames": 1 if role == "positive" else 0,
                        "detected_frames": 1 if role == "positive" else 0},
            "provenance_summary": {
                "maximum_absolute_raw_observation_offset_s": .2,
                "maximum_dual_source_horizontal_disagreement_km": .1}}


def test_summary_counts_clips_and_passes():
    rows = {"a|r1": item("a", 1, "positive", "p1"),
            "a|r2": item("a", 2, "positive", "p1"),
            "b|r3": item("b", 3, "geometry_eligible_miss", "p2")}
    assert ROLLING.summary(rows) == {"candidate_clips": 3, "positive_clips": 2,
        "miss_clips": 1, "positive_passes": 1, "miss_passes": 1,
        "unique_aircraft": 2}


def test_freeze_deduplicates_and_hashes_without_closing_ledger():
    ledger = {"status": "rolling_admission_open", "cutoff_ms": 1, "policy": {},
              "rf": {}, "config_epoch": "c", "receiver_epoch": "r",
              "candidates": {"a|r1": item("a", 1, "positive", "p1"),
                             "a|r2": item("a", 2, "positive", "p1"),
                             "b|r3": item("b", 3, "geometry_eligible_miss", "p2")}}
    frozen = ROLLING.freeze(ledger)
    assert frozen["status"] == "frozen_for_full_matrix_launch"
    assert frozen["summary"]["candidate_clips"] == 2
    assert frozen["summary"]["valid_windows_before_dedup"] == 3
    assert len(frozen["snapshot_sha256"]) == 64
    assert ledger["status"] == "rolling_admission_open"


def test_update_rejects_rf_epoch_mixing(monkeypatch):
    existing = {"status": "rolling_admission_open", "cutoff_ms": 1,
                "rf": {"frequency_hz": 563000000}, "config_epoch": "c",
                "receiver_epoch": "r", "candidates": {}, "created_at_ms": 1}
    try:
        ROLLING.update(existing, {}, {}, 1, {"frequency_hz": 564000000}, "c", "r")
    except ValueError as error:
        assert "RF metadata changed" in str(error)
    else:
        assert False
