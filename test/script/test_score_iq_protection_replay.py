import importlib.util
import json
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location(
    "score_iq_protection_replay", ROOT / "script/score-iq-protection-replay.py")
SCORE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(SCORE)


def test_track_delay_bins_are_converted_and_missing_measurement_is_unavailable(tmp_path):
    timestamp = 1234
    replay = {
        "arm": {"kind": "baseline", "beams": 0, "index": None},
        "frames": {
            "detection": [{"delay": [], "doppler": [], "snr": []}],
            "track": [{"data": [{"delay": 6.16, "doppler": -90.0,
                                    "state": "ACTIVE"}]}],
        },
    }
    path = tmp_path / "replay.json"
    path.write_text(json.dumps(replay))
    row = SCORE.summarize(path, {timestamp: (0.76947, -90.0)},
                          [{"timestamp_ms": timestamp}], 2_400_000,
                          0.01, 1.0)
    assert abs(row["track_delay_bin_km"] * 6.16 - 0.76947) < 1e-4
    assert row["truth_track_frames"] == 1
    assert row["truth_active_track_frames"] == 1
    assert row["truth_measured_track_frames"] is None


def test_only_admitted_endpoint_truth_enters_denominator(tmp_path):
    replay = {
        "arm": {"kind": "baseline", "beams": 0, "index": None},
        "frames": {
            "detection": [
                {"delay": [1.0], "doppler": [2.0], "snr": [8.0]},
                {"delay": [1.0], "doppler": [2.0], "snr": [9.0]},
            ],
            "track": [{"data": []}, {"data": []}],
        },
    }
    path = tmp_path / "replay.json"
    path.write_text(json.dumps(replay))
    row = SCORE.summarize(path, {2000: (1.0, 2.0)},
                          [{"timestamp_ms": 1000}, {"timestamp_ms": 2000}],
                          2_400_000, 0.01, 0.01)
    assert row["scorable_frames"] == 1
    assert row["truth_detection_frames"] == 1
    assert row["truth_detection_samples"] == 1
    assert row["truth_snr_mean_db"] == 9.0
