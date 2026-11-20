#!/usr/bin/env python3
"""Run an isolated HP IQ replay arm without touching live services.

The recording's portable replay profile supplies RF/DSP geometry.  Output is
captured from private loopback sockets and written as concatenated-JSON arrays
for later paired scoring.  ADS-B timeline data is deliberately not passed to
the processor; it is only named in the result for offline evaluation.
"""

import argparse
import copy
import hashlib
import importlib.util
import json
import math
import os
from pathlib import Path
import re
import statistics
import tempfile
import time


SESSION_SPEC = importlib.util.spec_from_file_location(
    "vectorwarp_replay_session", Path(__file__).with_name("replay_session.py"))
SESSION = importlib.util.module_from_spec(SESSION_SPEC)
SESSION_SPEC.loader.exec_module(SESSION)

# Admission truth is shared evaluation logic, never a processor input.
SCORE_SPEC = importlib.util.spec_from_file_location(
    "protection_replay_score", Path(__file__).with_name("score-iq-protection-replay.py"))
SCORE = importlib.util.module_from_spec(SCORE_SPEC)
SCORE_SPEC.loader.exec_module(SCORE)
file_sha256 = SCORE.file_sha256

# Match ResidentReplaySnapshot.h. max_bytes bounds EACH snapshot, not the run.
RESIDENT_SNAPSHOT_FRAME_LIMIT = 128
RESIDENT_SNAPSHOT_BYTE_LIMIT = 512 << 20
RESIDENT_SNAPSHOT_HEADER_BYTES = 2048
RESIDENT_LINEAGE_BYTES = 512


def process_identity(process):
    """Exact configured intent, not a substitute for a native RadarPlan receipt."""
    return hashlib.sha256(json.dumps(process, sort_keys=True, separators=(",", ":"),
                                     allow_nan=False).encode()).hexdigest()


def require_process_identity(config, expected):
    if process_identity(config["process"]) != expected:
        raise ValueError("replay processing configuration changed after admission")


def selected_backend(profile, requested=None):
    backend = requested if requested is not None else profile["process"]["performance"]["acceleration"]
    if backend not in ("cpu", "gpu", "gpu-resident", "auto"):
        raise ValueError("replay profile has no supported explicit acceleration setting")
    return backend


def parse_tracker_diagnostics(output):
    """Bounded diagnostic aggregation, never a physical qualification receipt."""
    if len(output) > 16 << 20:
        raise ValueError("tracker diagnostic log exceeds bounded size")
    totals, seen = {}, set()
    for line in output.splitlines():
        if not re.search(r"(?:^|\s)GPU_TRACKER_PREPARED(?:\s|$)", line):
            continue
        fields = {key: int(value) for key, value in re.findall(r"([A-Za-z_][A-Za-z0-9_]*)=(-?\d+)", line)}
        if "frame" not in fields or any(value < 0 for value in fields.values()):
            raise ValueError("invalid tracker diagnostic frame/counter")
        identity = tuple(fields.get(key, 0) for key in ("frame", "generation", "transaction"))
        if identity in seen:
            raise ValueError("duplicate prepared tracker diagnostic frame")
        seen.add(identity)
        for key, value in fields.items():
            if key not in ("frame", "generation", "transaction"):
                totals[key] = totals.get(key, 0) + value
    return {"frames": len(seen), "totals": totals}


def admitted_truth(admission):
    return SCORE.admitted_truth(admission,
        empty_error="map diagnostics require valid admission truth samples")


