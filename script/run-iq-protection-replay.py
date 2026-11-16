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
import socket
import statistics
import subprocess
import tempfile
import time


HERE = Path(__file__).resolve().parents[1]
HARNESS_PATH = HERE / "test/recording/processor_replay_test.py"
SPEC = importlib.util.spec_from_file_location("vectorwarp_processor_replay", HARNESS_PATH)
HARNESS = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(HARNESS)

# Match ResidentReplaySnapshot.h. max_bytes bounds EACH snapshot, not the run.
RESIDENT_SNAPSHOT_FRAME_LIMIT = 128
RESIDENT_SNAPSHOT_BYTE_LIMIT = 512 << 20
RESIDENT_SNAPSHOT_HEADER_BYTES = 2048
RESIDENT_LINEAGE_BYTES = 512


class ScientificSinkGroup(HARNESS.SinkGroup):
    def __init__(self, retain_maps=False):
        super().__init__()
        self.payloads["detection"] = bytearray()
        self.payloads["track"] = bytearray()
        self.payload_limits = {name: 1 << 20 for name in self.payloads}
        if retain_maps:
            self.payload_limits["map"] = 128 << 20

    def _serve(self, name, listener):
        while not self.stop.is_set():
            try:
                connection, _ = listener.accept()
            except socket.timeout:
                continue
            except OSError:
                return
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
                        limit = self.payload_limits[name]
                        remaining = limit - len(self.payloads[name])
                        if remaining > 0:
                            self.payloads[name].extend(block[:remaining])


def json_values(payload, name):
    if not payload:
        return []
    return HARNESS.json_values(payload, name)


def admitted_truth(admission):
    points = {}
    for capture in admission.get("captures", {}).values():
        if not capture.get("valid"):
            continue
        admitted = {row.get("timestamp_ms") for row in capture.get("cpis", [])}
        if None in admitted:
            raise ValueError("admitted CPI has no timestamp")
        for row in capture.get("truth_samples", []):
            delay, doppler = row.get("delay_km"), row.get("doppler_hz")
            if all(isinstance(value, (int, float)) and math.isfinite(value)
                   for value in (delay, doppler)):
                timestamp = row.get("timestamp_ms")
                if not isinstance(timestamp, int):
                    raise ValueError("admitted truth sample has no integer timestamp")
                if timestamp not in admitted:
                    continue
                value = (float(delay), float(doppler))
                if timestamp in points and points[timestamp] != value:
                    raise ValueError("conflicting admitted truth at one CPI endpoint")
                points[timestamp] = value
        missing = admitted - points.keys()
        if missing:
            raise ValueError("admitted CPI has no exact evaluation truth sample")
    if not points:
        raise ValueError("map diagnostics require valid admission truth samples")
    return points


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


def file_sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


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
        if args.acceleration == "gpu-resident":
            raise ValueError("gpu-resident requires the complete resident snapshot export")
        return None
    if not all(present):
        raise ValueError("all five --resident-snapshot-* options are required together")
    if args.acceleration != "gpu-resident":
        raise ValueError("resident snapshot export requires --acceleration gpu-resident")
    if args.diagnostic_cpu_fusion or args.protection_beams:
        raise ValueError("resident export cannot use CPU-fusion or CPU-only target-protection overrides")
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


