#!/usr/bin/python3
"""Bounded, loss-aware acceptance ledger for live VectorWarp nodes."""

from __future__ import annotations

import json
import hashlib
import fcntl
import math
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import time
import urllib.request
from urllib.parse import urlsplit
import shlex


DEFAULT_STATE = Path("/var/lib/home-portal/vectorwarp-two-node-soak")
MAX_SAMPLES = 1600
MAX_FRAME_AGE_MS = 3000
MAX_BACKLOG_MS = 1000
DEFAULT_CLEAN_WINDOW_SECONDS = 10 * 60
DEFAULT_MAX_SAMPLE_GAP_SECONDS = 180
IDENTITY_PROBE = Path(__file__).with_name("vectorwarp_soak_identity.py")
SHA256 = re.compile(r"^[0-9a-f]{64}$")
REGRESSION_CHECKS = ("detections", "measurement-backed-track-continuity",
                     "false-track-controls", "cpi-deadlines", "capture-health")
REGRESSION_RECEIPT_SCHEMA = 3
WHOLE_PASS_MODE = "frozen-baseline-prospective-operational-v1"
FUNCTIONAL_MODE = "bounded-functional-v1"


def acceptance_mode(binding):
    """Default claims never silently acquire a functional-only exception."""
    mode = binding.get("evaluationMode", WHOLE_PASS_MODE)
    if mode not in (WHOLE_PASS_MODE, FUNCTIONAL_MODE):
        raise ValueError("unknown release acceptance mode")
    if mode == FUNCTIONAL_MODE:
        if (not isinstance(binding.get("functionalAuthorizationSha256"), str) or
                not SHA256.fullmatch(binding["functionalAuthorizationSha256"]) or
                not isinstance(binding.get("releaseVersion"), str) or
                not re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+", binding["releaseVersion"]) or
                any(not counter(binding.get(key)) or binding[key] == 0
                    for key in ("candidateRunId", "candidateRunAttempt"))):
            raise ValueError("functional acceptance requires exact one-release authorization")
    elif "functionalAuthorizationSha256" in binding:
        raise ValueError("functional authorization cannot substitute for whole-pass acceptance")
    return mode


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError("duplicate binding key")
        result[key] = value
    return result