def align_product_timeline(products, timeline):
    """Bind complete CPI products to the trailing timeline boundaries.

    A duration-N capture timeline legitimately contains N+1 sample boundaries
    while the processor emits N complete CPI products.  Positional/count-only
    comparison previously rejected complete HP clips. Legacy MCHQ product
    timestamps describe replay execution rather than recording identity, so
    they must not be used to join recorded truth.  The first complete CPI ends
    at boundary one; therefore one extra leading boundary is discarded.
    """
    endpoints = timeline.get("frames", [])
    by_timestamp = {}
    for endpoint in endpoints:
        timestamp = endpoint.get("timestamp_ms") if isinstance(endpoint, dict) else None
        if not isinstance(timestamp, int) or timestamp in by_timestamp:
            raise ValueError("timeline has missing or duplicate integer CPI endpoints")
        by_timestamp[timestamp] = endpoint
    counts = {}
    for name, frames in products.items():
        if not isinstance(frames, list) or not frames:
            raise ValueError(f"{name} products are empty")
        counts[name] = len(frames)
    reference = counts.get("detection", 0)
    if not reference or any(count != reference for count in counts.values()):
        raise ValueError("detection/track/timing CPI counts differ")
    boundary_difference = len(endpoints) - reference
    if boundary_difference not in (0, 1):
        raise ValueError("timeline/product CPI count differs by more than one boundary")
    # With one extra boundary, a complete first CPI ends at frames[1].  The
    # independently serialized product and timeline clocks can differ by a
    # small constant number of milliseconds, so verify cadence/constant offset
    # rather than demanding byte-identical integer timestamps.
    return endpoints[boundary_difference:]


def map_diagnostics(maps, timeline, truth):
    if len(maps) != len(timeline):
        raise ValueError("map/timeline endpoint count differs")
    rows = []
    for index, (frame, endpoint) in enumerate(zip(maps, timeline)):
        timestamp = endpoint.get("timestamp_ms")
        if timestamp not in truth:
            rows.append({"index": index, "timestamp_ms": timestamp,
                         "scorable": False})
            continue
        expected_delay, expected_doppler = truth[timestamp]
        delay_axis, doppler_axis, data = frame.get("delay"), frame.get("doppler"), frame.get("data")
        if not delay_axis or not doppler_axis or len(data) != len(doppler_axis):
            raise ValueError("map diagnostic frame has invalid geometry")
        if not (min(delay_axis) <= expected_delay <= max(delay_axis) and
                min(doppler_axis) <= expected_doppler <= max(doppler_axis)):
            raise ValueError("admitted truth lies outside the replay map")
        center_column = min(range(len(delay_axis)),
                            key=lambda i: abs(delay_axis[i] - expected_delay))
        center_row = min(range(len(doppler_axis)),
                         key=lambda i: abs(doppler_axis[i] - expected_doppler))
        target_roi = [float(data[r][c])
                      for r in range(max(0, center_row - 1), min(len(data), center_row + 2))
                      for c in range(max(0, center_column - 1), min(len(delay_axis), center_column + 2))]
        target = max(target_roi)
        background = []
        for r in range(max(0, center_row - 8), min(len(data), center_row + 9)):
            for c in range(max(0, center_column - 8), min(len(delay_axis), center_column + 9)):
                if abs(r - center_row) <= 2 and abs(c - center_column) <= 2:
                    continue
                background.append(float(data[r][c]))
        local = statistics.median(background)
        rows.append({"index": index, "timestamp_ms": timestamp, "scorable": True,
                     "truth_delay_km": expected_delay,
                     "truth_doppler_hz": expected_doppler,
                     "center_delay_km": float(delay_axis[center_column]),
                     "center_doppler_hz": float(doppler_axis[center_row]),
                     "target_roi_db": target_roi, "background_roi_db": background,
                     "target_peak_db": target, "local_median_db": local,
                     "target_local_contrast_db": target - local})
    contrasts = [row["target_local_contrast_db"] for row in rows if row["scorable"]]
    if not contrasts:
        raise ValueError("map diagnostics have no admitted CPI endpoints")
    return {"frames": rows, "scored_frames": len(contrasts),
            "mean_contrast_db": statistics.fmean(contrasts),
            "median_contrast_db": statistics.median(contrasts),
            "maximum_contrast_db": max(contrasts)}


