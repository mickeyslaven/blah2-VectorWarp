#!/usr/bin/env python3
"""Physical GPU acceptance on one all-receiver artifact; no SDR or live service.

Uses the canonical six-CPI target twice to exercise target birth/update, absent
tail, history reset and post-qualification execution. Enumeration is never a
passing qualification. A missing requested feature class is a hard failure.
"""
import argparse
import hashlib
import importlib.util
import json
import math
import os
from pathlib import Path
import platform
import re
import subprocess
import tempfile
import time
import uuid

if not __debug__:
    raise RuntimeError("GPU acceptance cannot run with Python assertions disabled")

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("canonical_replay",
    ROOT / "test/recording/processor_replay_test.py")
replay = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(replay)
RECEIVERS = ("RspDuo", "Usrp", "HackRF", "Kraken")
FRAMES = 12
# Match the public two-decimal serialization envelope, not a percentage-based
# allowance that could hide a displaced target or changed track association.
ABS_TOLERANCE = .020001
REL_TOLERANCE = 1e-7


def digest(path):
    with Path(path).open("rb") as source:
        return hashlib.file_digest(source, "sha256").hexdigest()


def compare(expected, actual, label="products", *, exact=False):
    """Compare every field and array position; never erase IDs or lineage."""
    if isinstance(expected, dict):
        assert isinstance(actual, dict) and expected.keys() == actual.keys(), label
        for key in expected:
            compare(expected[key], actual[key], f"{label}.{key}",
                    exact=exact or key == "integration" or "sample" in key or key == "capture_epoch")
    elif isinstance(expected, list):
        assert isinstance(actual, list) and len(expected) == len(actual), label
        for index, (left, right) in enumerate(zip(expected, actual)):
            compare(left, right, f"{label}[{index}]", exact=exact)
    elif type(expected) in (int, float):
        assert type(actual) in (int, float), label
        assert math.isfinite(expected) and math.isfinite(actual), label
        tolerance = 0 if exact or (type(expected) is int and type(actual) is int) else \
            ABS_TOLERANCE + REL_TOLERANCE * max(abs(expected), abs(actual))
        assert abs(expected - actual) <= tolerance, f"{label}: {expected} != {actual}"
    else:
        assert type(expected) is type(actual) and expected == actual, \
            f"{label}: {expected!r} != {actual!r}"


def select_device(report, feature_class, selector=None):
    assert report.get("version") == 1 and report.get("available") is True, report
    assert report.get("qualification") == "not-run", report
    devices = report.get("devices")
    assert isinstance(devices, list) and devices, "no physical GPU discovered"
    candidates = [device for device in devices
                  if type(device.get("shaderFloat64")) is bool and
                  device["shaderFloat64"] == (feature_class == "fp64") and
                  isinstance(device.get("id"), str) and device["id"] and
                  isinstance(device.get("name"), str) and device["name"] and
                  (not selector or device["id"] == selector)]
    assert candidates, f"required {feature_class} physical device unavailable (selector={selector!r})"
    return sorted(candidates, key=lambda device: device["id"])[0]


