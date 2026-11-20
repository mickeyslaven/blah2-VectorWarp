#!/usr/bin/env python3

import hashlib
import importlib.util
import json
import os
from pathlib import Path
import struct
import tempfile
import unittest


HERE = Path(__file__).resolve().parent
SPEC = importlib.util.spec_from_file_location(
    "replay_experiment_ledger", HERE / "replay_experiment_ledger.py")
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)
if os.environ.get("VECTORWARP_TEST_RECORDING_INSPECTOR"):
    MODULE.RECORDING_INSPECTOR = Path(os.environ["VECTORWARP_TEST_RECORDING_INSPECTOR"])


def write_blah2iq(path, channels=5, sample_rate=2400000, frequency=509000000,
                  samples=4000, acquisition=None):
    header = bytearray(64)
    header[:8] = b"BLAH2IQ\0"
    struct.pack_into("<IIIIII", header, 8, 2 if acquisition is not None else 1,
                     128 if acquisition is not None else 64, channels, sample_rate, frequency, 1)
    struct.pack_into("<QQ", header, 40, samples, 1)
    if acquisition is not None:
        struct.pack_into("<I", header, 32, len(acquisition))
    with path.open("wb") as stream:
        stream.write(header)
        if acquisition is not None:
            stream.write(hashlib.sha256(acquisition).hexdigest().encode("ascii"))
            stream.write(acquisition)
        for first in range(0, samples, 262144):
            count = min(262144, samples - first)
            stream.write(struct.pack("<IIQ", count, 0, first))
            stream.write(b"\0" * (count * channels * 8))


def acquisition_contract(receiver="Kraken", channels=5):
    return {"schema": "vectorwarp-recording-acquisition-v1", "receiver": receiver,
            "channels": channels, "sampleRateHz": 2400000, "frequencyHz": 509000000,
            "epoch": "7", "firstSample": "9007199254740993", "endSample": "9007199254744993",
            "sourcePositionAvailable": True, "frameSequenceSource": "recorder-ingress",
            "metadataAvailable": True,
            "frames": [{"sequence": "17", "sourceFirst": "9007199254740993",
                        "sourceCount": "4000", "sliceOffset": "0", "count": "4000"}],
            "gainProvenance": "sample-aligned-readback",
            "perChannelGainDb": [14.199999809265137] * channels,
            "receiverDetails": {"requestedGain": 15, "diagnosticFraction": 1e-7},
            "topology": {"referenceMode": "dedicated", "referenceChannels": [0],
                         "surveillanceChannels": list(range(1, channels)), "coherent": True},
            "geometry": None, "geometrySha256": hashlib.sha256(b"null").hexdigest(),
            "geometryConfirmed": False, "manifoldSha256": {},
            "configurationSha256": "a" * 64, "codeSha256": "b" * 64}


def encode_contract(contract):
    # This fixture deliberately exercises RapidJSON's exponent spelling. It is
    # not a general canonicalizer; production canonicalization is native.
    return json.dumps(contract, separators=(",", ":"), allow_nan=False).replace("1e-07", "1e-7").encode()