def validate_resident_snapshot(snapshot):
    required = {"enabled", "output_directory", "max_frames", "max_bytes",
                "source_sha256", "rf_profile_file", "rf_profile_sha256"}
    if not isinstance(snapshot, dict) or set(snapshot) != required:
        raise ValueError("resident snapshot requires every exact export field")
    if snapshot["enabled"] is not True:
        raise ValueError("resident snapshot export must be explicitly enabled")
    directory = Path(snapshot["output_directory"])
    if not directory.is_absolute() or not directory.is_dir():
        raise ValueError("resident snapshot directory must be existing and absolute")
    if next(directory.iterdir(), None) is not None:
        raise ValueError("resident snapshot directory must be empty")
    frames, size = snapshot["max_frames"], snapshot["max_bytes"]
    if type(frames) is not int or not 1 <= frames <= RESIDENT_SNAPSHOT_FRAME_LIMIT:
        raise ValueError("resident snapshot max_frames must be 1..128")
    if type(size) is not int or not RESIDENT_SNAPSHOT_HEADER_BYTES <= size <= RESIDENT_SNAPSHOT_BYTE_LIMIT:
        raise ValueError("resident snapshot max_bytes must be 2048..536870912 per frame")
    for key in ("source_sha256", "rf_profile_sha256"):
        value = snapshot[key]
        if not isinstance(value, str) or not re.fullmatch(r"[0-9a-f]{64}", value) or value == "0"*64:
            raise ValueError(f"resident snapshot {key} must be a nonzero lowercase SHA-256")
    rf_file = Path(snapshot["rf_profile_file"])
    if not rf_file.is_absolute() or not rf_file.is_file():
        raise ValueError("resident snapshot RF profile must be an existing absolute file")
    if file_sha256(rf_file) != snapshot["rf_profile_sha256"]:
        raise ValueError("resident snapshot RF profile SHA-256 mismatch")


def resident_snapshot_options(args):
    """No auto-enablement, inferred limits, filesystem creation or IQ reads."""
    names = ("directory", "max_frames", "max_bytes", "rf_profile",
             "rf_profile_sha256")
    values = {name: getattr(args, "resident_snapshot_"+name, None) for name in names}
    present = [value is not None for value in values.values()]
    if not any(present):
        return None
    if not all(present):
        raise ValueError("all five --resident-snapshot-* options are required together")
    if args.acceleration != "gpu-resident":
        raise ValueError("resident snapshot export requires --acceleration gpu-resident")
    snapshot = {
        "enabled": True, "output_directory": str(values["directory"]),
        "max_frames": values["max_frames"], "max_bytes": values["max_bytes"],
        "source_sha256": args.sha256,
        "rf_profile_file": str(values["rf_profile"]),
        "rf_profile_sha256": values["rf_profile_sha256"],
    }
    validate_resident_snapshot(snapshot)
    # The native exporter hashes an opaque RF file. Bind that file to the
    # actual replay profile used here, not an unrelated equally well-formed file.
    if file_sha256(args.profile) != snapshot["rf_profile_sha256"]:
        raise ValueError("resident RF profile differs from the replay --profile")
    return snapshot


