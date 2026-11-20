#!/usr/bin/env python3
"""Produce pre-soak evidence from retained canonical production replay runs.

This is a bounded postprocessor, not another radar pipeline or a replay launcher.
The caller pins the authorized plan independently and retains the existing
replay harness's config, observer, public products, execution and acquisition
receipts. Missing measurements fail closed; policy approval is never inferred
from observed results. The trusted qualification workflow owns authenticity.
"""
import argparse
import copy
import hashlib
import json
from pathlib import Path
import subprocess

import pre_soak_receipt_gate as gate
import unified_physical_corpus as corpus
import clutter_replay_qualification as clutter_checkpoint

MAX_BYTES = 64 << 20


def canonical(value):
    return json.dumps(value, sort_keys=True, separators=(",", ":"), allow_nan=False).encode() + b"\n"


def require(condition, message):
    if not condition:
        raise ValueError(message)


def integer(value):
    return type(value) is int and 0 <= value < 2**64


def pinned(reference, root):
    require(isinstance(reference, dict) and gate.digest(reference.get("sha256")), "Missing external evidence pin")
    path = root / reference["path"]
    require(path.is_file() and not path.is_symlink() and 0 < path.stat().st_size <= MAX_BYTES,
            "Retained evidence is missing, linked or exceeds 64 MiB")
    body = path.read_bytes()
    require(hashlib.sha256(body).hexdigest() == reference["sha256"], "Retained evidence SHA-256 mismatch")
    return json.loads(body)


class Evidence:
    def __init__(self, root):
        self.root, self.values, self.bytes = root, {}, 0
        root.mkdir()

    def put(self, value):
        body = canonical(value)
        require(len(body) <= MAX_BYTES, "Measurement exceeds 64 MiB")
        digest = hashlib.sha256(body).hexdigest()
        if digest not in self.values:
            require(self.bytes + len(body) <= 1 << 30, "Retained evidence exceeds 1 GiB")
            with (self.root / (digest + ".json")).open("xb") as output:
                output.write(body)
            self.values[digest] = value
            self.bytes += len(body)
        return digest


def observation_key(item):
    values = tuple(item.get(key) for key in ("captureEpoch", "captureFirst", "captureEnd"))
    require(all(integer(v) for v in values) and values[2] > values[1], "Invalid exact observer interval")
    return (item["lane"], *values)


def observations(run, stage):
    found = {}
    for item in run["observer"]:
        require(item.get("schema") == "vectorwarp-coherent-observer-v1", "Unknown production observer schema")
        if item.get("stage") != stage:
            continue
        key = observation_key(item)
        if not run["config"]["process"].get("multi_illuminator", {}).get("enabled") and key[0] == "primary":
            key = ("single", *key[1:])
        require(key not in found, "Duplicate production observation")
        found[key] = item
    return found


def validate_run(run, plan):
    require(run.get("sourceSha") == plan["sourceSha"] and run.get("artifacts") == plan["artifacts"],
            "Retained run is for another exact candidate")
    acquisition = run["acquisition"]
    require(acquisition.get("schema") == "vectorwarp-recording-inspection-v1" and
            all(acquisition.get(field) is True for field in
                ("exact_acquisition", "acquisition_hash_trusted", "acquisition_integrity_valid", "acquisition_capable")),
            "Recorded run lacks production-parser exact acquisition admission")
    require(all(gate.digest(acquisition.get(field)) for field in ("recording_sha256", "acquisition_sha256")),
            "Recorded acquisition identity is missing")
    config = run["config"]
    replay = config["capture"]["replay"]
    qualification = replay.get("qualification", {})
    pins = qualification.get("acquisition", {})
    require(replay.get("state") is True and replay.get("loop") is not True and
            qualification.get("enabled") is True and
            qualification.get("confirmation") == "offline-physical-gpu-qualification-v1" and
            pins.get("require_exact") is True and
            pins.get("recording_sha256") == acquisition["recording_sha256"] and
            pins.get("acquisition_sha256") == acquisition["acquisition_sha256"],
            "Run did not pin exact completed recorded IQ at production admission")
    require(run.get("returncode") == 0 and run.get("artifactReceipt", {}).get("passed") is True,
            "Run lacks successful canonical replay/artifact acceptance")
    binding = run["artifactBinding"]
    for artifact in binding["artifacts"].values():
        require(plan["artifacts"].get(Path(artifact["path"]).name) == artifact["sha256"],
                "Executed artifact differs from the exact candidate")
    require("processor" in binding["artifacts"], "Executed processor binding is absent")
    loaded = run["artifactReceipt"].get("loaded", [])
    if run.get("deviceClass") in ("fp32", "fp64"):
        require(bool(loaded) and bool(run["artifactReceipt"].get("workers")), "Physical replay has no early loaded-module evidence")
        require(any(any(device.get("id") == run["device"]["id"] and
                        device.get("shaderFloat64") is (run["deviceClass"] == "fp64")
                        for device in item.get("driverStatus", {}).get("devices", [])) for item in loaded),
                "Early module receipt does not bind the observed physical device")
    for item in loaded:
        require(item.get("module") == binding["artifacts"].get(item.get("role")) and
                item.get("nonce") == binding.get("nonce"), "Loaded module differs from replay artifact binding")
    require(run.get("truthUsedForInference") is False, "Evaluation truth entered inference")
    for status in run["statuses"]:
        require(not status.get("error") and not any(status.get("captureDroppedSamples", [])),
                "Recorded run contains errors or dropped samples")
    require(any(status.get("state") == "complete" for status in run["statuses"]), "Recorded run lacks completed EOF")
    # Shared publication validator checks every sink/event/interval, including
    # trailing output. Only its fixture-specific lane-count descriptor is adapted.
    normalized = dict(run)
    multi = config["process"].get("multi_illuminator", {})
    count = sum(lane.get("enabled", True) for lane in multi.get("illuminators", [])) if multi.get("enabled") else 1
    normalized["case"] = {"variant": {"lanes": count}}
    return corpus.complete_publications(normalized)


def select(run, lanes, window, physical):
    first, end = window["firstSample"], window["endSample"]
    require(integer(first) and integer(end) and end > first, "Invalid predeclared sample window")
    epoch = int(run["acquisition"]["epoch"])
    selected = {}
    detectors = observations(run, "detector")
    products = observations(run, "products")
    for lane, frames in lanes.items():
        chosen = [frame for frame in frames if corpus.lineage_key(frame)[0] == epoch and
                  frame["capture_first_input_sample"] >= first and frame["capture_end_input_sample"] <= end]
        cursor = first
        require(bool(chosen), "Whole-pass window has no published frames")
        for frame in chosen:
            _, begin, finish = corpus.lineage_key(frame)
            require(begin == cursor, "Whole-pass exposure has a gap, duplicate or partial CPI")
            cursor = finish
            key = (lane, epoch, begin, finish)
            observed = detectors.get(key, {})
            require(observed.get("measurement") == "production-cfar-pre-centroid-v1" and
                    observed.get("enabled") is True and observed.get("ready") is True,
                    "Missing ready production evaluated-CUT observation")
            require(all(integer(observed.get(name)) for name in ("evaluated_cells", "accepted_cells", "nonfinite_cells")) and
                    0 < observed["evaluated_cells"] and observed["accepted_cells"] <= observed["evaluated_cells"] and
                    observed["nonfinite_cells"] == 0, "Invalid/nonfinite evaluated-CUT population")
            require(key in products, "Detector observation lacks its production CAF")
            rate = observed["sampleRateHz"] * observed["inputSamplesPerOutput"]
            require(rate == run["acquisition"]["sample_rate_hz"], "Observer and recording sample rates differ")
            if physical:
                device = run["device"]
                require(run.get("deviceClass") in ("fp32", "fp64") and
                        device.get("shaderFloat64") is (run["deviceClass"] == "fp64"), "Wrong physical device class")
                for stage in ("acceleration", "clutterAcceleration"):
                    execution = frame.get(stage, {})
                    require(execution.get("gpuExecuted") is True and execution.get("cpuExecuted") is False and
                            execution.get("state") == "ready" and execution.get("active") in ("vulkan", "vulkan-resident") and
                            execution.get("device") in (device["id"], device["name"]),
                            "Evaluated exposure did not execute the declared physical GPU")
            else:
                require(frame.get("acceleration", {}).get("active") == "cpu", "Negative baseline is not CPU")
        require(cursor == end, "Whole-pass exposure is incomplete")
        selected[lane] = chosen
    return selected, detectors


