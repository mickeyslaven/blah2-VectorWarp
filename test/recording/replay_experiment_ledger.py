#!/usr/bin/env python3
"""Validate leakage-safe exact-IQ bootstrap/temporal experiment manifests."""

import argparse
import hashlib
import json
import struct
import importlib.util
import subprocess
from pathlib import Path


ARM_TYPES = {"baseline", "post_clutter_bootstrap", "temporal_shadow"}
SPLIT_TYPES = {"calibration", "holdout", "negative_control", "miss"}
BLAH2IQ_MAGIC = b"BLAH2IQ\0"
BLAH2IQ_HEADER_BYTES = 64
BLAH2IQ_BLOCK_HEADER_BYTES = 16
BLAH2IQ_MAX_BLOCK_SAMPLES = 262144
BLAH2IQ_BYTES_PER_COMPLEX_F32 = 8
ACQUISITION_MAXIMUM_BYTES = 1 << 20
RECORDING_INSPECTOR = Path(__file__).resolve().parents[2] / "bin" / "inspectRecordingProvenance"

_ARTIFACT_SPEC = importlib.util.spec_from_file_location(
    "temporal_evidence_artifact", Path(__file__).with_name("temporal_evidence_artifact.py"))
_ARTIFACT = importlib.util.module_from_spec(_ARTIFACT_SPEC)
_ARTIFACT_SPEC.loader.exec_module(_ARTIFACT)


def _sha256(value, name):
    if not isinstance(value, str) or len(value) != 64 or any(
            character not in "0123456789abcdef" for character in value):
        raise ValueError(f"{name} sha256 is invalid")
    return value


