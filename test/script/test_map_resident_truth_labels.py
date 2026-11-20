"""Tiny synthetic offline contracts; no replay, GPU, sockets or live services."""
import copy
import contextlib
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("resident_labels", ROOT / "script/map-resident-truth-labels.py")
M = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(M)
NATIVE = sys.argv[1:3] if len(sys.argv) == 3 and all(Path(value).is_file() for value in sys.argv[1:3]) else None
if NATIVE:
    sys.argv = sys.argv[:1]


def fixture():
    hashes = {name: hashlib.sha256(name.encode()).hexdigest()
              for name in ("admission", "timeline", "metadata", "profile", "replay_receipt")}
    source = "1" * 64
    profile = {"schema": "vectorwarp-replay-profile-v2", "configuration_complete": True,
               "capture": {"sample_rate_hz": 1000000, "frequency_hz": 509000000}}
    delay = 1.1 * M.SPEED_OF_LIGHT_M_S / 1e9
    predicted = {"hex": "abcdef", "timestamp": 1.5, "delay": delay, "doppler": 3.1,
                 "frequency_hz": 509000000, "delay_definition": "excess_bistatic_path_km",
                 "provenance": {"source_id": "synthetic-only"}}
    truth = {**predicted, "timestamp_ms": 1500, "position_timestamp_s": 1.5, "delay_km": delay, "doppler_hz": 3.1}
    admission = {"schema": "vectorwarp-capture-admission-v1", "state": "admitted",
                 "integrity": {"valid": True, "data_sha256": source,
                               "sidecar_sha256": hashes["metadata"],
                               "data_sha256_verified_by": "independent-read-sha256"},
                 "events": [{"event_id": "event-a", "pass_id": "pass-a", "icao": "ABCDEF",
                             "kind": "adsb-eligible-aircraft", "capture_ids": ["capture-a"]}],
                 "captures": {"capture-a": {"valid": True, "admission_state": "admitted",
                                            "cpi_ms": 500, "decision_grade": False,
                                            "cpis": [{"timestamp_ms": 1500}], "truth_samples": [truth]}}}
    timeline = {"schema": "vectorwarp-recording-timeline-v2", "replay_profile": profile,
                "start_ms": 1000, "end_ms": 2000, "sample_rate_hz": 1000000,
                "frequency_hz": 509000000, "frames": [
                    {"timestamp_ms": 1000, "delay_doppler": {"abcdef": predicted}},
                    {"timestamp_ms": 1500, "delay_doppler": {"abcdef": predicted}}]}
    metadata = {"dataSha256": source, "windowStart": 1000, "windowEnd": 2000,
                "sampleRate": 1000000, "frequency": 509000000, "epoch": 1,
                "firstEpochSample": 0, "endEpochSample": 1000000, "samplesPerChannel": 1000000,
                "passIds": ["pass-a"], "eventIds": ["event-a"],
                "eventTime": {"schema": "vectorwarp-event-time-v1", "estimateAvailable": True,
                              "source": "synthetic-only", "exact": False, "uncertaintyBounded": False,
                              "epoch": "1", "firstSample": "0", "endSample": "1000000",
                              "sampleRateHz": 1000000, "epochEstimateNs": "1000000000",
                              "estimateNs": "1500000000"}}
    frame = {"snapshot": "/synthetic/frame.vwrps", "snapshot_sha256": "2" * 64,
             "snapshot_and_lineage_sha256": "3" * 64, "lineage_file_sha256": "4" * 64,
             "source_sha256": source, "rf_sha256": hashes["profile"],
             "logical_frame": "1", "consumed_sequence": "1",
             # Deliberately unrelated replay clock: it must not drive the join.
             "signal_timestamp_ms": "999999999", "capture_epoch": "1",
             "first_sample": "250000", "end_sample": "750000",
             "clutter_samples": "500000", "sample_rate_hz": "1000000", "frequency_hz": "509000000",
             "delay_axis_samples": [-3, -2, -1, 0, 1, 2, 3], "doppler_axis_hz": [-8.2, -4.1, 0, 4.1, 8.2]}
    geometry = {"schema": "vectorwarp-resident-snapshot-geometry-v1", "evaluation_only": True, "frames": [frame]}
    receipt = {"schema": "vectorwarp-iq-protection-replay-v1", "recording_sha256": source,
               **{name + "_sha256": hashes[name] for name in ("admission", "timeline", "metadata", "profile")},
               "final_status": {"state": "complete", "captureDroppedSamples": [0, 0]},
               "frames": {"timing": [{"captureSamplePositionAvailable": True, "captureEpoch": 1,
                                      "captureFirstEpochSample": 250000, "captureEndEpochSample": 750000,
                                      "inputDiscontinuity": False}]},
               "resident_snapshot_export": {"files": [{"sha256": "2" * 64, "lineage_sha256": "4" * 64,
                                                       "logical_frame": 1, "consumed_sequence": 1}]}}
    return geometry, admission, timeline, metadata, profile, receipt, hashes, "pass-a"


