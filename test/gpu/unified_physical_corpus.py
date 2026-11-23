#!/usr/bin/env python3
"""Trusted supplemental physical corpus on the exact unified native artifact.

Use --plan to inspect the bounded matrix without starting processes. A success
receipt is written only after every required check has measured evidence.
"""
import argparse
import copy
import hashlib
import itertools
import json
import math
import os
from pathlib import Path
import platform
import re
import socket
import subprocess
import sys
import tempfile
import threading
import time
import uuid

import product_parity_test as parity
import unified_corpus_fixtures as fixtures
import verify_qualification_receipts as verify

# The oracle imports this module for shared helpers. Preserve the single live
# deadline/state when this file is invoked as a script, not as a module.
if __name__ == "__main__":
    sys.modules["unified_physical_corpus"] = sys.modules[__name__]

if not __debug__:
    raise RuntimeError("Physical corpus cannot run with Python assertions disabled")

replay = parity.replay
PUBLISHED_FRAMES = 55  # Full epoch + 24 CPIs after rewind, including the gap.
CASE_TIMEOUT = 180
MAX_EVIDENCE_BYTES = 32 << 30
DEADLINE = None


def variants(contract, device_class):
    settings = contract["settings"]
    def values(name, default):
        value = settings.get(name, default)
        return value if isinstance(value, list) else [value]
    for lanes, channels, window, motion, fusion, statistic in itertools.product(
            values("lanes", 1), values("channels", 2), values("maxCpis", 1),
            values("motion", "stationary"), values("fusion", "power"),
            values("statistics", "power")):
        if statistic == "resident-only-coherent":
            channels, fusion, window = 5, "coherent", 4
        if channels > 2 and window == 1:
            window = 4  # Bearing candidates require production TBD.
        modes = [mode.replace("-if-fp64", "") for mode in settings["modes"]
                 if "-if-fp64" not in mode or device_class == "fp64"]
        yield {"lanes": lanes, "channels": channels, "window": window, "motion": motion,
               "fusion": fusion, "modes": modes, "statistic": statistic}


def planned_cases(contract, device_class):
    for variant in variants(contract, device_class):
        # Target and null controls always run through the same modes and IQ
        # recipe. CPU is a numerical control, never physical execution proof.
        for target in (True, False):
            if "cpu" not in variant["modes"]:
                cpu_variant = copy.deepcopy(variant)
                if cpu_variant["fusion"] == "noise-whitened": cpu_variant["fusion"] = "coherent"
                yield {"variant": cpu_variant, "mode": "cpu", "target": target, "role": "numerical-control"}
            for mode in variant["modes"]:
                yield {"variant": variant, "mode": mode, "target": target}
        if "bad-manifold-refusal" in contract["checks"]:
            for refusal in ("bad-manifold", "missing-calibration"):
                yield {"variant": variant, "mode": "gpu-resident", "target": True, "refusal": refusal}
        for fault in contract["settings"].get("faults", []):
            if fault == "input-gap":
                continue  # Every non-fault run injects a real capture gap.
            for mode in variant["modes"]:
                # Single AUTO selects portable, whose transaction has no
                # prepare/commit boundary. Those boundaries are exercised by
                # forced resident and multilane AUTO on the FP64 artifact.
                if fault in ("prepare", "commit") and mode == "auto" and variant["lanes"] == 1:
                    continue
                for target in (True, False):
                    yield {"variant": variant, "mode": mode, "target": target, "fault": fault}