def resident_snapshot_receipt(snapshot, expected_frames):
    """Do not report success if an older binary ignored the export request.

    This checks complete bounded file pairs, not their native numerical ABI;
    downstream scorers must still use loadValidatedSnapshot for that validation.
    """
    directory = Path(snapshot["output_directory"])
    paths = list(directory.iterdir())
    frames = sorted(path for path in paths if path.suffix == ".vwrps")
    wanted = min(expected_frames, snapshot["max_frames"])
    if wanted <= 0 or len(frames) != wanted or len(paths) != 2*wanted:
        raise ValueError("resident snapshot export is missing, incomplete or contains unexpected files")
    entries, logical_frames = [], set()
    for path in frames:
        match = re.fullmatch(r"resident-frame-([1-9][0-9]*)-([1-9][0-9]*)\.vwrps", path.name)
        sidecar = Path(str(path)+".vwrl")
        if (not match or path.is_symlink() or not path.is_file() or
                sidecar.is_symlink() or not sidecar.is_file()):
            raise ValueError("resident snapshot filename or lineage pair is invalid")
        logical_frames.add(int(match.group(1)))
        size = path.stat().st_size
        if (not RESIDENT_SNAPSHOT_HEADER_BYTES <= size <= snapshot["max_bytes"] or
                sidecar.stat().st_size != RESIDENT_LINEAGE_BYTES):
            raise ValueError("resident snapshot/lineage byte bound differs")
        entries.append({"logical_frame": int(match.group(1)),
                        "consumed_sequence": int(match.group(2)),
                        "path": str(path), "bytes": size, "sha256": file_sha256(path),
                        "lineage": str(sidecar), "lineage_sha256": file_sha256(sidecar)})
    if logical_frames != set(range(1, wanted+1)):
        raise ValueError("resident snapshots do not contain the complete declared frame prefix")
    entries.sort(key=lambda item: item["logical_frame"])
    return {"config": snapshot, "exported_frames": wanted, "files": entries,
            "maximum_total_bytes_including_lineage":
                snapshot["max_frames"]*(snapshot["max_bytes"]+RESIDENT_LINEAGE_BYTES),
            "validation": "file-pair/bounds/hashes only; native snapshot decoding still required"}


def configuration(recording, profile, status_port, ports, acceleration=None, *,
                  resident_snapshot=None, recording_sha256=None,
                  acquisition_sha256=None, complex_cutout=None,
                  manifold_evidence_export=False):
    backend = selected_backend(profile, acceleration)
    if resident_snapshot is not None:
        validate_resident_snapshot(resident_snapshot)
        if backend != "gpu-resident":
            raise ValueError("resident snapshot export requires gpu-resident acceleration")
        if profile.get("configuration_complete") is not True:
            raise ValueError("resident export requires a complete replay profile")
        if profile["process"].get("multi_illuminator", {}).get("enabled"):
            raise ValueError("resident export requires single-lane replay")
    capture = profile["capture"]
    process = copy.deepcopy(profile["process"])
    # A backend comparison is explicit. Omission preserves the recorded choice;
    # no replay-only math, scheduling defaults or missing stage blocks are added.
    if acceleration is not None:
        process.setdefault("performance", {})["acceleration"] = backend
    if manifold_evidence_export:
        recorder = process.setdefault("bearing", {}).setdefault("manifold_recorder", {})
        recorder["enabled"] = "on"
    device = {
        "type": capture["source_receiver"],
        "channel_count": capture["channel_count"],
        "reference_channel": capture["reference_channel"],
        "surveillance_channels": capture["surveillance_channels"],
        "array_geometry": capture["array_geometry"],
    }
    for name in ("reference_mode", "reference_channels"):
        if name in capture:
            device[name] = copy.deepcopy(capture[name])
    config = {
        "capture": {
            "fs": capture["sample_rate_hz"],
            "fc": capture["frequency_hz"],
            "device": device,
            "replay": {
                "state": True,
                "loop": False,
                # The processor runs in an isolated temporary directory. A
                # caller-relative path would validate here, then fail only
                # after processor startup changed working directories.
                "file": str(recording.resolve(strict=True)),
                "format": "auto",
                "provenance": "legacy-unqualified",
            },
        },
        "process": process,
        # Optional historical convenience only. Native replay resolves sites
        # from the externally pinned acquisition contract, never this profile.
        "location": copy.deepcopy(profile.get("location", {})),
        "network": {"ip": SESSION.HOST,
                    "ports": dict(api=status_port, **ports)},
        "save": {"iq": False, "map": False, "detection": False,
                 "timing": False,
                 "path": str(recording.resolve(strict=True).parent)},
    }
    if complex_cutout is not None:
        config["save"]["complex_cutout"] = copy.deepcopy(complex_cutout)
    if resident_snapshot is not None:
        config["capture"]["replay"]["resident_snapshot"] = copy.deepcopy(resident_snapshot)
    for name, digest in (("recording_sha256", recording_sha256), ("acquisition_sha256", acquisition_sha256)):
        if digest is not None:
            if not isinstance(digest, str) or not re.fullmatch(r"[a-f0-9]{64}", digest):
                raise ValueError(f"{name} must be an externally trusted lowercase SHA-256")
            config["capture"]["replay"][name] = digest
    if acquisition_sha256 is not None and recording_sha256 is None:
        raise ValueError("acquisition_sha256 requires the trusted recording SHA-256")
    return config


