#!/usr/bin/env python3
"""Validate receipts downloaded from the trusted GPU qualification workflow.

This validates evidence, not its authenticity: the caller MUST bind downloads
to a successful workflow_dispatch run of gpu-qualification.yml at sourceSha.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re

if not __debug__:
    raise RuntimeError("GPU receipt validation cannot run with Python assertions disabled")

RECEIVERS = {"RspDuo", "Usrp", "HackRF", "Kraken"}
REQUIREMENTS_PATH = Path(__file__).with_name("unified_qualification_requirements.json")
REQUIREMENTS = json.loads(REQUIREMENTS_PATH.read_text())
CONTRACT_SHA256 = hashlib.sha256(REQUIREMENTS_PATH.read_bytes()).hexdigest()


def sha256(value):
    return isinstance(value, str) and re.fullmatch(r"[0-9a-f]{64}", value) is not None


def validate(receipt, source_sha, feature_class):
    assert receipt.get("schema") == "vectorwarp-gpu-product-parity-v2", "obsolete baseline receipt"
    assert receipt.get("qualificationScope") == "two-channel-products"
    assert receipt.get("featureContractSha256") == CONTRACT_SHA256, "feature acceptance contract changed"
    assert receipt.get("passed") is True and receipt.get("sourceSha") == source_sha
    assert receipt.get("system") == "Linux" and receipt.get("deviceClass") == feature_class
    device = receipt.get("device", {})
    assert type(device.get("shaderFloat64")) is bool
    assert device["shaderFloat64"] == (feature_class == "fp64")
    assert device.get("id") and device.get("name")
    native = receipt.get("nativeCapabilities", {})
    assert native.get("schema") == 1 and all(native.get(key) is True for key in
        ("portableGpu", "residentGpu", "widebandGpu")), "required native stage not compiled"
    receiver_report = receipt.get("receiverCapabilities", {})
    assert receiver_report.get("schema") == 1 and receiver_report.get("hardwareProbed") is False
    adapters = {item.get("receiver"): item for item in receiver_report.get("receivers", [])}
    assert RECEIVERS <= adapters.keys()
    for name in ("Kraken", "Usrp", "HackRF"):
        assert adapters[name].get("compiled") is True and adapters[name].get("moduleLoadable") is True
    rsp = adapters["RspDuo"]
    assert (rsp.get("compiled") is True and rsp.get("moduleLoadable") is True) or \
        (rsp.get("compiled") is False and rsp.get("localBuildable") is True and rsp.get("error"))
    artifacts = receipt.get("artifacts", {})
    assert all(name in artifacts for name in ("blah2", "blah2-gpu-worker",
        "blah2-gpu-vulkan.so", "blah2-gpu-wideband-vulkan.so", "blah2-gpu-resident-vulkan.so",
        "blah2-receiver-usrp.so", "blah2-receiver-hackrf.so"))
    assert artifacts and all(sha256(value) for value in artifacts.values())
    assert receipt.get("tolerance") == {"absolute": .020001, "relative": 1e-7}
    assert set(receipt.get("fixtures", {})) == {"False", "True"}
    assert all(sha256(item.get(key)) for item in receipt["fixtures"].values() for key in ("iq", "lineage"))
    expected = {(receiver, mode, temporal) for receiver in RECEIVERS
                for mode in ("cpu", "gpu", "gpu-resident") for temporal in (False, True)}
    observed = set()
    generations = set()
    plans = {}
    for case in receipt.get("cases", []):
        key = (case.get("receiver"), case.get("mode"), case.get("temporal", False))
        assert key in expected and key not in observed, ("duplicate/unexpected case", key)
        observed.add(key)
        if key[1] == "gpu-resident" and feature_class == "fp32":
            assert case.get("refused") is True and case.get("reason") == "shaderFloat64=0", case
            continue
        assert not case.get("refused") and sha256(case.get("productsSha256")), case
        assert isinstance(case.get("processorGeneration"), str) and case["processorGeneration"]
        assert case["processorGeneration"] not in generations, "execution reused across process generations"
        generations.add(case["processorGeneration"])
        assert type(case.get("processorPid")) is int and case["processorPid"] > 0
        assert sha256(case.get("configSha256"))
        assert sha256(case.get("radarPlanSha256")), "missing native resolved plan identity"
        identity = (key[0], key[2])
        assert plans.setdefault(identity, case["radarPlanSha256"]) == case["radarPlanSha256"], \
            "backend changed native resolved plan identity"
        frames = case.get("execution", [])
        assert [frame.get("nCpi") for frame in frames] == list(range(1, 13))
        for frame in frames:
            assert all(type(frame.get(field)) is int and 0 <= frame[field] < 2**53 for field in
                ("captureEpoch", "captureFirstEpochSample", "captureEndEpochSample"))
            assert frame["captureEndEpochSample"] > frame["captureFirstEpochSample"]
            stages = frame.get("stages", {})
            assert set(stages) == {"acceleration", "widebandAcceleration" if key[2] else "clutterAcceleration"}
            for name, stage in stages.items():
                if key[1] == "cpu":
                    assert stage.get("active") == "cpu" and stage.get("gpuExecuted") is False \
                        and stage.get("cpuExecuted") is True
                    continue
                active = "vulkan-wideband" if name == "widebandAcceleration" else \
                    "vulkan-resident" if key[1] == "gpu-resident" else "vulkan"
                ready = "active" if name == "widebandAcceleration" else "ready"
                checking = "qualifying" if name == "widebandAcceleration" else "checking"
                assert stage.get("active") == active and stage.get("gpuExecuted") is True
                assert stage.get("device") in (device["id"], device["name"])
                assert stage.get("state") in (ready, checking) and type(stage.get("cpuExecuted")) is bool
                if frame["nCpi"] >= 11:
                    assert stage["state"] == ready and stage["cpuExecuted"] is False
    assert observed == expected, ("missing cases", expected - observed)
    return artifacts


def required_scenarios(feature_class):
    return {scenario["id"]: scenario for scenario in REQUIREMENTS["scenarios"]
            if feature_class in scenario["deviceClasses"]}


def validate_features(feature_receipts, baselines, source_sha):
    """Check supplemental measured reports from the SAME trusted dispatch.

    This is deliberately not a producer: declarations, CPU doubles, native
    shader compilation and baseline two-channel parity do not create evidence.
    Each required check references retained measured evidence, and the trusted
    workflow remains responsible for running the corresponding corpus oracle.
    """
    by_class = {receipt.get("deviceClass"): receipt for receipt in feature_receipts}
    missing = set(baselines) - set(by_class)
    assert not missing, "Missing unified feature receipts: " + "; ".join(
        f"{kind}: {', '.join(required_scenarios(kind))}" for kind in sorted(missing))
    assert len(feature_receipts) == 2 and set(by_class) == set(baselines), \
        "exactly one supplemental receipt per physical device class required"
    for kind, baseline in baselines.items():
        receipt = by_class[kind]
        assert receipt.get("schema") == "vectorwarp-unified-gpu-features-v1"
        assert receipt.get("passed") is True and receipt.get("sourceSha") == source_sha
        assert receipt.get("featureContractSha256") == CONTRACT_SHA256, "feature contract mismatch"
        assert receipt.get("system") == "Linux" and receipt.get("device") == baseline["device"]
        assert receipt.get("artifacts") == baseline["artifacts"], "supplemental artifact mismatch"
        required = required_scenarios(kind)
        observed = set()
        generations = set()
        for scenario in receipt.get("scenarios", []):
            name = scenario.get("id")
            assert name in required and name not in observed, ("unexpected/duplicate feature scenario", name)
            observed.add(name)
            contract = required[name]
            assert scenario.get("settings") == contract["settings"], ("incomplete feature matrix", name)
            assert scenario.get("passed") is True, ("feature scenario did not pass", name)
            windows = contract["settings"].get("maxCpis", 1)
            longest = max(windows) if isinstance(windows, list) else windows
            assert type(scenario.get("observedFrames")) is int and \
                scenario["observedFrames"] >= 2 * longest + 2, \
                ("too few frames for warmup, reset and accepted execution", name)
            generation = scenario.get("processorGeneration")
            assert isinstance(generation, str) and generation and generation not in generations
            generations.add(generation)
            for field in ("configSha256", "fixtureSha256", "lineageSha256", "executionSha256",
                          "productsSha256", "oracleSha256"):
                assert sha256(scenario.get(field)), ("missing measured evidence digest", name, field)
            checks = scenario.get("checks", {})
            expected_checks = set(REQUIREMENTS["commonChecks"]) | set(contract["checks"])
            assert set(checks) == expected_checks, ("missing/unexpected feature checks", name,
                                                   expected_checks - set(checks))
            for check, evidence in checks.items():
                assert isinstance(evidence, dict) and evidence.get("passed") is True \
                    and sha256(evidence.get("evidenceSha256")), ("unproved feature check", name, check)
        assert observed == set(required), ("missing feature scenarios", kind, set(required) - observed)


def validate_pair(receipts, source_sha, feature_receipts=()):
    assert len(receipts) == 2, "exactly one FP32-only and one FP64 receipt required"
    by_class = {receipt.get("deviceClass"): receipt for receipt in receipts}
    assert set(by_class) == {"fp32", "fp64"}, "missing physical device class"
    first = validate(by_class["fp32"], source_sha, "fp32")
    assert first == validate(by_class["fp64"], source_sha, "fp64"), \
        "device classes did not exercise the SAME unified artifact"
    assert by_class["fp32"]["fixtures"] == by_class["fp64"]["fixtures"], "fixture mismatch"
    validate_features(feature_receipts, by_class, source_sha)


def validate_evidence_files(receipt, receipt_path):
    """Require the measured products/oracles/check logs, not only their labels."""
    digests = set()
    for scenario in receipt["scenarios"]:
        digests.update(scenario[field] for field in
                       ("executionSha256", "productsSha256", "oracleSha256"))
        digests.update(check["evidenceSha256"] for check in scenario["checks"].values())
    verified = set()
    while digests:
        expected = digests.pop()
        if expected in verified:
            continue
        assert len(verified) < 100000, "feature evidence reference bound exceeded"
        assert sha256(expected), "invalid evidence digest"
        path = receipt_path.parent / "gpu-feature-evidence" / f"{expected}.json"
        assert path.is_file() and not path.is_symlink(), f"missing retained feature evidence: {path}"
        with path.open("rb") as source:
            assert hashlib.file_digest(source, "sha256").hexdigest() == expected, \
                f"feature evidence changed: {path}"
        # Evidence is structured and portable; binary logs or empty marker files
        # are not an acceptable substitute for measured observation records.
        evidence = json.loads(path.read_text())
        assert isinstance(evidence, (dict, list)) and evidence, f"empty feature evidence: {path}"
        verified.add(expected)
        if isinstance(evidence, dict) and evidence.get("schema") == "vectorwarp-unified-evidence-v1":
            references = evidence.get("references")
            assert isinstance(references, list) and all(sha256(value) for value in references)
            assert evidence.get("payload"), f"empty measured feature payload: {path}"
            digests.update(references)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--receipts", type=Path)
    parser.add_argument("--source-sha")
    parser.add_argument("--requirements", action="store_true",
        help="print required supplemental scenario definitions; does not qualify anything")
    args = parser.parse_args()
    if args.requirements:
        print(json.dumps({**REQUIREMENTS, "featureContractSha256": CONTRACT_SHA256}, indent=2))
        return
    if args.receipts is None or args.source_sha is None:
        parser.error("--receipts and --source-sha are required unless printing --requirements")
    assert re.fullmatch(r"[0-9a-f]{40}", args.source_sha)
    paths = sorted(args.receipts.rglob("gpu-product-parity-*.json"))
    feature_paths = sorted(args.receipts.rglob("gpu-unified-features-*.json"))
    feature_receipts = [json.loads(path.read_text()) for path in feature_paths]
    validate_pair([json.loads(path.read_text()) for path in paths], args.source_sha,
                  feature_receipts)
    for path, receipt in zip(feature_paths, feature_receipts):
        validate_evidence_files(receipt, path)
    print("Required Linux FP32-only + FP64 product parity and unified feature evidence match the exact source and artifact.")


if __name__ == "__main__":
    main()