class Evidence:
    def __init__(self, directory):
        self.directory = directory
        self.directory.mkdir(parents=True, exist_ok=True)
        self.bytes = 0
        self.known = set()

    def put(self, value):
        references = set()
        def collect(item):
            if isinstance(item, str) and item in self.known:
                references.add(item)
            elif isinstance(item, dict):
                for child in item.values(): collect(child)
            elif isinstance(item, list):
                for child in item: collect(child)
        collect(value)
        body = fixtures.canonical({"schema": "vectorwarp-unified-evidence-v1",
            "references": sorted(references), "payload": value}) + b"\n"
        digest = hashlib.sha256(body).hexdigest()
        path = self.directory / f"{digest}.json"
        if path.exists():
            assert path.read_bytes() == body, "evidence hash collision"
            self.known.add(digest)
            return digest
        assert self.bytes + len(body) <= MAX_EVIDENCE_BYTES, "physical evidence byte budget exceeded"
        with path.open("xb") as output:
            output.write(body)
        self.bytes += len(body)
        self.known.add(digest)
        return digest


class Checks:
    def __init__(self, contract, evidence):
        self.expected = set(verify.REQUIREMENTS["commonChecks"]) | set(contract["checks"])
        self.observations = {}
        self.evidence = evidence

    def record(self, name, observation):
        assert name in self.expected, ("unregistered physical check", name)
        assert observation, ("empty physical observation", name)
        self.observations.setdefault(name, []).append(observation)

    def finish(self):
        missing = self.expected - self.observations.keys()
        assert not missing, ("no measured oracle for required checks", sorted(missing))
        return {name: {"passed": True, "evidenceSha256": self.evidence.put(observations)}
                for name, observations in sorted(self.observations.items())}


class MapEvidenceStream:
    """Spool exact map arrays while retaining bounded publication metadata."""
    def __init__(self, root, limit=64 << 20, total=1 << 30):
        self.root, self.limit, self.total = Path(root).absolute(), limit, total
        self.root.mkdir()
        self.pending, self.hashes, self.bytes = {}, set(), 0
        self.lock = threading.Lock()

    def feed(self, name, block):
        pending = self.pending.setdefault(name, bytearray())
        pending.extend(block)
        output = bytearray()
        if b"}" in block:
            suffix, position = b"", 0
            try:
                text = pending.decode()
            except UnicodeDecodeError as error:
                if error.reason != "unexpected end of data": raise
                text, suffix = pending[:error.start].decode(), pending[error.start:]
            decoder = json.JSONDecoder()
            while position < len(text):
                while position < len(text) and text[position].isspace(): position += 1
                if position == len(text): break
                try:
                    value, end = decoder.raw_decode(text, position)
                except json.JSONDecodeError:
                    break
                assert len(text[position:end].encode()) <= self.limit, "map publication exceeds 64 MiB"
                radar_map = value if name == "map" else value["map"]
                if "data" in radar_map:
                    body = fixtures.canonical(radar_map.pop("data")) + b"\n"
                    assert len(body) <= self.limit, "map evidence exceeds 64 MiB"
                    digest = hashlib.sha256(body).hexdigest()
                    path = self.root / (digest + ".json")
                    with self.lock:
                        if digest not in self.hashes:
                            assert self.bytes + len(body) <= self.total, "map evidence exceeds 1 GiB"
                            with path.open("xb") as stream: stream.write(body)
                            self.hashes.add(digest);self.bytes += len(body)
                    radar_map["dataArtifact"] = {"path": str(path), "sha256": digest}
                output.extend(fixtures.canonical(value) + b"\n")
                position = end
            pending[:] = text[position:].encode() + suffix
        assert len(pending) <= self.limit, "incomplete map publication exceeds 64 MiB"
        return output

    def finish(self):
        assert all(not value.strip() for value in self.pending.values()), "incomplete map publication at EOF"