def load_binding(filename: str) -> dict:
    """An operator-reviewed binding is mandatory for release acceptance."""
    raw = Path(filename).read_bytes()
    if len(raw) > 32768:
        raise ValueError("oversized release binding")
    binding = json.loads(raw, object_pairs_hook=unique_object)
    if not isinstance(binding, dict):
        raise ValueError("release binding must be an object")
    acceptance_mode(binding)
    nodes = binding.get("nodes")
    if binding.get("schema") != 3 or not isinstance(nodes, dict) or not nodes:
        raise ValueError("release requires schema 3 reviewed node bindings; start a fresh cohort")
    if not counter(binding.get("monitorMinimumFreeBytes")) or binding["monitorMinimumFreeBytes"] == 0:
        raise ValueError("release binding requires a positive monitor disk reserve")
    if not isinstance(binding.get("sourceCommit"), str) or not re.fullmatch(r"[0-9a-f]{40}", binding["sourceCommit"]):
        raise ValueError("release binding requires a full source commit")
    if not isinstance(binding.get("regressionPolicySha256"), str) or not SHA256.fullmatch(binding["regressionPolicySha256"]):
        raise ValueError("release binding requires the reviewed regression policy digest")
    for name, node in nodes.items():
        if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_.-]{0,63}", name) or not isinstance(node, dict):
            raise ValueError("invalid node binding")
        if not isinstance(node.get("endpoint"), str):
            raise ValueError("node endpoint must be an HTTP origin")
        endpoint = urlsplit(node["endpoint"])
        if (endpoint.scheme not in ("http", "https") or not endpoint.hostname or
                endpoint.username or endpoint.password or endpoint.query or endpoint.fragment or
                endpoint.path not in ("", "/")):
            raise ValueError("node endpoint must be an HTTP origin")
        destination = node.get("ssh")
        if "ssh" not in node or (destination is not None and (not isinstance(destination, str) or
                not re.fullmatch(r"[A-Za-z0-9_.@:\[\]-]+", destination) or destination.startswith("-"))):
            raise ValueError("node requires an explicit local/null or safe SSH identity probe")
        disk = node.get("disk", {})
        if (not isinstance(disk, dict) or not isinstance(disk.get("path"), str) or
                not disk["path"].startswith("/") or "\x00" in disk["path"] or
                not counter(disk.get("minimumFreeBytes")) or disk["minimumFreeBytes"] == 0):
            raise ValueError("node requires a reviewed disk path and positive reserve")
        qualifications = node.get("qualifications", {})
        if (not isinstance(qualifications, dict) or "radar-behavior" not in qualifications or
                any(not re.fullmatch(r"[a-z][a-z0-9-]*", key) or not isinstance(value, str) or
                    not SHA256.fullmatch(value) for key, value in qualifications.items())):
            raise ValueError("node requires exact reviewed qualification evidence")
        if not finite_number(node.get("maximumCadenceMs"), 1) or node["maximumCadenceMs"] > 600:
            raise ValueError("release binding requires a reviewed cadence limit of at most 600 ms")
        for key in ("acceleration", "clutterAcceleration"):
            if node.get(key) not in ("cpu", "vulkan", "vulkan-resident"):
                raise ValueError(f"release binding requires an explicit {key} backend")
        if any(node[key] != "cpu" for key in ("acceleration", "clutterAcceleration")) and \
                "physical-gpu" not in qualifications:
            raise ValueError("GPU stages require physical GPU qualification")
        for key in ("artifactSha256", "configSha256", "hostIdSha256", "packageSha256"):
            if not isinstance(node.get(key), str) or not SHA256.fullmatch(node[key]):
                raise ValueError(f"release binding requires {key}")
        if not isinstance(node.get("release"), str) or not node["release"].startswith("/"):
            raise ValueError("release binding requires the resolved artifact path")
        if not isinstance(node.get("packageFilename"), str) or not re.fullmatch(r"vectorwarp[-_][A-Za-z0-9_.+-]+\.(deb|rpm)", node["packageFilename"]):
            raise ValueError("release binding requires the exact candidate package filename")
        executables = node.get("runtimeExecutables", {})
        if not isinstance(executables, dict) or set(executables) != {"vectorwarp-api.service", "vectorwarp-processor.service"}:
            raise ValueError("release binding requires both running executables")
        for executable in executables.values():
            if (not isinstance(executable, dict) or not isinstance(executable.get("sha256"), str) or
                    not SHA256.fullmatch(executable["sha256"]) or not isinstance(executable.get("path"), str) or
                    not executable["path"].startswith("/")):
                raise ValueError("invalid runtime executable binding")
        modules = node.get("runtimeModules", {})
        if not isinstance(modules, dict) or set(modules) != set(executables):
            raise ValueError("release binding requires loaded modules for both services")
        for group in modules.values():
            if not isinstance(group, dict) or any(
                    not str(path).startswith(node["release"] + "/bin/") or
                    not SHA256.fullmatch(digest) for path, digest in group.items()):
                raise ValueError("invalid loaded module binding")
    if len({node["hostIdSha256"] for node in nodes.values()}) != len(nodes):
        raise ValueError("release binding requires distinct measured hosts")
    return binding


def measured_process(ssh_destination: str | None = None, disk_path: str = "/var/lib") -> dict:
    if ssh_destination is None:
        args, payload = [sys.executable, str(IDENTITY_PROBE), "--disk-path", disk_path], None
    else:
        if not re.fullmatch(r"[A-Za-z0-9_.@:\[\]-]+", ssh_destination) or ssh_destination.startswith("-"):
            raise ValueError("invalid identity SSH destination")
        args = ["ssh", "-o", "BatchMode=yes", "-o", "ConnectTimeout=5", "--",
                ssh_destination, "python3 - --disk-path " + shlex.quote(disk_path)]
        payload = IDENTITY_PROBE.read_text()
    result = subprocess.run(args, input=payload, capture_output=True, text=True,
                            timeout=45, check=True)
    value = json.loads(result.stdout)
    if not isinstance(value, dict):
        raise ValueError("identity probe did not return an object")
    return value


def verify_binding(name: str, status: dict, process: dict, binding: dict) -> list[str]:
    failures = []
    expected = binding["nodes"][name]
    if status.get("build", {}).get("commit") != binding["sourceCommit"]:
        failures.append(f"{name}:source-binding")
    for field in ("release", "artifactSha256", "configSha256", "hostIdSha256",
                  "runtimeExecutables", "runtimeModules"):
        if process.get(field) != expected[field]:
            failures.append(f"{name}:artifact-binding:{field}")
    if any(status.get(key) != expected["configSha256"]
           for key in ("loadedRevision", "configRevision")):
        failures.append(f"{name}:config-binding")
    disk = process.get("disk", {})
    if (disk.get("path") != expected["disk"]["path"] or
            not counter(disk.get("freeBytes")) or
            disk["freeBytes"] < expected["disk"]["minimumFreeBytes"]):
        failures.append(f"{name}:disk-reserve")
    return failures