def operational_tracks(lanes):
    identities = set()
    for lane, frames in lanes.items():
        for frame in frames:
            tracks = frame["tracks"]
            require(type(tracks.get("n")) is int and tracks["n"] == len(tracks.get("data", [])), "Track publication count differs")
            current = set()
            for track in tracks["data"]:
                require("id" in track and track["id"] not in current, "Duplicate/missing published track identity")
                current.add(track["id"])
                if track.get("state") in ("ACTIVE", "COASTING"):
                    identities.add((lane, frame["capture_epoch"], track["id"]))
    return identities


def process_contract(run):
    process = copy.deepcopy(run["config"]["process"])
    process.get("performance", {}).pop("acceleration", None)
    return process


def golden_pair(baseline, candidate, pair, plan):
    """Full retained public products, not counts or a reimplemented tracker.

    Public output is rounded to two decimals: preserve the existing .020001
    absolute envelope, converting tracker delay bins to kilometres first.
    IDs, ordering, states, M/N counters, sample lineage and temporal support
    remain exact. No matching/reassociation can erase a changed lifecycle.
    """
    require(candidate["acquisition"] == baseline["acquisition"], "Golden replay changed exact IQ acquisition")
    left = validate_run(baseline, pair.get("baselineIdentity", plan))
    right = validate_run(candidate, plan)
    require(left.keys() == right.keys() and bool(left), "Golden replay lane set differs or is empty")
    require(pair.get("requiredPlanFields") and pair.get("planSha256ByLane"), "Golden plan lacks predeclared math identity")
    for path, value in pair.get("requiredProcessFields", {}).items():
        found = candidate["config"]["process"]
        for part in path.split("."):
            found = found.get(part) if isinstance(found, dict) else None
        require(found == value, "Golden replay changed a required frontend/fusion/bearing feature")
    expected = observations(baseline, "products")
    actual = observations(candidate, "products")
    detectors = observations(candidate, "detector")
    identities, counts = {}, {"frames": 0, "detections": 0, "operationalTrackFrames": 0}
    states, lifecycle, previous = set(), [], {}
    mode = candidate["config"]["process"]["performance"]["acceleration"]
    require(mode in ("cpu", "gpu", "gpu-resident"), "Golden execution requires an explicit backend")
    for lane in left:
        require(len(left[lane]) == len(right[lane]) > 0, "Golden replay frame count differs")
        for index, (first, second) in enumerate(zip(left[lane], right[lane])):
            position = corpus.lineage_key(second)
            require(corpus.lineage_key(first) == position, "Golden replay sample lineage differs")
            key = (lane, *position)
            require(key in expected and key in actual and key in detectors, "Golden replay lacks native frame evidence")
            identity = corpus.resolved_plan_identity(candidate, actual[key])
            require(identity == corpus.resolved_plan_identity(baseline, expected[key]) ==
                    pair["planSha256ByLane"].get(lane), "Golden resolved radar plan differs or drifted")
            identities[lane] = identity
            native = json.loads(actual[key]["resolved_product_plan"])
            require(all(native.get(field) == value for field, value in pair["requiredPlanFields"].items()),
                    "Golden replay disabled or changed a required math feature")
            observed = detectors[key]
            require(observed.get("enabled") is native["stages.detection"] and
                    observed.get("nonfinite_cells") == 0, "Golden detector execution contradicts its plan")
            for run, frame in ((baseline, first), (candidate, second)):
                backend = run["config"]["process"]["performance"]["acceleration"]
                require(backend in ("cpu", "gpu", "gpu-resident"), "Golden baseline requires explicit execution")
                for stage, enabled in (("acceleration", True), ("clutterAcceleration", native["stages.clutter"])):
                    execution = frame.get(stage, {})
                    if not enabled:
                        require(not execution.get("gpuExecuted") and not execution.get("cpuExecuted"),
                                "Golden replay executed a disabled stage")
                        continue
                    active = {"cpu": "cpu", "gpu": "vulkan", "gpu-resident": "vulkan-resident"}[backend]
                    require(execution.get("active") == active and execution.get("gpuExecuted") is (backend != "cpu"),
                            "Golden replay substituted the configured backend")
                    if backend != "cpu":
                        require(execution.get("device") in (run["device"]["id"], run["device"]["name"]),
                                "Golden execution device differs")
                        if index >= 2 or index == len(left[lane]) - 1:
                            require(execution.get("cpuExecuted") is False and execution.get("state") == "ready",
                                    "Golden replay did not reach physical GPU execution")
                    else:
                        require(execution.get("cpuExecuted") is True, "Golden CPU execution is absent")
            for name in ("map", "detections", "tracks"):
                a, b = corpus.semantic(first[name]), corpus.semantic(second[name])
                for item in (a, b):
                    if isinstance(item.get("detector_integration"), dict):
                        item["detector_integration"].pop("backend", None)
                if name == "tracks":
                    scale = 299792458.0 / actual[key]["sampleRateHz"] / 1000
                    a, b = copy.deepcopy(a), copy.deepcopy(b)
                    require(len(a["data"]) == len(b["data"]), "Golden track count differs")
                    for before, after in zip(a["data"], b["data"]):
                        for field in ("delay", "associated_delay"):
                            if field not in before and field not in after: continue
                            one, two = before.pop(field), after.pop(field)
                            physical = lambda value: [v * scale for v in value] if isinstance(value, list) else value * scale
                            corpus.parity.compare(physical(one), physical(two), f"{lane}.track.{field}_km",
                                                  absolute=corpus.parity.ABS_TOLERANCE * scale)
                corpus.parity.compare(a, b, f"{lane}.{name}")
            temporal = lambda frame: {key: value for key, value in (frame.get("detector_integration") or {}).items()
                                      if key not in ("backend", "processing_ms")}
            corpus.parity.compare(temporal(first), temporal(second), f"{lane}.temporal", exact=True)
            counts["frames"] += 1;counts["detections"] += len(second["detections"]["delay"])
            current = {item["id"]: item for item in second["tracks"]["data"]}
            for identifier, track in current.items():
                states.add(track["state"])
                counts["operationalTrackFrames"] += track["state"] in ("ACTIVE", "COASTING")
                lifecycle.append({"lane": lane, "capture": position, "id": identifier,
                    "event": "observed" if identifier in previous.get(lane, {}) else "first-publication",
                    "state": track["state"], "updates": track.get("n"), "measured": track.get("measured")})
            for identifier in previous.get(lane, {}).keys() - current.keys():
                lifecycle.append({"lane": lane, "capture": position, "id": identifier, "event": "removed"})
            previous[lane] = current
    corpus.parity.compare(corpus.semantic(baseline["products"]["bearing"]),
                          corpus.semantic(candidate["products"]["bearing"]), "bearing")
    require(set(pair.get("requiredTrackStates", [])) <= states, "Golden replay did not exercise required track lifecycle")
    for field, minimum in pair.get("minimumCounts", {}).items():
        require(field in counts and type(minimum) is int and minimum >= 0 and counts[field] >= minimum,
                "Golden replay lacks required positive/continuity exposure")
    return {"plans": identities, "counts": counts, "trackStates": sorted(states), "lifecycle": lifecycle,
            "recordingSha256": candidate["acquisition"]["recording_sha256"], "mode": mode,
            "detectionDelayUnit": "km", "trackerDelayUnit": "km", "dopplerUnit": "Hz",
            "tolerance": {"serializedAbsolute": corpus.parity.ABS_TOLERANCE,
                "relative": corpus.parity.REL_TOLERANCE, "trackerDelayKm": "serializedAbsolute*c/sampleRateHz/1000"}}


