#!/usr/bin/env python3
"""Synthetic functional-mode fixtures; never operational release evidence."""
import copy
import json
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
import test_operational_acceptance as fixtures

O, R, monitor = fixtures.O, fixtures.R, fixtures.monitor


class FunctionalAcceptance(unittest.TestCase):
    sample = fixtures.OperationalAcceptance.sample
    evaluate = fixtures.OperationalAcceptance.evaluate
    checks = fixtures.OperationalAcceptance.checks

    def setUp(self):
        fixtures.OperationalAcceptance.setUp(self)
        self.binding.update(evaluationMode=O.FUNCTIONAL_MODE, releaseVersion="1.2.3",
                            candidateRunId=123, candidateRunAttempt=1)
        self.binding["nodes"]["peer"] = copy.deepcopy(self.binding["nodes"]["site"])
        self.policy["baselineIdentities"]["peer"] = self.baseline_identity
        for sample in [self.baseline, *self.samples]:
            sample["nodes"]["peer"] = copy.deepcopy(sample["nodes"]["site"])
            peer = copy.deepcopy(sample["statistics"]["nodes"][0])
            peer["id"] = "peer"
            sample["statistics"]["nodes"].append(peer)
            # Whole-pass evidence is genuinely absent; no synthetic pass success.
            sample["statistics"]["system_wide_unique_passes"]["passes"] = []
        self.policy.update(schema=O.FUNCTIONAL_POLICY_SCHEMA,
                           baselineSha256=self.archive.result(self.baseline))
        del self.policy["minimumEligiblePasses"], self.policy["yieldTolerance"]
        self.inputs["schema"] = O.FUNCTIONAL_INPUT_SCHEMA
        for index, sample in enumerate(self.samples):
            sequence = index + 20
            frame = {"timestamp": sample["epoch"] * 1000, "illuminator_id": "primary",
                "input_lineage": {"available": True, "sampleInterval": "half-open",
                    "captureEpochExact": "0", "firstInputSampleExact": str(index * 100),
                    "endInputSampleExact": str((index + 1) * 100)},
                "detector_integration": {"sequence": sequence, "capture_epoch": 0, "history_epoch": 7, "history_reset": False},
                "trackerDiagnostics": {"schema": "vectorwarp-tracker-frame-diagnostics-v2",
                    "truth_blind": True, "no_op": False, "lineage_resets": 0, "detections": 2},
                "data": [{"id": "native-track", "state": "ACTIVE", "measured": True, "source": "detector",
                    "measurement_evidence": {"epoch_exact": "7", "newest_sequence_exact": str(sequence),
                        "cpis": 2, "support_sequences_exact": [str(sequence), str(sequence - 1)]}}]}
            sample["nativeFrames"] = {name: copy.deepcopy(frame) for name in self.binding["nodes"]}
        self.authorization = {"schema": "vectorwarp-functional-release-authorization-v1",
            "evaluationMode": O.FUNCTIONAL_MODE, "sourceCommit": self.identity["sourceCommit"],
            "releaseVersion": "1.2.3", "candidateRunId": 123, "candidateRunAttempt": 1,
            "nodes": {name: self.identity for name in self.binding["nodes"]},
            "windowSeconds": 600, "passYieldNonRegression": "not-evaluated",
            "authorizedAtEpoch": 1699, "reason": "Fixture-only explicit one-release authorization"}
        self.authorize()

    def authorize(self):
        digest = self.archive.result(self.authorization)
        self.policy["functionalAuthorizationSha256"] = digest
        self.binding["functionalAuthorizationSha256"] = digest

    def test_exact_bounded_functionality_without_whole_passes(self):
        receipt = self.evaluate()
        self.assertEqual(set(self.checks().values()), {"accepted"})
        self.assertEqual(receipt["evaluationMode"], O.FUNCTIONAL_MODE)
        self.assertEqual(receipt["passYieldNonRegression"], "not-evaluated")
        receipt["evidenceSha256"] = "9" * 64
        self.assertEqual(monitor.verify_regression(receipt,
            {"sha256": "2" * 64, "expected": self.binding}, 2600, 2000), [])

    def test_every_authorization_identity_is_required_and_pinned(self):
        original = copy.deepcopy(self.authorization)
        for key in original:
            with self.subTest(key=key):
                self.authorization = copy.deepcopy(original)
                del self.authorization[key]
                self.authorize()
                with self.assertRaises(ValueError): self.evaluate()
        for key, value in (("sourceCommit", "0" * 40), ("releaseVersion", "1.2.4"),
                           ("candidateRunId", 124), ("candidateRunAttempt", 2),
                           ("windowSeconds", 1800), ("passYieldNonRegression", "accepted"),
                           ("reason", " "), ("authorizedAtEpoch", 2001), ("nodes", {})):
            with self.subTest(key=key):
                self.authorization = {**original, key: value}
                self.authorize()
                with self.assertRaises(ValueError): self.evaluate()

    def test_receipt_mode_substitution_or_missing_disclosure_refused(self):
        receipt = self.evaluate()
        receipt["evidenceSha256"] = "9" * 64
        binding = {"sha256": "2" * 64, "expected": self.binding}
        for key, value in (("evaluationMode", O.MODE), ("functionalAuthorizationSha256", "0" * 64),
                           ("windowSeconds", 599), ("passYieldNonRegression", "accepted"),
                           ("evaluationStartEpoch", 2001)):
            with self.subTest(key=key):
                self.assertEqual(monitor.verify_regression({**receipt, key: value}, binding, 2600, 2000),
                                 ["soak:regression-evidence-invalid"])
        for key in ("functionalAuthorizationSha256", "passYieldNonRegression", "windowSeconds"):
            invalid = {k: v for k, v in receipt.items() if k != key}
            self.assertEqual(monitor.verify_regression(invalid, binding, 2600, 2000),
                             ["soak:regression-evidence-invalid"])
        del self.binding["evaluationMode"], self.binding["functionalAuthorizationSha256"]
        self.assertEqual(monitor.verify_regression({**receipt, "evaluationMode": O.MODE}, binding, 2600, 2000),
                         ["soak:regression-evidence-invalid"])
        with self.assertRaises(ValueError): self.evaluate()

    def test_missing_either_node_native_frames_refused(self):
        del self.samples[1]["nativeFrames"]["peer"]
        with self.assertRaises(ValueError): self.evaluate()

    def test_no_radar_detections_or_only_predicted_tracks_is_inconclusive(self):
        for sample in self.samples:
            frame = sample["nativeFrames"]["peer"]
            frame["trackerDiagnostics"]["detections"] = 0
            frame["data"][0].update(measured=False, source="prediction")
        receipt = self.evaluate()
        self.assertEqual(receipt["nodes"]["peer"]["checks"]["detections"]["status"], "inconclusive")
        self.assertEqual(receipt["nodes"]["peer"]["checks"]["measurement-backed-track-continuity"]["status"], "inconclusive")
        self.assertFalse(receipt["regressionDetected"])

    def test_singletons_or_coasts_never_manufacture_continuity(self):
        for index, sample in enumerate(self.samples):
            sample["nativeFrames"]["site"]["data"][0]["id"] = str(index)
        self.assertEqual(self.checks()["measurement-backed-track-continuity"], "inconclusive")
        for sample in self.samples:
            sample["nativeFrames"]["site"]["data"][0].update(id="same", state="COASTING")
        self.assertEqual(self.checks()["measurement-backed-track-continuity"], "inconclusive")

    def test_stale_repeated_reset_or_malformed_native_evidence_refused(self):
        original = copy.deepcopy(self.samples)
        mutations = [lambda f: f.update(timestamp=0), lambda f: f.update(timestamp=2060001),
            lambda f: f["input_lineage"].update(captureEpochExact="1"),
            lambda f: f["input_lineage"].update(firstInputSampleExact="0"),
            lambda f: f["trackerDiagnostics"].update(truth_blind=False),
            lambda f: f["trackerDiagnostics"].update(no_op=True),
            lambda f: f["trackerDiagnostics"].update(detections=None),
            lambda f: f["trackerDiagnostics"].update(lineage_resets=1),
            lambda f: f["detector_integration"].update(sequence=20),
            lambda f: f["detector_integration"].update(history_reset=True),
            lambda f: f["detector_integration"].pop("history_epoch"),
            lambda f: f["detector_integration"].update(history_epoch=0),
            lambda f: f["detector_integration"].update(history_epoch=True),
            lambda f: f["detector_integration"].update(history_epoch=7.0),
            lambda f: f["detector_integration"].update(history_epoch="7"),
            lambda f: f["detector_integration"].update(history_epoch=2**53),
            lambda f: f["detector_integration"].update(history_epoch=8),
            lambda f: f["data"][0].update(id=" \t"),
            lambda f: f["data"][0].pop("measurement_evidence"),
            lambda f: f["data"][0].update(source="prediction"),
            lambda f: f["data"][0]["measurement_evidence"].update(epoch_exact="1"),
            lambda f: f["data"][0]["measurement_evidence"].update(epoch_exact="0"),
            lambda f: f["data"][0]["measurement_evidence"].update(newest_sequence_exact="20"),
            lambda f: f["data"][0]["measurement_evidence"].update(support_sequences_exact=["21", "21"]),
            lambda f: f["data"].append(copy.deepcopy(f["data"][0]))]
        for index, mutate in enumerate(mutations):
            with self.subTest(mutation=index):
                self.samples = copy.deepcopy(original)
                mutate(self.samples[1]["nativeFrames"]["site"])
                with self.assertRaises(ValueError): self.evaluate()

    def test_coherent_detector_history_reset_still_rejects_window(self):
        frame = self.samples[1]["nativeFrames"]["site"]
        frame["detector_integration"]["history_epoch"] = 8
        frame["data"][0]["measurement_evidence"]["epoch_exact"] = "8"
        with self.assertRaises(ValueError): self.evaluate()

    def test_actual_native_frame_keeps_capture_and_measurement_history_distinct(self):
        # Unmodified field projection from HP /api/tracker/native at the fixture
        # timestamp, collected read-only2026-11-26; not candidate acceptance evidence.
        frame = json.loads((Path(__file__).parent / "fixtures/native-functional-hp-frame.json").read_text())
        sample = {"epoch": frame["timestamp"] / 1000, "nativeFrames": {"hp": frame},
                  "statistics": {"nodes": [{"id": "hp", "channels": [{"id": "primary"}]}]}}
        def state(): return {"hp": {"detectedFrames": 0, "tracks": {}}}
        observed = state()
        O.functional_observation(sample, observed, frame["timestamp"])
        self.assertEqual(observed["hp"]["detectedFrames"], 1)
        self.assertEqual(observed["hp"]["tracks"], {})  # ASSOCIATED is not ACTIVE.
        frame["data"][0]["measurement_evidence"]["epoch_exact"] = "0"
        with self.assertRaises(ValueError): O.functional_observation(sample, state(), frame["timestamp"])
        frame["data"][0]["measurement_evidence"]["epoch_exact"] = "1"
        del frame["detector_integration"]["history_epoch"]
        with self.assertRaises(ValueError): O.functional_observation(sample, state(), frame["timestamp"])

    def test_full_window_proxy_health_and_deadlines_remain_required(self):
        self.samples[-1]["statistics"]["nodes"][0]["channels"][0]["operational_track_window"]["complete"] = False
        self.assertEqual(self.checks()["detections"], "inconclusive")
        self.samples[-1]["statistics"]["nodes"][0]["channels"][0]["operational_track_window"].update(complete=True, unmatched_tracks=1)
        self.samples[1]["statistics"]["nodes"][0]["timing"]["drops"] = [1]
        self.samples[2]["statistics"]["nodes"][0]["timing"]["maximum_completed_cycle_ms"] = 501
        checks = self.checks()
        self.assertEqual(checks["false-track-controls"], "rejected")
        self.assertEqual(checks["capture-health"], "rejected")
        self.assertEqual(checks["cpi-deadlines"], "rejected")

    def test_short_window_or_stale_tail_cannot_supply_600_seconds(self):
        self.inputs["evaluationStartEpoch"] = 2060
        self.samples.pop(0)
        self.assertEqual(self.checks()["detections"], "inconclusive")
        self.setUp()
        window = self.samples[-1]["statistics"]["nodes"][0]["channels"][0]["operational_track_window"]
        window.update(started_at=1970000, ended_at=2570000, observed_since=1970000, observed_through=2570000)
        self.assertEqual(self.checks()["detections"], "inconclusive")

    def test_missing_binding_authorization_and_unknown_mode_refused(self):
        for mode in ("implicit-functional", None):
            self.binding["evaluationMode"] = mode
            with self.assertRaises(ValueError): self.evaluate()
        self.binding["evaluationMode"] = O.FUNCTIONAL_MODE
        del self.binding["functionalAuthorizationSha256"]
        with self.assertRaises(ValueError): self.evaluate()

    def test_old_measurements_are_not_credited_to_the_current_window(self):
        state = {name: {"detectedFrames": 0, "tracks": {}} for name in self.binding["nodes"]}
        O.functional_observation(self.samples[0], state, 2000001)
        self.assertTrue(all(value["detectedFrames"] == 0 and not value["tracks"] for value in state.values()))

    def test_real_cli_and_monitor_keep_atomic_revocation_and_bound_identity(self):
        self.authorization["candidateRunId"] = 1
        self.authorize()
        services = {name: {"path": "/release/bin/" + name, "sha256": "a" * 64}
                    for name in ("vectorwarp-api.service", "vectorwarp-processor.service")}
        self.binding["nodes"]["peer"].update(release="/release", hostIdSha256="c" * 64,
            packageFilename="vectorwarp-test.deb", acceleration="cpu", clutterAcceleration="cpu",
            runtimeExecutables=services, runtimeModules={key: {} for key in services},
            endpoint="http://localhost", ssh=None, disk={"path": "/var/lib", "minimumFreeBytes": 1})
        # Exercise the same production CLI, archive and monitor consumer as the
        # default mode; the helper then corrupts an input and verifies revocation.
        fixtures.OperationalAcceptance.test_real_retained_cli_receipt_passes_monitor_and_revokes_success_on_failure(self)


if __name__ == "__main__":
    unittest.main()
