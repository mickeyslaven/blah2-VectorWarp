#!/usr/bin/env python3
"""Bounded multi-lane exact-byte ingress regression; never a live-service tool.

Consumes a completed recording and its portable profile, using the maintained
replay harness's private loopback peers. Explicit expectations replace ADS-B
timeline counts. Legacy provenance stays unqualified; this is not numerical or
physical qualification. Run inside the site's approved resource-limited job.
"""
import argparse
import collections
import importlib.util
import json
import math
from pathlib import Path
import re
import time

SPEC = importlib.util.spec_from_file_location(
    "isolated_replay_configuration", Path(__file__).with_name("run-iq-protection-replay.py"))
PROFILE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(PROFILE)
SESSION = PROFILE.SESSION
ARTIFACTS = SESSION.ARTIFACTS
INPUT_PATHS = ("contiguous_double", "partial_contiguous_double", "native_float_borrowed")


def write_json(path, value):
    path.write_text(json.dumps(value, indent=2, allow_nan=False) + "\n")


def pinned(path, digest):
    if not re.fullmatch(r"[a-f0-9]{64}", digest or ""):
        raise ValueError("pins must be lowercase SHA-256")
    if PROFILE.file_sha256(path) != digest:
        raise ValueError(f"SHA-256 mismatch: {path}")
    return {"path": str(path), "sha256": digest}


def eof_contract(args):
    total = args.recording_samples if args.recording_samples is not None else args.expected_samples
    return {"input": "replay", "error": "", "loops": 0,
            "positionSamples": total, "totalSamples": total,
            "trailingSamples": args.expected_trailing_samples}


def validate(products, statuses, args, config):
    """Fail closed on missing counters, incomplete EOF, loss or missing lanes."""
    errors = []
    final = statuses[-1] if statuses else {}
    complete = SESSION.completed_status(statuses)
    if complete is None:
        errors.append("completed EOF receipt missing before shutdown")
    for label, status in (("EOF", complete or {}), ("final", final)):
        for key, value in eof_contract(args).items():
            if status.get(key) != value:
                errors.append(f"{label} {key}: {status.get(key)!r} != {value!r}")
    if final.get("state") not in ("complete", "stopped"):
        errors.append("unexpected final replay state")
    errored = [index for index, item in enumerate(statuses)
               if item.get("state") == "error" or item.get("error")]
    last_complete = max((index for index, item in enumerate(statuses)
                         if item.get("state") == "complete" and not item.get("error")), default=-1)
    recovered_errors = (len(errored) if args.acceleration == "auto" and
                        all(index < last_complete for index in errored) else 0)
    if errored and not recovered_errors:
        errors.append("processor reported an unrecovered or explicit-mode error during replay")
    terminal = [frame for frame in products["timing"] if frame.get("captureTerminal") is True]
    timing = [frame for frame in products["timing"] if frame.get("captureTerminal") is not True]
    if len(timing) != args.expected_cpis:
        errors.append(f"timing count: {len(timing)} != {args.expected_cpis}")
    channels = config["capture"]["device"]["channel_count"]
    if len(terminal) > 1 or terminal and products["timing"][-1] is not terminal[0]:
        errors.append("duplicate/misordered terminal capture accounting")
    elif terminal:
        end = terminal[0]
        dropped, discarded = end.get("captureDroppedSamples"), end.get("captureStopDiscardedSamples")
        if (end.get("nCpi") != args.expected_cpis or
                end.get("captureBoundaryDiscardedSamples") != [0] * channels or
                not isinstance(discarded, list) or len(discarded) != channels or
                any(type(value) is not int or not 0 <= value <= args.expected_trailing_samples for value in discarded) or
                dropped != discarded):
            errors.append("terminal capture accounting contains unexplained loss")
    # Replay skips live-only captureTerminal timing, but always posts these
    # final capture counters after its intentional complete -> stopped change.
    terminal_status = (final.get("captureStopped") is True and
                       final.get("captureBacklogSamples") == [0] * channels and
                       final.get("captureDroppedSamples") == [0] * channels)
    if (not terminal or final.get("state") == "stopped") and not terminal_status:
        errors.append("missing/lossy terminal stopped-capture status")
    cpi_samples = round(config["capture"]["fs"] * config["process"]["data"]["cpi"])
    if args.expected_cpis * cpi_samples + args.expected_trailing_samples != args.expected_samples:
        errors.append("expected CPI/EOF sample arithmetic disagrees with profile")
    identities = []
    for index, frame in enumerate(timing):
        if frame.get("nCpi") != index + 1:
            errors.append(f"timing {index}: duplicate/missing CPI sequence")
        drops = frame.get("captureDroppedSamples")
        if not isinstance(drops, list) or len(drops) != config["capture"]["device"]["channel_count"] or any(drops):
            errors.append(f"timing {index}: missing/nonzero capture drops")
        identity = tuple(frame.get(key) for key in
                         ("captureEpoch", "captureFirstEpochSample", "captureEndEpochSample"))
        if (frame.get("captureSamplePositionAvailable") is not True or
                any(type(value) is not int for value in identity) or
                identity[2] - identity[1] != cpi_samples):
            errors.append(f"timing {index}: invalid sample identity")
        elif identities and (identity[0] != identities[-1][0] or identity[1] != identities[-1][2]):
            errors.append(f"timing {index}: sample gap/duplicate/epoch change")
        identities.append(identity)
    lanes = collections.defaultdict(list)
    for frame in products["illuminator"]:
        lanes[frame.get("id")].append(frame)
    if set(lanes) != set(args.expected_lane):
        errors.append(f"lane set: {list(lanes)} != {args.expected_lane}")
    summaries = {}
    definitions = {lane["id"]: lane for lane in
                   config["process"]["multi_illuminator"]["illuminators"] if lane.get("enabled", True)}
    for lane, frames in lanes.items():
        if len(frames) != args.expected_cpis:
            errors.append(f"{lane}: {len(frames)} frames != {args.expected_cpis}")
        expected_widened = round(definitions.get(lane, {}).get("output_sample_rate_hz", 0) *
                                 config["process"]["data"]["cpi"]) * config["capture"]["device"]["channel_count"]
        for index, frame in enumerate(frames):
            identity = tuple(frame.get(key) for key in
                             ("capture_epoch", "capture_first_input_sample", "capture_end_input_sample"))
            if frame.get("capture_sample_position_available") is not True or index >= len(identities) or identity != identities[index]:
                errors.append(f"{lane}/{index}: publication identity mismatch")
            counters = frame.get("input_processing", {})
            allocations = counters.get("contiguous_allocations")
            if type(allocations) is not int or allocations < 0:
                errors.append(f"{lane}/{index}: missing/invalid contiguous_allocations")
            path = counters.get("path")
            if path not in args.expected_input_path:
                errors.append(f"{lane}/{index}: unexpected input path {path!r}")
            widened = 0 if path == "native_float_borrowed" else expected_widened
            expected = {"staging_copied_samples": 0, "deque_materializations": 0,
                        "deque_materialized_bytes": 0, "deque_materialization_ms": 0,
                        "contiguous_fp64_samples": widened,
                        "staging_widened_samples": widened,
                        "contiguous_fp64_bytes_written": 16 * widened}
            if path == "native_float_borrowed" or index >= args.warmup_cpis:
                expected["contiguous_allocations"] = 0
            for key, value in expected.items():
                if counters.get(key) != value:
                    errors.append(f"{lane}/{index}: {key}={counters.get(key)!r}, expected {value}")
        summaries[lane] = {"frames": len(frames), "paths": sorted({
            frame.get("input_processing", {}).get("path", "missing") for frame in frames})}
    return {"passed": not errors, "errors": errors, "capture_cpis": len(timing),
            "terminal_records": len(terminal), "terminal_capture_status": terminal_status,
            "completed_eof_observed": complete is not None,
            "recovered_error_statuses": recovered_errors, "lanes": summaries}