def assert_execution(timings, lane_frames, mode, temporal, device):
    assert len(timings) == FRAMES, "incomplete frame-bound execution evidence"
    assert [frame.get("nCpi") for frame in timings] == list(range(1, FRAMES + 1))
    stages = ("acceleration", "widebandAcceleration") if temporal else \
        ("acceleration", "clutterAcceleration")
    steady = {stage: 0 for stage in stages}
    evidence = []
    for index, frame in enumerate(timings):
        position = {key: frame.get(key) for key in
            ("captureEpoch", "captureFirstEpochSample", "captureEndEpochSample")}
        assert frame.get("captureSamplePositionAvailable") is True, position
        assert all(type(value) is int and 0 <= value < 2**53 for value in position.values()), position
        assert position["captureEndEpochSample"] > position["captureFirstEpochSample"], position
        frame_evidence = {"nCpi": frame["nCpi"], **position, "stages": {}}
        for stage in stages:
            value = frame.get(stage, {})
            assert type(value.get("gpuExecuted")) is bool and \
                type(value.get("cpuExecuted")) is bool, (index, stage, value)
            if mode == "cpu":
                assert value.get("active") == "cpu" and value["gpuExecuted"] is False \
                    and value["cpuExecuted"] is True, (index, stage, value)
            else:
                active = "vulkan-wideband" if stage == "widebandAcceleration" else \
                    "vulkan-resident" if mode == "gpu-resident" else "vulkan"
                assert value.get("active") == active and value["gpuExecuted"] is True, \
                    (index, stage, value)
                assert value.get("device") in (device["id"], device["name"]), value
                ready = "active" if stage == "widebandAcceleration" else "ready"
                checking = "qualifying" if stage == "widebandAcceleration" else "checking"
                assert value.get("state") in (ready, checking), value
                if value["state"] == ready and not value["cpuExecuted"]:
                    steady[stage] += 1
                # The final two complete CPIs must be accepted GPU execution,
                # not merely startup CPU/GPU oracle work or a fallback.
                if index >= FRAMES - 2:
                    assert value["state"] == ready and not value["cpuExecuted"], value
            frame_evidence["stages"][stage] = {key: value.get(key) for key in
                ("active", "device", "state", "gpuExecuted", "cpuExecuted")}
        if temporal:
            lane = lane_frames[index]
            assert lane.get("timestamp") == frame.get("timestamp"), "unbound lane/timing frame"
            assert (lane.get("capture_epoch"), lane.get("capture_first_input_sample"),
                    lane.get("capture_end_input_sample")) == tuple(position.values())
            assert lane.get("ambiguityGpuExecuted") is (mode != "cpu"), lane.get("acceleration")
            assert lane.get("ambiguityCpuExecuted") is (mode == "cpu"), lane.get("acceleration")
        evidence.append(frame_evidence)
    if mode != "cpu":
        assert all(count >= 2 for count in steady.values()), steady
    return evidence


