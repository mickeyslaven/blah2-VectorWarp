"""Pre-soak admission, separate from arithmetic and synthetic GPU parity.

The trusted workflow supplies authenticity. This gate recomputes retained
paired differential power and recorded-negative exposure/counts; it does not
derive empirical PFA from synthetic fixtures or invent approved risk bounds.
"""
import hashlib
import json
import math
from pathlib import Path
import re

if not __debug__:
    raise RuntimeError("Pre-soak admission cannot run with Python assertions disabled")

POLICY_PATH = Path(__file__).with_name("pre_soak_policy.json")


def digest(value):
    return isinstance(value, str) and re.fullmatch(r"[0-9a-f]{64}", value) is not None


def finite(value):
    return type(value) in (int, float) and math.isfinite(value)


def validate_policy(policy):
    assert policy.get("schema") == "vectorwarp-pre-soak-policy-v1"
    assert policy.get("status") == "approved-before-measurement", "Pre-soak risk/exposure bounds have not been approved"
    for name in ("minimumEchoRetention", "minimumRecordedNegativeSeconds", "minimumEvaluatedCells",
                 "maximumObservedFalseAlarmFraction", "maximumFalseTracksPerHour",
                 "maximumPfaUpperBound", "minimumPfaConfidence"):
        assert finite(policy.get(name)), ("Missing/nonfinite pre-soak bound", name)
    assert 0 < policy["minimumEchoRetention"] <= 1
    assert policy["minimumRecordedNegativeSeconds"] > 0
    assert type(policy["minimumEvaluatedCells"]) is int and policy["minimumEvaluatedCells"] > 0
    assert 0 < policy["maximumObservedFalseAlarmFraction"] < 1
    assert 0 < policy["maximumPfaUpperBound"] < 1 and .95 <= policy["minimumPfaConfidence"] < 1
    assert policy["maximumFalseTracksPerHour"] >= 0


def differential_power(control, injected):
    assert isinstance(control, list) and len(control) == len(injected) and 0 < len(control) <= 1048576
    total = 0.
    for left, right in zip(control, injected):
        assert isinstance(left, list) and isinstance(right, list) and len(left) == len(right) == 2
        assert all(finite(value) for value in left + right), "Nonfinite differential echo observation"
        total += (right[0] - left[0]) ** 2 + (right[1] - left[1]) ** 2
    assert math.isfinite(total)
    return total