def verify_regression(receipt: dict, binding_receipt: dict, now: float,
                      cohort_epoch: float) -> list[str]:
    """External evaluation owns pass selection/statistics; never invent a policy.

    ADS-B-derived metrics enter only this offline acceptance receipt, never the
    processor. The reviewed policy and evaluator evidence are cryptographically
    tied to every declared package/config/qualification and the observed cohort.
    """
    try:
        mode = acceptance_mode(binding_receipt["expected"])
    except ValueError:
        return ["soak:regression-evidence-invalid"]
    functional_claim = (isinstance(receipt, dict) and not any(key in receipt for key in
        ("functionalAuthorizationSha256", "passYieldNonRegression", "windowSeconds"))) if mode != FUNCTIONAL_MODE else (isinstance(receipt, dict) and
        receipt.get("functionalAuthorizationSha256") == binding_receipt["expected"]["functionalAuthorizationSha256"] and
        receipt.get("passYieldNonRegression") == "not-evaluated" and
        receipt.get("windowSeconds") == 600 and
        finite_number(receipt.get("evaluationStartEpoch")) and finite_number(receipt.get("evaluatedThroughEpoch")) and
        receipt["evaluatedThroughEpoch"] - receipt["evaluationStartEpoch"] >= 600)
    valid = (functional_claim and isinstance(receipt, dict) and receipt.get("schema") == REGRESSION_RECEIPT_SCHEMA and
             receipt.get("evaluationMode") == mode and
             receipt.get("bindingSha256") == binding_receipt["sha256"] and
             receipt.get("policySha256") == binding_receipt["expected"]["regressionPolicySha256"] and
             isinstance(receipt.get("nodes"), dict) and
             set(receipt["nodes"]) == set(binding_receipt["expected"]["nodes"]) and
             all(receipt["nodes"][name].get("qualifications") == node["qualifications"]
                 for name, node in binding_receipt["expected"]["nodes"].items()
                 if isinstance(receipt["nodes"][name], dict)) and
             all(isinstance(node, dict) and all(counter(node.get(key))
                 for key in ("retiredCpis", "retiredSamples")) for node in receipt["nodes"].values()) and
             all(isinstance(node.get("checks"), dict) and
                 set(node["checks"]) == set(REGRESSION_CHECKS) and
                 all(isinstance(check, dict) and type(check.get("accepted")) is bool and
                     check.get("status") in ("accepted", "rejected", "inconclusive") and
                     check["accepted"] == (check["status"] == "accepted") and
                     isinstance(check.get("evidenceSha256"), str) and
                     SHA256.fullmatch(check["evidenceSha256"])
                     for check in node["checks"].values())
                 for node in receipt["nodes"].values()) and
             isinstance(receipt.get("evidenceSha256"), str) and
             SHA256.fullmatch(receipt["evidenceSha256"]) and
             finite_number(receipt.get("evaluationStartEpoch")) and
             finite_number(receipt.get("evaluatedThroughEpoch")) and
             receipt["evaluationStartEpoch"] <= cohort_epoch and
             receipt["evaluationStartEpoch"] <= receipt["evaluatedThroughEpoch"] and
             0 <= now - receipt["evaluatedThroughEpoch"] <= DEFAULT_MAX_SAMPLE_GAP_SECONDS and
             type(receipt.get("regressionDetected")) is bool)
    if not valid:
        return ["soak:regression-evidence-invalid"]
    if any(node[key] for node in receipt["nodes"].values() for key in ("retiredCpis", "retiredSamples")):
        return ["soak:regression-retired-work"]
    statuses = [check["status"] for node in receipt["nodes"].values() for check in node["checks"].values()]
    if receipt["regressionDetected"] or "rejected" in statuses:
        return ["soak:regression-detected"]
    return ["soak:insufficient-exposure"] if "inconclusive" in statuses else []


def fetch(url: str) -> dict:
    request = urllib.request.Request(url, headers={"User-Agent": "VectorWarp-Soak/2"})
    with urllib.request.urlopen(request, timeout=5) as response:
        value = json.load(response)
    if not isinstance(value, dict):
        raise ValueError("response is not an object")
    return value


def command(args: list[str]) -> str:
    return subprocess.run(args, capture_output=True, text=True, timeout=8,
                          check=True).stdout.strip()