class CorpusSinks(replay.SinkGroup):
    def __init__(self, map_evidence=None):
        self.map_evidence, self.errors = map_evidence, []
        self.connected, self.eof = set(), set()
        super().__init__()
        name = "bearing"
        listener = replay.free_listener()
        self.listeners[name] = listener
        self.bytes[name] = 0
        self.payloads[name] = bytearray()
        self.threads.append(threading.Thread(target=self._serve, args=(name, listener), daemon=True))

    def _serve(self, name, listener):
        while not self.stop.is_set():
            try:
                connection, _ = listener.accept()
            except socket.timeout:
                continue
            except OSError:
                return
            self.connected.add(name)
            with connection:
                connection.settimeout(.2)
                while not self.stop.is_set():
                    try:
                        block = connection.recv(65536)
                    except socket.timeout:
                        continue
                    except OSError:
                        break
                    if not block:
                        break
                    self.bytes[name] += len(block)
                    if name in self.payloads:
                        if name in self.truncated: continue
                        if self.map_evidence and name in ("map", "illuminator"):
                            try:
                                block = self.map_evidence.feed(name, block)
                            except (AssertionError, OSError, ValueError, KeyError) as error:
                                self.errors.append(str(error));self.truncated.add(name)
                                continue
                        remaining = replay.MAX_RETAINED_OUTPUT - len(self.payloads[name])
                        if len(block) > remaining:
                            self.truncated.add(name)
                        if remaining > 0:
                            self.payloads[name].extend(block[:remaining])
            self.eof.add(name)

    def drain_after_exit(self):
        started = time.monotonic()
        # Allow an already queued connection to be accepted, then require EOF
        # on every connected stream before inspecting any final product count.
        assert replay.wait_for(lambda: time.monotonic() - started >= .25 and
                               self.connected <= self.eof, started + 3), "output streams did not drain to EOF"
        assert not self.errors, self.errors
        if self.map_evidence: self.map_evidence.finish()


def parse_events(output):
    prefix = "VECTORWARP_GPU_QUALIFICATION "
    return [json.loads(line.split(prefix, 1)[1]) for line in output.splitlines() if prefix in line]


def read_observer(root):
    path = root / "observer" / "coherent-observer.jsonl"
    assert path.is_file(), "production-path complex observer is absent"
    values = [json.loads(line) for line in path.read_text().splitlines() if line]
    assert values and all(value.get("schema") == "vectorwarp-coherent-observer-v1" for value in values)
    return values


