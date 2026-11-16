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


DEFAULT_STATE = Path("/var/lib/home-portal/vectorwarp-two-node-soak")
MAX_SAMPLES = 1600
MAX_FRAME_AGE_MS = 3000
MAX_BACKLOG_MS = 1000
DEFAULT_CLEAN_WINDOW_SECONDS = 24 * 60 * 60
DEFAULT_MAX_SAMPLE_GAP_SECONDS = 180
IDENTITY_PROBE = Path(__file__).with_name("vectorwarp_soak_identity.py")
SHA256 = re.compile(r"^[0-9a-f]{64}$")


def load_binding(filename: str) -> dict:
    """An operator-reviewed binding is mandatory for release acceptance."""
    binding = json.loads(Path(filename).read_text())
    if binding.get("schema") != 1 or set(binding.get("nodes", {})) != {"homeportal", "hp"}:
        raise ValueError("release binding must contain both live nodes")
    if not re.fullmatch(r"[0-9a-f]{40}", binding.get("sourceCommit", "")):
        raise ValueError("release binding requires a full source commit")
    for node in binding["nodes"].values():
        for key in ("artifactSha256", "configSha256"):
            if not SHA256.fullmatch(node.get(key, "")):
                raise ValueError(f"release binding requires {key}")
        if not isinstance(node.get("release"), str) or not node["release"].startswith("/"):
            raise ValueError("release binding requires the resolved artifact path")
        executables = node.get("runtimeExecutables", {})
        if set(executables) != {"vectorwarp-api.service", "vectorwarp-processor.service"}:
            raise ValueError("release binding requires both running executables")
        for executable in executables.values():
            if not SHA256.fullmatch(executable.get("sha256", "")) or not executable.get("path"):
                raise ValueError("invalid runtime executable binding")
    return binding


def measured_process(ssh_destination: str | None = None) -> dict:
    if ssh_destination is None:
        args, payload = [sys.executable, str(IDENTITY_PROBE)], None
    else:
        if not re.fullmatch(r"[A-Za-z0-9_.@:\[\]-]+", ssh_destination) or ssh_destination.startswith("-"):
            raise ValueError("invalid identity SSH destination")
        args = ["ssh", "-o", "BatchMode=yes", "-o", "ConnectTimeout=5", "--",
                ssh_destination, "python3", "-"]
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
    for field in ("release", "artifactSha256", "configSha256", "runtimeExecutables"):
        if process.get(field) != expected[field]:
            failures.append(f"{name}:artifact-binding:{field}")
    if any(status.get(key) != expected["configSha256"]
           for key in ("loadedRevision", "configRevision")):
        failures.append(f"{name}:config-binding")
    return failures


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


def numeric_list(value: object) -> list[int] | None:
    if not isinstance(value, list) or not value or not all(counter(item) for item in value):
        return None
    return list(value)


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
        "captureTerminal", "captureEpoch", "captureDroppedSamples",
        "captureBoundaryDiscardedSamples", "captureStopDiscardedSamples",
        "gpu_resident_ready", "output_radar_data", "multi_capture_prepare",
        "multi_channelizer_wall", "multi_illuminator_radar", "multi_serialize",
        "multi_output", "multi_pipeline_advance",
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
        "runtimeExecutables": process.get("runtimeExecutables"),
        "units": {name: {key: values.get(key) for key in
                         ("MainPID", "NRestarts", "ExecMainStartTimestampMonotonic")}
                  for name, values in process.get("units", {}).items()},
    }


def evaluate(name: str, status: dict, timing: dict, process: dict,
             previous: dict | None, acceleration: str,
             clutter: str | None = None) -> tuple[list[str], dict]:
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
            old_cpi = previous.get("nCpi")
            elapsed = time.time() - float(previous.get("epoch", time.time()))
            if counter(old_cpi) and counter(new_cpi):
                advancement = new_cpi - old_cpi
                if advancement <= 0:
                    failures.append(f"{name}:cpi-not-advancing")
                elif elapsed > 0:
                    cadence_ms = 1000 * elapsed / advancement
                    if cadence_ms > 600:
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
    if (not previous or restarted) and any(any(values or []) for values in counters.values()):
        failures.append(f"{name}:preexisting-capture-loss")

    observation = {
        "epoch": time.time(), "identity": identity, "nCpi": timing.get("nCpi"),
        "counters": counters, "advancement": advancement,
        "cadenceMs": round(cadence_ms, 3) if cadence_ms is not None else None,
        "restarted": restarted,
    }
    return failures, observation


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
    startup_failures = set(prior.get("startupFailures", [])) if prior else set(failures)
    # A restart/loss/counter discontinuity anywhere in the cohort invalidates
    # its zero-loss startup claim, even after that row leaves the bounded log.
    startup_failures.update(failure for failure in failures if
                           "capture-" in failure or "-binding" in failure or
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
                            acceptance.get("policy", {}).get("cleanWindowSeconds", 0) >=
                            DEFAULT_CLEAN_WINDOW_SECONDS)
    performance = {}
    for name in ("homeportal", "hp"):
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
    hp_host = os.environ.get("VECTORWARP_SOAK_HP_HOST", "100.126.186.65")
    clean_window_seconds = float(os.environ.get("VECTORWARP_SOAK_CLEAN_WINDOW_SECONDS",
                                               DEFAULT_CLEAN_WINDOW_SECONDS))
    max_sample_gap_seconds = float(os.environ.get("VECTORWARP_SOAK_MAX_SAMPLE_GAP_SECONDS",
                                                  DEFAULT_MAX_SAMPLE_GAP_SECONDS))
    binding_path = os.environ.get("VECTORWARP_SOAK_BINDING")
    binding = load_binding(binding_path) if binding_path else None
    binding_receipt = None
    hp_ssh = os.environ.get("VECTORWARP_SOAK_HP_SSH")
    if binding:
        if not hp_ssh:
            raise ValueError("release soak requires VECTORWARP_SOAK_HP_SSH for measured identity")
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
    nodes = (
        ("homeportal", "http://127.0.0.1:3000", measured_process if binding else local_process, "vulkan", "cpu"),
        ("hp", f"http://{hp_host}:3000", (lambda: measured_process(hp_ssh)) if binding else remote_process,
         "vulkan-resident", "vulkan-resident"),
    )
    for name, base, process_fn, acceleration, clutter in nodes:
        try:
            # Identity hashing may take seconds; acquire freshness-sensitive
            # telemetry afterwards so the probe itself cannot stale a frame.
            process = process_fn()
            status, timing = (fetch(base + "/api/system/status"), fetch(base + "/api/timing"))
            node_failures, observation = evaluate(
                name, status, timing, process, started["previous"].get(name),
                acceleration, clutter)
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

    free_gb = os.statvfs(state).f_bavail * os.statvfs(state).f_frsize / 1e9
    sample["homeportalFreeGb"] = round(free_gb, 3)
    if free_gb < 200:
        failures.append("homeportal:disk-reserve")
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
