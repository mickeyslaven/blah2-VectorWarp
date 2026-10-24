#!/usr/bin/env python3
"""Synthetic same-IQ acceptance for the native full-bank replay emitter."""

import argparse
import hashlib
import importlib.util
import json
import subprocess
import tempfile
from pathlib import Path


ROOT = Path(__file__).parents[2]
RECEIPT_PATH = ROOT / "script/generate-detection-manifold-production-receipt.py"
SPEC = importlib.util.spec_from_file_location("production_receipt", RECEIPT_PATH)
RECEIPT = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(RECEIPT)


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", required=True, type=Path)
    args = parser.parse_args()
    binary = args.binary.resolve()
    generator_sha256 = digest(binary)
    with tempfile.TemporaryDirectory(prefix="vectorwarp-native-grid-") as temporary:
        root = Path(temporary)
        corpus, candidate, iq, config = (root / name for name in
            ("corpus.jsonl", "candidate.json", "same-iq.raw", "config.json"))
        native_input, native_output = root / "native-input.json", root / "native-output.json"
        corpus.write_bytes(b'{"positive":"snapshot"}\n')
        iq.write_bytes(b"one immutable same-IQ capture")
        responses = [
            [[1, 0], [1, 0]], [[1, 0], [0, 1]],
            [[1, 0], [-1, 0]], [[1, 0], [0, -1]],
        ]
        bank_sha256 = RECEIPT.canonical_hash(responses)
        candidate.write_text(json.dumps({
            "schema": "vectorwarp-detection-manifold-runtime-v1",
            "runtime_compatible": True, "intended_use": "offline-production-equivalent-replay-only",
            "frequency_hz": 539000000, "channels": 2, "beams": 4,
            "elevation_range_deg": [0, 10], "responses": responses,
            "bank_sha256": bank_sha256,
        }, separators=(",", ":")))
        gain = {"requested_common_gain": 12, "requested_channel_gains": None,
                "actual_channel_gains_db": [12.1, 11.9]}
        configuration = {"config_fingerprint": "synthetic-site", "frequency_hz": 539000000,
            "sample_rate_hz": 2400000, "channels": 2, "channel_layout": [0, 1],
            "fusion_mode": "coherent_beam_bank",
            "positions_m": [[0, 0, 0], [.1, 0, 0]], "direct_path_azimuth_rad": 0,
            "direct_path_null_db": 0, "x_axis_bearing_deg_true": 0,
            "receiver_location": [40, -80, 300],
            "transmitter_location": [40.1, -80, 400], "receiver_gain_v2": gain}
        baseline_descriptor = {"frequency_hz": 539000000.0, "beams": 4,
            "positions_m": configuration["positions_m"], "direct_path_azimuth_rad": 0.0,
            "direct_path_null_db": 0.0}
        baseline_bank_sha256 = RECEIPT.canonical_hash(baseline_descriptor)
        config_value = {"capture": {"fc": 539000000, "fs": 2400000, "device": {
            "channel_count": 2, "surveillance_channels": [0, 1], "heimdall": {"gain": 12},
            "array_geometry": {"mapping_confirmed": True, "geometry_confirmed": True,
                "orientation_confirmed": True, "x_axis_bearing_deg_true": 0,
                "elements": [{"daq_channel": 0, "position": [0, 0, 0]},
                             {"daq_channel": 1, "position": [.1, 0, 0]}]}}},
            "location": {"rx": {"latitude": 40, "longitude": -80, "altitude": 300},
                         "tx": {"latitude": 40.1, "longitude": -80, "altitude": 400}},
            "process": {"detection": {"pfa": .04, "nGuard": 0, "nTrain": 2,
                "integration_cpis": 1}, "surveillance_fusion": {"mode": "coherent_beam_bank",
                "beams": 4, "direct_path_null_db": 0,
                "effective_beam_trials": 4, "detection_manifold_replay": {
                    "trusted_native_generator_sha256": [generator_sha256]}}}}
        config.write_text(json.dumps(config_value, separators=(",", ":")))
        provenance = {"candidate_model_sha256": digest(candidate), "iq_sha256": digest(iq),
            "config_sha256": digest(config), "generator_sha256": generator_sha256,
            "frequency_sha256": RECEIPT.canonical_hash(539000000),
            "sample_rate_sha256": RECEIPT.canonical_hash(2400000),
            "channel_layout_sha256": RECEIPT.canonical_hash([0, 1]),
            "gain_sha256": RECEIPT.canonical_hash(gain)}
        cells = lambda scale: {"cut": [[scale, 0], [scale / 2, scale / 3]],
            "training": [{"offset": offset,
                "channels": [[scale / divisor, 0], [scale / (divisor + 1), 0]]}
                for offset, divisor in zip((-2, -1, 1, 2), (5, 4, 6, 7))]}
        target, nuisance = cells(4), cells(.4)
        native_value = {"schema": "vectorwarp-detection-manifold-native-input-v1",
            "production_equivalent": True, "same_iq": True, "truth_selected_beam": False,
            "configuration": configuration,
            "detector": {"contract": "vectorwarp-production-ca-cfar-full-bank-v1",
                "configured_pfa": .04, "detector_pfa": .01, "effective_beam_trials": 4,
                "motion_trials": 1, "integration_cpis": 1, "guard_cells": 0,
                "training_cells_per_side": 2},
            "banks": {"baseline": {"sha256": baseline_bank_sha256, "beams": 4},
                "candidate": {"sha256": bank_sha256, "beams": 4,
                    "runtime_compatible": True, "elevation_range_deg": [0, 10],
                    "responses": responses}},
            "reference_weights": [[1, 0], [1, 0]],
            "truth_label": {"selection": "target-cut-only-no-beam-selection",
                "target_cut_id": "target", "local_azimuth_deg": 45, "elevation_deg": 5},
            "cuts": [{"id": "target", "role": "target", **target},
                     {"id": "nuisance-1", "role": "nuisance", **nuisance}],
            "provenance": provenance}
        native_input.write_text(json.dumps(native_value, separators=(",", ":")))
        subprocess.run([str(binary), str(native_input), str(native_output)], check=True)
        emitted = json.loads(native_output.read_text())
        assert emitted["same_iq"] and not emitted["truth_selected_beam"]
        assert len(emitted["cuts"]) == 2
        for cut in emitted["cuts"]:
            for arm in ("baseline", "candidate"):
                assert len(cut[arm]["cut_power_by_beam"]) == 4
                assert len(cut[arm]["training_power_by_offset_beam"]) == 4
                assert all(len(row["power_by_beam"]) == 4
                           for row in cut[arm]["training_power_by_offset_beam"])
        receipt = RECEIPT.generate(corpus, candidate, iq, config, native_output, binary,
                                   generator_sha256, "synthetic-pass", "miss")
        assert receipt["replay"]["complete_grid"]
        assert receipt["provenance"]["iq_sha256"] == digest(iq)

        native_value["banks"]["candidate"]["responses"][1][1] = [0.5, 0.5]
        native_input.write_text(json.dumps(native_value, separators=(",", ":")))
        subprocess.run([str(binary), str(native_input), str(native_output)], check=True)
        try:
            RECEIPT.generate(corpus, candidate, iq, config, native_output, binary,
                             generator_sha256, "tampered-bank-pass", "miss")
            raise AssertionError("receipt accepted weights that differ from the candidate artifact")
        except ValueError as error:
            assert "exact dense runtime candidate contents" in str(error)
        native_value["banks"]["candidate"]["responses"][1][1] = [0, 1]

        native_value["cuts"][0]["training"].pop()
        native_input.write_text(json.dumps(native_value, separators=(",", ":")))
        rejected = subprocess.run([str(binary), str(native_input), str(native_output)],
                                  capture_output=True, text=True)
        assert rejected.returncode != 0 and "training grid is incomplete" in rejected.stderr
    print("Native detection grid: same-IQ full-bank and strict receipt chain passed.")


if __name__ == "__main__":
    main()