def run_case(binary, device, case, recording, scene, *, timeout=CASE_TIMEOUT, configuration=None, map_evidence=None):
    if DEADLINE is not None:
        timeout = min(timeout, DEADLINE - time.monotonic())
        assert timeout > 0, "three-hour physical corpus deadline exceeded"
    with tempfile.TemporaryDirectory(prefix="vectorwarp-unified-case-") as temporary:
        root = Path(temporary)
        (root / "observer").mkdir(mode=0o700)
        status, sinks = replay.StatusServer(), CorpusSinks(map_evidence)
        process = artifact_channel = None
        generation = str(uuid.uuid4())
        log = (root / "processor.log").open("w+")
        config = fixtures.configure(replay, case["variant"], case["mode"], recording, scene,
                                    root, status.port, sinks.ports(), refusal=case.get("refusal")) if configuration is None else copy.deepcopy(configuration)
        if configuration is not None:
            config["capture"]["replay"].update(state=True, loop=False, file=str(recording))
            config["capture"]["replay"]["qualification"]["observer"]["directory"] = str(root / "observer")
            config["network"]["ip"] = "127.0.0.1"
            config["network"]["ports"].update(sinks.ports(), api=status.port)
            config["save"]["path"] = str(root)
        fault = case.get("fault")
        if fault or not case.get("noGap"):
            config["capture"]["replay"]["qualification"]["fault"] = {
                "kind": fault or "input-gap", "route": "capture" if not fault else "host" if fault == "cpu-oracle-reset" else
                "resident" if case["mode"] == "gpu-resident" or (case["mode"] == "auto" and
                    case["variant"]["lanes"] > 1 and device["shaderFloat64"]) else "portable",
                "lane": 0, "frame": 0 if fault in ("startup", "allocation") else 16 if fault else 12}
        cfg = root / "config.json"
        binding = parity.artifacts.prepare(binary, case["mode"] != "cpu")
        try:
            status.start()
            sinks.start()
            artifact_channel = parity.artifacts.ReceiptChannel(binding, config,
                {**os.environ, "BLAH2_GPU_DEVICE": device["id"]})
            cfg.write_bytes(fixtures.canonical(config) + b"\n")
            parity.artifacts.verify_files(binding)
            process = subprocess.Popen([str(binary), "-c", str(cfg)], cwd=root,
                env=artifact_channel.environment, pass_fds=artifact_channel.pass_fds,
                stdout=log, stderr=subprocess.STDOUT,
                start_new_session=True)
            artifact_channel.launched()
            def completed():
                if process.poll() is not None:
                    return True
                if case.get("refusal") and any(value.get("state") == "error" for value in status.snapshot()):
                    return True
                if configuration is not None:
                    return any(value.get("state") == "complete" for value in status.snapshot())
                try:
                    return len(replay.json_values(sinks.payloads["timing"], "timing")) >= PUBLISHED_FRAMES
                except (ValueError, AssertionError):
                    return False
            assert replay.wait_for(completed, time.monotonic() + timeout), "bounded physical replay timed out"
            exited = process.poll() is not None
            forced = replay.stop_process(process)
            sinks.drain_after_exit()
            log.seek(0)
            output = log.read()
            artifact_receipt = parity.artifacts.validate(binding, artifact_channel.finish(), process.pid,
                require_worker=case["mode"] != "cpu" and not case.get("refusal"))
            assert "Setting up device" not in output, "physical corpus attempted SDR capture"
            assert not forced and not sinks.truncated, "processor stop or retained output bound failed"
            assert not any(any(frame.get("captureDroppedSamples", [])) for frame in status.snapshot())
            events = parse_events(output)
            if not case.get("refusal") and (not fault or fault == "cpu-oracle-reset"):
                assert not exited and process.returncode == 0, output[-6000:]
            values = {name: replay.json_values(payload, name) if payload else []
                      for name, payload in sinks.payloads.items()}
            for value in values.values(): replay.assert_finite(value)
            observer = read_observer(root) if values["timing"] else []
            return {"case": case, "processorGeneration": generation, "processorPid": process.pid,
                    "artifactBinding": binding, "artifactReceipt": artifact_receipt,
                    "config": config, "configSha256": parity.digest(cfg), "scene": scene,
                    "products": values, "observer": observer, "events": events,
                    "exited": exited, "returncode": process.returncode, "log": output,
                    "statuses": status.snapshot(), "calibrations": {
                        path.name: {"sha256": parity.digest(path), "contents": json.loads(path.read_text())}
                        for path in root.glob("manifold*.json")}}
        finally:
            if process is not None: replay.stop_process(process)
            if artifact_channel is not None: artifact_channel.close()
            sinks.close()
            status.close()
            log.close()


def semantic(value):
    """Exclude only execution/scheduling metadata; retain IDs and all lineage."""
    if isinstance(value, dict):
        return {key: semantic(item) for key, item in value.items()
                if key not in ("timestamp", "eventTime", "processing_ms", "product_stage_execution")}
    if isinstance(value, list): return [semantic(item) for item in value]
    return value


def resolved_plan_identity(run, observed):
    return parity.resolved_plan_identity(run, observed)


def lane_frames(run, *, bounded=True):
    count = run["case"]["variant"]["lanes"]
    if run["config"]["process"].get("multi_illuminator", {}).get("enabled", False):
        definitions = run["config"]["process"]["multi_illuminator"]["illuminators"]
        result = {lane["id"]: [] for lane in definitions if lane.get("enabled", True)}
        assert len(result) == count, "declared/public lane count differs"
        for frame in run["products"]["illuminator"]:
            assert frame.get("id") in result, "unexpected lane publication"
            if not bounded or len(result[frame["id"]]) < PUBLISHED_FRAMES:
                result[frame["id"]].append(frame)
        return result
    timing = run["products"]["timing"]
    if bounded:
        timing = timing[:PUBLISHED_FRAMES]
    else:
        for stream in ("map", "detection", "track", "timestamp"):
            assert len(run["products"][stream]) == len(timing), ("partial trailing publication", stream)
    result = []
    for index, frame in enumerate(timing):
        value = {"id": "single", "timestamp": frame["timestamp"],
            "capture_epoch": frame.get("captureEpoch"),
            "capture_first_input_sample": frame.get("captureFirstEpochSample"),
            "capture_end_input_sample": frame.get("captureEndEpochSample"),
            "acceleration": frame.get("acceleration"), "clutterAcceleration": frame.get("clutterAcceleration"),
            "trackerDiagnostics": frame.get("trackerDiagnostics", {})}
        for public, stream in (("map", "map"), ("detections", "detection"), ("tracks", "track")):
            items = run["products"][stream]
            assert len(items) > index and items[index]["timestamp"] == frame["timestamp"], "partial/unbound publication"
            value[public] = items[index]
        value["detector_integration"] = value["detections"].get("detector_integration")
        result.append(value)
    return {"single": result}