def local_process() -> dict:
    fields = command(["systemctl", "show", "vectorwarp-api", "vectorwarp-processor",
                      "--property=Id,ActiveState,MainPID,NRestarts,Result"])
    units = {}
    for block in fields.split("\n\n"):
        values = dict(line.split("=", 1) for line in block.splitlines() if "=" in line)
        if values.get("Id"):
            units[values["Id"]] = values
    return {"release": os.path.realpath("/opt/vectorwarp/current"), "units": units}


def remote_process() -> dict:
    # Never manufacture a remote release identity. The API build/config/server
    # identity below remains independently observed even without administrative SSH.
    return {"release": None, "units": {}}


def atomic(path: Path, value: dict) -> None:
    descriptor, temporary = tempfile.mkstemp(prefix=f".{path.name}.", dir=path.parent)
    try:
        with os.fdopen(descriptor, "w") as output:
            json.dump(value, output, sort_keys=True, indent=2)
            output.write("\n")
            output.flush()
            os.fsync(output.fileno())
        os.replace(temporary, path)
    finally:
        try:
            os.unlink(temporary)
        except FileNotFoundError:
            pass


def finite_number(value: object, minimum: float = 0) -> bool:
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        return False
    try:
        return math.isfinite(value) and value >= minimum
    except OverflowError:
        return False


def counter(value: object) -> bool:
    return isinstance(value, int) and not isinstance(value, bool) and 0 <= value <= 2**64 - 1


def exact_counter(value: object) -> str:
    # API JSON numbers have crossed JavaScript; only decimal strings retain
    # uint64 precision above its safe integer range.
    if isinstance(value, str) and re.fullmatch(r"0|[1-9][0-9]{0,19}", value):
        number = int(value)
    elif counter(value) and value <= 2**53 - 1:
        number = value
    else:
        raise ValueError("counter is missing or inexact")
    if not counter(number):
        raise ValueError("counter exceeds uint64")
    return str(number)


def receiver_continuity(status: dict) -> dict:
    """Retain ingress receipts by capability, independent of receiver identity."""
    health = status.get("processor", {}).get("receiverHealth") or {}
    if not isinstance(health, dict):
        raise ValueError("invalid receiver receipt")
    epochs, counters = {}, {}
    for section in ("deviceTime", "sampleTime", "gain", "pairing"):
        if section in health:
            epochs[section] = exact_counter(health[section].get("epoch"))
    for section, fields in {
        "recovery": ("discontinuities", "requalifications", "discardedBlocks",
                     "discardedSamplesPerChannel"),
        "pairing": ("discontinuities", "requalifications", "discardedSamples"),
    }.items():
        if section not in health:
            continue
        for field in fields:
            values = health[section].get(field)
            values = values if isinstance(values, list) else [values]
            if not values:
                raise ValueError("empty receiver counter")
            counters[f"{section}.{field}"] = [exact_counter(value) for value in values]
    return {"epochs": epochs, "counters": counters}


def numeric_list(value: object) -> list[int] | None:
    if not isinstance(value, list) or not value:
        return None
    try:
        return [int(exact_counter(item)) for item in value]
    except ValueError:
        return None


def counter_delta(current: list[int], previous: list[int]) -> list[int]:
    if len(current) != len(previous) or any(now < old for now, old in zip(current, previous)):
        raise ValueError("capture counter shape changed or regressed")
    return [now - old for now, old in zip(current, previous)]


def percentile(values: list[float], fraction: float) -> float | None:
    finite = sorted(value for value in values if math.isfinite(value))
    if not finite:
        return None
    return round(finite[min(len(finite) - 1, int((len(finite) - 1) * fraction))], 3)


def compact_status(status: dict) -> dict:
    recording = status.get("featureDiagnostics", {}).get("recording", {})
    return {
        "serverId": status.get("serverId"),
        "build": status.get("build"),
        "loadedRevision": status.get("loadedRevision"),
        "configRevision": status.get("configRevision"),
        "radar": status.get("radar"),
        "processorFresh": status.get("processorFresh"),
        "lastFrameAt": status.get("lastFrameAt"),
        "acceleration": status.get("acceleration"),
        "clutterAcceleration": status.get("clutterAcceleration"),
        "errors": status.get("errors"),
        "recording": recording,
    }


def compact_timing(timing: dict) -> dict:
    retained = {
        "timestamp", "nCpi", "uptime_s", "cpi", "capture_backlog_ms",
        "frame_age_ms", "frame_excess_age_ms", "inputDiscontinuity",
        "captureTerminal", "captureEpoch", "eventTime", "captureDroppedSamples",
        "captureBoundaryDiscardedSamples", "captureStopDiscardedSamples",
        "gpu_resident_ready", "output_radar_data", "multi_capture_prepare",
        "multi_channelizer_wall", "multi_illuminator_radar", "multi_serialize",
        "multi_output", "multi_pipeline_advance", "processingCycle",
    }
    return {key: timing.get(key) for key in retained if key in timing}