def run_case(binary, recording, receiver, mode, temporal, device, *, refuse=False):
    with tempfile.TemporaryDirectory(prefix="vectorwarp-product-parity-") as temporary:
        root = Path(temporary)
        generation = str(uuid.uuid4())
        status, sinks = replay.StatusServer(), replay.SinkGroup()
        process = None
        log = (root / "processor.log").open("w+")
        try:
            status.start()
            sinks.start()
            config = replay.config(receiver, 2, recording, status.port, sinks.ports(),
                                   loop=True, temporal_lane=temporal)
            config["capture"]["fs"] = 2000000
            config["process"]["performance"]["acceleration"] = mode
            # Resident and portable must run the same algorithm, not differing
            # defaults. Resident requires clutter and an explicit tracker bound.
            config["process"]["clutter"].update(enable=True, delayMin=-1, delayMax=3)
            config["process"]["tracker"]["gpu_capacity"] = 4096
            config["process"]["tracker"]["initiate"].update(
                stationary_clutter={"enable": False}, acceleration_model="hypothesis_bank")
            cfg = root / "config.json"
            cfg.write_text(json.dumps(config))
            environment = {**os.environ, "BLAH2_GPU_DEVICE": device["id"]}
            process = subprocess.Popen([str(binary), "-c", str(cfg)], cwd=root,
                env=environment, stdout=log, stderr=subprocess.STDOUT,
                start_new_session=True)
            def completed():
                if process.poll() is not None:
                    return True
                if refuse:
                    return any(frame.get("state") == "error" for frame in status.snapshot())
                try:
                    return len(replay.json_values(sinks.payloads["timing"], "timing")) >= FRAMES
                except (ValueError, AssertionError):
                    return False  # a socket write can still be in progress
            assert replay.wait_for(completed, time.monotonic() + 180), "bounded GPU replay timed out"
            exited = process.poll() is not None
            forced = replay.stop_process(process)
            # All socket connections must drain before parsing their final byte.
            for thread in sinks.threads:
                thread.join(.05)
            log.seek(0)
            output = log.read()
            assert "Setting up device" not in output, "replay attempted live SDR setup"
            assert not sinks.truncated, f"product retention overflow: {sinks.truncated}"
            if refuse:
                reason = output + json.dumps(status.snapshot())
                feature_refusal = "shaderFloat64=0" in reason
                platform_refusal = "does not provide the resident" in reason or \
                    "Resident GPU module was not built" in reason
                assert feature_refusal or (platform.system() == "Darwin" and platform_refusal), reason[-4000:]
                assert exited or any(frame.get("state") == "error" for frame in status.snapshot()), reason[-4000:]
                assert not any(sinks.bytes[name] for name in ("map", "detection", "track", "illuminator")), \
                    "unsupported resident route published numerical products"
                return {"receiver": receiver, "mode": mode, "refused": True,
                        "reason": "shaderFloat64=0" if feature_refusal else "resident-module-not-built"}
            assert not exited and not forced and process.returncode == 0, output[-6000:]
            assert not any(any(frame.get("captureDroppedSamples", [])) for frame in status.snapshot()), \
                "lossless replay dropped samples"
            timings = replay.json_values(sinks.payloads["timing"], "timing")[:FRAMES]
            lanes = replay.json_values(sinks.payloads["illuminator"], "illuminator")[:FRAMES] if temporal else []
            if temporal:
                trace = replay.canonical_temporal_trace(sinks.payloads["illuminator"])[:FRAMES]
                assert len(trace) == FRAMES
                replay.assert_temporal_loop_reset(trace)
                products = {"temporal": trace}
            else:
                replay.assert_target_trace(sinks)
                products = {}
                for name in ("map", "detection", "track"):
                    values = replay.json_values(sinks.payloads[name], name)[:FRAMES]
                    assert len(values) == FRAMES, f"missing {name} frames"
                    assert [frame.get("timestamp") for frame in values] == \
                        [frame.get("timestamp") for frame in timings], f"unbound {name}/timing frame"
                    products[name] = [replay.canonical_product(frame) for frame in values]
            execution = assert_execution(timings, lanes, mode, temporal, device)
            return {"receiver": receiver, "mode": mode, "temporal": temporal,
                    "processorGeneration": generation, "processorPid": process.pid,
                    "configSha256": digest(cfg), "execution": execution,
                    "products": products}
        except BaseException as error:
            if process is not None:
                replay.stop_process(process)
            log.seek(0)
            raise AssertionError(f"{receiver}/{mode}/temporal={temporal}: {error}; {log.read()[-6000:]}") from error
        finally:
            if process is not None:
                replay.stop_process(process)
            sinks.close()
            status.close()
            log.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--device-class", choices=("fp32", "fp64"), required=True)
    parser.add_argument("--device", default=os.environ.get("VECTORWARP_QUALIFICATION_GPU_DEVICE"))
    parser.add_argument("--source-sha", required=True)
    parser.add_argument("--receipt", type=Path, required=True)
    args = parser.parse_args()
    assert re.fullmatch(r"[0-9a-f]{40}", args.source_sha), "exact source revision required"
    assert subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip() == args.source_sha
    binary = args.binary.resolve(strict=True)
    native = json.loads(subprocess.check_output([str(binary), "--processing-capabilities"], timeout=10))
    assert native.get("portableGpu") is True and native.get("widebandGpu") is True, native
    if platform.system() == "Linux":
        assert native.get("residentGpu") is True, "Linux unified artifact omitted resident module"
    receivers = json.loads(subprocess.check_output([str(binary), "--receiver-status"], timeout=15))
    adapters = {item["receiver"]: item for item in receivers.get("receivers", [])}
    assert set(RECEIVERS) <= adapters.keys(), "receiver capability report omitted an identity"
    for name in ("Usrp", "HackRF"):
        assert adapters[name].get("compiled") is True and adapters[name].get("moduleLoadable") is True, adapters[name]
    if adapters["RspDuo"].get("compiled"):
        assert adapters["RspDuo"].get("moduleLoadable") is True, adapters["RspDuo"]
    else:
        assert adapters["RspDuo"].get("localBuildable") is True and adapters["RspDuo"].get("error"), adapters["RspDuo"]
        assert (binary.parent.parent / "receiver-source/rspduo/kit.json").is_file(), \
            "unified artifact omitted the manual-SDK RSPduo kit"
    if platform.system() == "Linux":
        manifest = binary.parent.parent / ".vectorwarp-build"
        fields = dict(line.split("=", 1) for line in manifest.read_text().splitlines() if "=" in line)
        assert fields.get("backend") == "all" and fields.get("test_only") == "false", fields
        assert fields.get("source_commit") == args.source_sha, "artifact came from another source revision"
    worker = binary.with_name("blah2-gpu-worker")
    report = json.loads(subprocess.check_output([str(worker), "--driver-status"], timeout=30))
    device = select_device(report, args.device_class, args.device)
    # Every invocation below reuses these exact immutable native bytes.
    artifacts = {path.name: digest(path) for path in sorted(binary.parent.iterdir()) if path.is_file()}
    receiver_suffix = ".dylib" if platform.system() == "Darwin" else ".so"
    assert all(name in artifacts for name in ("blah2", "blah2-gpu-worker", "blah2-gpu-vulkan.so",
               "blah2-gpu-wideband-vulkan.so", "blah2-receiver-usrp" + receiver_suffix,
               "blah2-receiver-hackrf" + receiver_suffix)), artifacts.keys()
    cases, fixture_hashes = [], {}
    with tempfile.TemporaryDirectory(prefix="vectorwarp-canonical-fixtures-") as temporary:
        for temporal in (False, True):
            recording = Path(temporary) / f"target-{temporal}.blah2iq"
            replay.write_target_blah2iq(recording, 2000000, 204640000,
                delay_samples=80 if temporal else 8, stationary_clutter=.25)
            replay.write_portable_lineage(recording, 2, 2000000, 204640000, 240000)
            fixture_hashes[str(temporal)] = {"iq": digest(recording), "lineage": digest(str(recording) + ".json")}
            identity_baseline = None
            for receiver in RECEIVERS:
                cpu = run_case(binary, recording, receiver, "cpu", temporal, device)
                if identity_baseline is not None:
                    compare(identity_baseline, cpu["products"], f"{receiver}.receiverIdentity", exact=True)
                identity_baseline = cpu["products"]
                modes = ["gpu"] + (["gpu-resident"] if not temporal and args.device_class == "fp64" else [])
                cases.append(cpu)
                for mode in modes:
                    gpu = run_case(binary, recording, receiver, mode, temporal, device)
                    compare(cpu["products"], gpu["products"], f"{receiver}.{mode}.cpuParity")
                    cases.append(gpu)
                if not temporal and args.device_class == "fp32":
                    cases.append(run_case(binary, recording, receiver, "gpu-resident", False, device, refuse=True))
    assert artifacts == {path.name: digest(path) for path in sorted(binary.parent.iterdir()) if path.is_file()}, \
        "native artifact changed during qualification"
    for case in cases:
        if "products" in case:
            encoded = json.dumps(case.pop("products"), sort_keys=True, allow_nan=False).encode()
            case["productsSha256"] = hashlib.sha256(encoded).hexdigest()
    receipt = {"schema": "vectorwarp-gpu-product-parity-v1", "passed": True,
        "sourceSha": args.source_sha, "system": platform.system(), "architecture": platform.machine(),
        "deviceClass": args.device_class, "device": device, "nativeCapabilities": native,
        "receiverCapabilities": receivers,
        "artifacts": artifacts, "fixtures": fixture_hashes,
        "tolerance": {"absolute": ABS_TOLERANCE, "relative": REL_TOLERANCE}, "cases": cases}
    args.receipt.parent.mkdir(parents=True, exist_ok=True)
    args.receipt.write_text(json.dumps(receipt, sort_keys=True, indent=2) + "\n")
    print(json.dumps({"passed": True, "receipt": str(args.receipt), "cases": len(cases)}))


if __name__ == "__main__":
    main()