def complete_publications(run):
    """Validate every drained sink, including frames beyond the scoring prefix."""
    products = run["products"]
    timing = products["timing"]
    events = [item for item in run["events"] if item.get("boundary") == "publication-complete"]
    assert len(events) == len(timing), "publication event/timing count differs"
    intervals = [(item["captureEpoch"], item["captureFirstEpochSample"], item["captureEndEpochSample"])
                 for item in events]
    assert len(intervals) == len(set(intervals)), "duplicate complete capture publication"
    assert len(products["timestamp"]) == len(timing), "partial trailing timestamp publication"
    lanes = lane_frames(run, bounded=False)
    for name, frames in lanes.items():
        assert len(frames) == len(timing), ("partial trailing lane publication", name)
        assert [lineage_key(frame) for frame in frames] == intervals, "publication event interval differs"
        for index, frame in enumerate(frames):
            assert frame["timestamp"] == timing[index]["timestamp"] == products["timestamp"][index]["timestamp"]
            for product in ("map", "detections", "tracks"):
                assert lineage_key(frame[product]) == intervals[index], "unbound mathematical product"
                assert frame[product]["timestamp"] == frame["timestamp"]
    # Multi-lane products are carried only by the illuminator stream; a write
    # to an inactive primary sink is itself a publication contract violation.
    for stream in ("map", "detection", "track"):
        expected_count = 0 if run["config"]["process"].get("multi_illuminator", {}).get("enabled", False) else len(timing)
        assert len(products[stream]) == expected_count, ("partial trailing primary publication", stream)
        for index, item in enumerate(products[stream]):
            assert item["timestamp"] == timing[index]["timestamp"]
            assert lineage_key(item) == intervals[index], "unbound primary product"
    expected_bearings = len(timing) if not run["config"]["process"].get("multi_illuminator", {}).get("enabled", False) and \
        run["config"]["process"].get("bearing", {}).get("enable") else 0
    assert len(products["bearing"]) == expected_bearings, "partial trailing bearing publication"
    for index, item in enumerate(products["bearing"]):
        assert item["timestamp"] == timing[index]["timestamp"], "unbound bearing timestamp"
        assert lineage_key(item) == intervals[index], "unbound bearing lineage"
    return lanes


def lineage_key(frame):
    return tuple(frame.get(field) for field in
                 ("capture_epoch", "capture_first_input_sample", "capture_end_input_sample"))


def complex_values(value):
    if isinstance(value, list) and len(value) == 2 and all(type(v) in (int, float) for v in value):
        return [complex(*value)]
    assert isinstance(value, list), "complex observer shape is invalid"
    return [item for child in value for item in complex_values(child)]


def complex_error(expected, actual, rms_limit=.002, peak_limit=.005):
    left, right = complex_values(expected), complex_values(actual)
    assert left and len(left) == len(right), "complex observer geometry differs"
    assert all(math.isfinite(v.real) and math.isfinite(v.imag) for v in left + right)
    errors = [abs(a - b) ** 2 for a, b in zip(left, right)]
    power = [abs(value) ** 2 for value in left]
    rms = math.sqrt(sum(errors) / max(sum(power), 1e-30))
    peak = math.sqrt(max(errors) / max(max(power), 1e-30))
    assert rms <= rms_limit and peak <= peak_limit, ("complex numerical oracle", rms, peak)
    return {"values": len(left), "relativeRms": rms, "relativePeak": peak,
            "limits": {"rms": rms_limit, "peak": peak_limit}}