def capture_run(plan, name, root, output):
    """Collect an exact retained recording through the existing corpus lifecycle."""
    spec = plan["captureRuns"][name]
    config = pinned(spec["config"], root)
    binary, inspector = (root / spec[key]["path"] for key in ("binary", "inspector"))
    for key, path in (("binary", binary), ("inspector", inspector)):
        require(corpus.parity.digest(path) == spec[key]["sha256"], "Replay executable pin differs")
    replay = config["capture"]["replay"]
    bounded_clutter = spec.get("qualificationKind") == "clutter-echo-v1"
    require(spec.get("qualificationKind", "golden-v1") in ("golden-v1", "clutter-echo-v1"),
            "Unknown recorded qualification capture kind")
    require(bounded_clutter or "clutter_checkpoint" not in replay.get("qualification", {}),
            "Private clutter checkpoint cannot enter ordinary golden capture")
    require(replay.get("state") is True and replay.get("loop") is False and
            replay.get("format") in ("blah2", "mchq") and
            (bounded_clutter or not replay.get("inject_echo", {}).get("state")),
            "Golden capture requires unchanged completed native IQ")
    checkpoint_plan = None
    if bounded_clutter:
        checkpoint_plan, _ = clutter_checkpoint.descriptor(config, plan["sourceSha"])
        require(checkpoint_plan["warm_frame"] < spec["maximumFrames"] <= 64,
                "Bounded clutter capture requires a post-warm CPI within 64 frames")
        if checkpoint_plan["mode"] == "export":
            require(not Path(checkpoint_plan["path"]).exists(), "Checkpoint export cannot replace prior evidence")
        else:
            clutter_checkpoint.retained_checkpoint(checkpoint_plan)
    recording = root / spec["recording"]["path"]
    pins = replay["qualification"]["acquisition"]
    require(pins["require_exact"] is True and pins["recording_sha256"] == spec["recording"]["sha256"],
            "Golden capture lacks independently pinned acquisition")
    acquisition = json.loads(subprocess.check_output([str(inspector), str(recording), replay["format"],
        str(config["capture"]["device"]["channel_count"]), str(config["capture"]["fs"]),
        str(config["capture"]["fc"]), pins["recording_sha256"], pins.get("acquisition_sha256", ""), "1"], timeout=120))
    mode = config["process"]["performance"]["acceleration"]
    require(mode in ("cpu", "gpu", "gpu-resident"), "Golden capture requires explicit execution")
    kind = spec["deviceClass"]
    require((mode == "cpu") == (kind == "cpu"), "Golden capture device class contradicts backend")
    device = {"id": "cpu", "name": "cpu"} if mode == "cpu" else corpus.parity.select_device(
        json.loads(subprocess.check_output([str(binary.with_name("blah2-gpu-worker")), "--driver-status"], timeout=30)),
        kind, spec.get("device"))
    require(1 <= spec["maximumFrames"] <= 256, "Golden capture exceeds the qualification frame bound")
    replay["qualification"]["observer"] = {"summary_only": True, "directory": "assigned-by-private-runner",
        "max_frames": spec["maximumFrames"], "max_samples_per_channel": 16777216,
        "max_map_cells": 4194304, "max_bytes": 268435456}
    if bounded_clutter:
        replay["qualification"]["observer"].update(summary_only=False, max_samples_per_channel=4096,
            max_map_cells=8192, clutter={"enable": True, "filter_policy": "refit-weights"})
    require("fault" not in replay["qualification"], "Golden capture cannot inject a fault")
    multi = config["process"].get("multi_illuminator", {})
    lanes = sum(item.get("enabled", True) for item in multi.get("illuminators", [])) if multi.get("enabled") else 1
    paths = {}
    def files(value):
        if isinstance(value, dict):
            for key, item in value.items():
                if key.endswith("_file") and item:
                    path = Path(item)
                    require(path.name not in paths or paths[path.name] == path, "Ambiguous calibration filenames")
                    paths[path.name] = path
                else: files(item)
        elif isinstance(value, list):
            for item in value: files(item)
    files(config["process"])
    calibrations = {name: {"sha256": corpus.parity.digest(path)} for name, path in paths.items()}
    timeout = spec.get("timeoutSeconds", 180)
    require(type(timeout) in (int, float) and 0 < timeout <= 1800, "Golden capture deadline exceeds 30 minutes")
    corpus.replay.MAX_RETAINED_OUTPUT = MAX_BYTES
    run = corpus.run_case(binary, device, {"variant": {"lanes": lanes}, "mode": mode, "noGap": True},
        recording, {}, timeout=timeout, configuration=config)
    require(calibrations == {name: {"sha256": corpus.parity.digest(path)} for name, path in paths.items()},
            "Calibration changed during golden capture")
    run.update(sourceSha=plan["sourceSha"], artifacts=plan["artifacts"], acquisition=acquisition,
        deviceClass=kind, device=device, truthUsedForInference=False, calibrations=calibrations)
    if bounded_clutter:
        run["clutterCheckpoint"] = clutter_checkpoint.retained_checkpoint(checkpoint_plan)
        run["qualificationScope"] = "bounded-clutter-replay-v1"
    validate_run(run, plan)
    body = canonical(run)
    require(len(body) <= MAX_BYTES, "Golden run exceeds retained evidence bound")
    with output.open("xb") as stream: stream.write(body)
    return {"path": str(output), "sha256": hashlib.sha256(body).hexdigest(),
        "plans": {row["lane"]: corpus.resolved_plan_identity(run, row)
                  for row in run["observer"] if row["stage"] == "products"}}


POLICY_FIELDS = ("detection.track_before_detect.enable", "detection.track_before_detect.max_cpis",
    "detection.track_before_detect.motion_bins", "detection.track_before_detect.delay_motion_bins",
    "detection.track_before_detect.doppler_motion_bins", "detection.track_before_detect.fractional_delay",
    "tracker.initiate.M", "tracker.initiate.N", "tracker.delete")
NATIVE_POLICY_FIELDS = ("integrationCpis", "integrationMotion", "trackerM", "trackerN", "trackerDelete")
# These are assigned by corpus.run_case, not radar controls. Each actual value
# remains independently pinned in the run declaration before cross-arm removal.
HARNESS_FIELDS = ("capture.replay.file", "capture.replay.qualification.observer.directory",
                  "network.ports", "save.path")


def field_at(value, path, *, remove=False):
    parts = path.split(".")
    for part in parts[:-1]:
        require(isinstance(value, dict) and part in value, "Missing declared field: " + path)
        value = value[part]
    require(isinstance(value, dict) and parts[-1] in value, "Missing declared field: " + path)
    return value.pop(parts[-1]) if remove else value[parts[-1]]