def write_acquisition_manifest(path, contract):
    digest = hashlib.sha256(encode_contract(contract)).hexdigest()
    value = {"schema": "vectorwarp-acquisition-manifest-v1", "complete": True,
             "recordingSha256": hashlib.sha256(path.read_bytes()).hexdigest(),
             "acquisitionSha256": digest, "acquisition": contract}
    # Pretty whitespace and Python's exponent spelling differ from the hash's
    # canonical representation. The actual reader must still accept it.
    Path(str(path) + ".acquisition.json").write_text(json.dumps(value, indent=2))
    return digest


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

    def recorded(self, root, *, contract=None, adjacent=False):
        path = root / "capture.blah2iq"
        contract = acquisition_contract() if contract is None else contract
        write_blah2iq(path, channels=contract["channels"],
                      acquisition=None if adjacent else encode_contract(contract))
        recording = self.manifest()["recording"]
        recording.update(format="blah2iq-v1" if adjacent else "blah2iq-v2", hardware_epoch="7",
                         channels=contract["channels"], sha256=hashlib.sha256(path.read_bytes()).hexdigest())
        if adjacent:
            recording["acquisition"] = {"sha256": write_acquisition_manifest(path, contract)}
        return path, recording

    def test_v2_embedded_exact_round_trip_and_precision(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary); _, recording = self.recorded(root)
            recording["acquisition"] = {"require_exact": True}
            result = MODULE.verify_artifact(recording, root, "recording")
            self.assertTrue(result["exact_acquisition"])
            self.assertEqual(result["source_first_sample"], "9007199254740993")
            self.assertFalse(result["exact_utc"])
            recording["hardware_epoch"] = "8"
            with self.assertRaisesRegex(ValueError, "epoch"):
                MODULE.verify_artifact(recording, root, "recording")

    def test_adjacent_requires_external_metadata_hash_not_self_declared(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary); _, recording = self.recorded(root, adjacent=True)
            trusted = recording["acquisition"]["sha256"]
            recording["acquisition"] = {}
            result = MODULE.verify_artifact(recording, root, "recording")
            self.assertTrue(result["acquisition_integrity_valid"])
            self.assertTrue(result["acquisition_capable"])
            self.assertFalse(result["exact_acquisition"])
            recording["acquisition"] = {"require_exact": True}
            with self.assertRaisesRegex(ValueError, "Exact acquisition"):
                MODULE.verify_artifact(recording, root, "recording")
            recording["acquisition"]["sha256"] = trusted
            self.assertTrue(MODULE.verify_artifact(recording, root, "recording")["exact_acquisition"])
            recording["acquisition"]["sha256"] = "f" * 64
            with self.assertRaisesRegex(ValueError, "trusted replay metadata"):
                MODULE.verify_artifact(recording, root, "recording")

    def test_embedded_tamper_and_untrusted_rehashed_metadata(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary); path, recording = self.recorded(root)
            original = path.read_bytes(); changed = bytearray(original)
            changed[128 + 30] ^= 1; path.write_bytes(changed)
            with self.assertRaisesRegex(ValueError, "do not match"):
                MODULE.verify_artifact(recording, root, "recording")
            recording["sha256"] = hashlib.sha256(changed).hexdigest()
            with self.assertRaisesRegex(ValueError, "embedded acquisition SHA-256"):
                MODULE.verify_artifact(recording, root, "recording")
            path, recording = self.recorded(root, adjacent=True)
            altered = acquisition_contract(); altered["codeSha256"] = "c" * 64
            write_acquisition_manifest(path, altered)  # attacker updates its own hash too
            with self.assertRaisesRegex(ValueError, "trusted replay metadata"):
                MODULE.verify_artifact(recording, root, "recording")

    def test_missing_fields_and_lineage_fail_semantic_validation_even_when_rehashed(self):
        for field in ("perChannelGainDb", "topology", "geometry", "manifoldSha256", "frames", "codeSha256"):
            with self.subTest(field=field), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary); contract = acquisition_contract(); del contract[field]
                _, recording = self.recorded(root, contract=contract)
                with self.assertRaisesRegex(ValueError, "acquisition validation failed"):
                    MODULE.verify_artifact(recording, root, "recording")
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary); contract = acquisition_contract(); contract["frames"][0]["count"] = "3999"
            _, recording = self.recorded(root, contract=contract)
            with self.assertRaisesRegex(ValueError, "frame count"):
                MODULE.verify_artifact(recording, root, "recording")

    def test_all_receiver_topologies_and_requested_gain_rejection(self):
        for receiver in ("Kraken", "Usrp", "HackRF", "RspDuo"):
            with self.subTest(receiver=receiver), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary); contract = acquisition_contract(receiver, 5 if receiver == "Kraken" else 2)
                requested = receiver in ("HackRF", "RspDuo")
                if requested:
                    contract["gainProvenance"] = "requested-settings-only"
                _, recording = self.recorded(root, contract=contract)
                result = MODULE.verify_artifact(recording, root, "recording")
                self.assertEqual(result["exact_acquisition"], not requested)
                recording["acquisition"] = {"require_exact": True}
                if requested:
                    with self.assertRaisesRegex(ValueError, "Exact acquisition"):
                        MODULE.verify_artifact(recording, root, "recording")
                else:
                    self.assertTrue(MODULE.verify_artifact(recording, root, "recording")["exact_acquisition"])

    def test_v1_playback_preserved_but_missing_contract_cannot_be_exact(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary); path = root / "capture.blah2iq"; write_blah2iq(path)
            recording = self.manifest()["recording"]
            recording["sha256"] = hashlib.sha256(path.read_bytes()).hexdigest()
            self.assertFalse(MODULE.verify_artifact(recording, root, "recording")["exact_acquisition"])
            recording["acquisition"] = {"require_exact": True}
            with self.assertRaisesRegex(ValueError, "Exact acquisition"):
                MODULE.verify_artifact(recording, root, "recording")

    def test_inspector_missing_and_format_mismatch_fail_closed(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary); _, recording = self.recorded(root)
            with self.assertRaisesRegex(ValueError, "requires built"):
                MODULE.verify_artifact(recording, root, "recording", root / "not-built")
            recording["format"] = "blah2iq-v1"
            with self.assertRaisesRegex(ValueError, "version differs"):
                MODULE.verify_artifact(recording, root, "recording")

    def test_verified_execution_receipts_bind_outputs_and_configs(self):
        value = self.manifest()
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

    def test_new_format_plan_and_exact_control_validation(self):
        for format_name in ("blah2iq-v1", "blah2iq-v2", "mchq"):
            value = self.manifest(); value["recording"]["format"] = format_name
            value["recording"]["acquisition"] = {"require_exact": True, "sha256": "b" * 64}
            MODULE.validate_manifest(value)
        for controls in ({"require_exact": "true"}, {"sha256": "not-a-sha"}, {"exact": True}):
            value = self.manifest(); value["recording"]["acquisition"] = controls
            with self.assertRaisesRegex(ValueError, "acquisition"):
                MODULE.validate_manifest(value)

    def test_native_mcht_adjacent_manifest_and_payload_binding(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary); path = root / "capture.mchq"
            contract = acquisition_contract(); first = int(contract["firstSample"])
            contract["gainProvenance"] = "sample-aligned-receiver-header"
            contract["perChannelGainDb"] = [15.0] * 5
            payload = bytes([127, 128]) * 4000 * 5
            header = struct.pack(">8I", 0x4d434854, 5, 4000, 4, 0, 11, 2, 0)
            channels = b"".join(struct.pack("<ff", 509000000, 15) for _ in range(5))
            timing = struct.pack(">4I10Q", 1, 96, 1, 0, 7, first, 17, 1000,
                                 0, 1000, 100, 0, 17, first + 4000)
            path.write_bytes(header + channels + timing + payload)
            acquisition_hash = write_acquisition_manifest(path, contract)
            recording = self.manifest()["recording"]
            recording.update(path=path.name, format="mchq", hardware_epoch="7",
                             sha256=hashlib.sha256(path.read_bytes()).hexdigest(),
                             acquisition={"require_exact": True, "sha256": acquisition_hash})
            self.assertTrue(MODULE.verify_artifact(recording, root, "recording")["exact_acquisition"])
            data = bytearray(path.read_bytes()); data[-1] ^= 1; path.write_bytes(data)
            # Even an updated experiment IQ hash cannot repair the old manifest
            # binding to a different payload.
            recording["sha256"] = hashlib.sha256(data).hexdigest()
            with self.assertRaisesRegex(ValueError, "different recording bytes"):
                MODULE.verify_artifact(recording, root, "recording")
if __name__ == "__main__":
    unittest.main()
