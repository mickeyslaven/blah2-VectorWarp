#!/usr/bin/python3
"""Offline, fail-closed schema-3 release regression evaluator.

The input adapter/scorer and scientific policy require independent review. This
tool verifies their retained evidence, computes paired whole-pass outcomes, and
never connects to a radar processor or uses truth as an inference feature.
"""
from __future__ import annotations

import argparse
import fcntl
import hashlib
import json
import math
import os
from pathlib import Path
import re
import shutil
import sys
import tempfile
import time

from vectorwarp_two_node_soak import REGRESSION_CHECKS, REGRESSION_RECEIPT_SCHEMA, load_binding


POLICY_SCHEMA = "vectorwarp-release-regression-policy-v1"
INPUT_SCHEMA = "vectorwarp-release-regression-input-v1"
STRATA = ("retrospective-positive", "retrospective-negative",
          "prospective-positive", "prospective-negative")
IDENTITY_FIELDS = ("sourceCommit", "artifactSha256", "configSha256")
LOSS_FIELDS = ("droppedSamples", "discardedSamples", "discontinuities",
               "requalifications", "retiredCpis", "retiredSamples")
MAX_DOCUMENT_BYTES = 64 << 20
DIGEST = re.compile(r"[0-9a-f]{64}")


def require(condition, message):
    if not condition:
        raise ValueError(message)


def sha256(raw):
    return hashlib.sha256(raw).hexdigest()


def digest(value):
    return isinstance(value, str) and DIGEST.fullmatch(value) is not None


def number(value, minimum=0, maximum=math.inf):
    return type(value) in (int, float) and minimum <= value <= maximum and math.isfinite(value)


def count(value):
    # Offline JSON is parsed by Python, not rounded through JavaScript.
    return type(value) is int and 0 <= value < 2**64


def keys(value, expected, label):
    require(isinstance(value, dict) and set(value) == set(expected), f"{label}: unexpected/missing fields")


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        require(key not in result, f"duplicate JSON key: {key}")
        result[key] = value
    return result


def decode(raw):
    value = json.loads(raw, object_pairs_hook=unique_object,
                       parse_constant=lambda value: (_ for _ in ()).throw(ValueError(f"nonfinite JSON: {value}")))
    require(isinstance(value, dict), "JSON document must be an object")
    return value


def encoded(value):
    return (json.dumps(value, sort_keys=True, indent=2, allow_nan=False) + "\n").encode()


def read_bytes(path):
    require(path.is_file() and not path.is_symlink(), f"missing or symlinked evidence: {path}")
    with path.open("rb") as stream:
        raw = stream.read(MAX_DOCUMENT_BYTES + 1)
    require(0 < len(raw) <= MAX_DOCUMENT_BYTES, f"evidence size out of bounds: {path}")
    return raw


def sync_directory(path):
    descriptor = os.open(path, os.O_RDONLY | os.O_DIRECTORY)
    try:
        os.fsync(descriptor)
    finally:
        os.close(descriptor)


def atomic_bytes(path, raw):
    descriptor, temporary = tempfile.mkstemp(prefix=f".{path.name}.", dir=path.parent)
    try:
        with os.fdopen(descriptor, "wb") as stream:
            stream.write(raw)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
        sync_directory(path.parent)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


class Evidence:
    """One content-addressed archive; existing objects are never overwritten."""

    def __init__(self, directory, reserve):
        self.directory = directory
        self.reserve = reserve
        self.references = set()
        require(directory.is_dir() and not directory.is_symlink(), "evidence directory must exist and be real")

    def load(self, reference, structured=True):
        require(digest(reference), "invalid evidence digest")
        path = self.directory / reference
        if structured:
            raw = read_bytes(path)
            actual = sha256(raw)
        else:
            # Whole-pass recorded IQ can be large. Verify without loading it
            # into RAM; only structured scorer/runtime documents are capped.
            require(path.is_file() and not path.is_symlink() and path.stat().st_size > 0,
                    f"missing or empty retained evidence: {reference}")
            measured = hashlib.sha256()
            with path.open("rb") as stream:
                for chunk in iter(lambda: stream.read(1 << 20), b""):
                    measured.update(chunk)
            actual, raw = measured.hexdigest(), None
        require(actual == reference, f"evidence digest mismatch: {reference}")
        self.references.add(reference)
        return decode(raw) if structured else None

    def retain(self, raw):
        reference = sha256(raw)
        destination = self.directory / reference
        if destination.exists():
            require(read_bytes(destination) == raw, "retained object conflicts with digest")
        else:
            require(shutil.disk_usage(self.directory).free - len(raw) >= self.reserve,
                    "evaluator evidence disk reserve would be violated")
            # Serialized by the evaluator lock. Hash objects and output are
            # fsynced before the published receipt can refer to them.
            atomic_bytes(destination, raw)
        self.references.add(reference)
        return reference

    def result(self, value):
        return self.retain(encoded(value))