def run(args):
    if not math.isfinite(args.timeout) or not 0 < args.timeout <= 1800:
        raise ValueError("timeout must be positive and at most 1800 seconds")
    if not math.isfinite(args.error_stall_seconds) or not 0 < args.error_stall_seconds <= 60:
        raise ValueError("error stall limit must be positive and at most 60 seconds")
    if not 0 <= args.warmup_cpis < args.expected_cpis:
        raise ValueError("warmup must leave at least one steady-state CPI")
    if min(args.expected_cpis, args.expected_samples) <= 0 or args.expected_trailing_samples < 0:
        raise ValueError("invalid CPI/sample expectations")
    recording_samples = (args.recording_samples if args.recording_samples is not None
                         else args.expected_samples)
    if args.start_sample < 0 or recording_samples <= 0 or \
            args.start_sample + args.expected_samples != recording_samples:
        raise ValueError("replay start/segment/recording sample arithmetic is invalid")
    if not args.expected_lane or len(set(args.expected_lane)) != len(args.expected_lane):
        raise ValueError("expected lanes must be nonempty and unique")
    if (not args.expected_input_path or len(set(args.expected_input_path)) != len(args.expected_input_path)
            or not set(args.expected_input_path) <= set(INPUT_PATHS)):
        raise ValueError("expected input paths must be unique supported path names")
    for key in ("binary", "recording", "metadata", "profile"):
        setattr(args, key, getattr(args, key).resolve(strict=True))
    root = args.output.absolute()
    root.mkdir(mode=0o700)  # exclusive: reused, nonempty and symlink paths all fail
    started = time.monotonic()
    receipt = {"schema": "vectorwarp-isolated-iq-replay-v1", "passed": False,
               "acceleration": args.acceleration, "error_stall_seconds": args.error_stall_seconds,
               "start_sample": args.start_sample, "recording_samples": recording_samples,
               "provenance": "legacy-unqualified", "expectations": {
                   key: getattr(args, key) for key in ("expected_cpis", "expected_lane",
                       "expected_samples", "expected_trailing_samples", "warmup_cpis", "expected_input_path")}}
    try:
        receipt["binary"] = pinned(args.binary, args.binary_sha256)
        receipt["recording"] = pinned(args.recording, args.sha256)
        metadata = json.loads(args.metadata.read_text())
        profile = json.loads(args.profile.read_text())
        args.acceleration = PROFILE.selected_backend(profile, args.acceleration)
        receipt["acceleration"] = args.acceleration
        if metadata.get("dataSha256") != args.sha256 or profile.get("configuration_complete") is not True:
            raise ValueError("metadata pin/profile completeness mismatch")
        receipt["sources"] = {str(path): PROFILE.file_sha256(path) for path in
                              (args.metadata, args.profile, Path(__file__), Path(PROFILE.__file__), Path(SESSION.__file__), Path(ARTIFACTS.__file__))}
        experiment = SESSION.Experiment(args.binary, args.timeout,
            gpu=args.acceleration != "cpu", binary_sha256=args.binary_sha256,
            required=("illuminator", "timing"), eof=eof_contract(args),
            error_stall_seconds=args.error_stall_seconds)
        with SESSION.ReplaySession(experiment, root, receipt) as session:
            status, sinks = session.status, session.sinks
            config = PROFILE.configuration(args.recording, profile, status.port, sinks.ports(),
                args.acceleration, recording_sha256=args.sha256)
            if args.start_sample:
                config["capture"]["replay"]["start_sample"] = args.start_sample
            enabled = config["process"].get("multi_illuminator", {})
            if enabled.get("enabled") is not True or {lane["id"] for lane in enabled["illuminators"]
                    if lane.get("enabled", True)} != set(args.expected_lane):
                raise ValueError("expected lanes disagree with enabled profile")
            cpi_samples = round(config["capture"]["fs"] * config["process"]["data"]["cpi"])
            if (cpi_samples <= 0 or args.expected_trailing_samples >= cpi_samples or
                    args.expected_cpis * cpi_samples + args.expected_trailing_samples != args.expected_samples):
                raise ValueError("expected CPI/EOF sample arithmetic disagrees with profile")
            config["save"]["path"] = str(root)
            process_sha256 = PROFILE.process_identity(config["process"])
            products = session.execute(config)
            PROFILE.require_process_identity(config, process_sha256)
            PROFILE.require_process_identity(json.loads((root / "config.json").read_text()), process_sha256)
            receipt["process_config_sha256"] = process_sha256
            receipt["profile_process_sha256"] = PROFILE.process_identity(profile["process"])
            receipt["config_sha256"] = PROFILE.file_sha256(root / "config.json")
            for name, frames in products.items():
                write_json(root / f"{name}.json", frames)
            receipt["acceptance"] = validate(products, status.snapshot(), args, config)
            if not receipt["acceptance"]["passed"]:
                raise RuntimeError("replay acceptance failed")
            receipt["passed"] = True
    except Exception as error:
        receipt.update(passed=False, error=str(error))
    finally:
        receipt["elapsed_seconds"] = time.monotonic() - started
        write_json(root / "receipt.json", receipt)
    return receipt


