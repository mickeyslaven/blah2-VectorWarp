#!/usr/bin/env python3
"""Validate receipts downloaded from the trusted GPU qualification workflow.

This validates evidence, not its authenticity: the caller MUST bind downloads
to a successful workflow_dispatch run of gpu-qualification.yml at sourceSha.
"""
import argparse
import json
from pathlib import Path
import re

if not __debug__:
    raise RuntimeError("GPU receipt validation cannot run with Python assertions disabled")

RECEIVERS = {"RspDuo", "Usrp", "HackRF", "Kraken"}


def sha256(value):
    return isinstance(value, str) and re.fullmatch(r"[0-9a-f]{64}", value) is not None


def validate(receipt, source_sha, feature_class):
    assert receipt.get("schema") == "vectorwarp-gpu-product-parity-v1"
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
                for mode in ("cpu", "gpu") for temporal in (False, True)}
    expected |= {(receiver, "gpu-resident", False) for receiver in RECEIVERS}
    observed = set()
    generations = set()
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


def validate_pair(receipts, source_sha):
    assert len(receipts) == 2, "exactly one FP32-only and one FP64 receipt required"
    by_class = {receipt.get("deviceClass"): receipt for receipt in receipts}
    assert set(by_class) == {"fp32", "fp64"}, "missing physical device class"
    first = validate(by_class["fp32"], source_sha, "fp32")
    assert first == validate(by_class["fp64"], source_sha, "fp64"), \
        "device classes did not exercise the SAME unified artifact"
    assert by_class["fp32"]["fixtures"] == by_class["fp64"]["fixtures"], "fixture mismatch"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--receipts", type=Path, required=True)
    parser.add_argument("--source-sha", required=True)
    args = parser.parse_args()
    assert re.fullmatch(r"[0-9a-f]{40}", args.source_sha)
    paths = sorted(args.receipts.rglob("gpu-product-parity-*.json"))
    validate_pair([json.loads(path.read_text()) for path in paths], args.source_sha)
    print("Required Linux FP32-only + FP64 GPU receipts match the exact source and unified artifact.")


if __name__ == "__main__":
    main()
