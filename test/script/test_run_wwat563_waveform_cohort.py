import importlib.util
from pathlib import Path
from unittest.mock import patch


PATH = Path(__file__).parents[2] / "script" / "run-wwat563-waveform-cohort.py"
SPEC = importlib.util.spec_from_file_location("waveform_cohort", PATH)
COHORT = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(COHORT)


def test_prefetch_verifies_then_publishes_spatial_ready():
    row = {"capture": {"revision": 7, "file": "/recordings/example.mchq"}}
    with patch.object(COHORT, "copy_and_verify",
                      return_value=("/cache/example.mchq", "abc")) as copy, \
         patch.object(COHORT, "publish_spatial_ready",
                      return_value="/ready/example.json") as publish:
        result = COHORT.prefetch_and_publish(row, Path("runner.log"))
    assert result == ("/cache/example.mchq", "abc", "/ready/example.json")
    copy.assert_called_once_with(row, Path("runner.log"))
    publish.assert_called_once_with(COHORT.clip_id(row), "/cache/example.mchq",
                                    "abc", "/recordings/example.mchq")