def validate_policy(policy, binding, archive):
    keys(policy, ("schema", "status", "reviewedAtEpoch", "reviewEvidenceSha256",
                  "measurementMethodSha256", "selectionMethodSha256", "tuningCutoffEpoch",
                  "truthUsage", "minimumFreeBytes", "nodes"), "policy")
    require(policy["schema"] == POLICY_SCHEMA and policy["status"] == "reviewed-before-cohort",
            "policy has not been reviewed before the cohort")
    require(number(policy["reviewedAtEpoch"], 1) and number(policy["tuningCutoffEpoch"], 1),
            "policy requires explicit review/tuning cutoff times")
    require(policy["truthUsage"] == "offline-evaluation-only", "truth must be offline evaluation only")
    require(count(policy["minimumFreeBytes"]) and policy["minimumFreeBytes"] > 0,
            "policy requires positive evidence disk reserve")
    for name in ("reviewEvidenceSha256", "measurementMethodSha256", "selectionMethodSha256"):
        archive.load(policy[name], structured=False)
    keys(policy["nodes"], binding["nodes"], "policy node set")
    for name, node in policy["nodes"].items():
        keys(node, ("baseline", "exposure", "limits"), f"{name} policy")
        keys(node["baseline"], IDENTITY_FIELDS, "baseline identity")
        require(isinstance(node["baseline"]["sourceCommit"], str) and
                re.fullmatch(r"[0-9a-f]{40}", node["baseline"]["sourceCommit"]), "invalid baseline source")
        require(all(digest(node["baseline"][field]) for field in IDENTITY_FIELDS[1:]), "invalid baseline digests")
        keys(node["exposure"], STRATA, "exposure strata")
        for stratum, limits in node["exposure"].items():
            keys(limits, ("minimumPasses", "minimumSeconds", "minimumOpportunities"), "exposure")
            require(count(limits["minimumPasses"]) and limits["minimumPasses"] > 0 and
                    number(limits["minimumSeconds"], 1) and count(limits["minimumOpportunities"]), "invalid exposure limits")
            require(limits["minimumOpportunities"] > 0 if stratum.endswith("positive") else
                    limits["minimumOpportunities"] == 0, "incorrect positive/negative exposure")
        limits = node["limits"]
        keys(limits, ("minimumDetectionFraction", "minimumDetectionDelta", "minimumContinuityFraction",
                       "minimumContinuityDelta", "maximumFalseTracksPerHour", "maximumCompletedCycleMs"), "limits")
        for field in ("minimumDetectionFraction", "minimumDetectionDelta", "minimumContinuityFraction", "minimumContinuityDelta"):
            require(number(limits[field], 0, 1), f"invalid {field}")
        require(limits["minimumDetectionFraction"] > 0 and limits["minimumContinuityFraction"] > 0,
                "positive outcome floors must be nonzero")
        require(number(limits["maximumFalseTracksPerHour"]) and
                number(limits["maximumCompletedCycleMs"], 1, binding["nodes"][name]["maximumCadenceMs"]),
                "invalid false-track or complete-cycle bound")


