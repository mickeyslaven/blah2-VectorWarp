import argparse
import copy
import importlib.util
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
SPEC = importlib.util.spec_from_file_location("release_regression", ROOT / "tools/vectorwarp_release_regression.py")
E = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(E)


class ReleaseRegression(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.directory = Path(self.temp.name)
        self.archive_path = self.directory / "evidence"
        self.archive_path.mkdir()
        self.archive = E.Evidence(self.archive_path, 1)
        self.retain = lambda value: self.archive.result(value) if isinstance(value, dict) else self.archive.retain(value.encode())
        self.qualification = self.retain("independent radar qualification")
        self.candidate = {"sourceCommit": "c" * 40, "artifactSha256": "d" * 64, "configSha256": "e" * 64}
        self.baseline = {"sourceCommit": "a" * 40, "artifactSha256": "b" * 64, "configSha256": "e" * 64}
        self.policy = {
            "schema": E.POLICY_SCHEMA, "status": "reviewed-before-cohort", "reviewedAtEpoch": 900,
            "reviewEvidenceSha256": self.retain("fixture review, not scientific approval"),
            "measurementMethodSha256": self.retain("fixture measurement method"),
            "selectionMethodSha256": self.retain("fixture selection method"),
            "tuningCutoffEpoch": 500, "truthUsage": "offline-evaluation-only", "minimumFreeBytes": 1,
            "nodes": {"node": {"baseline": self.baseline,
                "exposure": {key: {"minimumPasses": 1, "minimumSeconds": 1,
                                   "minimumOpportunities": 1 if key.endswith("positive") else 0} for key in E.STRATA},
                "limits": {"minimumDetectionFraction": 0.5, "minimumDetectionDelta": 0,
                           "minimumContinuityFraction": 0.5, "minimumContinuityDelta": 0,
                           "maximumFalseTracksPerHour": 1, "maximumCompletedCycleMs": 500}}}}
        self.policy_sha = self.retain(self.policy)
        services = {name: {"path": "/release/bin/" + name, "sha256": "f" * 64}
                    for name in ("vectorwarp-api.service", "vectorwarp-processor.service")}
        self.binding = {"schema": 3, "sourceCommit": self.candidate["sourceCommit"], "candidateRunId": 1,
            "candidateRunAttempt": 1, "monitorMinimumFreeBytes": 1, "regressionPolicySha256": self.policy_sha,
            "nodes": {"node": {"release": "/release", "artifactSha256": self.candidate["artifactSha256"],
                "configSha256": self.candidate["configSha256"], "hostIdSha256": "1" * 64,
                "packageFilename": "vectorwarp-test.deb", "packageSha256": "2" * 64,
                "acceleration": "cpu", "clutterAcceleration": "cpu", "maximumCadenceMs": 600,
                "runtimeExecutables": services, "runtimeModules": {key: {} for key in services},
                "endpoint": "http://localhost", "ssh": None, "disk": {"path": "/var/lib", "minimumFreeBytes": 1},
                "qualifications": {"radar-behavior": self.qualification}}}}
        self.binding_sha = self.retain(self.binding)
        self.passes = []
        for index, stratum in enumerate(E.STRATA):
            split, kind = stratum.split("-")
            start = 10 + index if split == "retrospective" else 1000 + index - 2
            positive = kind == "positive"
            self.passes.append({"schema": "vectorwarp-release-whole-pass-v1", "passId": stratum, "sourceKind": "recorded-iq",
                "split": split, "kind": kind, "startEpoch": start, "endEpoch": start + 1,
                "wholePass": True, "usedForTuning": False, "truthUsage": "offline-evaluation-only",
                "measurementMethodSha256": self.policy["measurementMethodSha256"],
                "recordingSha256": self.retain("recording " + stratum),
                "acquisitionSha256": self.retain("acquisition " + stratum),
                "adjudicationSha256": self.retain("adjudication " + stratum),
                "opportunities": 2 if positive else 0, "eligibleTrackSeconds": 1 if positive else 0,
                **{role: {**identity, "executionSha256": self.retain(role + " execution " + stratum),
                    "productsSha256": self.retain(role + " products " + stratum),
                    "detectedOpportunities": (2 if role == "candidate" else 1) if positive else 0,
                    "continuousMeasurementBackedTrackSeconds": (1 if role == "candidate" else 0.5) if positive else 0,
                    "falseTracks": 0} for role, identity in (("candidate", self.candidate), ("baseline", self.baseline))}})
        capture = {"epoch": "capture-one", "acceptedSamples": 0, **{key: 0 for key in E.LOSS_FIELDS}}
        self.runtime = {"schema": "vectorwarp-release-runtime-v1", **self.candidate,
            "startEpoch": 1000, "endEpoch": 1002, "captureStart": capture,
            "captureEnd": {**capture, "acceptedSamples": 4000}, "completedCpisBefore": 0, "completedCpisThrough": 4,
            "cycles": [{"sequence": i, "startedEpoch": 1000 + i * 0.5 - 0.4,
                        "completedEpoch": 1000 + i * 0.5} for i in range(1, 5)],
            "rawEvidenceSha256": self.retain("complete raw runtime trace")}

    def inputs(self):
        population = {"schema": "vectorwarp-release-population-v1",
            "selectionMethodSha256": self.policy["selectionMethodSha256"],
            "completeFromEpoch": 1000, "completeThroughEpoch": 1002,
            "passes": [self.retain(item) for item in self.passes]}
        return {"schema": E.INPUT_SCHEMA, "bindingSha256": self.binding_sha, "policySha256": self.policy_sha,
            "cohortStartEpoch": 1000, "evaluationStartEpoch": 1000, "evaluatedThroughEpoch": 1002,
            "nodes": {"node": {"qualifications": {"radar-behavior": self.qualification},
                "populationSha256": self.retain(population), "runtimeBlocks": [self.retain(self.runtime)]}}}

    def evaluate(self, inputs=None, now=1002):
        return E.evaluate(self.binding, self.binding_sha, self.policy, self.policy_sha,
                          inputs or self.inputs(), self.archive, now)[0]

    def test_passes_and_retains_all_five_result_objects(self):
        receipt = self.evaluate()
        self.assertFalse(receipt["regressionDetected"])
        self.assertEqual(set(receipt["nodes"]["node"]["checks"]), set(E.REGRESSION_CHECKS))
        for check in receipt["nodes"]["node"]["checks"].values():
            self.assertTrue(check["accepted"])
            self.assertTrue(self.archive.load(check["evidenceSha256"])["accepted"])

    def test_missing_negative_exposure_cannot_pass(self):
        self.passes = [item for item in self.passes if item["split"] != "prospective" or item["kind"] != "negative"]
        receipt = self.evaluate()
        self.assertTrue(receipt["regressionDetected"])
        self.assertFalse(receipt["nodes"]["node"]["checks"]["false-track-controls"]["accepted"])

    def test_no_empty_positive_exposure_pass(self):
        self.passes = [item for item in self.passes if item["kind"] == "negative"]
        checks = self.evaluate()["nodes"]["node"]["checks"]
        self.assertFalse(checks["detections"]["accepted"])
        self.assertFalse(checks["measurement-backed-track-continuity"]["accepted"])

    def test_outcome_regressions_rejected_by_stratum(self):
        for field, value, check in (("detectedOpportunities", 0, "detections"),
                ("continuousMeasurementBackedTrackSeconds", 0, "measurement-backed-track-continuity"),
                ("falseTracks", 1, "false-track-controls")):
            with self.subTest(field=field):
                old = self.passes[2]["candidate"][field]
                self.passes[2]["candidate"][field] = value
                self.assertFalse(self.evaluate()["nodes"]["node"]["checks"][check]["accepted"])
                self.passes[2]["candidate"][field] = old

    def test_false_tracks_in_positive_traffic_are_counted(self):
        self.passes[0]["candidate"]["falseTracks"] = 1
        self.assertTrue(self.evaluate()["regressionDetected"])

    def test_every_completed_cycle_deadline_not_sampled_average(self):
        self.runtime["cycles"][1]["startedEpoch"] -= 0.2
        checks = self.evaluate()["nodes"]["node"]["checks"]
        self.assertFalse(checks["cpi-deadlines"]["accepted"])

    def test_missing_between_poll_cycle_cannot_pass(self):
        self.runtime["cycles"].pop(1)
        with self.assertRaisesRegex(ValueError, "incomplete CPI"):
            self.evaluate()

    def test_sequence_rewrite_cannot_mask_missing_cycle(self):
        self.runtime["cycles"][2]["sequence"] = 4
        with self.assertRaisesRegex(ValueError, "invalid completed cycle"):
            self.evaluate()

    def test_runtime_gap_cannot_pass(self):
        self.runtime["startEpoch"] = 1000.1
        with self.assertRaisesRegex(ValueError, "gaps/overlap"):
            self.evaluate()

    def test_capture_loss_retirement_and_epoch_change(self):
        for field in (*E.LOSS_FIELDS, "epoch"):
            with self.subTest(field=field):
                old = self.runtime["captureEnd"][field]
                self.runtime["captureEnd"][field] = "another-epoch" if field == "epoch" else 1
                receipt = self.evaluate()
                self.assertFalse(receipt["nodes"]["node"]["checks"]["capture-health"]["accepted"])
                if field in ("retiredCpis", "retiredSamples"):
                    self.assertEqual(receipt["nodes"]["node"][field], 1)
                self.runtime["captureEnd"][field] = old

    def test_capture_preexisting_loss_cannot_be_rebased(self):
        self.runtime["captureStart"]["droppedSamples"] = 1
        self.assertTrue(self.evaluate()["regressionDetected"])

    def test_binding_policy_nodes_and_qualifications_are_exact(self):
        for mutation in (lambda value: value.update(bindingSha256="9" * 64),
                         lambda value: value.update(policySha256="9" * 64),
                         lambda value: value["nodes"].update(extra=value["nodes"]["node"]),
                         lambda value: value["nodes"]["node"]["qualifications"].update(extra="9" * 64)):
            inputs = self.inputs()
            mutation(inputs)
            with self.assertRaises(ValueError):
                self.evaluate(inputs)

    def test_candidate_and_baseline_identity_drift_rejected(self):
        for role in ("candidate", "baseline"):
            old = self.passes[0][role]["artifactSha256"]
            self.passes[0][role]["artifactSha256"] = "9" * 64
            with self.assertRaisesRegex(ValueError, "identity differs"):
                self.evaluate()
            self.passes[0][role]["artifactSha256"] = old

    def test_future_stale_and_post_cohort_review_rejected(self):
        for now in (1001, 1183):
            with self.assertRaisesRegex(ValueError, "evaluation interval"):
                self.evaluate(now=now)
        self.policy["reviewedAtEpoch"] = 1001
        with self.assertRaisesRegex(ValueError, "evaluation interval"):
            self.evaluate()

    def test_duplicate_passes_and_recordings_rejected(self):
        self.passes.append(copy.deepcopy(self.passes[0]))
        with self.assertRaisesRegex(ValueError, "duplicate whole-pass"):
            self.evaluate()

    def test_tuning_and_truth_leakage_rejected(self):
        for field, value in (("usedForTuning", True), ("truthUsage", "inference-features"),
                             ("wholePass", False), ("sourceKind", "synthetic")):
            old = self.passes[2][field]
            self.passes[2][field] = value
            with self.assertRaisesRegex(ValueError, "ineligible whole-pass"):
                self.evaluate()
            self.passes[2][field] = old

    def test_unretained_and_altered_evidence_rejected(self):
        path = self.archive_path / self.passes[0]["recordingSha256"]
        original = path.read_bytes()
        path.write_bytes(b"tampered")
        with self.assertRaisesRegex(ValueError, "digest mismatch"):
            self.evaluate()
        path.write_bytes(original)
        path.unlink()
        with self.assertRaisesRegex(ValueError, "missing"):
            self.evaluate()

    def test_json_duplicate_nonfinite_and_boolean_counter_rejected(self):
        for text in ('{"x": 1, "x": 2}', '{"x": NaN}'):
            with self.assertRaises(ValueError):
                E.decode(text)
        self.runtime["captureEnd"]["retiredCpis"] = False
        with self.assertRaisesRegex(ValueError, "capture counters"):
            self.evaluate()

    def test_unapproved_repository_policy_cannot_accept(self):
        policy = json.loads((ROOT / "config/release-regression-policy.unapproved.json").read_text())
        with self.assertRaisesRegex(ValueError, "not been reviewed"):
            E.validate_policy(policy, self.binding, self.archive)

    def args(self):
        for name, value in (("binding", self.binding), ("policy", self.policy), ("inputs", self.inputs())):
            (self.directory / (name + ".json")).write_bytes(E.encoded(value))
        return argparse.Namespace(**{name: str(self.directory / (name + ".json")) for name in ("binding", "policy", "inputs")},
                                  evidence_dir=str(self.archive_path), output=str(self.directory / "latest.json"))

    def test_published_receipt_passes_canonical_soak_and_retains_inputs(self):
        import vectorwarp_two_node_soak as soak
        args = self.args()
        with patch.object(E.time, "time", return_value=1002):
            self.assertEqual(E.run(args), 0)
        receipt = json.loads(Path(args.output).read_text())
        self.assertEqual(soak.verify_regression(receipt, {"sha256": self.binding_sha, "expected": self.binding}, 1002, 1000), [])
        report = self.archive.load(receipt["evidenceSha256"])
        self.assertEqual(report["receipt"], {key: value for key, value in receipt.items() if key != "evidenceSha256"})
        for reference in report["retainedEvidence"]:
            self.archive.load(reference, structured=False)

    def test_invalid_next_evaluation_revokes_prior_success(self):
        args = self.args()
        with patch.object(E.time, "time", return_value=1002):
            E.run(args)
        Path(args.inputs).write_text("{}")
        with self.assertRaises(ValueError):
            E.run(args)
        receipt = json.loads(Path(args.output).read_text())
        self.assertTrue(receipt["regressionDetected"])
        self.assertNotIn("bindingSha256", receipt)

    def test_slow_evaluation_cannot_publish_a_stale_success(self):
        args = self.args()
        with patch.object(E.time, "time", side_effect=[1002, 1183]):
            with self.assertRaisesRegex(ValueError, "became stale"):
                E.run(args)
        self.assertTrue(json.loads(Path(args.output).read_text())["regressionDetected"])

    def test_evidence_disk_reserve_cannot_be_consumed(self):
        with patch.object(E.shutil, "disk_usage") as usage:
            usage.return_value.free = 1
            with self.assertRaisesRegex(ValueError, "disk reserve"):
                self.retain("new object")

    def test_output_cannot_destroy_input_or_evidence(self):
        args = self.args()
        original = Path(args.policy).read_bytes()
        args.output = args.policy
        with self.assertRaisesRegex(ValueError, "overwrite an input"):
            E.run(args)
        self.assertEqual(Path(args.policy).read_bytes(), original)
        args.output = str(self.archive_path / "latest.json")
        with self.assertRaisesRegex(ValueError, "outside hash-addressed"):
            E.run(args)


if __name__ == "__main__":
    unittest.main()