class TruthLabels(unittest.TestCase):
    def setUp(self):
        self.args = fixture()
        self.g, self.a, self.t, self.m, self.p, self.r, self.h, _ = self.args

    def mapped(self):
        return M.map_labels(*self.args)

    def rejects(self, text):
        with self.assertRaisesRegex(ValueError, text):
            self.mapped()

    def test_display_axis_units_offsets_and_replay_clock_are_not_indices(self):
        result = self.mapped()
        frame = result["frames"][0]
        self.assertEqual((frame["points"][0]["row"], frame["points"][0]["column"]), (3, 4))
        self.assertEqual(frame["snapshot_and_lineage_sha256"], "3" * 64)
        self.assertEqual(frame["join"]["timeline_timestamp_ms"], 1500)
        self.assertEqual(frame["join"]["signal_timestamp_ms"], "999999999")
        self.assertFalse(frame["join"]["clock_exact"])
        self.assertFalse(frame["join"]["admission_decision_grade"])
        self.assertFalse(result["inference_features"])
        self.assertFalse(result["control_labels_inferred"])

    def test_nearest_frame_within_half_cpi_is_selected(self):
        frame = self.g["frames"][0]
        frame.update(first_sample="300000", end_sample="800000")
        self.r["frames"]["timing"][0].update(captureFirstEpochSample=300000, captureEndEpochSample=800000)
        self.assertEqual(self.mapped()["frames"][0]["join"]["timeline_offset_ms"], -50)

    def test_native_mcht_and_recorder_sample_origins_require_bound_translation(self):
        self.m["epoch"] = 0
        self.m["eventTime"]["epoch"] = "0"
        self.m["upstreamTiming"] = {"protocol": "MCHT-v1", "acquisitionEpoch": 17, "firstSample": 9000000}
        self.g["frames"][0].update(capture_epoch="17", first_sample="9250000", end_sample="9750000")
        self.r["frames"]["timing"][0].update(captureEpoch=17, captureFirstEpochSample=9250000, captureEndEpochSample=9750000)
        join = self.mapped()["frames"][0]["join"]
        self.assertEqual(join["timeline_timestamp_ms"], 1500)
        self.assertEqual(join["recording_clock_first_sample"], "250000")
        self.assertEqual(join["sample_clock_mapping"], "metadata_bound_native_mcht_to_recording_origin")
        self.m.pop("upstreamTiming")
        self.rejects("source sample interval/epoch")

    def test_missing_exact_receipt_sample_correspondence_rejected(self):
        self.r["frames"]["timing"][0]["captureFirstEpochSample"] += 1
        self.rejects("exact snapshot/receipt sample correspondence")

    def test_nearest_endpoint_requires_its_own_admission_support(self):
        self.t["frames"].append({"timestamp_ms": 1550, "delay_doppler": {}})
        self.g["frames"][0].update(first_sample="350000", end_sample="850000")
        self.r["frames"]["timing"][0].update(captureFirstEpochSample=350000, captureEndEpochSample=850000)
        self.rejects("lacks selected pass admission support")

    def test_excessive_timestamp_mismatch_rejected(self):
        self.t["frames"] = [self.t["frames"][1]]
        self.t["frames"][0]["timestamp_ms"] = 1900
        capture = self.a["captures"]["capture-a"]
        capture["cpis"][0]["timestamp_ms"] = 1900
        capture["truth_samples"][0]["timestamp_ms"] = 1900
        self.rejects("mismatch exceeds")

    def test_tied_timeline_match_rejected(self):
        self.g["frames"][0].update(first_sample="0", end_sample="500000")
        self.r["frames"]["timing"][0].update(captureFirstEpochSample=0, captureEndEpochSample=500000)
        self.rejects("ambiguous nearest timeline")

    def test_ambiguous_pass_and_crosslinked_capture_rejected(self):
        self.a["events"].append(copy.deepcopy(self.a["events"][0]))
        self.rejects("pass identity missing or ambiguous")
        self.a["events"][-1].update(pass_id="pass-b", icao="123456")
        self.rejects("capture belongs to ambiguous")

    def test_duplicate_cpi_and_truth_rejected(self):
        capture = self.a["captures"]["capture-a"]
        capture["cpis"].append(copy.deepcopy(capture["cpis"][0]))
        self.rejects("duplicate admitted CPI")
        capture["cpis"].pop()
        capture["truth_samples"].append(copy.deepcopy(capture["truth_samples"][0]))
        self.rejects("duplicate admitted truth")

    def test_missing_truth_support_rejected(self):
        self.a["captures"]["capture-a"]["truth_samples"] = []
        self.rejects("selected pass truth support")

    def test_source_and_lineage_hash_changes_rejected(self):
        self.g["frames"][0]["source_sha256"] = "5" * 64
        self.rejects("source/RF profile")
        self.g["frames"][0]["source_sha256"] = "1" * 64
        self.r["resident_snapshot_export"]["files"][0]["lineage_sha256"] = "5" * 64
        self.rejects("lineage/sequence identity")

    def test_replay_artifact_hash_mismatch_rejected(self):
        self.r["timeline_sha256"] = "5" * 64
        self.rejects("timeline hash differs")

    def test_duplicate_snapshot_and_timeline_rejected(self):
        self.g["frames"].append(copy.deepcopy(self.g["frames"][0]))
        self.rejects("duplicate snapshot/lineage")
        self.g["frames"].pop()
        self.t["frames"].append(copy.deepcopy(self.t["frames"][1]))
        self.rejects("timeline duplicate")

    def test_epoch_source_clock_and_receipt_duplicates_rejected(self):
        self.g["frames"][0]["capture_epoch"] = "2"
        self.rejects("sample interval/epoch")
        self.g["frames"][0]["capture_epoch"] = "1"
        self.r["frames"]["timing"].append(copy.deepcopy(self.r["frames"]["timing"][0]))
        self.rejects("sample correspondence missing or ambiguous")
        self.r["frames"]["timing"].pop()
        self.m["eventTime"]["estimateNs"] = "1800000000"
        self.rejects("clock midpoint mismatch")

    def test_missing_clock_does_not_fall_back_to_signal_wall_time(self):
        self.g["frames"][0]["signal_timestamp_ms"] = "1500"
        self.m.pop("eventTime")
        self.rejects("source-time clock support missing")

    def test_truth_rf_definition_and_provenance_required(self):
        predicted = self.t["frames"][1]["delay_doppler"]["abcdef"]
        predicted["frequency_hz"] += 2
        self.rejects("RF/delay definition")
        predicted["frequency_hz"] -= 2
        predicted["delay_definition"] = "monostatic_range_km"
        self.rejects("RF/delay definition")
        predicted["delay_definition"] = "excess_bistatic_path_km"
        predicted.pop("provenance")
        self.rejects("observation provenance missing")

    def test_coordinate_disagreement_and_outside_map_rejected(self):
        self.t["frames"][1]["delay_doppler"]["abcdef"]["doppler"] += .01
        self.rejects("truth endpoint mismatch")
        self.t["frames"][1]["delay_doppler"]["abcdef"]["doppler"] -= .01
        self.g["frames"][0]["delay_axis_samples"] = [-3, -2, -1, 0, 1]
        self.rejects("outside snapshot delay")

    def test_truth_observation_time_identity_and_cpi_duration_required(self):
        self.a["captures"]["capture-a"]["truth_samples"][0]["position_timestamp_s"] = 1.4
        self.rejects("observation identity differs")
        self.a["captures"]["capture-a"]["truth_samples"][0]["position_timestamp_s"] = 1.5
        self.a["captures"]["capture-a"]["cpi_ms"] = 1000
        self.rejects("CPI duration differs")

    def test_nonfinite_duplicate_axes_and_ambiguous_bins_rejected(self):
        for axis, value, match in (([0, 1, 2], 1.5, "equidistant"), ([0, 1, 1], 1, "unordered/duplicate"),
                                   ([0, math_nan()], 1, "nonfinite")):
            with self.assertRaisesRegex(ValueError, match):
                M.nearest(axis, value, "test")

    def test_strict_json_and_unsigned_integer_inputs(self):
        for data in (b'{"a":1,"a":2}', b'{"a":NaN}'):
            with self.assertRaises(ValueError):
                M.parse_json(data)
        for value in (True, -1, 1.5, "01", "-1", 2**64):
            with self.assertRaises(ValueError):
                M.integer(value, "test")

    def test_cli_publishes_bound_manifest_and_never_overwrites(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            paths = {name: root / (name + ".json") for name in
                     ("admission", "timeline", "metadata", "profile", "replay_receipt")}
            docs = {"admission": self.a, "timeline": self.t, "metadata": self.m,
                    "profile": self.p, "replay_receipt": self.r}
            for name in ("metadata", "profile", "timeline"):
                paths[name].write_text(json.dumps(docs[name]))
                self.h[name] = M.file_sha256(paths[name])
            self.a["integrity"]["sidecar_sha256"] = self.h["metadata"]
            paths["admission"].write_text(json.dumps(self.a))
            self.h["admission"] = M.file_sha256(paths["admission"])
            frame = self.g["frames"][0]
            frame["rf_sha256"] = self.h["profile"]
            snapshot = root / "frame.vwrps"
            snapshot.write_bytes(b"not native: subprocess is mocked in this CLI-only test")
            sidecar = Path(str(snapshot) + ".vwrl")
            sidecar.write_bytes(b"synthetic lineage")
            helper = root / "helper"
            helper.write_bytes(b"not executed")
            frame["snapshot"] = str(snapshot)
            self.r["resident_snapshot_export"]["files"][0]["lineage_sha256"] = M.file_sha256(sidecar)
            for name in ("admission", "timeline", "metadata", "profile"):
                self.r[name + "_sha256"] = self.h[name]
            paths["replay_receipt"].write_text(json.dumps(self.r))
            output = root / "labels.json"
            args = [item for name, path in paths.items() for item in ("--" + name.replace("_", "-"), str(path))]
            args += ["--geometry-helper", str(helper), "--output", str(output), "--pass-id", "pass-a", str(snapshot)]
            result = subprocess.CompletedProcess([], 0, json.dumps(self.g).encode(), b"")
            with patch.object(M.subprocess, "run", return_value=result) as run, contextlib.redirect_stdout(io.StringIO()):
                M.main(args)
                run.assert_called_once()
            labelled = json.loads(output.read_text())
            self.assertEqual(labelled["version"], 1)
            self.assertEqual(labelled["frames"][0]["points"][0]["column"], 4)
            before = output.read_bytes()
            with self.assertRaisesRegex(ValueError, "output already exists"):
                M.main(args)
            self.assertEqual(output.read_bytes(), before)
            # A rejected native helper cannot leave a label file behind.
            args[args.index(str(output))] = str(root / "failed.json")
            rejected = subprocess.CompletedProcess([], 1, b"", b"missing lineage")
            with patch.object(M.subprocess, "run", return_value=rejected), self.assertRaisesRegex(ValueError, "native snapshot validation failed"):
                M.main(args)
            self.assertFalse((root / "failed.json").exists())


def math_nan():
    return float("nan")


@unittest.skipUnless(NATIVE, "pass native geometry and fixture executables to run sealed ABI checks")
class NativeGeometry(unittest.TestCase):
    def test_shared_native_decoder_axes_hash_and_sidecar_failure(self):
        with tempfile.TemporaryDirectory() as directory:
            subprocess.run([NATIVE[1], "--fixtures", directory], check=True, capture_output=True)
            paths = [str(Path(directory) / f"{index}.vwrps") for index in range(1, 9)]
            completed = subprocess.run([NATIVE[0], *paths], check=True, capture_output=True)
            records = json.loads(completed.stdout)["frames"]
            self.assertEqual(len(records), 8)
            self.assertEqual(records[0]["delay_axis_samples"], list(range(-4, 5)))
            self.assertEqual(records[0]["doppler_axis_hz"], [i / (63. / 128000) for i in range(-3, 4)])
            self.assertEqual(records[0]["snapshot_sha256"], M.file_sha256(paths[0]))
            # Independently reproduce the exact declared canonical identity
            # solely as a test oracle, not as another production ABI decoder.
            sidecar = Path(paths[0] + ".vwrl").read_bytes()
            values = struct.unpack_from("<11Q", sidecar, 16)
            gains = struct.unpack_from("<I", sidecar, 12)[0]
            canonical = struct.pack("<13Q", *values, struct.unpack_from("<I", sidecar, 104)[0], gains)
            canonical += sidecar[112:304] + sidecar[400:432] + sidecar[304:304 + 8*gains]
            self.assertEqual(records[0]["snapshot_and_lineage_sha256"], hashlib.sha256(canonical).hexdigest())
            rejected = subprocess.run([NATIVE[0], paths[1], paths[0]], capture_output=True)
            self.assertNotEqual(rejected.returncode, 0)
            self.assertEqual(rejected.stdout, b"")
            Path(paths[0] + ".vwrl").unlink()
            rejected = subprocess.run([NATIVE[0], paths[0]], capture_output=True)
            self.assertNotEqual(rejected.returncode, 0)
            self.assertIn(b"sidecar missing", rejected.stderr)


if __name__ == "__main__":
    unittest.main()