def evaluate_passes(population, policy, node_policy, candidate, start, through, archive):
    keys(population, ("schema", "selectionMethodSha256", "completeFromEpoch", "completeThroughEpoch", "passes"), "population")
    require(population["schema"] == "vectorwarp-release-population-v1" and
            population["selectionMethodSha256"] == policy["selectionMethodSha256"], "unreviewed pass selection")
    require(number(population["completeFromEpoch"]) and population["completeFromEpoch"] <= start and
            number(population["completeThroughEpoch"]) and population["completeThroughEpoch"] == through,
            "population does not cover the evaluation interval")
    require(isinstance(population["passes"], list), "missing whole-pass population")
    totals = {key: {"passes": 0, "seconds": 0, "opportunities": 0, "eligibleTrackSeconds": 0,
                    "baselineDetections": 0, "candidateDetections": 0, "baselineTrackSeconds": 0,
                    "candidateTrackSeconds": 0, "baselineFalseTracks": 0, "candidateFalseTracks": 0} for key in STRATA}
    seen_ids, seen_recordings, windows = set(), set(), []
    for reference in population["passes"]:
        item = archive.load(reference)
        keys(item, ("schema", "passId", "sourceKind", "split", "kind", "startEpoch", "endEpoch", "wholePass",
                    "usedForTuning", "truthUsage", "measurementMethodSha256", "recordingSha256",
                    "acquisitionSha256", "adjudicationSha256", "opportunities", "eligibleTrackSeconds",
                    "baseline", "candidate"), "whole-pass measurement")
        require(item["schema"] == "vectorwarp-release-whole-pass-v1" and item["sourceKind"] == "recorded-iq" and
                item["wholePass"] is True and
                item["usedForTuning"] is False and item["truthUsage"] == "offline-evaluation-only" and
                item["measurementMethodSha256"] == policy["measurementMethodSha256"], "ineligible whole-pass measurement")
        require(isinstance(item["passId"], str) and 0 < len(item["passId"]) <= 256 and
                item["passId"] not in seen_ids and item["recordingSha256"] not in seen_recordings,
                "duplicate whole-pass identity or recording")
        seen_ids.add(item["passId"])
        seen_recordings.add(item["recordingSha256"])
        stratum = f"{item['split']}-{item['kind']}"
        require(stratum in STRATA, "unknown whole-pass stratum")
        require(number(item["startEpoch"], 1) and number(item["endEpoch"], item["startEpoch"]) and
                item["endEpoch"] > item["startEpoch"], "invalid pass interval")
        if item["split"] == "prospective":
            require(item["startEpoch"] > policy["tuningCutoffEpoch"] and item["startEpoch"] >= start and
                    item["endEpoch"] <= through, "prospective pass outside untouched evaluation interval")
        else:
            require(item["endEpoch"] <= policy["tuningCutoffEpoch"], "retrospective/prospective split overlap")
        windows.append((item["startEpoch"], item["endEpoch"]))
        for field in ("recordingSha256", "acquisitionSha256", "adjudicationSha256"):
            archive.load(item[field], structured=False)
        require(count(item["opportunities"]) and number(item["eligibleTrackSeconds"]), "invalid measured exposure")
        require((item["opportunities"] > 0 and item["eligibleTrackSeconds"] > 0) if item["kind"] == "positive" else
                (item["opportunities"] == 0 and item["eligibleTrackSeconds"] == 0), "invalid positive/negative measurement")
        total = totals[stratum]
        for role, expected in (("baseline", node_policy["baseline"]), ("candidate", candidate)):
            result = item[role]
            keys(result, (*IDENTITY_FIELDS, "executionSha256", "productsSha256", "detectedOpportunities",
                          "continuousMeasurementBackedTrackSeconds", "falseTracks"), f"{role} result")
            require(all(result[field] == expected[field] for field in IDENTITY_FIELDS), f"{role} identity differs")
            for field in ("executionSha256", "productsSha256"):
                archive.load(result[field], structured=False)
            require(count(result["detectedOpportunities"]) and result["detectedOpportunities"] <= item["opportunities"] and
                    number(result["continuousMeasurementBackedTrackSeconds"], 0, item["eligibleTrackSeconds"]) and
                    count(result["falseTracks"]), "invalid measured outcome")
            total[role + "Detections"] += result["detectedOpportunities"]
            total[role + "TrackSeconds"] += result["continuousMeasurementBackedTrackSeconds"]
            total[role + "FalseTracks"] += result["falseTracks"]
        total["passes"] += 1
        total["seconds"] += item["endEpoch"] - item["startEpoch"]
        total["opportunities"] += item["opportunities"]
        total["eligibleTrackSeconds"] += item["eligibleTrackSeconds"]
    windows.sort()
    require(all(left[1] <= right[0] for left, right in zip(windows, windows[1:])), "overlapping whole-pass exposure")
    decisions = {check: [] for check in REGRESSION_CHECKS[:3]}
    limits = node_policy["limits"]
    for stratum, total in totals.items():
        exposure = node_policy["exposure"][stratum]
        sufficient = (total["passes"] >= exposure["minimumPasses"] and total["seconds"] >= exposure["minimumSeconds"] and
                      total["opportunities"] >= exposure["minimumOpportunities"])
        if stratum.endswith("positive"):
            denominator = total["opportunities"] or 1
            detection = total["candidateDetections"] / denominator
            detection_delta = (total["candidateDetections"] - total["baselineDetections"]) / denominator
            denominator = total["eligibleTrackSeconds"] or 1
            continuity = total["candidateTrackSeconds"] / denominator
            continuity_delta = (total["candidateTrackSeconds"] - total["baselineTrackSeconds"]) / denominator
            decisions["detections"].append((sufficient, detection >= limits["minimumDetectionFraction"] and
                                             detection_delta >= limits["minimumDetectionDelta"]))
            decisions["measurement-backed-track-continuity"].append((sufficient,
                continuity >= limits["minimumContinuityFraction"] and continuity_delta >= limits["minimumContinuityDelta"]))
            total.update(detectionFraction=detection, detectionDelta=detection_delta,
                         continuityFraction=continuity, continuityDelta=continuity_delta)
        # False tracks in positive traffic also matter; empty-scene controls
        # alone cannot excuse false associations when aircraft are present.
        false_rate = total["candidateFalseTracks"] * 3600 / (total["seconds"] or 1)
        decisions["false-track-controls"].append((sufficient, total["candidateFalseTracks"] <= total["baselineFalseTracks"] and
                                                 false_rate <= limits["maximumFalseTracksPerHour"]))
        total.update(exposureSufficient=sufficient, falseTracksPerHour=false_rate)
    statuses = {check: ("rejected" if any(sufficient and not passed for sufficient, passed in values) else
                       "inconclusive" if any(not sufficient for sufficient, _ in values) else "accepted")
                for check, values in decisions.items()}
    return statuses, totals