def configuration(recording, profile, status_port, ports, acceleration,
                  protection_beams, protection_index, diagnostic_cpu_fusion,
                  diagnostic_delay_max, diagnostic_doppler_min,
                  diagnostic_doppler_max, diagnostic_clutter_delay_max,
                  resident_snapshot=None):
    if (acceleration == "gpu-resident") != (resident_snapshot is not None):
        raise ValueError("gpu-resident and exact resident snapshot export must be selected together")
    if resident_snapshot is not None:
        validate_resident_snapshot(resident_snapshot)
        if diagnostic_cpu_fusion or protection_beams:
            raise ValueError("resident export cannot use CPU-only diagnostic overrides")
        if profile.get("configuration_complete") is not True:
            raise ValueError("resident export requires a complete replay profile")
        if profile["process"].get("multi_illuminator", {}).get("enabled"):
            raise ValueError("resident export requires single-lane replay")
    capture = profile["capture"]
    process = copy.deepcopy(profile["process"])
    # Portable profiles can carry the live artifact-only observer. Native
    # replay rejects that live-capture contract, and it is not an input to the
    # production detector or tracker. Disable only that observer offline.
    live_shadow = process.get("detection", {}).get("live_temporal_shadow")
    if isinstance(live_shadow, dict):
        live_shadow["enabled"] = False
        live_shadow["artifact_only"] = False
    causal_funnel = process.get("evidence", {}).get("causal_funnel")
    if isinstance(causal_funnel, dict):
        causal_funnel["enabled"] = False
    process["performance"] = {
        "surveillance_workers": 1,
        "fft_threads": 1,
        "acceleration": acceleration,
    }
    if diagnostic_delay_max is not None:
        process.setdefault("ambiguity", {})["delayMax"] = diagnostic_delay_max
    if diagnostic_doppler_min is not None:
        process.setdefault("ambiguity", {})["dopplerMin"] = diagnostic_doppler_min
    if diagnostic_doppler_max is not None:
        process.setdefault("ambiguity", {})["dopplerMax"] = diagnostic_doppler_max
    if diagnostic_clutter_delay_max is not None:
        clutter = process.setdefault("clutter", {})
        clutter["delayMax"] = diagnostic_clutter_delay_max
        clutter["followAmbiguity"] = False
    if diagnostic_cpu_fusion:
        # Preserve the complete spatial beam bank and array geometry while
        # replacing only the resident-only noise whitening for this bounded
        # CPU diagnostic. Every compared arm uses this identical fusion path.
        original_fusion = process.get("surveillance_fusion", {})
        process["surveillance_fusion"] = {
            "mode": "coherent_beam_bank",
            "beams": original_fusion.get("beams", 12),
            "effective_beam_trials": original_fusion.get(
                "effective_beam_trials", original_fusion.get("beams", 12)),
            "direct_path_null_db": original_fusion.get("direct_path_null_db", 0),
        }
        tbd = process.setdefault("detection", {}).setdefault("track_before_detect", {})
        tbd["enable"] = False
        tbd["fractional_delay"] = False
        live_shadow = process["detection"].setdefault("live_temporal_shadow", {})
        live_shadow["enabled"] = False
        live_shadow["artifact_only"] = False
        process.setdefault("tracker", {}).setdefault("track_guided_shadow", {})["enable"] = False
        process["tracker"]["track_guided_shadow"]["promote"] = False
        process["tracker"]["track_guided_shadow"]["sustain"] = False
        process["tracker"].setdefault("initiate", {}).setdefault(
            "stationary_clutter", {})["enable"] = False
        process.setdefault("evidence", {}).setdefault("causal_funnel", {})["enabled"] = False
    reference = process.setdefault("reference_synthesis", {})
    if protection_beams:
        reference["target_protection_beams"] = protection_beams
        reference["target_protection_beam_index"] = protection_index
    else:
        reference.pop("target_protection_beams", None)
        reference.pop("target_protection_beam_index", None)
    device = {
        "type": capture["source_receiver"],
        "channel_count": capture["channel_count"],
        "reference_channel": capture["reference_channel"],
        "surveillance_channels": capture["surveillance_channels"],
        "array_geometry": capture["array_geometry"],
    }
    config = {
        "capture": {
            "fs": capture["sample_rate_hz"],
            "fc": capture["frequency_hz"],
            "device": device,
            "replay": {
                "state": True,
                "loop": False,
                "file": str(recording),
                "format": "auto",
                "provenance": "legacy-unqualified",
            },
        },
        "process": process,
        "location": profile["location"],
        "network": {"ip": HARNESS.HOST,
                    "ports": dict(api=status_port, **ports)},
        "save": {"iq": False, "map": False, "detection": False,
                 "timing": False, "path": str(recording.parent)},
    }
    if resident_snapshot is not None:
        config["capture"]["replay"]["resident_snapshot"] = copy.deepcopy(resident_snapshot)
    return config