def complex_cutout_options(args):
    values = (getattr(args, "complex_cutout", None),
              getattr(args, "complex_cutout_delay_km", None),
              getattr(args, "complex_cutout_doppler_hz", None))
    if not any(value is not None for value in values):
        return None
    if not all(value is not None for value in values):
        raise ValueError("all complex-cutout path/delay/Doppler options are required")
    path, delay, doppler = values
    if not path.is_absolute() or not path.parent.is_dir() or path.exists():
        raise ValueError(
            "complex-cutout path must be absolute, new, and have an existing parent")
    if not (math.isfinite(delay) and math.isfinite(doppler)):
        raise ValueError("complex-cutout coordinates must be finite")
    return {"state": True, "path": str(path), "delay_km": delay,
            "doppler_hz": doppler, "delay_radius_bins": 8,
            "doppler_radius_bins": 8, "max_frames": 128, "max_mb": 64}


def manifold_export_options(args):
    path = getattr(args, "manifold_evidence_export", None)
    if path is None:
        return None
    if not path.is_absolute() or not path.parent.is_dir() or path.exists():
        raise ValueError(
            "manifold evidence export path must be absolute, new, and have an existing parent")
    if getattr(args, "acquisition_sha256", None) is None:
        raise ValueError("manifold evidence export requires an authenticated acquisition SHA-256")
    if not re.fullmatch(r"[a-f0-9]{64}", getattr(args, "binary_sha256", None) or ""):
        raise ValueError("manifold evidence export requires an independently pinned native binary SHA-256")
    return {"path": path, "max_bytes": 64 << 20}


def write_manifold_export(rows, options):
    # The marker must originate in the corrected native producer, not be
    # manufactured here from old rounded output or a caller's desired config.
    if not rows or any(row.get("precision_contract") != "binary64-json-roundtrip-v1" or
            row.get("schema") not in ("vectorwarp-manifold-evidence-v1",
                "vectorwarp-manifold-evidence-v2", "vectorwarp-manifold-evidence-v3")
            for row in rows):
        raise ValueError("native manifold evidence lacks its full-precision producer contract")
    payload = b"".join((json.dumps(row, separators=(",", ":"),
        ensure_ascii=False, allow_nan=False) + "\n").encode() for row in rows)
    if not 0 < len(payload) <= options["max_bytes"]:
        raise ValueError("manifold evidence export exceeds its 64 MiB bound")
    # Refuse a competing creation or symlink after the preflight path check.
    with options["path"].open("xb") as output:
        output.write(payload)
    return payload