def policy_run(run, declared, plan):
    lanes = validate_run(run, plan)
    require(gate.digest(declared.get("configSha256")) and run.get("configSha256") == declared["configSha256"] and
            hashlib.sha256(canonical(run["config"])).hexdigest() == declared.get("resolvedConfigSha256"),
            "Policy run configuration pin differs")
    require(run.get("calibrations", {}) == declared.get("calibrations", {}), "Policy calibration pins differ")
    config = copy.deepcopy(run["config"])
    require(set(declared["harnessBindings"]) == set(HARNESS_FIELDS), "Incomplete harness bindings")
    for field in HARNESS_FIELDS:
        require(field_at(config, field, remove=True) == declared["harnessBindings"][field],
                "Policy harness binding drift")
    require(set(declared["fields"]) == set(POLICY_FIELDS), "Policy must declare every allowed field")
    for field in POLICY_FIELDS:
        require(field_at(config["process"], field, remove=True) == declared["fields"][field],
                "Undeclared tracking-policy variation")
    fields = declared["fields"]
    m, n, delete = (fields[key] for key in ("tracker.initiate.M", "tracker.initiate.N", "tracker.delete"))
    require(all(type(value) is int for value in (m, n, delete)) and
            (m, n, delete) in ((2, 9, 7), (3, 17, 7), (4, 25, 7)), "Unreviewed M/N/delete cohort")
    tbd = "detection.track_before_detect."
    require(type(fields[tbd + "enable"]) is bool and fields[tbd + "max_cpis"] == 8 and
            type(fields[tbd + "fractional_delay"]) is bool and
            all(type(fields[tbd + key]) is int and 0 <= fields[tbd + key] <= maximum
                for key, maximum in (("motion_bins", 1), ("delay_motion_bins", 2), ("doppler_motion_bins", 16))),
            "Unreviewed depth/motion/fractional policy")
    depth = 8 if fields[tbd + "enable"] else 1
    detection = config["process"]["detection"]
    require(detection.get("enable") is True and detection.get("integration_cpis") == 1 and
            detection.get("integration_motion_bins") == 0 and config["process"]["tracker"].get("enable") is True,
            "Policy comparison requires enabled canonical detection/tracking and the depth-one control")
    require(not run["config"]["capture"]["replay"].get("inject_echo", {}).get("state") and
            "fault" not in run["config"]["capture"]["replay"]["qualification"], "Policy run contains injected evidence")
    product_rows, detector_rows = observations(run, "products"), observations(run, "detector")
    require(set(declared["planSha256ByLane"]) == set(lanes), "Policy lane plan pins differ")
    static, stripped = {}, {**run, "config": config}
    for lane, frames in lanes.items():
        previous = None
        width = frames[0]["capture_end_input_sample"] - frames[0]["capture_first_input_sample"]
        for index, frame in enumerate(frames):
            epoch, first, end = corpus.lineage_key(frame)
            require(all(integer(value) for value in (epoch, first, end)) and end-first == width and width > 0 and
                    (previous is None or (epoch, first) == previous), "Policy replay has a gap or epoch reset")
            previous = (epoch, end)
            key = (lane, epoch, first, end)
            observed, detector = product_rows.get(key), detector_rows.get(key)
            require(observed is not None and detector is not None, "Policy run lacks native observer evidence")
            require(corpus.resolved_plan_identity(run, observed) == declared["planSha256ByLane"][lane],
                    "Policy resolved-plan pin differs")
            native = json.loads(observed["resolved_product_plan"])
            require(all(native.get(key) == value for key, value in
                {"integrationCpis": depth, "trackerM": m, "trackerN": n, "trackerDelete": delete}.items()) and
                all(native.get("stages." + stage) is True for stage in ("detection", "tracking")),
                "Native policy contradicts declared configuration")
            require(integer(native.get("integrationMotion")) and (depth == 8 or native["integrationMotion"] == 0),
                    "Native depth-one control contains integration motion")
            require(observed["sampleRateHz"] * observed["inputSamplesPerOutput"] == run["acquisition"]["sample_rate_hz"],
                    "Policy observer uses a different acquisition sample domain")
            require(detector.get("enabled") is True and type(detector.get("ready")) is bool and
                    detector.get("nonfinite_cells") == 0, "Policy detector execution is missing or nonfinite")
            reference = copy.deepcopy(observed)
            for field in NATIVE_POLICY_FIELDS: native.pop(field)
            reference["resolved_product_plan"] = json.dumps(native, sort_keys=True)
            identity = corpus.resolved_plan_identity(stripped, reference)
            require(lane not in static or static[lane] == identity, "Policy physical/math geometry drifted within a run")
            static[lane] = identity
            backend = run["config"]["process"]["performance"]["acceleration"]
            require(backend in ("cpu", "gpu", "gpu-resident"), "Policy backend is not explicit")
            for stage, enabled in (("acceleration", True), ("clutterAcceleration", native["stages.clutter"])):
                execution = frame.get(stage, {})
                if not enabled:
                    require(not execution.get("gpuExecuted") and not execution.get("cpuExecuted"), "Disabled policy stage executed")
                    continue
                active = {"cpu": "cpu", "gpu": "vulkan", "gpu-resident": "vulkan-resident"}[backend]
                require(execution.get("active") == active and execution.get("gpuExecuted") is (backend != "cpu"),
                        "Policy run substituted its backend")
                if backend == "cpu": require(execution.get("cpuExecuted") is True, "Policy CPU work is absent")
                else:
                    require(execution.get("device") in (run["device"]["id"], run["device"]["name"]), "Policy GPU changed device")
                    if index >= 2 or index == len(frames)-1:
                        require(execution.get("cpuExecuted") is False and execution.get("state") == "ready", "Policy GPU fallback")
    return lanes, config, static, {"depth": depth, "M": m, "N": n, "delete": delete}