def validate_manifest(value, require_execution=False):
    if not isinstance(value, dict) or value.get("schema") != "vectorwarp-replay-experiment-v1":
        raise ValueError("replay experiment schema is invalid")
    recording = value.get("recording")
    if not isinstance(recording, dict) or not isinstance(recording.get("path"), str):
        raise ValueError("recording path is required")
    fingerprint = _sha256(recording.get("sha256"), "recording")
    if recording.get("format") not in ("blah2iq-v1", "blah2iq-v2", "mchq"):
        raise ValueError("recording format must be completed blah2iq-v1, blah2iq-v2 or mchq")
    acquisition = recording.get("acquisition", {})
    if not isinstance(acquisition, dict) or set(acquisition) - {"require_exact", "sha256"}:
        raise ValueError("recording acquisition controls are invalid")
    if "require_exact" in acquisition and type(acquisition["require_exact"]) is not bool:
        raise ValueError("recording acquisition require_exact must be boolean")
    if "sha256" in acquisition:
        _sha256(acquisition["sha256"], "acquisition")
    for name in ("sample_count", "channels", "sample_rate_hz", "center_frequency_hz", "cpi_samples"):
        if type(recording.get(name)) is not int or recording[name] < 1:
            raise ValueError(f"recording {name} is required")
    if not isinstance(recording.get("hardware_epoch"), str) or not recording["hardware_epoch"]:
        raise ValueError("recording hardware_epoch is required")
    warmup = value.get("warmup_cpis")
    if type(warmup) is not int or warmup < 8:
        raise ValueError("at least eight warmup CPIs are required")
    if recording["sample_count"] < warmup * recording["cpi_samples"]:
        raise ValueError("recording lacks samples for the declared warmup CPIs")
    arms = value.get("arms")
    if not isinstance(arms, list) or len(arms) < 3:
        raise ValueError("baseline and both candidate arms are required")
    types = [arm.get("type") for arm in arms if isinstance(arm, dict)]
    if set(types) != ARM_TYPES or len(types) != len(set(types)):
        raise ValueError("experiment arms must be one baseline, bootstrap and temporal shadow")
    execution_receipts = []
    for arm in arms:
        _sha256(arm.get("config_sha256"), "arm config")
        _sha256(arm.get("build_sha256"), "arm build")
        if arm.get("source_sha256") != fingerprint:
            raise ValueError("all arms must consume the exact same IQ bytes")
        if arm["type"] == "temporal_shadow" and arm.get("lineage") is not True:
            raise ValueError("temporal shadow must retain evidence lineage")
        if arm["type"] == "post_clutter_bootstrap" and arm.get("covariance") not in (
                "diagonal", "shrinkage"):
            raise ValueError("bootstrap covariance must be diagonal or shrinkage")
        if arm["type"] in ("post_clutter_bootstrap", "temporal_shadow"):
            if arm.get("maximum_cpis") not in (4, 8) or \
                    type(arm.get("delay_motion_bins")) is not int or \
                    not 0 <= arm["delay_motion_bins"] <= 2 or \
                    type(arm.get("doppler_motion_bins")) is not int or \
                    not 0 <= arm["doppler_motion_bins"] <= 16:
                raise ValueError("candidate arm temporal contract is incomplete")
        if require_execution:
            receipt = arm.get("execution")
            output = arm.get("output")
            if not isinstance(receipt, dict) or receipt.get("completed") is not True or \
                    receipt.get("exit_code") != 0 or \
                    receipt.get("consumed_source_sha256") != fingerprint or \
                    receipt.get("executed_source_sha256") != fingerprint or \
                    receipt.get("executed_config_sha256") != arm["config_sha256"] or \
                    receipt.get("executed_build_sha256") != arm["build_sha256"] or \
                    type(receipt.get("warmup_cpis_observed")) is not int or \
                    receipt["warmup_cpis_observed"] < warmup:
                raise ValueError("arm execution receipt is incomplete")
            if not isinstance(output, dict) or not isinstance(output.get("path"), str):
                raise ValueError("arm output artifact is required")
            _sha256(output.get("sha256"), "arm output")
            if arm["type"] in ("post_clutter_bootstrap", "temporal_shadow") and \
                    output.get("format") != "VWTBD01":
                raise ValueError("candidate arm output must be a VWTBD01 artifact")
            if arm["type"] in ("post_clutter_bootstrap", "temporal_shadow"):
                for field in ("rows", "columns", "record_count"):
                    if type(output.get(field)) is not int or output[field] < 1:
                        raise ValueError("candidate arm output geometry is incomplete")
            execution_receipts.append(receipt)
    splits = value.get("splits")
    if not isinstance(splits, list) or not splits:
        raise ValueError("replay splits are required")
    kinds = set()
    intervals = []
    pass_ids = set()
    for split in splits:
        if not isinstance(split, dict) or split.get("kind") not in SPLIT_TYPES:
            raise ValueError("replay split kind is invalid")
        warmup, start, end = (split.get("warmup_first_sample"),
                              split.get("first_sample"), split.get("end_sample"))
        if type(warmup) is not int or type(start) is not int or type(end) is not int or \
                warmup < 0 or start <= warmup or end <= start or \
                end > recording["sample_count"]:
            raise ValueError("replay split sample bounds are invalid")
        if start - warmup < value["warmup_cpis"] * recording["cpi_samples"]:
            raise ValueError("replay split lacks its declared warmup samples")
        if not isinstance(split.get("pass_id"), str) or not split["pass_id"] or \
                split["pass_id"] in pass_ids:
            raise ValueError("replay splits require unique whole-pass identities")
        pass_ids.add(split["pass_id"])
        if split.get("hardware_epoch") != recording["hardware_epoch"]:
            raise ValueError("replay split hardware epoch differs from recording")
        for other_start, other_end in intervals:
            if max(warmup, other_start) < min(end, other_end):
                raise ValueError("replay warmup and evaluation intervals must be sample-disjoint")
        intervals.append((warmup, end))
        kinds.add(split["kind"])
    if not {"calibration", "holdout", "negative_control", "miss"}.issubset(kinds):
        raise ValueError("calibration, holdout, negative-control and miss splits are required")
    if require_execution:
        for receipt in execution_receipts:
            evaluated = receipt.get("evaluated_pass_ids")
            if not isinstance(evaluated, list) or len(evaluated) != len(pass_ids) or \
                    set(evaluated) != pass_ids:
                raise ValueError("arm execution receipt does not cover every declared pass")
    return value


def verify_artifact(artifact, root, name, recording_inspector=None):
    path = Path(artifact["path"])
    if not path.is_absolute():
        path = root / path
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    if digest.hexdigest() != artifact["sha256"]:
        raise ValueError(f"{name} bytes do not match the experiment manifest")
    if name == "recording":
        return verify_blah2iq_recording(artifact, root, recording_inspector)