def run(args):
    snapshot = resident_snapshot_options(args)
    manifold_export = manifold_export_options(args)
    profile = json.loads(args.profile.read_text())
    acceleration = selected_backend(profile, args.acceleration)
    if profile.get("configuration_complete") is False:
        raise ValueError("recording replay profile is incomplete")
    multi = profile.get("process", {}).get("multi_illuminator", {})
    enabled_lanes = [lane.get("id") for lane in multi.get("illuminators", [])
                     if lane.get("enabled", True)] if multi.get("enabled") is True else []
    if len(enabled_lanes) > 1:
        raise ValueError("single-lane protection replay cannot score a multi-illuminator "
                         "recording; use run-isolated-iq-replay.py and lane-bound scoring")
    metadata = json.loads(args.metadata.read_text())
    if metadata.get("dataSha256") != args.sha256:
        raise ValueError("recording digest does not match declared metadata")
    if file_sha256(args.recording) != args.sha256:
        raise ValueError("recording bytes do not match declared digest")
    if snapshot is not None:
        device = os.environ.get("BLAH2_GPU_DEVICE", "").strip()
        if not device or device == "auto":
            raise ValueError("resident export requires an explicit BLAH2_GPU_DEVICE")
    retained = {name: 1 << 20 for name in ("detection", "track", "timing")}
    if args.map_diagnostics:
        retained["map"] = 128 << 20
    if manifold_export is not None:
        retained["manifold"] = manifold_export["max_bytes"]
    experiment = SESSION.Experiment(args.binary, args.timeout, gpu=acceleration != "cpu",
        binary_sha256=getattr(args, "binary_sha256", None), required=tuple(retained), retained=retained)
    started = time.monotonic()
    with tempfile.TemporaryDirectory(prefix="vectorwarp-iq-protection-") as temporary:
        root = Path(temporary)
        with SESSION.ReplaySession(experiment, root) as session:
            status, sinks = session.status, session.sinks
            config = configuration(args.recording, profile, status.port,
                                   sinks.ports(), args.acceleration,
                                   resident_snapshot=snapshot, recording_sha256=args.sha256,
                                   acquisition_sha256=getattr(args, "acquisition_sha256", None),
                                   complex_cutout=complex_cutout_options(args),
                                   manifold_evidence_export=manifold_export is not None)
            process_sha256 = process_identity(config["process"])
            products = session.execute(config)
            require_process_identity(config, process_sha256)
            require_process_identity(json.loads((root / "config.json").read_text()), process_sha256)
            frames = {name: products[name] for name in ("detection", "track", "timing")}
            maps = products.get("map", [])
            output = (root / "processor.log").read_text(errors="replace")
            timeline = json.loads(args.timeline.read_text())
            endpoints = align_product_timeline(frames, timeline)
            expected_frames = len(endpoints)
            map_result = None
            if args.map_diagnostics:
                map_result = map_diagnostics(maps, endpoints,
                    admitted_truth(json.loads(args.admission.read_text())))
            final = status.snapshot()[-1]
            result = {
                "schema": "vectorwarp-iq-protection-replay-v1",
                "recording": str(args.recording),
                "recording_sha256": args.sha256,
                "timeline": str(args.timeline),
                "timeline_sha256": file_sha256(args.timeline),
                "metadata_sha256": file_sha256(args.metadata),
                "metadata": str(args.metadata),
                "profile_sha256": file_sha256(args.profile),
                "profile": str(args.profile),
                "admission_sha256": file_sha256(args.admission) if args.admission else None,
                "admission": str(args.admission) if args.admission else None,
                "binary": str(args.binary),
                "binary_sha256": file_sha256(args.binary),
                "resolved_config_sha256": hashlib.sha256(
                    json.dumps(config, sort_keys=True, separators=(",", ":")).encode()).hexdigest(),
                "resolved_config": config,
                "runner_sha256": file_sha256(Path(__file__)),
                "runner": str(Path(__file__).resolve()),
                "arm": {"kind": "baseline", "beams": 0, "index": None},
                "acceleration": acceleration,
                "process_config_sha256": process_sha256,
                "profile_process_sha256": process_identity(profile["process"]),
                "elapsed_seconds": time.monotonic() - started,
                "frames": frames,
                "map_diagnostics": map_result,
                "processor_log_tail": output[-12000:],
                "tracker_diagnostics": parse_tracker_diagnostics(output),
                "final_status": final,
                "session": session.receipt,
                "session_sources": {str(path): file_sha256(path) for path in
                    (Path(SESSION.__file__), Path(SESSION.ARTIFACTS.__file__))},
            }
            if snapshot is not None:
                result["resident_snapshot_export"] = resident_snapshot_receipt(
                    snapshot, expected_frames)
            if args.complex_cutout is not None:
                if not args.complex_cutout.is_file():
                    raise ValueError(
                        "processor did not produce the requested complex cutout")
                size = args.complex_cutout.stat().st_size
                if not 0 < size <= (64 << 20):
                    raise ValueError(
                        "complex cutout is empty or exceeds its 64 MiB bound")
                result["complex_cutout"] = {
                    "path": str(args.complex_cutout), "bytes": size,
                    "sha256": file_sha256(args.complex_cutout)}
            if manifold_export is not None:
                rows = products.get("manifold", [])
                payload = write_manifold_export(rows, manifold_export)
                result["manifold_evidence_export"] = {
                    "schema": "vectorwarp-native-manifold-evidence-export-v1",
                    "path": str(manifold_export["path"]), "rows": len(rows),
                    "bytes": len(payload),
                    "sha256": hashlib.sha256(payload).hexdigest(),
                    "native_stream_sha256": hashlib.sha256(
                        bytes(sinks.payloads["manifold"])).hexdigest(),
                    "recording_sha256": args.sha256,
                    "acquisition_sha256": args.acquisition_sha256,
                    "binary_sha256": args.binary_sha256,
                    "precision_contract": "binary64-json-roundtrip-v1",
                    "identity_qualified": True,
                    "scientific_qualification": False,
                    "selection": "detected-positive-cells-only; not independent miss or target-free exposure",
                }
        args.output.write_text(json.dumps(result, indent=2) + "\n")
        return result


