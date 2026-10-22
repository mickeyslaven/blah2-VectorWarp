import importlib.util
from pathlib import Path


SPEC = importlib.util.spec_from_file_location(
    "stack", Path(__file__).parents[2] / "script" / "stack-current-epoch-evidence.py")
STACK = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(STACK)


def test_familywise_control_uses_per_offset_maximum_across_combinations():
    methods = {
        "a": {"target": 2, "controls": [0, 0]},
        "b": {"target": 2, "controls": [0, 100]},
        "c": {"target": 2, "controls": [0, 0]},
    }
    result = STACK.stack(methods)
    # The b-only extreme propagates into a combination at its same aligned
    # offset.  A pooled mixture would dilute it; max-stat does not.
    assert result["trial_count"] == 7
    assert result["aligned_control_offsets"] == 2
    assert result["threshold_p99_fwer"] >= max(
        row["target"] for row in result["rows"] if row["combination"] == ["b"])
