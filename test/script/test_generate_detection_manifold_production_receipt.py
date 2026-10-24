import hashlib
import importlib.util
import json
import math
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


SCRIPT = Path(__file__).parents[2] / "script" / "generate-detection-manifold-production-receipt.py"
SPEC = importlib.util.spec_from_file_location("production_receipt", SCRIPT)
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


class ProductionReceiptTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        root = Path(self.temporary.name)
        self.corpus, self.candidate, self.iq = root / "corpus.jsonl", root / "candidate.json", root / "iq.raw"
        self.config, self.native, self.generator = root / "config.json", root / "native.json", root / "native-generator"
        self.corpus.write_bytes(b'{"sample":1}\n')
        responses = [[[1, 0], [0, 1], [-1, 0]],
                     [[0, 1], [-1, 0], [0, -1]],
                     [[-1, 0], [0, -1], [1, 0]]]
        candidate_bank_sha256 = MODULE.canonical_hash(responses)
        self.candidate.write_text(json.dumps({"schema": "vectorwarp-detection-manifold-runtime-v1",
                                              "runtime_compatible": True,
                                              "beams": 3,
                                              "bank_sha256": candidate_bank_sha256,
                                              "elevation_range_deg": [0, 10],
                                              "responses": responses}))
        self.iq.write_bytes(b"same iq bytes")
        self.generator.write_bytes(b"trusted native generator executable")
        self.generator.chmod(0o700)
        self.generator_sha256 = digest(self.generator)
        config = {"capture": {"fc": 539000000, "fs": 2400000, "device": {
            "channel_count": 3, "surveillance_channels": [0, 1, 2], "heimdall": {"gain": 12},
            "array_geometry": {"mapping_confirmed": True, "geometry_confirmed": True,
                "orientation_confirmed": True, "x_axis_bearing_deg_true": 0,
                "elements": [{"daq_channel": 0, "position": [0, 0, 0]},
                             {"daq_channel": 1, "position": [.1, 0, 0]},
                             {"daq_channel": 2, "position": [0, .1, 0]}]}}},
            "location": {"rx": {"latitude": 40, "longitude": -80, "altitude": 300},
                         "tx": {"latitude": 40.1, "longitude": -80, "altitude": 400}},
            "process": {"detection": {"pfa": .1, "nGuard": 1, "nTrain": 2,
                "integration_cpis": 1}, "surveillance_fusion": {"mode": "coherent_beam_bank",
                "beams": 3, "direct_path_null_db": 0,
                "effective_beam_trials": 3, "detection_manifold_replay": {
                    "trusted_native_generator_sha256": [self.generator_sha256]}}}}
        self.config.write_text(json.dumps(config, sort_keys=True))
        gain = {"requested_common_gain": 12, "requested_channel_gains": None,
                "actual_channel_gains_db": [12, 12, 12]}
        baseline_descriptor = {"frequency_hz": 539000000, "beams": 3,
            "positions_m": [[0, 0, 0], [.1, 0, 0], [0, .1, 0]],
            "direct_path_azimuth_rad": 0, "direct_path_null_db": 0}
        baseline_bank_sha256 = MODULE.canonical_hash(baseline_descriptor)
        bank = lambda value: {"bank_sha256": value, "cut_power_by_beam": [1, 5, 2],
                              "training_power_by_offset_beam": [
                                  {"offset": offset, "power_by_beam": [power / 2, power, power / 3]}
                                  for offset, power in zip((-3, -2, 2, 3), (1, 2, 3, 4))]}
        native = {"schema": "vectorwarp-native-detection-full-bank-grid-v1",
                  "production_equivalent": True, "same_iq": True, "truth_selected_beam": False,
                  "configuration": {"config_fingerprint": "site", "frequency_hz": 539000000,
                      "sample_rate_hz": 2400000, "channels": 3, "channel_layout": [0, 1, 2],
                      "fusion_mode": "coherent_beam_bank",
                      "x_axis_bearing_deg_true": 0,
                      "receiver_location": [40, -80, 300],
                      "transmitter_location": [40.1, -80, 400],
                      "positions_m": baseline_descriptor["positions_m"],
                      "direct_path_azimuth_rad": 0, "direct_path_null_db": 0,
                      "receiver_gain_v2": gain},
                  "detector": {"contract": "vectorwarp-production-ca-cfar-full-bank-v1",
                      "configured_pfa": .1, "detector_pfa": .1 / 3,
                      "effective_beam_trials": 3, "motion_trials": 1, "integration_cpis": 1,
                      "native_alpha": MODULE.cfar_alpha(.1 / 3, 4, 1),
                      "guard_cells": 1, "training_cells_per_side": 2},
                  "banks": {"baseline": {"sha256": baseline_bank_sha256, "beams": 3,
                                              "descriptor": baseline_descriptor},
                            "candidate": {"sha256": candidate_bank_sha256, "beams": 3,
                                "runtime_compatible": True, "elevation_range_deg": [0, 10],
                                "responses": responses}},
                  "truth_label": {"selection": "target-cut-only-no-beam-selection",
                                  "target_cut_id": "target",
                                  "local_azimuth_deg": 45, "elevation_deg": 5},
                  "cuts": [{"id": "target", "role": "target",
                            "baseline": bank(baseline_bank_sha256), "candidate": bank(candidate_bank_sha256)},
                           {"id": "nuisance-1", "role": "nuisance",
                            "baseline": bank(baseline_bank_sha256), "candidate": bank(candidate_bank_sha256)}],
                  "provenance": {"candidate_model_sha256": digest(self.candidate),
                                  "iq_sha256": digest(self.iq), "config_sha256": digest(self.config),
                                  "generator_sha256": self.generator_sha256,
                                  "frequency_sha256": MODULE.canonical_hash(539000000),
                                  "sample_rate_sha256": MODULE.canonical_hash(2400000),
                                  "channel_layout_sha256": MODULE.canonical_hash([0, 1, 2]),
                                  "gain_sha256": MODULE.canonical_hash(gain)}}
        self.native.write_text(json.dumps(native, sort_keys=True))

    def tearDown(self):
        self.temporary.cleanup()

    def generate(self):
        return MODULE.generate(self.corpus, self.candidate, self.iq, self.config, self.native,
                               self.generator, self.generator_sha256, "validation-pass-1", "miss")

    def test_complete_full_bank_receipt_is_bound_and_trial_corrected(self):
        receipt = self.generate()
        self.assertEqual(receipt["schema"], "vectorwarp-detection-manifold-production-replay-v1")
        self.assertFalse(receipt["replay"]["truth_selected_beam"])
        self.assertEqual(receipt["provenance"]["input_grid_sha256"], digest(self.native))
        self.assertEqual(receipt["provenance"]["corpus_sha256"], digest(self.corpus))
        self.assertEqual(receipt["provenance"]["generator_trust_policy_sha256"],
                         MODULE.canonical_hash([self.generator_sha256]))
        target = receipt["replay"]["cuts"][0]
        self.assertEqual(target["baseline"]["beams_evaluated"], 3)
        self.assertEqual(target["baseline"]["cut_power"], 5)
        self.assertEqual(target["baseline"]["training_mean_power"], 2.5)
        alpha = MODULE.cfar_alpha(.1 / 3, 4, 1)
        self.assertTrue(math.isclose(target["baseline"]["threshold"], alpha * 2.5))
        self.assertEqual(receipt["pass"]["truth"]["selection"],
                         "target-cut-only-no-beam-selection")
        self.assertEqual(receipt["configuration"]["positions_m"],
                         [[0, 0, 0], [.1, 0, 0], [0, .1, 0]])
        self.assertEqual(receipt["configuration"]["direct_path_azimuth_rad"], 0)
        self.assertEqual(receipt["configuration"]["direct_path_null_db"], 0)
        self.assertEqual(receipt["configuration"]["fusion_mode"], "coherent_beam_bank")
        self.assertEqual(receipt["detector"]["guard_cells"], 1)
        self.assertEqual(receipt["detector"]["training_cells_per_side"], 2)

    def test_canonical_hash_matches_javascript_for_native_double_rendering(self):
        descriptor = {"frequency_hz": 539000000.0, "beams": 3,
                      "positions_m": [[0.0, 0.0, 0.0], [.1, 0.0, 0.0]],
                      "direct_path_azimuth_rad": 0.0, "direct_path_null_db": 0.0}
        javascript = subprocess.run(["node", "-e",
            "const crypto=require('crypto'),fs=require('fs'),v=JSON.parse(fs.readFileSync(0,'utf8'));"
            "const c=x=>Array.isArray(x)?'['+x.map(c).join(',')+']':x&&typeof x==='object'?"
            "'{'+Object.keys(x).sort().map(k=>JSON.stringify(k)+':'+c(x[k])).join(',')+'}':"
            "JSON.stringify(x);process.stdout.write(crypto.createHash('sha256').update(c(v)).digest('hex'));"],
            input=json.dumps(descriptor), text=True, capture_output=True, check=True).stdout
        self.assertEqual(MODULE.canonical_hash(descriptor), javascript)

    def test_cli_writes_importable_receipt_atomically(self):
        output = Path(self.temporary.name) / "receipt.json"
        subprocess.run([sys.executable, str(SCRIPT), "--corpus", str(self.corpus),
            "--candidate", str(self.candidate), "--iq", str(self.iq),
            "--config", str(self.config), "--native-export", str(self.native),
            "--native-generator", str(self.generator),
            "--expected-generator-sha256", self.generator_sha256,
            "--output", str(output), "--pass-id", "validation-pass-1",
            "--label", "miss"], check=True)
        receipt = json.loads(output.read_text())
        self.assertEqual(receipt["provenance"]["native_export_sha256"], digest(self.native))
        self.assertFalse(output.with_name(f"{output.name}.tmp").exists())
        validator = Path(__file__).parents[2] / "api" / "detection-manifold-corpus.js"
        subprocess.run(["node", "-e",
            "const fs=require('fs'); const {validateProductionReplay}=require(process.argv[1]); "
            "validateProductionReplay(JSON.parse(fs.readFileSync(process.argv[2], 'utf8')));",
            str(validator), str(output)], check=True)

    def test_rejects_incomplete_bank_tampering_and_nonruntime_candidate(self):
        native = json.loads(self.native.read_text())
        native["cuts"][0]["candidate"]["cut_power_by_beam"] = [1, 2]
        self.native.write_text(json.dumps(native, sort_keys=True))
        with self.assertRaisesRegex(ValueError, "every finite nonnegative beam power"):
            self.generate()
        candidate = json.loads(self.candidate.read_text())
        candidate["runtime_compatible"] = False
        self.candidate.write_text(json.dumps(candidate))
        with self.assertRaisesRegex(ValueError, "not a runtime-compatible"):
            self.generate()

    def test_rejects_renamed_or_mismatched_iq_and_truth_beam_selection(self):
        self.iq.write_bytes(b"different bytes under the same path")
        with self.assertRaisesRegex(ValueError, "iq_sha256"):
            self.generate()
        self.iq.write_bytes(b"same iq bytes")
        native = json.loads(self.native.read_text())
        native["truth_selected_beam"] = True
        self.native.write_text(json.dumps(native, sort_keys=True))
        with self.assertRaisesRegex(ValueError, "same-IQ full-bank grid"):
            self.generate()

    def test_rejects_unpinned_or_nonallowlisted_generator_and_bank_mismatch(self):
        with self.assertRaisesRegex(ValueError, "operator-pinned"):
            MODULE.generate(self.corpus, self.candidate, self.iq, self.config, self.native,
                            self.generator, "f" * 64, "validation-pass-1", "miss")
        self.generator.write_bytes(b"other exact native executable")
        self.generator.chmod(0o700)
        other_generator_sha256 = digest(self.generator)
        with self.assertRaisesRegex(ValueError, "config-allowlisted"):
            MODULE.generate(self.corpus, self.candidate, self.iq, self.config, self.native,
                            self.generator, other_generator_sha256, "validation-pass-1", "miss")
        self.generator.write_bytes(b"trusted native generator executable")
        self.generator.chmod(0o700)
        native = json.loads(self.native.read_text())
        native["banks"]["candidate"]["sha256"] = "b" * 64
        for cut in native["cuts"]:
            cut["candidate"]["bank_sha256"] = "b" * 64
        self.native.write_text(json.dumps(native, sort_keys=True))
        with self.assertRaisesRegex(ValueError, "exact dense runtime candidate contents"):
            self.generate()

    def test_rejects_truth_elevation_outside_runtime_bank_support(self):
        native = json.loads(self.native.read_text())
        native["truth_label"]["elevation_deg"] = 20
        self.native.write_text(json.dumps(native, sort_keys=True))
        with self.assertRaisesRegex(ValueError, "outside candidate bank support"):
            self.generate()

    def test_rejects_native_cfar_primitive_disagreement(self):
        native = json.loads(self.native.read_text())
        native["detector"]["native_alpha"] *= 1.1
        self.native.write_text(json.dumps(native, sort_keys=True))
        with self.assertRaisesRegex(ValueError, "CA-CFAR alpha disagrees"):
            self.generate()

    def test_rejects_unimplemented_whitening_integration_and_cfar_mismatch(self):
        config = json.loads(self.config.read_text())
        config["process"]["surveillance_fusion"]["mode"] = "noise_whitened_beam_bank"
        self.config.write_text(json.dumps(config, sort_keys=True))
        with self.assertRaisesRegex(ValueError, "single-CPI coherent_beam_bank"):
            self.generate()
        config["process"]["surveillance_fusion"]["mode"] = "coherent_beam_bank"
        config["process"]["detection"]["nTrain"] = 3
        self.config.write_text(json.dumps(config, sort_keys=True))
        native = json.loads(self.native.read_text())
        native["provenance"]["config_sha256"] = digest(self.config)
        self.native.write_text(json.dumps(native, sort_keys=True))
        with self.assertRaisesRegex(ValueError, "unsupported motion/integration accounting"):
            self.generate()

    def test_maps_full_capture_gain_vector_to_surveillance_subset(self):
        config = json.loads(self.config.read_text())
        config["capture"]["device"]["channel_count"] = 5
        config["capture"]["device"]["surveillance_channels"] = [1, 3, 4]
        config["capture"]["device"]["heimdall"]["channel_gains"] = [10, 11, 12, 13, 14]
        config["capture"]["device"]["array_geometry"]["elements"] = [
            {"daq_channel": channel, "position": [channel * .1, 0, 0]}
            for channel in range(5)]
        identity = MODULE.config_identity(config)
        self.assertEqual(identity["requested_gain"]["requested_channel_gains"], [11, 13, 14])
        self.assertEqual(identity["requested_gain"]["requested_capture_channel_gains"],
                         [10, 11, 12, 13, 14])


if __name__ == "__main__":
    unittest.main()
