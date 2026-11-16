#!/usr/bin/env python3

import hashlib
import importlib.util
from pathlib import Path
import struct
import tempfile
import unittest


HERE = Path(__file__).resolve().parent
SPEC = importlib.util.spec_from_file_location(
    "replay_experiment_ledger", HERE / "replay_experiment_ledger.py")
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


def write_blah2iq(path, channels=5, sample_rate=2400000, frequency=509000000,
                  samples=4000):
    header = bytearray(64)
    header[:8] = b"BLAH2IQ\0"
    struct.pack_into("<IIIIII", header, 8, 1, 64, channels, sample_rate, frequency, 1)
    struct.pack_into("<QQ", header, 40, samples, 1)
    with path.open("wb") as stream:
        stream.write(header)
        for first in range(0, samples, 262144):
            count = min(262144, samples - first)
            stream.write(struct.pack("<IIQ", count, 0, first))
            stream.write(b"\0" * (count * channels * 8))


class ReplayExperimentLedgerTest(unittest.TestCase):
    def manifest(self):
        fingerprint = "a" * 64
        return {
            "schema": "vectorwarp-replay-experiment-v1",
            "recording": {"path": "capture.blah2iq", "sha256": fingerprint,
                          "sample_count": 4000, "channels": 5,
                          "sample_rate_hz": 2400000, "center_frequency_hz": 509000000,
                          "cpi_samples": 100, "format": "blah2iq-v1",
                          "hardware_epoch": "epoch-a"},
            "warmup_cpis": 8,
            "arms": [
                {"type": "baseline", "source_sha256": fingerprint,
                 "config_sha256": "b" * 64, "build_sha256": "c" * 64},
                {"type": "post_clutter_bootstrap", "source_sha256": fingerprint,
                 "config_sha256": "d" * 64, "build_sha256": "c" * 64,
                 "covariance": "diagonal", "maximum_cpis": 8,
                 "delay_motion_bins": 2, "doppler_motion_bins": 4},
                {"type": "temporal_shadow", "source_sha256": fingerprint,
                 "config_sha256": "e" * 64, "build_sha256": "c" * 64,
                 "lineage": True, "maximum_cpis": 8,
                 "delay_motion_bins": 2, "doppler_motion_bins": 4}
            ],
            "splits": [
                {"kind": "calibration", "warmup_first_sample": 0,
                 "first_sample": 800, "end_sample": 900,
                 "pass_id": "pass-a", "hardware_epoch": "epoch-a"},
                {"kind": "holdout", "warmup_first_sample": 1000,
                 "first_sample": 1800, "end_sample": 1900,
                 "pass_id": "pass-b", "hardware_epoch": "epoch-a"},
                {"kind": "negative_control", "warmup_first_sample": 2000,
                 "first_sample": 2800, "end_sample": 2900,
                 "pass_id": "pass-c", "hardware_epoch": "epoch-a"},
                {"kind": "miss", "warmup_first_sample": 3000,
                 "first_sample": 3800, "end_sample": 3900,
                 "pass_id": "pass-d", "hardware_epoch": "epoch-a"}
            ]
        }

    def test_accepts_complete_exact_iq_plan(self):
        self.assertEqual(MODULE.validate_manifest(self.manifest())["warmup_cpis"], 8)

    def test_rejects_overlap_and_missing_lineage(self):
        value = self.manifest()
        value["splits"][1]["warmup_first_sample"] = 50
        with self.assertRaisesRegex(ValueError, "sample-disjoint"):
            MODULE.validate_manifest(value)
        value = self.manifest()
        value["arms"][2]["lineage"] = False
        with self.assertRaisesRegex(ValueError, "lineage"):
            MODULE.validate_manifest(value)

    def test_verifies_exact_recording_bytes(self):
        value = self.manifest()
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            recording = root / "capture.blah2iq"
            write_blah2iq(recording)
            data = recording.read_bytes()
            value["recording"]["sha256"] = hashlib.sha256(data).hexdigest()
            for arm in value["arms"]:
                arm["source_sha256"] = value["recording"]["sha256"]
            MODULE.verify_artifact(MODULE.validate_manifest(value)["recording"],
                                   root, "recording")
            recording.write_bytes(data + b"changed")
            with self.assertRaisesRegex(ValueError, "do not match"):
                MODULE.verify_artifact(value["recording"], root, "recording")

    def test_rejects_non_blah2iq_and_header_geometry_mismatch(self):
        value = self.manifest()
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            recording = root / "capture.blah2iq"
            recording.write_bytes(b"arbitrary recording bytes")
            value["recording"]["sha256"] = hashlib.sha256(recording.read_bytes()).hexdigest()
            with self.assertRaisesRegex(ValueError, "not a BLAH2IQ"):
                MODULE.verify_artifact(value["recording"], root, "recording")
            write_blah2iq(recording, channels=4)
            value["recording"]["sha256"] = hashlib.sha256(recording.read_bytes()).hexdigest()
            with self.assertRaisesRegex(ValueError, "declared geometry"):
                MODULE.verify_artifact(value["recording"], root, "recording")
            write_blah2iq(recording, samples=999)
            value["recording"]["sha256"] = hashlib.sha256(recording.read_bytes()).hexdigest()
            with self.assertRaisesRegex(ValueError, "declared geometry"):
                MODULE.verify_artifact(value["recording"], root, "recording")

    def test_requires_warmup_samples_for_all_declared_cpis(self):
        value = self.manifest()
        value["recording"]["cpi_samples"] = 501
        with self.assertRaisesRegex(ValueError, "lacks samples"):
            MODULE.validate_manifest(value)

    def test_requires_verified_execution_receipts_and_outputs(self):
        value = self.manifest()
        with self.assertRaisesRegex(ValueError, "execution receipt"):
            MODULE.validate_manifest(value, require_execution=True)
        for index, arm in enumerate(value["arms"]):
            arm["execution"] = {"completed": True, "exit_code": 0,
                                "consumed_source_sha256": "a" * 64,
                                "executed_source_sha256": "a" * 64,
                                "executed_config_sha256": arm["config_sha256"],
                                "executed_build_sha256": arm["build_sha256"],
                                "warmup_cpis_observed": 8,
                                "evaluated_pass_ids": ["pass-a", "pass-b", "pass-c", "pass-d"]}
            arm["output"] = {"path": f"arm-{index}.jsonl",
                             "sha256": "f" * 64,
                             **({"format": "VWTBD01", "rows": 601,
                                 "columns": 1261, "record_count": 40}
                                if arm["type"] != "baseline" else {})}
        self.assertEqual(len(MODULE.validate_manifest(
            value, require_execution=True)["arms"]), 3)
        value["arms"][1]["execution"]["executed_config_sha256"] = "0" * 64
        with self.assertRaisesRegex(ValueError, "execution receipt"):
            MODULE.validate_manifest(value, require_execution=True)
        value["arms"][1]["execution"]["executed_config_sha256"] = value["arms"][1]["config_sha256"]
        value["arms"][1]["execution"]["executed_source_sha256"] = "0" * 64
        with self.assertRaisesRegex(ValueError, "execution receipt"):
            MODULE.validate_manifest(value, require_execution=True)


if __name__ == "__main__":
    unittest.main()