def evaluate_runtime(references, candidate, start, through, cadence_ms, cycle_ms, archive):
    require(isinstance(references, list) and references, "missing complete runtime ledger")
    previous_end, previous_capture, previous_cpi, previous_completion = start, None, None, start
    maximum_cycle, maximum_cadence, completed = 0, 0, 0
    capture_ok, retired_cpis, retired_samples = True, 0, 0
    for reference in references:
        block = archive.load(reference)
        keys(block, ("schema", *IDENTITY_FIELDS, "startEpoch", "endEpoch", "captureStart", "captureEnd",
                     "completedCpisBefore", "completedCpisThrough", "cycles", "rawEvidenceSha256"), "runtime block")
        require(block["schema"] == "vectorwarp-release-runtime-v1" and
                all(block[field] == candidate[field] for field in IDENTITY_FIELDS), "runtime identity differs")
        require(block["startEpoch"] == previous_end and number(block["endEpoch"], block["startEpoch"]) and
                block["endEpoch"] > block["startEpoch"] and block["endEpoch"] <= through,
                "runtime blocks have gaps/overlap or invalid bounds")
        archive.load(block["rawEvidenceSha256"], structured=False)
        before, after = block["completedCpisBefore"], block["completedCpisThrough"]
        require(count(before) and count(after) and after > before and isinstance(block["cycles"], list) and
                len(block["cycles"]) == after - before and (previous_cpi is None or before == previous_cpi),
                "incomplete CPI sequence coverage")
        if previous_capture is not None:
            require(block["captureStart"] == previous_capture, "capture blocks do not share an exact boundary")
        for index, cycle in enumerate(block["cycles"], before + 1):
            keys(cycle, ("sequence", "startedEpoch", "completedEpoch"), "completed cycle")
            require(count(cycle["sequence"]) and cycle["sequence"] == index and number(cycle["startedEpoch"], 1) and
                    number(cycle["completedEpoch"], cycle["startedEpoch"]) and
                    previous_completion < cycle["completedEpoch"] <= block["endEpoch"], "invalid completed cycle")
            maximum_cycle = max(maximum_cycle, 1000 * (cycle["completedEpoch"] - cycle["startedEpoch"]))
            maximum_cadence = max(maximum_cadence, 1000 * (cycle["completedEpoch"] - previous_completion))
            previous_completion = cycle["completedEpoch"]
        for capture in (block["captureStart"], block["captureEnd"]):
            keys(capture, ("epoch", "acceptedSamples", *LOSS_FIELDS), "capture receipt")
            require(isinstance(capture["epoch"], str) and 0 < len(capture["epoch"]) <= 256 and
                    all(count(capture[field]) for field in ("acceptedSamples", *LOSS_FIELDS)), "invalid capture counters")
            if previous_capture is not None:
                capture_ok = capture_ok and capture["epoch"] == previous_capture["epoch"] and all(
                    capture[field] >= previous_capture[field] for field in ("acceptedSamples", *LOSS_FIELDS))
            capture_ok = capture_ok and all(capture[field] == 0 for field in LOSS_FIELDS)
            previous_capture = capture
            retired_cpis = max(retired_cpis, capture["retiredCpis"])
            retired_samples = max(retired_samples, capture["retiredSamples"])
        capture_ok = capture_ok and block["captureEnd"]["acceptedSamples"] > block["captureStart"]["acceptedSamples"]
        previous_cpi, previous_end = after, block["endEpoch"]
        completed += len(block["cycles"])
    require(previous_end == through, "runtime ledger does not cover evaluated-through time")
    maximum_cadence = max(maximum_cadence, 1000 * (through - previous_completion))
    metrics = {"completedCpis": completed, "maximumCompletedCycleMs": maximum_cycle,
               "maximumCadenceMs": maximum_cadence, "retiredCpis": retired_cpis, "retiredSamples": retired_samples}
    return {"cpi-deadlines": maximum_cycle <= cycle_ms and maximum_cadence <= cadence_ms,
            "capture-health": capture_ok}, metrics