def event_count(run, boundary):
    return sum(event.get("boundary") == boundary for event in run["events"])


def main():
    global DEADLINE
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path)
    parser.add_argument("--baseline-receipt", type=Path)
    parser.add_argument("--device-class", choices=("fp32", "fp64"), required=True)
    parser.add_argument("--source-sha")
    parser.add_argument("--receipt", type=Path)
    parser.add_argument("--plan", action="store_true")
    args = parser.parse_args()
    contracts = verify.required_scenarios(args.device_class)
    if args.plan:
        print(json.dumps({"schema": "vectorwarp-unified-corpus-plan-v1",
            "deviceClass": args.device_class, "featureContractSha256": verify.CONTRACT_SHA256,
            "scenarios": {name: list(planned_cases(contract, args.device_class))
                          for name, contract in contracts.items()}}, indent=2))
        return
    if any(value is None for value in (args.binary, args.baseline_receipt, args.source_sha, args.receipt)):
        parser.error("execution requires --binary, --baseline-receipt, --source-sha and --receipt")
    assert platform.system() == "Linux" and re.fullmatch(r"[0-9a-f]{40}", args.source_sha)
    assert subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=parity.ROOT, text=True).strip() == args.source_sha
    baseline = json.loads(args.baseline_receipt.read_text())
    artifacts = verify.validate(baseline, args.source_sha, args.device_class)
    binary = args.binary.resolve(strict=True)
    actual = {path.name: parity.digest(path) for path in sorted(binary.parent.iterdir()) if path.is_file()}
    assert actual == artifacts, "supplemental runner received another native artifact"
    device = parity.select_device(json.loads(subprocess.check_output(
        [str(binary.with_name("blah2-gpu-worker")), "--driver-status"], timeout=30)),
        args.device_class, baseline["device"]["id"])
    assert device == baseline["device"], "physical device changed since baseline"
    replay.MAX_RETAINED_OUTPUT = 64 << 20
    evidence = Evidence(args.receipt.parent / "gpu-feature-evidence")
    result = {key: copy.deepcopy(baseline[key]) for key in
              ("sourceSha", "system", "deviceClass", "device", "artifacts", "featureContractSha256")}
    result.update(schema="vectorwarp-unified-gpu-features-v1", passed=False, scenarios=[])
    # Scenario implementations register only checks actually evaluated against
    # their retained observations; there is no blanket marking of contract IDs.
    from unified_corpus_oracles import qualify_scenario
    started = time.monotonic()
    DEADLINE = started + 10800
    with tempfile.TemporaryDirectory(prefix="vectorwarp-unified-fixtures-") as temporary:
        cache = {}
        for name, contract in contracts.items():
            assert time.monotonic() - started < 10800, "three-hour physical corpus deadline exceeded"
            result["scenarios"].append(qualify_scenario(
                contract, args.device_class, binary, device, Path(temporary), cache, evidence))
            print(json.dumps({"scenario": name, "passed": True}), flush=True)
    assert artifacts == {path.name: parity.digest(path) for path in sorted(binary.parent.iterdir()) if path.is_file()}, \
        "native artifact changed during supplemental qualification"
    result["passed"] = True
    args.receipt.parent.mkdir(parents=True, exist_ok=True)
    with args.receipt.open("x") as output:
        output.write(json.dumps(result, sort_keys=True, indent=2, allow_nan=False) + "\n")
    verify.validate_evidence_files(result, args.receipt)
    print(json.dumps({"passed": True, "receipt": str(args.receipt), "scenarios": len(result["scenarios"])}))


if __name__ == "__main__":
    main()