def parse_args(argv=None):
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--binary-sha256", help="optional externally pinned processor digest")
    parser.add_argument("--recording", type=Path, required=True)
    parser.add_argument("--metadata", type=Path, required=True)
    parser.add_argument("--profile", type=Path, required=True)
    parser.add_argument("--timeline", type=Path, required=True)
    parser.add_argument("--admission", type=Path)
    parser.add_argument("--sha256", required=True)
    parser.add_argument("--acquisition-sha256",
                        help="externally pinned adjacent acquisition manifest hash; required for recorded geographic sites when metadata is not embedded")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--acceleration", choices=("cpu", "gpu", "gpu-resident", "auto"),
                        help="explicit backend comparison; omission preserves the profile")
    parser.add_argument("--resident-snapshot-directory", type=Path,
                        help="existing absolute empty export directory; never created by this runner")
    parser.add_argument("--resident-snapshot-max-frames", type=int,
                        help="explicit first-frame prefix bound, 1..128")
    parser.add_argument("--resident-snapshot-max-bytes", type=int,
                        help="explicit bytes PER snapshot, 2048..536870912, plus 512-byte lineage per frame")
    parser.add_argument("--resident-snapshot-rf-profile", type=Path,
                        help="existing absolute file byte-identical to --profile")
    parser.add_argument("--resident-snapshot-rf-profile-sha256",
                        help="pinned RF profile SHA-256; --sha256 already pins the source IQ")
    parser.add_argument("--map-diagnostics", action="store_true")
    parser.add_argument("--complex-cutout", type=Path,
                        help="new absolute replay-only complex CAF cutout path")
    parser.add_argument("--complex-cutout-delay-km", type=float)
    parser.add_argument("--complex-cutout-doppler-hz", type=float)
    parser.add_argument("--manifold-evidence-export", type=Path,
                        help="new absolute bounded JSONL path for identity-qualified native array evidence")
    parser.add_argument("--timeout", type=float, default=900)
    args = parser.parse_args(argv)
    if args.acquisition_sha256 is not None and not re.fullmatch(r"[a-f0-9]{64}", args.acquisition_sha256):
        parser.error("--acquisition-sha256 must be an externally trusted lowercase SHA-256")
    if args.map_diagnostics and not args.admission:
        parser.error("--map-diagnostics requires --admission")
    try:
        resident_snapshot_options(args)
        complex_cutout_options(args)
        manifold_export_options(args)
    except (ValueError, OSError, TypeError) as error:
        parser.error(str(error))
    return args


def main(argv=None):
    args = parse_args(argv)
    result = run(args)
    print(json.dumps({"output": str(args.output), "elapsed_seconds": result["elapsed_seconds"],
                      "detections": len(result["frames"]["detection"]),
                      "tracks": len(result["frames"]["track"])}))


if __name__ == "__main__":
    main()
