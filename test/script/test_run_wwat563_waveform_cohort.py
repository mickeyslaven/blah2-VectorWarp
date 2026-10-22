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


def test_process_phase_does_not_schedule_the_next_phase():
    positive = {"capture": {"revision": 1, "file": "/recordings/positive.mchq"}}
    miss = {"capture": {"revision": 2, "file": "/recordings/miss.mchq"}}
    calls = []

    def prefetch(row, _log):
        calls.append(("prefetch", row["capture"]["file"]))
        return "/cache/raw", "digest", "/ready/raw.json"

    def process(row, *_args, **_kwargs):
        calls.append(("process", row["capture"]["file"]))

    with patch.object(COHORT, "prefetch_and_publish", side_effect=prefetch), \
         patch.object(COHORT, "process", side_effect=process):
        COHORT.process_phase([positive], {"clips": {}}, Path("state.json"),
                             Path("runner.log"))
        assert calls == [
            ("prefetch", "/recordings/positive.mchq"),
            ("process", "/recordings/positive.mchq"),
        ]
        COHORT.process_phase([miss], {"clips": {}}, Path("state.json"),
                             Path("runner.log"))
    assert calls[-2:] == [
        ("prefetch", "/recordings/miss.mchq"),
        ("process", "/recordings/miss.mchq"),
    ]