def evaluate(binding, binding_sha, policy, policy_sha, inputs, archive, now):
    validate_policy(policy, binding, archive)
    keys(inputs, ("schema", "bindingSha256", "policySha256", "cohortStartEpoch", "evaluationStartEpoch",
                  "evaluatedThroughEpoch", "nodes"), "evaluation input")
    require(inputs["schema"] == INPUT_SCHEMA and inputs["bindingSha256"] == binding_sha and
            inputs["policySha256"] == policy_sha == binding["regressionPolicySha256"], "input/binding/policy identity differs")
    start, through, cohort = (inputs[key] for key in ("evaluationStartEpoch", "evaluatedThroughEpoch", "cohortStartEpoch"))
    require(number(start, 1) and number(through, start) and number(cohort, start) and
            start <= cohort <= through <= now and now - through <= 180 and
            policy["reviewedAtEpoch"] < start and policy["tuningCutoffEpoch"] < start,
            "invalid, future, stale, or unreviewed evaluation interval")
    keys(inputs["nodes"], binding["nodes"], "evaluation nodes")
    receipt = {"schema": REGRESSION_RECEIPT_SCHEMA, "bindingSha256": binding_sha, "policySha256": policy_sha,
               "evaluationStartEpoch": start, "evaluatedThroughEpoch": through, "nodes": {}, "regressionDetected": False}
    results = {}
    for name, bound in binding["nodes"].items():
        node = inputs["nodes"][name]
        keys(node, ("qualifications", "populationSha256", "runtimeBlocks"), "evaluation node")
        require(node["qualifications"] == bound["qualifications"], "qualification set differs from binding")
        for reference in bound["qualifications"].values():
            archive.load(reference, structured=False)
        candidate = {"sourceCommit": binding["sourceCommit"], **{key: bound[key] for key in IDENTITY_FIELDS[1:]}}
        statuses, passes = evaluate_passes(archive.load(node["populationSha256"]), policy, policy["nodes"][name],
                                        candidate, start, through, archive)
        runtime_checks, runtime = evaluate_runtime(node["runtimeBlocks"], candidate, start, through,
            bound["maximumCadenceMs"], policy["nodes"][name]["limits"]["maximumCompletedCycleMs"], archive)
        statuses.update({check: "accepted" if passed else "rejected" for check, passed in runtime_checks.items()})
        results[name] = {"wholePassStrata": passes, "runtime": runtime}
        result = {"retiredCpis": runtime["retiredCpis"], "retiredSamples": runtime["retiredSamples"],
                  "qualifications": bound["qualifications"], "checks": {}}
        for check in REGRESSION_CHECKS:
            evidence = {"schema": "vectorwarp-release-check-result-v1", "bindingSha256": binding_sha,
                        "policySha256": policy_sha, "node": name, "check": check,
                        "accepted": statuses[check] == "accepted", "status": statuses[check],
                        "evaluationStartEpoch": start, "evaluatedThroughEpoch": through,
                        "populationSha256": node["populationSha256"], "runtimeBlocks": node["runtimeBlocks"],
                        "measurements": passes if check in REGRESSION_CHECKS[:3] else runtime}
            result["checks"][check] = {"accepted": evidence["accepted"], "status": statuses[check],
                                       "evidenceSha256": archive.result(evidence)}
        receipt["nodes"][name] = result
        receipt["regressionDetected"] |= "rejected" in statuses.values()
    return receipt, results