def run(args):
    snapshot = resident_snapshot_options(args)
    profile = json.loads(args.profile.read_text())
    if profile.get("configuration_complete") is False:
        raise ValueError("recording replay profile is incomplete")
    metadata = json.loads(args.metadata.read_text())
    if metadata.get("dataSha256") != args.sha256:
        raise ValueError("recording digest does not match declared metadata")
    if file_sha256(args.recording) != args.sha256:
        raise ValueError("recording bytes do not match declared digest")
    if snapshot is not None:
        device = os.environ.get("BLAH2_GPU_DEVICE", "").strip()
        if not device or device == "auto":
            raise ValueError("resident export requires an explicit BLAH2_GPU_DEVICE")
    status = HARNESS.StatusServer()
    sinks = ScientificSinkGroup(args.map_diagnostics)
    processor = None
    started = time.monotonic()
    with tempfile.TemporaryDirectory(prefix="vectorwarp-iq-protection-") as temporary:
        root = Path(temporary)
        try:
            status.start()
            sinks.start()
            config = configuration(args.recording, profile, status.port,
                                   sinks.ports(), args.acceleration,
                                   args.protection_beams, args.protection_index,
                                   args.diagnostic_cpu_fusion,
                                   args.diagnostic_delay_max,
                                   args.diagnostic_doppler_min,
                                   args.diagnostic_doppler_max,
                                   args.diagnostic_clutter_delay_max,
                                   resident_snapshot=snapshot)
            config_path = root / "config.json"
            config_path.write_text(json.dumps(config))
            process_env = os.environ.copy()
            process_env.update({
                "OPENBLAS_NUM_THREADS": "1", "OMP_NUM_THREADS": "1",
                "BLIS_NUM_THREADS": "1", "MKL_NUM_THREADS": "1",
                "VECLIB_MAXIMUM_THREADS": "1", "NUMEXPR_NUM_THREADS": "1",
                "VECTORWARP_FFTW_PLAN": "estimate",
            })
            processor = subprocess.Popen(
                ["stdbuf", "-oL", "-eL", str(args.binary), "-c", str(config_path)], cwd=root,
                env=process_env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                start_new_session=True)
            deadline = time.monotonic() + args.timeout
            complete = False
            while time.monotonic() < deadline:
                if any(item.get("state") == "complete" for item in status.snapshot()):
                    complete = True
                    break
                if processor.poll() is not None:
                    output = processor.stdout.read() if processor.stdout else ""
                    raise RuntimeError(
                        f"processor exited before completion ({processor.returncode}): "
                        f"{output[-12000:]}")
                time.sleep(.05)
            if not complete:
                HARNESS.stop_process(processor, timeout=10)
                output = processor.stdout.read() if processor.stdout else ""
                raise RuntimeError(
                    f"replay did not complete: {status.snapshot()[-3:]}; "
                    f"processor tail: {output[-12000:]}")
            forced = HARNESS.stop_process(processor, timeout=10)
            output = processor.stdout.read() if processor.stdout else ""
            if forced or processor.returncode != 0:
                raise RuntimeError(
                    f"processor stop failed ({processor.returncode}): {output[-12000:]}")
            frames = {
                name: json_values(sinks.payloads[name], name)
                for name in ("detection", "track", "timing")
            }
            maps = json_values(sinks.payloads["map"], "map") if args.map_diagnostics else []
            timeline = json.loads(args.timeline.read_text())
            endpoints = timeline.get("frames", [])
            expected_frames = len(endpoints)
            for name, values in frames.items():
                if len(values) != expected_frames:
                    raise RuntimeError(
                        f"{name}/timeline endpoint count differs: {len(values)} != {expected_frames}")
            map_result = None
            if args.map_diagnostics:
                if sinks.bytes["map"] > sinks.payload_limits["map"]:
                    raise RuntimeError("map diagnostic payload exceeded its declared bound")
                map_result = map_diagnostics(maps, endpoints,
                    admitted_truth(json.loads(args.admission.read_text())))
            final = status.snapshot()[-1]
            if any(final.get("captureDroppedSamples", [])):
                raise RuntimeError("lossless replay dropped capture samples")
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
                "arm": {
                    "kind": "target_protection" if args.protection_beams else "baseline",
                    "beams": args.protection_beams,
                    "index": args.protection_index if args.protection_beams else None,
                },
                "acceleration": args.acceleration,
                "diagnostic_cpu_fusion": args.diagnostic_cpu_fusion,
                "diagnostic_window": {
                    "delay_max_bins": args.diagnostic_delay_max,
                    "doppler_min_hz": args.diagnostic_doppler_min,
                    "doppler_max_hz": args.diagnostic_doppler_max,
                    "clutter_delay_max_bins": args.diagnostic_clutter_delay_max,
                },
                "elapsed_seconds": time.monotonic() - started,
                "frames": frames,
                "map_diagnostics": map_result,
                "processor_log_tail": output[-12000:],
                "final_status": final,
            }
            if snapshot is not None:
                result["resident_snapshot_export"] = resident_snapshot_receipt(
                    snapshot, expected_frames)
            args.output.write_text(json.dumps(result, indent=2) + "\n")
            return result
        finally:
            if processor is not None:
                HARNESS.stop_process(processor)
                if processor.stdout:
                    processor.stdout.close()
            sinks.close()
            status.close()