def _inspect_acquisition(recording, path, recording_inspector=None):
    """Use production semantics, including RapidJSON double canonicalization.

    The inspector is a tool selected by the operator, never a path/command from
    the untrusted recording or its adjacent metadata. The experiment manifest's
    IQ and optional acquisition hashes are the external trust anchors.
    """
    inspector = Path(recording_inspector or RECORDING_INSPECTOR).resolve()
    if not inspector.is_file():
        raise ValueError("acquisition validation requires built inspectRecordingProvenance")
    controls = recording.get("acquisition", {})
    arguments = [str(inspector), str(path.resolve()),
                 "mchq" if recording["format"] == "mchq" else "blah2",
                 str(recording["channels"]), str(recording["sample_rate_hz"]),
                 str(recording["center_frequency_hz"]),
                 _sha256(recording.get("sha256"), "recording"),
                 controls.get("sha256", ""), "1" if controls.get("require_exact") else "0"]
    try:
        completed = subprocess.run(arguments, check=False, capture_output=True,
                                   text=True, timeout=30)
    except (OSError, subprocess.TimeoutExpired) as error:
        raise ValueError(f"acquisition inspector unavailable: {error}") from error
    if completed.returncode:
        raise ValueError("acquisition validation failed: " + completed.stderr.strip()[:2048])
    if len(completed.stdout) > 16384:
        raise ValueError("acquisition inspector output exceeds bound")
    try:
        result = json.loads(completed.stdout)
    except (ValueError, TypeError) as error:
        raise ValueError("acquisition inspector returned invalid JSON") from error
    if not isinstance(result, dict) or result.get("schema") != "vectorwarp-recording-inspection-v1" or \
            result.get("recording_sha256") != recording["sha256"] or \
            result.get("channels") != recording["channels"] or \
            result.get("sample_rate_hz") != recording["sample_rate_hz"] or \
            result.get("center_frequency_hz") != recording["center_frequency_hz"] or \
            result.get("sample_count") != str(recording["sample_count"]):
        raise ValueError("acquisition inspector identity differs from experiment")
    if result.get("exact_acquisition") is True and (
            result.get("acquisition_capable") is not True or
            result.get("acquisition_hash_trusted") is not True or
            result.get("acquisition_integrity_valid") is not True):
        raise ValueError("untrusted acquisition cannot be exact")
    if controls.get("require_exact") and (result.get("exact_acquisition") is not True or
            result.get("epoch") != recording["hardware_epoch"]):
        raise ValueError("exact acquisition identity/epoch differs from experiment")
    return result


def verify_blah2iq_recording(recording, root, recording_inspector=None):
    """Check bounded v1/v2 framing and production acquisition admission.

    MCHQ/MCHT parsing also delegates to the same production reader. This is
    acquisition reproducibility, never a physical-RF or exact-UTC receipt.
    """
    path = Path(recording["path"])
    if not path.is_absolute():
        path = root / path
    if recording["format"] == "mchq":
        return _inspect_acquisition(recording, path, recording_inspector)
    expected_channels = recording["channels"]
    expected_samples = recording["sample_count"]
    expected_rate = recording["sample_rate_hz"]
    expected_frequency = recording["center_frequency_hz"]
    size = path.stat().st_size
    with path.open("rb") as stream:
        header = stream.read(BLAH2IQ_HEADER_BYTES)
        if len(header) != BLAH2IQ_HEADER_BYTES or header[:8] != BLAH2IQ_MAGIC:
            raise ValueError("recording is not a BLAH2IQ file")
        version, header_size, channels, sample_rate, frequency, encoding = struct.unpack_from(
            "<IIIIII", header, 8)
        samples, complete = struct.unpack_from("<QQ", header, 40)
        if (version, header_size) not in ((1, BLAH2IQ_HEADER_BYTES), (2, 128)) or \
                (encoding, complete) != (1, 1):
            raise ValueError("recording BLAH2IQ header is unsupported or incomplete")
        if recording["format"] != f"blah2iq-v{version}":
            raise ValueError("recording BLAH2IQ version differs from experiment")
        if channels != expected_channels or sample_rate != expected_rate or \
                frequency != expected_frequency or samples != expected_samples:
            raise ValueError("recording BLAH2IQ header differs from declared geometry")
        if version == 2:
            length = struct.unpack_from("<I", header, 32)[0]
            if not 0 < length <= ACQUISITION_MAXIMUM_BYTES or 128 + length > size:
                raise ValueError("recording acquisition metadata exceeds bound or is truncated")
            embedded_hash = stream.read(64)
            acquisition_json = stream.read(length)
            if hashlib.sha256(acquisition_json).hexdigest().encode("ascii") != embedded_hash:
                raise ValueError("recording embedded acquisition SHA-256 mismatch")
        position = 0
        while position < samples:
            block = stream.read(BLAH2IQ_BLOCK_HEADER_BYTES)
            if len(block) != BLAH2IQ_BLOCK_HEADER_BYTES:
                raise ValueError("recording BLAH2IQ block header is truncated")
            count, reserved, first = struct.unpack("<IIQ", block)
            if not count or count > BLAH2IQ_MAX_BLOCK_SAMPLES or reserved != 0 or first != position or \
                    count > samples - position:
                raise ValueError("recording BLAH2IQ block geometry is invalid")
            payload_bytes = count * channels * BLAH2IQ_BYTES_PER_COMPLEX_F32
            if payload_bytes > size - stream.tell():
                raise ValueError("recording BLAH2IQ payload is truncated")
            stream.seek(payload_bytes, 1)
            position += count
        if stream.tell() != size:
            raise ValueError("recording BLAH2IQ file has trailing bytes")
    if version == 2 or Path(str(path) + ".acquisition.json").exists() or recording.get("acquisition"):
        return _inspect_acquisition(recording, path, recording_inspector)
    return {"exact_acquisition": False, "acquisition_integrity_valid": False,
            "acquisition_hash_trusted": False,
            "acquisition_reason": "Legacy recording has no acquisition contract"}