def node_identity(status: dict, process: dict) -> dict:
    return {
        "serverId": status.get("serverId"),
        "build": status.get("build"),
        "loadedRevision": status.get("loadedRevision"),
        "configRevision": status.get("configRevision"),
        "release": process.get("release"),
        "bootId": process.get("bootId"),
        "artifactSha256": process.get("artifactSha256"),
        "hostIdSha256": process.get("hostIdSha256"),
        "runtimeExecutables": process.get("runtimeExecutables"),
        "runtimeModules": process.get("runtimeModules"),
        "runtimeChildren": process.get("runtimeChildren"),
        "units": {name: {key: values.get(key) for key in
                         ("MainPID", "NRestarts", "ExecMainStartTimestampMonotonic")}
                  for name, values in process.get("units", {}).items()},
    }


def evaluate(name: str, status: dict, timing: dict, process: dict,
             previous: dict | None, acceleration: str,
             clutter: str | None = None, maximum_cadence_ms: float = 600) -> tuple[list[str], dict]:
    failures = []
    now_ms = time.time() * 1000
    frame = status.get("lastFrameAt")
    age = now_ms - frame if finite_number(frame) else math.inf
    if status.get("processorFresh") is not True or not 0 <= age <= MAX_FRAME_AGE_MS:
        failures.append(f"{name}:stale-frame")
    timing_age = timing.get("frame_age_ms")
    if not finite_number(timing_age) or timing_age > MAX_FRAME_AGE_MS:
        failures.append(f"{name}:stale-timing")
    if status.get("errors"):
        failures.append(f"{name}:api-errors")
    if status.get("radar") != "receiving":
        failures.append(f"{name}:radar-not-receiving")
    if status.get("acceleration", {}).get("active") != acceleration:
        failures.append(f"{name}:acceleration")
    if clutter is not None and status.get("clutterAcceleration", {}).get("active") != clutter:
        failures.append(f"{name}:clutter-acceleration")
    recording = status.get("featureDiagnostics", {}).get("recording", {})
    if recording.get("active") or recording.get("requested") or recording.get("errorPresent"):
        failures.append(f"{name}:recording")
    for unit, values in process.get("units", {}).items():
        if values.get("ActiveState") != "active" or values.get("Result") not in ("success", ""):
            failures.append(f"{name}:{unit}:state")
    backlog = timing.get("capture_backlog_ms")
    if not finite_number(backlog) or backlog > MAX_BACKLOG_MS:
        failures.append(f"{name}:capture-backlog")
    if timing.get("inputDiscontinuity") is True or timing.get("captureTerminal") is True:
        failures.append(f"{name}:capture-discontinuity")
    capture_epoch, continuity = None, None
    try:
        capture_epoch = exact_counter(timing.get("eventTime", {}).get(
            "epoch", timing.get("captureEpoch")))
    except (ValueError, TypeError, AttributeError):
        failures.append(f"{name}:capture-epoch-unverifiable")
    try:
        continuity = receiver_continuity(status)
    except (ValueError, TypeError, AttributeError):
        failures.append(f"{name}:capture-receiver-unverifiable")

    counters = {
        "drops": numeric_list(timing.get("captureDroppedSamples")),
        "boundary": numeric_list(timing.get("captureBoundaryDiscardedSamples")),
        "stop": numeric_list(timing.get("captureStopDiscardedSamples")),
    }
    if any(values is None for values in counters.values()) or len({
            len(values) for values in counters.values() if values is not None}) != 1:
        failures.append(f"{name}:invalid-capture-counters")
    new_cpi = timing.get("nCpi")
    if not counter(new_cpi):
        failures.append(f"{name}:invalid-cpi-counter")
    identity = node_identity(status, process)
    advancement = None
    cadence_ms = None
    restarted = False
    if previous:
        restarted = previous.get("identity") != identity
        if restarted:
            failures.append(f"{name}:process-restarted")
        else:
            try:
                if exact_counter(previous.get("captureEpoch")) != capture_epoch:
                    failures.append(f"{name}:capture-epoch-reset")
            except ValueError:
                failures.append(f"{name}:capture-epoch-unverifiable")
            prior_continuity = previous.get("receiverContinuity")
            if continuity is None or not isinstance(prior_continuity, dict) or any(
                    not isinstance(prior_continuity.get(key), dict) for key in ("epochs", "counters")):
                failures.append(f"{name}:capture-receiver-unverifiable")
            elif (continuity["epochs"].keys() != prior_continuity.get("epochs", {}).keys() or
                  continuity["counters"].keys() != prior_continuity.get("counters", {}).keys()):
                failures.append(f"{name}:capture-receiver-unverifiable")
            else:
                if continuity["epochs"] != prior_continuity["epochs"]:
                    failures.append(f"{name}:capture-epoch-reset")
                for key, values in continuity["counters"].items():
                    try:
                        delta = counter_delta([int(value) for value in values],
                            [int(exact_counter(value)) for value in prior_continuity["counters"][key]])
                    except (ValueError, TypeError):
                        failures.append(f"{name}:capture-receiver-counter-reset")
                        continue
                    if any(delta):
                        failures.append(f"{name}:capture-receiver-loss")
            old_cpi = previous.get("nCpi")
            elapsed = time.time() - float(previous.get("epoch", time.time()))
            if counter(old_cpi) and counter(new_cpi):
                advancement = new_cpi - old_cpi
                if advancement <= 0:
                    failures.append(f"{name}:cpi-not-advancing")
                    if advancement < 0:
                        failures.append(f"{name}:capture-cpi-counter-reset")
                elif elapsed > 0:
                    cadence_ms = 1000 * elapsed / advancement
                    if cadence_ms > maximum_cadence_ms:
                        failures.append(f"{name}:cadence")
                if elapsed <= 0:
                    failures.append(f"{name}:invalid-observation-clock")
            elif not counter(old_cpi):
                failures.append(f"{name}:invalid-previous-cpi-counter")
            for kind, current in counters.items():
                old = numeric_list(previous.get("counters", {}).get(kind))
                if current is None or old is None:
                    failures.append(f"{name}:capture-{kind}-unverifiable")
                    continue
                try:
                    delta = counter_delta(current, old)
                except ValueError:
                    failures.append(f"{name}:capture-{kind}-reset")
                    continue
                if any(delta):
                    failures.append(f"{name}:capture-{kind}")
    receiver_loss = continuity and any(int(value) for values in
        continuity["counters"].values() for value in values)
    if (not previous or restarted) and (receiver_loss or
            any(any(values or []) for values in counters.values())):
        failures.append(f"{name}:preexisting-capture-loss")

    observation = {
        "epoch": time.time(), "identity": identity, "nCpi": timing.get("nCpi"),
        "captureEpoch": capture_epoch,
        "receiverContinuity": continuity,
        "counters": {kind: [str(value) for value in values] if values is not None else None
                     for kind, values in counters.items()}, "advancement": advancement,
        "cadenceMs": round(cadence_ms, 3) if cadence_ms is not None else None,
        "restarted": restarted,
    }
    return list(dict.fromkeys(failures)), observation


