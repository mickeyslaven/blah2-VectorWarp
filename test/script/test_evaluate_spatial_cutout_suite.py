import importlib.util
import math
from pathlib import Path


PATH = Path(__file__).parents[2] / "script" / "evaluate-spatial-cutout-suite.py"
SPEC = importlib.util.spec_from_file_location("suite", PATH)
SUITE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(SUITE)


def test_calibrated_vector_preserves_aligned_offsets_and_target():
    result = {"target_score": 4.0, "control_score_vector": [
        {"doppler_offset_bins": -1, "delay_offset_bins": 2, "raw_score": 1.0},
        {"doppler_offset_bins": 3, "delay_offset_bins": 4, "raw_score": 3.0},
    ]}
    job = {"job_id": "job", "event_id": "event", "recording": "clip",
           "family_trials": 6}
    vector = SUITE.calibrated_vector("spatial_glrt", "raw_as_acquired",
                                     result, job, 12)
    assert vector["method_id"] == "spatial_glrt"
    assert vector["target"]["raw_score"] == 4.0
    assert math.isclose(vector["target"]["empirical_survival_p"], 1 / 3)
    assert vector["controls"][1]["doppler_offset_bins"] == 3
    assert vector["trial_family_count"] == 6
    assert vector["internal_search_trials_per_path"] == 12
    assert vector["gain_treatment"] == "raw_as_acquired"


def test_gain_equalization_uses_exact_voltage_scale_for_data_and_reference():
    cutouts = [{"channels": [
        [[[1.0, 0.0]]], [[[1.0, 0.0]]], [[[1.0, 0.0]]],
        [[[1.0, 0.0]]], [[[1.0, 0.0]]]],
        "reference_weights": [[1.0, 0.0]] * 5}]
    gains = [19.7, 25.4, 20.7, 25.4, 22.9]
    changed, scales = SUITE.gain_equalized(cutouts, gains)
    assert math.isclose(20 * math.log10(scales[0]), 3.12, abs_tol=1e-12)
    assert math.isclose(20 * math.log10(scales[1]), -2.58, abs_tol=1e-12)
    for index, scale in enumerate(scales):
        assert changed[0]["channels"][index][0][0][0] == scale
        assert changed[0]["reference_weights"][index][0] == scale