def policy_metrics(run, lanes, labels, policy, gates):
    """Join frozen public products to externally pinned labels; never feed DSP."""
    rate = run["acquisition"]["sample_rate_hz"]
    require(type(rate) is int and rate > 0 and all(gate.finite(gates.get(key)) and gates[key] > 0
            for key in ("delayKm", "dopplerHz")), "Invalid policy exposure or matching gates")
    require(labels.get("schema") == "vectorwarp-policy-labels-v1" and
            labels.get("recordingSha256") == run["acquisition"]["recording_sha256"] and
            labels.get("acquisitionSha256") == run["acquisition"]["acquisition_sha256"] and
            labels.get("captureEpoch") == int(run["acquisition"]["epoch"]), "Policy labels bind another acquisition")
    truths = {}
    for row in labels["frames"]:
        key = (row["lane"], labels["captureEpoch"], row["firstSample"], row["endSample"])
        require(key not in truths and type(row.get("exposureValid")) is bool and
                type(row.get("truthEligible")) is bool and isinstance(row.get("targets"), list), "Duplicate/invalid label interval")
        require(row["truthEligible"] or not row["targets"], "Ineligible CPI contains evaluation targets")
        truths[key] = row
    expected = {(lane, *corpus.lineage_key(frame)) for lane, frames in lanes.items() for frame in frames}
    require(set(truths) == expected, "Policy labels omit/add published CPI exposure")
    passes = {(item["lane"], item["id"]): {**item, "eligibleCpis": 0, "detectionCpis": 0,
        "activeCpis": 0, "coastedCpis": 0, "operationalCpis": 0, "windowCpis": 0, "admittedCpis": 0, "trackIds": set(), "activeEndpoints": [],
        "detectedEndpoints": [], "covered": []} for item in labels["passes"]}
    require(len(passes) == len(labels["passes"]) and all(item["lane"] in lanes and
        integer(item["entrySample"]) and integer(item["exitSample"]) and item["exitSample"] > item["entrySample"]
        and type(item.get("wholePass")) is bool for item in labels["passes"]), "Invalid/duplicate labelled pass")
    totals, lifecycle, product_rows = {}, [], observations(run, "products")
    for lane, frames in lanes.items():
        total = dict(admittedCpis=0, eligibleCpis=0, exposureSeconds=0., labelledExposureSeconds=0.,
            detectionCpis=0, activeCpis=0, coastedCpis=0, activeTrackSeconds=0., coastedTrackSeconds=0.,
            unmatchedActiveBirths=0, unmatchedActiveTrackSeconds=0., unmatchedCoastedTrackSeconds=0.,
            ambiguousMatches=0, leftCensoredActive=0, deleteBoundaryViolations=0,
            nativeInactivityDeleted=0, nativeTentativeExpired=0)
        previous, ever_confirmed, last_measured, coast, retired = {}, set(), {}, {}, set()
        for index, frame in enumerate(frames):
            epoch, first, end = corpus.lineage_key(frame); row = truths[(lane, epoch, first, end)]
            seconds = (end-first)/rate
            total["admittedCpis"] += row["exposureValid"]
            total["exposureSeconds"] += seconds if row["exposureValid"] else 0
            eligible = row["exposureValid"] and row["truthEligible"]
            total["eligibleCpis"] += eligible; total["labelledExposureSeconds"] += seconds if eligible else 0
            targets = row["targets"] if eligible else []
            require(len({target["passId"] for target in targets}) == len(targets), "Duplicate target/pass in CPI")
            for (pass_lane, identifier), item in passes.items():
                if pass_lane == lane and item["entrySample"] <= first < end <= item["exitSample"]:
                    item["windowCpis"] += 1
                    item["admittedCpis"] += row["exposureValid"]
                    require(not eligible or identifier in {target["passId"] for target in targets},
                            "Eligible truth omits a declared pass")
            for target in targets:
                require((lane, target["passId"]) in passes and all(gate.finite(target.get(key)) for key in ("delayKm", "dopplerHz")),
                        "Unknown/nonfinite truth target")
                item = passes[(lane, target["passId"])]
                require(item["entrySample"] <= first < end <= item["exitSample"], "Truth lies outside fixed pass window")
                item["eligibleCpis"] += 1
            def matched(delay, doppler):
                require(gate.finite(delay) and gate.finite(doppler), "Nonfinite public detection/track")
                hits = [target["passId"] for target in targets if abs(delay-target["delayKm"]) <= gates["delayKm"] and
                        abs(doppler-target["dopplerHz"]) <= gates["dopplerHz"]]
                total["ambiguousMatches"] += len(hits) > 1
                return hits[0] if len(hits) == 1 else None, bool(hits)
            detection = frame["detections"]
            require(len(detection["delay"]) == len(detection["doppler"]), "Policy detection arrays differ")
            detected = {matched(delay, doppler)[0] for delay, doppler in zip(detection["delay"], detection["doppler"])} - {None}
            total["detectionCpis"] += bool(detected)
            current, active, coasted = {}, set(), set()
            matched_ids = {target["passId"]: set() for target in targets}
            tracks = frame["tracks"]
            require(integer(tracks.get("n")) and integer(tracks.get("nTentative", 0)) and
                    tracks["n"] == len(tracks["data"]) + tracks.get("nTentative", 0),
                    "Policy public track population differs")
            for track in tracks["data"]:
                identifier, state = track["id"], track["state"]
                require(type(identifier) in (int, str) and identifier not in current and identifier not in retired and
                        state in ("ASSOCIATED", "TENTATIVE", "ACTIVE", "COASTING") and type(track.get("measured")) is bool,
                        "Policy track identity/state/measurement is invalid")
                require(state not in ("ACTIVE", "COASTING") or track["measured"] is (state == "ACTIVE"),
                        "Policy conflates coasting predictions with measured ACTIVE tracks")
                current[identifier] = state
                target, any_match = matched(track["delay"]*299792458.0/product_rows[(lane, epoch, first, end)]["sampleRateHz"]/1000,
                                            track["doppler"])
                if track["measured"]: last_measured[identifier] = index
                if state in ("ACTIVE", "COASTING"):
                    total["activeTrackSeconds" if state == "ACTIVE" else "coastedTrackSeconds"] += seconds if row["exposureValid"] else 0
                    if target is not None:
                        (active if state == "ACTIVE" else coasted).add(target)
                        passes[(lane, target)]["trackIds"].add(identifier)
                        matched_ids[target].add(identifier)
                    if eligible and not any_match:
                        total["unmatchedActiveTrackSeconds" if state == "ACTIVE" else "unmatchedCoastedTrackSeconds"] += seconds
                    if state == "ACTIVE" and identifier not in ever_confirmed:
                        total["leftCensoredActive"] += index == 0
                        total["unmatchedActiveBirths"] += eligible and not any_match and index > 0
                        lifecycle.append({"lane": lane, "id": identifier, "sample": end,
                            "event": "observed-active-birth", "leftCensored": index == 0, "matchedPass": target})
                    ever_confirmed.add(identifier)
                coast[identifier] = coast.get(identifier, 0) + 1 if state == "COASTING" else 0
            for identifier in previous.keys() - current.keys():
                retired.add(identifier)
                confirmed = identifier in ever_confirmed
                missed = index-last_measured[identifier] if identifier in last_measured else None
                boundary = missed == policy["delete"]+1 if confirmed and missed is not None else None
                total["deleteBoundaryViolations"] += boundary is False
                lifecycle.append({"lane": lane, "id": identifier, "sample": end, "event": "removed",
                    "confirmed": confirmed, "coastedCpis": coast.get(identifier, 0), "missedCpis": missed,
                    "configuredDeleteBoundary": boundary})
            diagnostics = frame.get("trackerDiagnostics", {})
            for field, output in (("inactivity_deleted", "nativeInactivityDeleted"), ("tentative_expired", "nativeTentativeExpired")):
                require(integer(diagnostics.get(field)), "Policy lifecycle diagnostics are missing")
                total[output] += diagnostics[field]
            total["activeCpis"] += bool(active); total["coastedCpis"] += bool(coasted)
            for target in targets:
                item = passes[(lane, target["passId"])]; identity = target["passId"]
                item["detectionCpis"] += identity in detected; item["activeCpis"] += identity in active
                item["coastedCpis"] += identity in coasted; item["operationalCpis"] += identity in active | coasted
                item["covered"].append((index, identity in active | coasted, matched_ids[identity]))
                if identity in active: item["activeEndpoints"].append(end)
                if identity in detected: item["detectedEndpoints"].append(end)
            previous = current
        for identifier, state in previous.items():
            lifecycle.append({"lane": lane, "id": identifier, "event": "right-censored", "state": state,
                              "coastedCpis": coast.get(identifier, 0)})
        denominator = total["labelledExposureSeconds"] / 3600
        for field in ("unmatchedActiveBirths", "unmatchedActiveTrackSeconds", "unmatchedCoastedTrackSeconds"):
            total[field + "PerLabelledHour"] = total[field]/denominator if denominator else None
        for field in ("detectionCpis", "activeCpis", "coastedCpis"):
            total[field + "Fraction"] = total[field]/total["eligibleCpis"] if total["eligibleCpis"] else None
        duration = total["activeTrackSeconds"] + total["coastedTrackSeconds"]
        total["coastedTrackFraction"] = total["coastedTrackSeconds"]/duration if duration else None
        totals[lane] = total
    for (lane, _), item in passes.items():
        eligible, covered = item["eligibleCpis"], item.pop("covered")
        active, detected = item.pop("activeEndpoints"), item.pop("detectedEndpoints")
        item["trackIds"] = sorted(item["trackIds"], key=str)
        item["additionalTrackIdentities"] = max(0, len(item["trackIds"])-1)
        item["splits"], item["maximumConcurrentTracks"], prior_ids = 0, 0, set()
        longest = current = consecutive = longest_eligible = gaps = 0
        previous_index, previous_value = -2, False
        for index, value, identifiers in covered:
            item["maximumConcurrentTracks"] = max(item["maximumConcurrentTracks"], len(identifiers))
            if identifiers:
                item["splits"] += bool(prior_ids) and not bool(prior_ids & identifiers)
                prior_ids = identifiers
            consecutive = consecutive+1 if index == previous_index+1 else 1
            longest_eligible = max(longest_eligible, consecutive)
            current = current+1 if value and previous_value and index == previous_index+1 else int(value)
            longest = max(longest, current)
            gaps += previous_value and (not value or index != previous_index+1)
            previous_index, previous_value = index, value
        item["coverageGaps"], item["longestCoveredCpis"] = gaps, longest
        item["unscorableCpis"] = item["windowCpis"]-eligible
        frames = lanes[lane]
        complete_pass = item["wholePass"] and frames[0]["capture_first_input_sample"] <= item["entrySample"] and \
            frames[-1]["capture_end_input_sample"] >= item["exitSample"] and not item["unscorableCpis"]
        item["fullDepthConfirmationCensored"] = longest_eligible < policy["depth"]*policy["M"] or not complete_pass
        item["firstActiveFromEntrySeconds"] = (active[0]-item["entrySample"])/rate if active else None
        item["firstActiveFromMeasuredEvidenceSeconds"] = (active[0]-detected[0])/rate if active and detected and active[0] >= detected[0] else None
        item["wholePassDetected"] = bool(item["detectionCpis"]) if complete_pass and eligible else None
        item["wholePassActive"] = bool(active) if complete_pass and not item["fullDepthConfirmationCensored"] else None
        item["confirmationOutcome"] = "observed" if active else "censored" if item["fullDepthConfirmationCensored"] else "not-observed"
        for field in ("detectionCpis", "activeCpis", "coastedCpis", "operationalCpis"):
            item[field + "Fraction"] = item[field]/eligible if eligible else None
    complete = [item for item in passes.values() if item["wholePassDetected"] is not None]
    confirmed = [item for item in passes.values() if item["wholePassActive"] is not None]
    return {"lanes": totals, "passes": list(passes.values()), "lifecycle": lifecycle,
        "passDetectionRate": sum(item["wholePassDetected"] for item in complete)/len(complete) if complete else None,
        "passActiveRate": sum(item["wholePassActive"] for item in confirmed)/len(confirmed) if confirmed else None,
        "unmatchedIsFalse": False, "falseTracks": None, "nativeTentativeIdentityObservable": False}


