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
    vector = SUITE.calibrated_vector("spatial_glrt", result, job, 12)
    assert vector["method_id"] == "spatial_glrt"
    assert vector["target"]["raw_score"] == 4.0
    assert math.isclose(vector["target"]["empirical_survival_p"], 1 / 3)
    assert vector["controls"][1]["doppler_offset_bins"] == 3
    assert vector["trial_family_count"] == 6
    assert vector["internal_search_trials_per_path"] == 12