def verify_temporal_output(arm, manifest, root):
    output = arm["output"]
    path = Path(output["path"])
    if not path.is_absolute():
        path = root / path
    index = _ARTIFACT.index_artifact(path)
    try:
        records = index["records"]
        expected_statistic = "post_clutter_bootstrap" if \
            arm["type"] == "post_clutter_bootstrap" else arm.get("input_statistic", "fused")
        if index["input_statistic"] != expected_statistic or \
                (arm["type"] == "post_clutter_bootstrap" and
                 index["covariance"] != arm["covariance"]) or \
                index["mature_cpis"] != arm["maximum_cpis"] or \
                index["delay_motion_bins"] != arm["delay_motion_bins"] or \
                index["doppler_motion_bins"] != arm["doppler_motion_bins"] or \
                index["capture_cpi_samples"] != manifest["recording"]["cpi_samples"] or \
                index["mature_cpis"] > manifest["warmup_cpis"] or \
                len(records) != output["record_count"] or not records or \
                any(record["rows"] != output["rows"] or
                    record["columns"] != output["columns"] for record in records):
            raise ValueError("candidate artifact identity or geometry differs from the manifest")
        for split in manifest["splits"]:
            warmup_records = [record for record in records
                if record["capture_first_sample"] >= split["warmup_first_sample"] and
                record["capture_end_sample"] < split["first_sample"]]
            evaluated_records = [record for record in records
                if record["capture_end_sample"] >= split["first_sample"] and
                record["capture_first_sample"] < split["end_sample"]]
            if len(warmup_records) < manifest["warmup_cpis"] or not evaluated_records:
                raise ValueError("candidate artifact does not cover declared split warmup/evaluation")
    finally:
        _ARTIFACT.close_artifact(index)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("manifest", type=Path)
    parser.add_argument("--verify-recording", action="store_true")
    parser.add_argument("--require-execution", action="store_true")
    parser.add_argument("--recording-inspector", type=Path,
                        help="Explicit trusted native inspection tool; never read from acquisition metadata")
    arguments = parser.parse_args()
    manifest = validate_manifest(json.loads(arguments.manifest.read_text()),
                                 require_execution=arguments.require_execution)
    acquisition = None
    if arguments.verify_recording or arguments.require_execution:
        acquisition = verify_artifact(manifest["recording"], arguments.manifest.parent,
                                      "recording", arguments.recording_inspector)
    if arguments.require_execution:
        for arm in manifest["arms"]:
            verify_artifact(arm["output"], arguments.manifest.parent,
                            f"{arm['type']} output")
            if arm["type"] in ("post_clutter_bootstrap", "temporal_shadow"):
                verify_temporal_output(arm, manifest, arguments.manifest.parent)
    print(json.dumps({"valid": True, "arms": len(manifest["arms"]),
                      "splits": len(manifest["splits"]), "acquisition": acquisition}, separators=(",", ":")))


if __name__ == "__main__":
    main()