def produce_policy_comparison(plan, root, output, plan_sha256):
    require(plan.get("schema") == "vectorwarp-recorded-policy-comparison-plan-v1" and gate.digest(plan_sha256),
            "Unknown/unpinned policy comparison plan")
    cohort = pinned(plan["cohortManifest"], root)
    require(cohort.get("schema") == "vectorwarp-policy-cohort-v1" and cohort.get("reviewed") is True,
            "Policy comparison lacks a reviewed partition ledger")
    # Retain the independently pinned inventory/admission provenance used to
    # author this ledger; never infer a reserved partition from outcome files.
    require(bool(cohort.get("basis")), "Policy cohort has no pinned source inventories")
    basis = [pinned(reference, root) for reference in cohort["basis"]]
    records, partitions = {}, {}
    for entry in cohort["recordings"]:
        digest = entry["recordingSha256"]
        require(gate.digest(digest) and digest not in records and isinstance(entry.get("source"), str) and entry["source"] and
                isinstance(entry.get("passIds"), list) and len(entry["passIds"]) == len(set(entry["passIds"])) and
                type(entry.get("wholePass")) is bool,
                "Invalid/duplicate policy recording membership")
        records[digest] = entry
        for identity in entry["passIds"]:
            require(identity not in partitions or partitions[identity] == entry["partition"], "Pass leaks across corpus partitions")
            partitions[identity] = entry["partition"]
    require(plan.get("phase") in ("development", "holdout"), "Policy phase is not explicit")
    selection = audit = None
    if plan["phase"] == "holdout":
        selection, audit = pinned(plan["selectionReceipt"], root), pinned(plan["holdoutAudit"], root)
        require(selection.get("schema") == "vectorwarp-frozen-policy-selection-v1" and selection.get("frozen") is True and
                selection.get("sourceSha") == plan["sourceSha"] and selection.get("artifacts") == plan["artifacts"] and
                audit.get("schema") == "vectorwarp-holdout-exposure-audit-v1" and audit.get("untouched") is True and
                audit.get("cohortManifestSha256") == plan["cohortManifest"]["sha256"], "Uncertified or unfrozen holdout use")
    runs = {name: pinned(reference, root) for name, reference in plan["runs"].items()}
    require(set(runs) == set(plan["runPolicies"]), "Policy declarations omit/add runs")
    resolved = {name: policy_run(run, plan["runPolicies"][name], plan) for name, run in runs.items()}
    results, intervals, identities, used, retained_labels, anchors = [], {}, set(), set(), {}, {}
    for pair in plan["policyPairs"]:
        require(isinstance(pair.get("id"), str) and pair["id"] and pair["id"] not in identities, "Duplicate/missing policy pair")
        identities.add(pair["id"])
        left, right = (runs[pair[key]] for key in ("baseline", "candidate"))
        a, b = (resolved[pair[key]] for key in ("baseline", "candidate"))
        used.update((pair["baseline"], pair["candidate"]))
        require(left["acquisition"] == right["acquisition"] and a[1:3] == b[1:3] and
                left["deviceClass"] == right["deviceClass"] and left["device"] == right["device"] and
                left.get("calibrations", {}) == right.get("calibrations", {}), "Policy pair has non-policy drift")
        require(a[3]["depth"] == 1 and b[3]["depth"] == 8 and
                all(a[3][key] == b[3][key] for key in ("M", "N", "delete")), "Policy pair is not a matched depth1/depth8 cohort")
        require(set(a[0]) == set(b[0]) and all([corpus.lineage_key(frame) for frame in a[0][lane]] ==
                [corpus.lineage_key(frame) for frame in b[0][lane]] for lane in a[0]), "Policy pair changed sample exposure")
        member = records.get(left["acquisition"]["recording_sha256"])
        require(member is not None, "Recording is outside the frozen policy cohort")
        anchor_key = (member["source"], left["acquisition"]["recording_sha256"])
        anchor = (a[1:3], left["acquisition"], left["deviceClass"], left["device"], left.get("calibrations", {}))
        require(anchor_key not in anchors or anchors[anchor_key] == anchor, "Policy grid has cross-cohort non-policy drift")
        anchors[anchor_key] = anchor
        allowed = ("fit", "development_calibration", "development_only") if plan["phase"] == "development" else ("reserved_holdout",)
        require(member["partition"] in allowed, "Policy phase would expose a reserved/excluded partition")
        labels = pinned(pair["labels"], root)
        retained_labels[pair["id"]] = labels
        require(labels.get("source") == member["source"] and labels.get("partition") == member["partition"] and
                {item["id"] for item in labels["passes"]} == set(member["passIds"]) and
                all(item.get("wholePass") is member["wholePass"] for item in labels["passes"]) and
                labels.get("timingQualified") is True, "Labels drifted from partition/pass identity or lack qualified timing")
        if selection is not None:
            require(selection.get("policy") == b[3] and
                    selection.get("policyFields") == plan["runPolicies"][pair["candidate"]]["fields"] and
                    set(member["passIds"]) <= set(audit.get("passIds", [])),
                    "Holdout policy changed after selection or pass was not exposure-audited")
        for lane, frames in a[0].items():
            key = (member["source"], left["acquisition"]["recording_sha256"], lane, a[3]["M"])
            start, end = frames[0]["capture_first_input_sample"], frames[-1]["capture_end_input_sample"]
            require(all(end <= before or start >= after for before, after in intervals.get(key, [])),
                    "Policy comparison double-counts recording exposure")
            intervals.setdefault(key, []).append((start, end))
        results.append({"id": pair["id"], "source": member["source"], "partition": member["partition"],
            "policy": b[3], "recordingSha256": left["acquisition"]["recording_sha256"],
            "labelsSha256": pair["labels"]["sha256"], "baseline": policy_metrics(left, a[0], labels, a[3], plan["matchingGates"]),
            "candidate": policy_metrics(right, b[0], labels, b[3], plan["matchingGates"])})
    require(results and used == set(runs), "Policy comparison has no pairs or unmeasured runs")
    output.mkdir(); evidence = Evidence(output / "policy-evidence")
    report = {"schema": "vectorwarp-recorded-policy-comparison-v1", "passed": True,
        "releaseQualified": False, "policySelected": None, "planSha256": plan_sha256,
        "sourceSha": plan["sourceSha"], "artifacts": plan["artifacts"], "phase": plan["phase"],
        "cohortManifestSha256": plan["cohortManifest"]["sha256"], "pairs": results,
        "runEvidenceSha256": {name: evidence.put(run) for name, run in runs.items()},
        "labelEvidenceSha256": {name: evidence.put(labels) for name, labels in retained_labels.items()},
        "evidenceSha256": evidence.put({"plan": plan, "cohort": cohort, "basis": basis,
            "selection": selection, "holdoutAudit": audit}),
        "limits": ["Unmatched ACTIVE first-publications are not adjudicated false tracks.",
            "Native tentative identities are not published; first-publication is not native birth.",
            "Short or incomplete pass exposure is censored, not a detection or confirmation miss.",
            "Sources, partitions and policies remain separate; no automatic selection or release qualification."]}
    (output / "policy-comparison.json").write_bytes(canonical(report))
    return report