def advance_acceptance(started: dict, sample: dict,
                       clean_window_seconds: float = DEFAULT_CLEAN_WINDOW_SECONDS,
                       max_sample_gap_seconds: float = DEFAULT_MAX_SAMPLE_GAP_SECONDS) -> None:
    """Persist bounded cohort evidence separately from the rolling sample ledger.

    Startup failures are sticky. A later clean window can qualify steady state,
    but never erases startup loss. Missing observations break that clean window.
    """
    if not finite_number(clean_window_seconds, 1) or not finite_number(max_sample_gap_seconds, 1):
        raise ValueError("soak acceptance durations must be finite and at least one second")
    policy = {"cleanWindowSeconds": clean_window_seconds,
              "maximumSampleGapSeconds": max_sample_gap_seconds}
    prior = started.get("acceptance")
    epoch = sample.get("epoch")
    if not finite_number(epoch):
        raise ValueError("soak sample epoch is invalid")
    failures = sample["failures"]
    if prior and (epoch <= prior["lastSampleEpoch"] or
                  epoch - prior["lastSampleEpoch"] > max_sample_gap_seconds):
        failures.append("soak:observation-gap")
    sample["healthy"] = not failures
    startup_failures = (set(prior.get("startupFailures", [])) if prior else
                        set(failures) - {"soak:insufficient-exposure"})
    # A restart/loss/counter discontinuity anywhere in the cohort invalidates
    # its zero-loss startup claim, even after that row leaves the bounded log.
    startup_failures.update(failure for failure in failures if
                           "capture-" in failure or "-binding" in failure or "regression" in failure or
                           failure.endswith("process-restarted"))
    continuing = (prior and prior.get("policy") == policy and
                  prior.get("cleanSinceEpoch") is not None and sample["healthy"])
    clean_since = (prior["cleanSinceEpoch"] if continuing else epoch) if sample["healthy"] else None
    clean_samples = (prior["cleanSamples"] + 1 if continuing else 1) if sample["healthy"] else 0
    clean_seconds = epoch - clean_since if clean_since is not None else 0
    started["acceptance"] = {
        "policy": policy, "lastSampleEpoch": epoch,
        "startupFailures": sorted(startup_failures),
        "startupAccepted": not startup_failures,
        "cleanSinceEpoch": clean_since, "cleanSamples": clean_samples,
        "cleanSeconds": clean_seconds,
        "steadyStateAccepted": bool(sample["healthy"] and clean_samples >= 2 and
                                    clean_seconds >= clean_window_seconds),
    }