def validate(receipt, source_sha, artifacts, policy, policy_sha, load):
    validate_policy(policy)
    assert receipt.get("schema") == "vectorwarp-pre-soak-radar-evidence-v1"
    assert receipt.get("sourceSha") == source_sha and receipt.get("artifacts") == artifacts, "Pre-soak evidence is for another candidate"
    assert receipt.get("policySha256") == policy_sha, "Pre-soak measurement did not use the approved policy"
    assert receipt.get("passed") is True
    assert receipt.get("syntheticOnly") is False, "Synthetic nulls cannot qualify operational exposure"
    assert receipt.get("measurementContract") == "paired-recorded-iq-and-adjudicated-whole-pass-negatives-v1"
    coverage = set()
    for reference in receipt.get("echoMeasurements", []):
        assert digest(reference)
        item = load(reference)
        assert item.get("sourceKind") == "recorded-iq" and item.get("exactAcquisition") is True
        for name in ("recordingSha256", "acquisitionSha256", "configSha256", "executionSha256", "productsSha256"):
            assert digest(item.get(name)), ("Missing retained echo identity", name)
            load(item[name]) if name in ("executionSha256", "productsSha256") else None
        assert item.get("deviceClass") in ("fp32", "fp64") and item.get("gpuExecuted") is True
        assert item.get("filterPolicy") in ("fixed-weights", "refit-weights")
        assert item.get("qualificationScope") != "bounded-clutter-replay-v1", \
            "Bounded 4096-sample/64-CPI clutter replay is not operational pre-soak qualification"
        if item["filterPolicy"] == "fixed-weights":
            assert digest(item.get("checkpointSha256")) and digest(item.get("warmPrefixSha256")), \
                "Fixed weights lack authenticated immutable checkpoint/warm-prefix evidence"
        assert finite(item.get("dopplerHz")) and item["dopplerHz"] != 0
        assert len(item["unfilteredControl"]) == len(item["filteredControl"]), \
            "Echo retention must compare the same observation shape"
        before = differential_power(item["unfilteredControl"], item["unfilteredInjected"])
        after = differential_power(item["filteredControl"], item["filteredInjected"])
        assert before > 0 and after / before >= policy["minimumEchoRetention"], "Measured differential echo retention failed"
        assert type(item.get("matchedDetections")) is int and item["matchedDetections"] > 0
        assert type(item.get("matchedOperationalTracks")) is int and item["matchedOperationalTracks"] > 0
        coverage.add((item["deviceClass"], item["filterPolicy"], 1 if item["dopplerHz"] > 0 else -1))
    assert coverage == {(device, mode, sign) for device in ("fp32", "fp64")
                        for mode in ("fixed-weights", "refit-weights") for sign in (-1, 1)}, \
        "Missing physical fixed/refit signed-Doppler echo retention"
    coverage = set()
    seen = set()
    for reference in receipt.get("negativeMeasurements", []):
        assert digest(reference)
        item = load(reference)
        kind = item.get("deviceClass")
        assert kind in ("fp32", "fp64") and kind not in coverage
        assert item.get("sourceKind") == "adjudicated-recorded-negative" and item.get("exactAcquisition") is True
        assert item.get("wholePassDisjoint") is True and item.get("truthUsedForInference") is False
        assert item.get("gpuExecuted") is True
        for name in ("adjudicationSha256", "executionSha256", "productsSha256"):
            assert digest(item.get(name)); load(item[name])
        seconds, evaluated, false_alarms, false_tracks, baseline_false = 0., 0, 0, 0, 0
        for window in item.get("windows", []):
            assert digest(window.get("recordingSha256")) and digest(window.get("acquisitionSha256"))
            assert type(window.get("captureEpoch")) is int and 0 <= window["captureEpoch"] < 2**64, \
                "Recorded-negative exposure lacks exact epoch lineage"
            key = (kind, window["recordingSha256"], window.get("captureEpoch"))
            assert key not in seen, "Recorded-negative exposure reused within a device class"
            seen.add(key)
            for field in ("firstSample", "endSample", "sampleRate", "evaluatedCells", "falseAlarmCells", "falseTracks", "baselineFalseTracks"):
                assert type(window.get(field)) is int and window[field] >= 0, ("Missing measured negative count", field)
            assert window["endSample"] > window["firstSample"] and window["sampleRate"] > 0
            assert window["falseAlarmCells"] <= window["evaluatedCells"]
            seconds += (window["endSample"] - window["firstSample"]) / window["sampleRate"]
            evaluated += window["evaluatedCells"]; false_alarms += window["falseAlarmCells"]
            false_tracks += window["falseTracks"]; baseline_false += window["baselineFalseTracks"]
        assert seconds >= policy["minimumRecordedNegativeSeconds"] and evaluated >= policy["minimumEvaluatedCells"], "Insufficient adjudicated recorded-negative exposure"
        assert false_alarms / evaluated <= policy["maximumObservedFalseAlarmFraction"], "Observed recorded-negative false-alarm fraction exceeded bound"
        assert false_tracks * 3600 / seconds <= policy["maximumFalseTracksPerHour"] and false_tracks <= baseline_false, \
            "Recorded-negative false-track bound/increase failed"
        # The raw fraction is not a calibrated PFA/confidence bound. Require a
        # separate retained analysis of the recorded controls, with an explicit
        # independence/block model and a reviewed method identity.
        assert digest(item.get("pfaAnalysisSha256")), "Missing measured PFA analysis"
        analysis = load(item["pfaAnalysisSha256"])
        assert analysis.get("schema") == "vectorwarp-recorded-negative-pfa-v1"
        assert analysis.get("syntheticOnly") is False and analysis.get("methodReviewed") is True
        assert analysis.get("method") in ("whole-pass-block-bootstrap", "exact-binomial-independent-trials")
        assert digest(analysis.get("methodSourceSha256")) and digest(analysis.get("independenceEvidenceSha256"))
        load(analysis["independenceEvidenceSha256"])
        assert all(finite(analysis.get(name)) for name in ("estimate", "upperBound", "confidence")), "Null/nonfinite measured PFA evidence"
        assert analysis.get("evaluatedCells") == evaluated and analysis.get("falseAlarmCells") == false_alarms
        assert abs(analysis["estimate"] - false_alarms / evaluated) <= 1e-15
        assert analysis["estimate"] <= analysis["upperBound"] <= policy["maximumPfaUpperBound"]
        assert policy["minimumPfaConfidence"] <= analysis["confidence"] < 1
        coverage.add(kind)
    assert coverage == {"fp32", "fp64"}, "Missing physical recorded-negative exposure"


def validate_directory(directory, source_sha, artifacts):
    policy_bytes = POLICY_PATH.read_bytes()
    policy = json.loads(policy_bytes)
    validate_policy(policy)
    paths = sorted(directory.rglob("pre-soak-radar-evidence.json"))
    assert len(paths) == 1, "Missing exact-candidate pre-soak physical retention/negative-control receipt"
    path = paths[0]
    def load(expected):
        source = path.parent / "pre-soak-evidence" / f"{expected}.json"
        assert source.is_file() and not source.is_symlink(), "Missing retained pre-soak measurement"
        assert 0 < source.stat().st_size <= 64 << 20, "Pre-soak evidence exceeds bounded size"
        body = source.read_bytes()
        assert hashlib.sha256(body).hexdigest() == expected, "Pre-soak retained evidence digest differs"
        result = json.loads(body)
        assert isinstance(result, dict) and result, "Empty pre-soak measurement"
        return result
    validate(json.loads(path.read_text()), source_sha, artifacts, policy,
             hashlib.sha256(policy_bytes).hexdigest(), load)


if __name__ == "__main__":
    validate_policy(json.loads(POLICY_PATH.read_bytes()))