def negative_window(candidate, baseline, window, adjudication, plan):
    require(adjudication.get("schema") == "vectorwarp-recorded-negative-adjudication-v1" and
            adjudication.get("reviewed") is True and adjudication.get("coverageComplete") is True and
            adjudication.get("wholePassDisjoint") is True and adjudication.get("noTargets") is True,
            "Negative window is not coverage-complete adjudicated whole-pass evidence")
    require(candidate["acquisition"] == baseline["acquisition"] and
            process_contract(candidate) == process_contract(baseline), "Negative baseline changes recording or radar settings")
    acquisition = candidate["acquisition"]
    identity = {"recordingSha256": acquisition["recording_sha256"],
                "acquisitionSha256": acquisition["acquisition_sha256"],
                "captureEpoch": int(acquisition["epoch"]), "firstSample": window["firstSample"],
                "endSample": window["endSample"], "passId": window["passId"]}
    require(all(adjudication.get(key) == value for key, value in identity.items()), "Adjudication is for another exact window")
    actual, detector = select(candidate, validate_run(candidate, plan), window, True)
    control, _ = select(baseline, validate_run(baseline, plan), window, False)
    require(actual.keys() == control.keys(), "Negative baseline lane set differs")
    counts = [detector[(lane, *corpus.lineage_key(frame))] for lane, frames in actual.items() for frame in frames]
    return {**identity, "sampleRate": acquisition["sample_rate_hz"],
            "evaluatedCells": sum(item["evaluated_cells"] for item in counts),
            "falseAlarmCells": sum(item["accepted_cells"] for item in counts),
            "falseTracks": len(operational_tracks(actual)), "baselineFalseTracks": len(operational_tracks(control))}


def echo_pair(control, injected, pair, plan):
    require(control["acquisition"] == injected["acquisition"] and control["deviceClass"] == injected["deviceClass"] and
            process_contract(control) == process_contract(injected), "Echo pair changes acquisition/device/radar settings")
    unmodified = control["config"]["capture"]["replay"].get("inject_echo", {})
    stimulus = injected["config"]["capture"]["replay"].get("inject_echo", {})
    target = pair["target"]
    declared_checkpoint = control["config"]["capture"]["replay"].get("qualification", {}).get("clutter_checkpoint")
    checkpoint_evidence = clutter_checkpoint.validate_pair(control, injected, pair, plan["sourceSha"]) if declared_checkpoint else {}
    require((bool(checkpoint_evidence) or unmodified.get("state") is not True) and stimulus.get("state") is True and
            stimulus.get("delay_km") == target["delayKm"] and stimulus.get("doppler_hz") == target["dopplerHz"],
            "Echo controls do not match the predeclared production injection")
    require(all(gate.finite(target.get(key)) for key in
                ("delayKm", "dopplerHz", "delayToleranceKm", "dopplerToleranceHz")) and target["dopplerHz"] != 0 and
            target["delayToleranceKm"] > 0 and target["dopplerToleranceHz"] > 0, "Invalid predeclared echo matching gate")
    left, _ = select(control, validate_run(control, plan), pair, True)
    right, _ = select(injected, validate_run(injected, plan), pair, True)
    require(left.keys() == right.keys() and target["lane"] in right, "Echo lane set differs")
    a, b = observations(control, "clutter"), observations(injected, "clutter")
    arrays = {name: [] for name in ("unfilteredControl", "unfilteredInjected", "filteredControl", "filteredInjected")}
    matched_detections, matched_tracks, policies = 0, set(), set()
    def matches(delay, doppler):
        return (gate.finite(delay) and gate.finite(doppler) and
                abs(delay-target["delayKm"]) <= target["delayToleranceKm"] and
                abs(doppler-target["dopplerHz"]) <= target["dopplerToleranceHz"])
    lane = target["lane"]
    for frame in right[lane]:
        key = (lane, *corpus.lineage_key(frame))
        require(key in a and key in b, "Missing paired production pre/post-clutter observation")
        before, after = a[key], b[key]
        expected_policy = ("fixed-weights" if declared_checkpoint and declared_checkpoint["mode"] == "fixed" else "refit-weights")
        require(before["filter_policy"] == after["filter_policy"] == expected_policy,
                "Fixed weights require a supported native frozen-checkpoint control")
        policies.add(after["filter_policy"])
        require(all(row.get("gpu_executed") is True and row.get("cpu_executed") is False
                    for row in (before, after)), "Clutter observation includes CPU qualification/fallback")
        require(all(before.get(field) == after.get(field) for field in
                    ("channel_ids", "reference_channel_ids", "delay_min", "delay_max_exclusive", "relative_diagonal_loading")),
                "Echo pair changes clutter/reference topology")
        for name, row, field in (("unfilteredControl", before, "unfiltered_surveillance"),
                                 ("unfilteredInjected", after, "unfiltered_surveillance"),
                                 ("filteredControl", before, "filtered_surveillance"),
                                 ("filteredInjected", after, "filtered_surveillance")):
            for channel in row[field]:
                arrays[name].extend(channel)
        detections = frame["detections"]
        require(len(detections["delay"]) == len(detections["doppler"]), "Detection publication shape differs")
        matched_detections += sum(matches(delay, doppler) for delay, doppler in zip(detections["delay"], detections["doppler"]))
        rate = after["sampleRateHz"]
        for track in frame["tracks"].get("data", []):
            if track.get("state") in ("ACTIVE", "COASTING") and matches(track["delay"]*299792458.0/rate/1000, track["doppler"]):
                matched_tracks.add((frame["capture_epoch"], track["id"]))
    before = gate.differential_power(arrays["unfilteredControl"], arrays["unfilteredInjected"])
    after = gate.differential_power(arrays["filteredControl"], arrays["filteredInjected"])
    require(before > 0 and matched_detections > 0 and bool(matched_tracks), "Injected echo lacks measurable power, detections or operational tracks")
    acquisition = injected["acquisition"]
    return {"sourceKind": "recorded-iq", "exactAcquisition": True, "gpuExecuted": True,
            "recordingSha256": acquisition["recording_sha256"], "acquisitionSha256": acquisition["acquisition_sha256"],
            "configSha256": hashlib.sha256(canonical(injected["config"])).hexdigest(),
            "deviceClass": injected["deviceClass"], "filterPolicy": policies.pop(), "dopplerHz": target["dopplerHz"],
            **arrays, "matchedDetections": matched_detections, "matchedOperationalTracks": len(matched_tracks),
            "echoRetention": after/before, **checkpoint_evidence}