def summarize(lines: list[str], started: dict, sample: dict) -> dict:
    decoded = [json.loads(line) for line in lines]
    acceptance = started.get("acceptance", {})
    binding = started.get("releaseBinding")
    release_accepted = bool(binding and acceptance.get("startupAccepted") and
                            acceptance.get("steadyStateAccepted") and
                            acceptance.get("policy", {}).get("maximumSampleGapSeconds", math.inf) <=
                            DEFAULT_MAX_SAMPLE_GAP_SECONDS and
                            acceptance.get("policy", {}).get("cleanWindowSeconds", 0) >=
                            DEFAULT_CLEAN_WINDOW_SECONDS)
    performance = {}
    for name in (binding["expected"]["nodes"] if binding else sample["nodes"]):
        rows = [row.get("nodes", {}).get(name, {}) for row in decoded]
        cpis = [row.get("timing", {}).get("cpi") for row in rows]
        cadence = [row.get("observation", {}).get("cadenceMs") for row in rows]
        backlog = [row.get("timing", {}).get("capture_backlog_ms") for row in rows]
        performance[name] = {
            "cpiMsP50": percentile([x for x in cpis if isinstance(x, (int, float))], .5),
            "cpiMsP95": percentile([x for x in cpis if isinstance(x, (int, float))], .95),
            "cadenceMsP50": percentile([x for x in cadence if isinstance(x, (int, float))], .5),
            "cadenceMsP95": percentile([x for x in cadence if isinstance(x, (int, float))], .95),
            "backlogMsP95": percentile([x for x in backlog if isinstance(x, (int, float))], .95),
        }
    return {
        "schema": 3, "startedEpoch": started["epoch"],
        "elapsedSeconds": round(time.time() - started["epoch"], 3),
        "latestHealthy": sample["healthy"], "latestFailures": sample["failures"],
        "samples": len(lines),
        "failedSamples": sum(bool(row.get("failures")) for row in decoded),
        "startupAccepted": acceptance.get("startupAccepted", False),
        "startupFailures": acceptance.get("startupFailures", ["soak:acceptance-unavailable"]),
        "steadyStateAccepted": acceptance.get("steadyStateAccepted", False),
        "releaseAccepted": release_accepted,
        "releaseBinding": binding,
        "cleanWindow": acceptance,
        "diagnosticWindow": acceptance.get("policy", {}).get(
            "cleanWindowSeconds", DEFAULT_CLEAN_WINDOW_SECONDS) < DEFAULT_CLEAN_WINDOW_SECONDS,
        "performance": performance, "latest": sample,
    }