def parse_args(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("binary", "recording", "metadata", "profile", "output"):
        parser.add_argument("--" + name, type=Path, required=True,
                            help="must not already exist" if name == "output" else None)
    parser.add_argument("--binary-sha256", required=True)
    parser.add_argument("--sha256", required=True, help="externally pinned recording SHA-256")
    parser.add_argument("--expected-cpis", required=True, type=int)
    parser.add_argument("--expected-lane", required=True, action="append")
    parser.add_argument("--expected-input-path", required=True, action="append", choices=INPUT_PATHS,
                        help="allowed observed ingress path; repeat to admit multiple paths")
    parser.add_argument("--expected-samples", required=True, type=int)
    parser.add_argument("--start-sample", type=int, default=0,
                        help="exact sample offset at which bounded replay begins")
    parser.add_argument("--recording-samples", type=int,
                        help="full recording length; must equal start plus expected samples")
    parser.add_argument("--expected-trailing-samples", required=True, type=int)
    parser.add_argument("--warmup-cpis", type=int, default=1)
    parser.add_argument("--timeout", type=float, default=900)
    parser.add_argument("--error-stall-seconds", type=float, default=5,
                        help="fail capture state=error after this long without sample progress (max 60)")
    parser.add_argument("--acceleration", choices=("cpu", "gpu", "gpu-resident", "auto"),
                        help="explicit backend comparison; omission preserves the profile")
    return parser.parse_args(argv)


if __name__ == "__main__":
    result = run(parse_args())
    print(json.dumps({key: result.get(key) for key in ("passed", "error", "acceptance")}))
    raise SystemExit(0 if result["passed"] else 1)