def produce(plan, root, output, policy_bytes, *, golden_only=False):
    require(plan.get("schema") == "vectorwarp-recorded-iq-qualification-plan-v1", "Unknown authorized corpus plan")
    policy = json.loads(policy_bytes)
    require(plan.get("policySha256") == hashlib.sha256(policy_bytes).hexdigest(), "Plan did not pin the current policy before measurement")
    output.mkdir()  # Never replace or mix an earlier qualification epoch.
    evidence = Evidence(output / "pre-soak-evidence")
    runs = {name: pinned(reference, root) for name, reference in plan["runs"].items()}
    golden = []
    for pair in plan.get("goldenPairs", []):
        pair = copy.deepcopy(pair)
        for feature in plan.get("featureManifest", {}).values():
            if pair["id"] not in feature["pairs"]: continue
            require(feature.get("requiredPlanFields") or feature.get("requiredProcessFields") or
                    feature.get("requiredTrackStates") or feature.get("minimumCounts"),
                    "Feature manifest must declare observable acceptance")
            for key in ("requiredPlanFields", "requiredProcessFields", "minimumCounts"):
                for field, value in feature.get(key, {}).items():
                    if key != "minimumCounts":
                        require(field not in pair.get(key, {}) or pair[key][field] == value,
                                "Conflicting predeclared math feature requirements")
                    pair.setdefault(key, {})[field] = max(pair.get(key, {}).get(field, 0), value) if key == "minimumCounts" else value
            pair["requiredTrackStates"] = sorted(set(pair.get("requiredTrackStates", [])) |
                                                  set(feature.get("requiredTrackStates", [])))
        measured = golden_pair(runs[pair["baseline"]], runs[pair["candidate"]], pair, plan)
        measured["id"] = pair["id"]
        measured["evidenceSha256"] = evidence.put({"pair": pair, "baseline": runs[pair["baseline"]],
            "candidate": runs[pair["candidate"]]})
        golden.append(measured)
    if golden_only:
        require(bool(golden) and len({item["id"] for item in golden}) == len(golden), "Golden pairs missing or duplicated")
        manifest = plan.get("featureManifest", {})
        require(manifest and all(isinstance(feature.get("pairs"), list) and feature["pairs"] and
                set(feature["pairs"]) <= {item["id"] for item in golden}
                for feature in manifest.values()), "Golden feature manifest has unmeasured coverage")
        report = {"schema": "vectorwarp-recorded-golden-parity-v1", "passed": True,
            "releaseQualified": False, "sourceSha": plan["sourceSha"], "artifacts": plan["artifacts"],
            "featureManifest": manifest, "pairs": golden}
        (output / "recorded-golden-parity.json").write_bytes(canonical(report))
        return report
    receipt = {"schema": "vectorwarp-pre-soak-radar-evidence-v1", "sourceSha": plan["sourceSha"],
               "artifacts": plan["artifacts"], "policySha256": plan["policySha256"], "passed": True,
               "syntheticOnly": False, "measurementContract": "paired-recorded-iq-and-adjudicated-whole-pass-negatives-v1",
               "echoMeasurements": [], "negativeMeasurements": []}
    for pair in plan.get("echoPairs", []):
        control, injected = runs[pair["control"]], runs[pair["injected"]]
        item = echo_pair(control, injected, pair, plan)
        item["executionSha256"] = evidence.put({"control": control, "injected": injected, "predeclaredPair": pair})
        item["productsSha256"] = evidence.put(injected["products"])
        receipt["echoMeasurements"].append(evidence.put(item))
    grouped, seen_passes, seen_intervals = {}, set(), set()
    for window in plan.get("negativeWindows", []):
        candidate, baseline = runs[window["candidate"]], runs[window["baseline"]]
        kind = candidate["deviceClass"]
        adjudication = pinned(window["adjudication"], root)
        measured = negative_window(candidate, baseline, window, adjudication, plan)
        identity = (kind, measured["recordingSha256"], measured["captureEpoch"])
        require((kind, measured["passId"]) not in seen_passes and identity not in seen_intervals,
                "Recorded-negative whole pass or recording epoch reused")
        seen_passes.add((kind, measured["passId"])); seen_intervals.add(identity)
        group = grouped.setdefault(kind, {"windows": [], "runs": [], "adjudications": []})
        group["windows"].append(measured);group["runs"].append({"candidate": candidate, "baseline": baseline})
        group["adjudications"].append(adjudication)
    for kind, group in sorted(grouped.items()):
        analysis = pinned(plan["pfaAnalyses"][kind], root)
        # The analysis references retained independent/block-model evidence;
        # import it by its external pin instead of accepting a self-hashed claim.
        independence = pinned(analysis.pop("independenceEvidence"), root)
        analysis["independenceEvidenceSha256"] = evidence.put(independence)
        item = {"deviceClass": kind, "sourceKind": "adjudicated-recorded-negative", "exactAcquisition": True,
                "wholePassDisjoint": True, "truthUsedForInference": False, "gpuExecuted": True,
                "windows": group["windows"], "adjudicationSha256": evidence.put({"windows": group["adjudications"]}),
                "executionSha256": evidence.put({"runs": group["runs"]}),
                "productsSha256": evidence.put({"runs": [entry["candidate"]["products"] for entry in group["runs"]]}),
                "pfaAnalysisSha256": evidence.put(analysis)}
        receipt["negativeMeasurements"].append(evidence.put(item))
    report = {"schema": "vectorwarp-measured-pre-soak-report-v1", "releaseQualified": False,
              "policy": policy, "candidateEvidence": receipt, "blockingReasons": []}
    try:
        gate.validate(receipt, plan["sourceSha"], plan["artifacts"], policy, plan["policySha256"], evidence.values.__getitem__)
    except (AssertionError, KeyError, ValueError) as error:
        report["blockingReasons"].append(str(error))
    else:
        report["releaseQualified"] = True
        (output / "pre-soak-radar-evidence.json").write_bytes(canonical(receipt))
    (output / "measured-pre-soak-report.json").write_bytes(canonical(report))
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--plan", type=Path, required=True)
    parser.add_argument("--plan-sha256", required=True, help="independently authorized plan digest")
    parser.add_argument("--output", type=Path, required=True, help="new evidence directory")
    parser.add_argument("--golden-only", action="store_true", help="paired parity only; never release qualification")
    parser.add_argument("--policy-comparison", action="store_true", help="score declared depth1/depth8 policies; never select or qualify a release")
    parser.add_argument("--capture", help="collect one named predeclared captureRuns entry into a new JSON file")
    args = parser.parse_args()
    require(sum(bool(value) for value in (args.capture, args.golden_only, args.policy_comparison)) <= 1,
            "Capture, golden parity and policy comparison are separate modes")
    plan = pinned({"path": args.plan.name, "sha256": args.plan_sha256}, args.plan.parent)
    if args.policy_comparison:
        report = produce_policy_comparison(plan, args.plan.parent, args.output, args.plan_sha256)
        print(json.dumps({"passed": report["passed"], "releaseQualified": False, "pairs": len(report["pairs"])}))
        return 0
    if args.capture:
        print(json.dumps(capture_run(plan, args.capture, args.plan.parent, args.output)))
        return 0
    report = produce(plan, args.plan.parent, args.output, gate.POLICY_PATH.read_bytes(), golden_only=args.golden_only)
    if args.golden_only:
        print(json.dumps({"passed": report["passed"], "releaseQualified": False, "pairs": len(report["pairs"])}))
        return 0
    print(json.dumps({"releaseQualified": report["releaseQualified"], "blockingReasons": report["blockingReasons"]}))
    return 0 if report["releaseQualified"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
