import importlib.util
from pathlib import Path


PATH = Path(__file__).parents[2] / "script" / "evaluate-inverted-spatial-tail.py"
SPEC = importlib.util.spec_from_file_location("inverted", PATH)
INVERTED = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(INVERTED)


def job(job_id, event_id, role, timestamp):
    return {"job_id": job_id, "event_id": event_id, "hex": event_id,
            "role": role, "trajectory": [{"timestamp_ms": timestamp}]}


def result(target, controls):
    return {"score_vectors": {"sparse_beam": {
        "target": {"raw_score": target},
        "controls": [{"raw_score": value} for value in controls]}}}


def test_hypothesis_event_is_excluded_and_failed_gate_keeps_misses_closed():
    jobs = [job("hyp", "hyp-event", "positive", 0),
            job("q", "qualification", "positive", 1),
            job("v", "validation", "positive", 2),
            job("miss", "miss", "miss", 3)]
    state = {"results": {
        "hyp": result(.01, [0, .1, .2, .3]),
        "q": result(.25, [.1, .2, .3, .4]),
        "v": result(.25, [.1, .2, .3, .4]),
        "miss": result(.001, [.1, .2, .3, .4]),
    }}
    output = INVERTED.evaluate({"jobs": jobs}, state, "hyp")
    assert output["hypothesis_event_excluded"] == "hyp-event"
    assert output["qualification"]["events"][0]["event_id"] == "qualification"
    assert output["validation"]["events"][0]["event_id"] == "validation"
    assert output["frozen_for_misses"] is False
    assert output["misses_scored"] is False
    assert "misses" not in output


def test_fisher_probability_decreases_for_small_probabilities():
    assert INVERTED.fisher_probability([.01, .01]) < .01
    assert INVERTED.fisher_probability([.5, .5]) > .5