def run(args):
    output = Path(args.output)
    require(output.parent.is_dir() and not output.is_symlink(), "output parent must exist; output cannot be a symlink")
    evidence_directory = Path(args.evidence_dir)
    require(not output.resolve().is_relative_to(evidence_directory.resolve()),
            "publish receipt outside hash-addressed evidence directory")
    require(output.resolve() not in {Path(path).resolve() for path in (args.binding, args.policy, args.inputs)},
            "output must not overwrite an input file")
    with (output.parent / (output.name + ".lock")).open("a+b") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        # Revoke a previous success before reading any new inputs. A failed
        # evaluation must not leave a still-fresh success receipt consumable.
        atomic_bytes(output, encoded({"schema": REGRESSION_RECEIPT_SCHEMA, "regressionDetected": True,
                                      "error": "evaluation-in-progress-or-failed"}))
        binding_raw, policy_raw, inputs_raw = (read_bytes(Path(path)) for path in (args.binding, args.policy, args.inputs))
        binding = load_binding(args.binding)
        require(decode(binding_raw) == binding, "binding changed during read")
        policy, inputs = decode(policy_raw), decode(inputs_raw)
        reserve = policy.get("minimumFreeBytes")
        require(count(reserve) and reserve > 0, "policy requires evidence reserve")
        archive = Evidence(evidence_directory, reserve)
        binding_sha, policy_sha, inputs_sha = (archive.retain(raw) for raw in (binding_raw, policy_raw, inputs_raw))
        receipt, results = evaluate(binding, binding_sha, policy, policy_sha, inputs, archive, time.time())
        report = {"schema": "vectorwarp-release-regression-report-v1", "bindingSha256": binding_sha,
                  "policySha256": policy_sha, "inputsSha256": inputs_sha,
                  "evaluatorSha256": archive.retain(Path(__file__).read_bytes()),
                  "soakContractSha256": archive.retain(Path(__file__).with_name("vectorwarp_two_node_soak.py").read_bytes()),
                  "results": results, "receipt": receipt, "retainedEvidence": sorted(archive.references)}
        receipt["evidenceSha256"] = archive.result(report)
        require(shutil.disk_usage(output.parent).free - len(encoded(receipt)) >= binding["monitorMinimumFreeBytes"],
                "monitor output disk reserve would be violated")
        require(0 <= time.time() - receipt["evaluatedThroughEpoch"] <= 180,
                "evidence became stale while evaluating; retain newer complete input")
        atomic_bytes(output, encoded(receipt))
        return int(any(not check["accepted"] for node in receipt["nodes"].values()
                       for check in node["checks"].values()))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for flag in ("binding", "policy", "inputs", "evidence-dir", "output"):
        parser.add_argument("--" + flag, required=True)
    args = parser.parse_args()
    try:
        return run(args)
    except (ValueError, OSError, TypeError, KeyError, OverflowError, RecursionError) as error:
        print(f"release regression evaluation refused: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