def parse_args(argv=None):
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--recording", type=Path, required=True)
    parser.add_argument("--metadata", type=Path, required=True)
    parser.add_argument("--profile", type=Path, required=True)
    parser.add_argument("--timeline", type=Path, required=True)
    parser.add_argument("--admission", type=Path)
    parser.add_argument("--sha256", required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--acceleration", choices=("cpu", "gpu", "gpu-resident"), default="cpu")
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
    parser.add_argument("--diagnostic-cpu-fusion", action="store_true",
                        help="replace resident-only noise whitening with the same CPU coherent beam bank in every arm")
    parser.add_argument("--map-diagnostics", action="store_true")
    parser.add_argument("--diagnostic-delay-max", type=int)
    parser.add_argument("--diagnostic-doppler-min", type=int)
    parser.add_argument("--diagnostic-doppler-max", type=int)
    parser.add_argument("--diagnostic-clutter-delay-max", type=int)
    parser.add_argument("--protection-beams", type=int, default=0)
    parser.add_argument("--protection-index", type=int, default=0)
    parser.add_argument("--timeout", type=float, default=900)
    args = parser.parse_args(argv)
    if args.protection_beams and not (4 <= args.protection_beams <= 360 and
                                      0 <= args.protection_index < args.protection_beams):
        parser.error("invalid target-protection beam bank/index")
    if not args.protection_beams and args.protection_index:
        parser.error("baseline cannot select a protection index")
    if args.map_diagnostics and not args.admission:
        parser.error("--map-diagnostics requires --admission")
    if ((args.diagnostic_doppler_min is None) !=
            (args.diagnostic_doppler_max is None)):
        parser.error("diagnostic Doppler bounds must be supplied together")
    if (args.diagnostic_doppler_min is not None and
            args.diagnostic_doppler_min >= args.diagnostic_doppler_max):
        parser.error("diagnostic Doppler bounds are empty")
    try:
        resident_snapshot_options(args)
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