def run_sample(state: Path) -> int:
    clean_window_seconds = float(os.environ.get("VECTORWARP_SOAK_CLEAN_WINDOW_SECONDS",
                                               DEFAULT_CLEAN_WINDOW_SECONDS))
    max_sample_gap_seconds = float(os.environ.get("VECTORWARP_SOAK_MAX_SAMPLE_GAP_SECONDS",
                                                  DEFAULT_MAX_SAMPLE_GAP_SECONDS))
    binding_path = os.environ.get("VECTORWARP_SOAK_BINDING")
    binding = load_binding(binding_path) if binding_path else None
    binding_receipt = None
    if binding:
        binding_receipt = {"expected": binding,
                           "sha256": hashlib.sha256(Path(binding_path).read_bytes()).hexdigest(),
                           "monitorSha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
                           "probeSha256": hashlib.sha256(IDENTITY_PROBE.read_bytes()).hexdigest()}
    started_path = state / "started.json"
    reset_cohort = False
    try:
        started = json.loads(started_path.read_text())
        if started.get("schema") != 3:
            raise ValueError("old schema")
    except FileNotFoundError:
        started = {"schema": 3, "epoch": time.time(), "previous": {}}
        reset_cohort = True
    except (ValueError, json.JSONDecodeError):
        # A damaged/older ledger must not silently become new release evidence.
        # Preserve it for diagnosis and require an explicit new cohort path.
        if binding:
            raise ValueError("invalid prior soak state; preserve it and use a fresh state directory")
        started = {"schema": 3, "epoch": time.time(), "previous": {}}
        reset_cohort = True

    if not reset_cohort and started.get("releaseBinding") != binding_receipt:
        raise ValueError("soak binding or verifier changed; preserve this cohort and use a fresh state directory")
    started["releaseBinding"] = binding_receipt

    failures: list[str] = []
    sample = {"epoch": time.time(), "nodes": {}, "failures": failures}
    nodes = binding["nodes"] if binding else diagnostic_nodes()
    for name, node in nodes.items():
        try:
            # Identity hashing may take seconds; acquire freshness-sensitive
            # telemetry afterwards so the probe itself cannot stale a frame.
            process = (measured_process(node["ssh"], node["disk"]["path"]) if binding else
                       local_process() if node["ssh"] is None else remote_process())
            base = node["endpoint"].rstrip("/")
            status, timing = (fetch(base + "/api/system/status"), fetch(base + "/api/timing"))
            node_failures, observation = evaluate(
                name, status, timing, process, started["previous"].get(name),
                node["acceleration"], node["clutterAcceleration"], node["maximumCadenceMs"])
            failures.extend(node_failures)
            if binding:
                failures.extend(verify_binding(name, status, process, binding))
            sample["nodes"][name] = {
                "status": compact_status(status), "timing": compact_timing(timing),
                "process": process, "observation": observation,
            }
            started["previous"][name] = observation
        except Exception as error:
            sample["nodes"][name] = {"error": f"{type(error).__name__}: {error}"[:300]}
            failures.append(f"{name}:unreachable")

    sample["epoch"] = time.time()
    if binding:
        try:
            regression = json.loads(Path(os.environ["VECTORWARP_SOAK_REGRESSION_RECEIPT"]).read_text())
            failures.extend(verify_regression(regression, binding_receipt, sample["epoch"], started["epoch"]))
            sample["regressionEvidence"] = regression
        except (KeyError, OSError, ValueError, TypeError):
            failures.append("soak:regression-evidence-unavailable")

    disk = os.statvfs(state)
    sample["monitorFreeBytes"] = disk.f_bavail * disk.f_frsize
    if sample["monitorFreeBytes"] < (binding["monitorMinimumFreeBytes"] if binding else 200_000_000_000):
        failures.append("soak:monitor-disk-reserve")
    advance_acceptance(started, sample, clean_window_seconds, max_sample_gap_seconds)
    atomic(started_path, started)
    ledger = state / "samples.jsonl"
    lines = (ledger.read_text().splitlines()[-(MAX_SAMPLES - 1):]
             if ledger.exists() and not reset_cohort else [])
    lines.append(json.dumps(sample, sort_keys=True, separators=(",", ":")))
    ledger.write_text("\n".join(lines) + "\n")
    atomic(state / "latest.json", summarize(lines, started, sample))
    print(json.dumps({"healthy": sample["healthy"], "failures": failures}, sort_keys=True))
    return 0


def diagnostic_nodes():
    """Keep the existing operator CLI defaults; unbound diagnostics never qualify.

    The historical HP environment spelling and script/state paths are consumed
    by deployed timers. Release topology comes exclusively from schema 3 bindings.
    """
    host = os.environ.get("VECTORWARP_SOAK_HP_HOST", "").strip()
    if not host:
        raise ValueError("set VECTORWARP_SOAK_HP_HOST or a reviewed VECTORWARP_SOAK_BINDING")
    return {name: {"endpoint": endpoint, "ssh": ssh, "acceleration": backend,
                   "clutterAcceleration": backend, "maximumCadenceMs": 600}
            for name, endpoint, ssh, backend in (
                ("local", "http://127.0.0.1:3000", None, "vulkan"),
                ("remote", f"http://{host}:3000", host, "vulkan-resident"))}


def main() -> int:
    state = Path(os.environ.get("VECTORWARP_SOAK_STATE", DEFAULT_STATE))
    state.mkdir(parents=True, exist_ok=True)
    # Overlapping timer/cron invocations must not erase each other's failures
    # or advance the same cohort from different previous observations.
    with (state / ".lock").open("a") as guard:
        fcntl.flock(guard, fcntl.LOCK_EX | fcntl.LOCK_NB)
        return run_sample(state)


if __name__ == "__main__":
    raise SystemExit(main())
