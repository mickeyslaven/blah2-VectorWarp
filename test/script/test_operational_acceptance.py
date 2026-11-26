#!/usr/bin/env python3
"""Operational acceptance fixtures are tests, never release evidence."""
import copy
from pathlib import Path
import sys
import tempfile
import unittest
from types import SimpleNamespace
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
import vectorwarp_operational_acceptance as O
import vectorwarp_release_regression as R
import vectorwarp_two_node_soak as monitor
from vectorwarp_release_regression import Evidence


class OperationalAcceptance(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.archive = Evidence(Path(self.directory.name), 1)
        self.identity = {"sourceCommit": "c" * 40, "artifactSha256": "d" * 64,
                         "configSha256": "e" * 64, "packageSha256": "f" * 64}
        self.baseline_identity = {**self.identity, "sourceCommit": "a" * 40, "artifactSha256": "b" * 64}
        self.binding = {"sourceCommit": self.identity["sourceCommit"],
            "nodes": {"site": {**self.identity, "maximumCadenceMs": 500,
                "qualifications": {"radar-behavior": "1" * 64}}}}
        self.baseline = self.sample(1700, self.baseline_identity)
        self.policy = {"schema": O.POLICY_SCHEMA, "status": "reviewed-before-install",
            "reviewedAtEpoch": 1700, "baselineFrozenAtEpoch": 1701,
            "reviewEvidenceSha256": self.archive.retain(b"fixture-only review"), "minimumFreeBytes": 1,
            "baselineIdentities": {"site": self.baseline_identity},
            "baselineSha256": self.archive.result(self.baseline), "minimumEligiblePasses": 3,
            "yieldTolerance": 0, "maximumBacklogMs": 500, "maximumSampleGapSeconds": 60}
        self.samples = [self.sample(epoch, self.identity) for epoch in range(2000, 2601, 60)]
        self.inputs = {"schema": O.INPUT_SCHEMA, "bindingSha256": "2" * 64,
            "evaluationStartEpoch": 2000, "evaluatedThroughEpoch": 2600}

    def sample(self, epoch, identity):
        end = epoch * 1000
        passes = [{"identity": str(index), "started_at": end - 10000 - index * 1000,
            "finalized_at": end - index * 1000, "eligible": True, "detected": True, "tracked": True,
            "provenance": [{"evaluation_policy_hash": "3" * 64, "sensitivity_epoch_mixed": False}]}
            for index in range(3)]
        window = {"schema": "vectorwarp-operational-track-window-v1", "started_at": end - 600000,
            "ended_at": end, "observed_since": end - 600000, "observed_through": end,
            "complete": True, "retained_tracks": 3, "unmatched_tracks": 0}
        node = {"id": "site", "available": True, "radar": "receiving", "processor_fresh": True,
            "config_match": True, "config_revision": identity["configSha256"],
            "loaded_revision": identity["configSha256"], "evidence_status": "healthy", "tracking_status": "healthy",
            "timing": {"cpi_ms": 400, "maximum_completed_cycle_ms": 450,
                "maximum_completion_interval_ms": 500, "backlog_ms": 0, "drops": [0],
                "retired_cpis": 0, "retired_samples": 0},
            "channels": [{"id": "primary", "operational_track_window": window}]}
        return {"schema": "vectorwarp-operational-release-snapshot-v1", "epoch": epoch,
            "nodes": {"site": identity}, "statistics": {"schema": "vectorwarp-network-statistics-v1",
                "updated_at": end, "nodes": [node], "evaluation": {"policy_compatible": True,
                    "policy_hashes": ["3" * 64]}, "statistics_window": {"alignment_pending": False},
                "system_wide_unique_passes": {"scope": "all-nodes-all-channels-deduplicated",
                    "window": "common-retained-finalized-passes", "alignment_pending": False,
                    "passes_complete": True, "passes": passes}}}

    def evaluate(self):
        policy_sha = self.archive.result(self.policy)
        self.binding["regressionPolicySha256"] = policy_sha
        self.inputs.update(policySha256=policy_sha, snapshots=[self.archive.result(row) for row in self.samples])
        return O.evaluate(self.binding, "2" * 64, self.policy, policy_sha, self.inputs, self.archive, 2600)[0]

    def checks(self):
        return {key: value["status"] for key, value in self.evaluate()["nodes"]["site"]["checks"].items()}

    def test_accepts_exact_complete_600_second_operational_window(self):
        self.assertEqual(set(self.checks().values()), {"accepted"})
        self.assertEqual(self.evaluate()["evaluationMode"], O.MODE)

    def test_no_traffic_is_inconclusive(self):
        self.samples[-1]["statistics"]["system_wide_unique_passes"]["passes"] = []
        self.assertEqual(self.checks()["detections"], "inconclusive")
        self.assertFalse(self.evaluate()["regressionDetected"])

    def test_partial_track_coverage_and_pass_overflow_are_inconclusive(self):
        self.samples[-1]["statistics"]["nodes"][0]["channels"][0]["operational_track_window"]["complete"] = False
        self.assertEqual(self.checks()["false-track-controls"], "inconclusive")
        self.setUp()
        self.samples[-1]["statistics"]["system_wide_unique_passes"]["passes_complete"] = False
        self.assertEqual(self.checks()["detections"], "inconclusive")

    def test_increase_in_unmatched_proxy_rejects(self):
        self.samples[-1]["statistics"]["nodes"][0]["channels"][0]["operational_track_window"]["unmatched_tracks"] = 1
        self.assertEqual(self.checks()["false-track-controls"], "rejected")

    def test_detection_and_measured_tracking_regressions_reject(self):
        row = self.samples[-1]["statistics"]["system_wide_unique_passes"]["passes"][0]
        row.update(detected=False, tracked=False)
        self.assertEqual(self.checks()["detections"], "rejected")
        row.update(detected=True, tracked=False)
        self.assertEqual(self.checks()["measurement-backed-track-continuity"], "rejected")

    def test_early_capture_or_deadline_fault_cannot_be_hidden_by_latest_success(self):
        self.samples[1]["statistics"]["nodes"][0]["timing"]["drops"] = [1]
        self.samples[2]["statistics"]["nodes"][0]["timing"]["maximum_completed_cycle_ms"] = 501
        self.assertEqual(self.checks()["capture-health"], "rejected")
        self.assertEqual(self.checks()["cpi-deadlines"], "rejected")

    def test_missing_fields_config_source_or_policy_mismatch_refused(self):
        original = copy.deepcopy(self.samples)
        for mutate in (lambda row: row["nodes"]["site"].update(artifactSha256="0" * 64),
                       lambda row: row["statistics"]["nodes"][0].update(loaded_revision="0" * 64),
                       lambda row: row["statistics"]["evaluation"].update(policy_hashes=["0" * 64]),
                       lambda row: row["statistics"]["nodes"][0]["timing"].pop("drops")):
            self.samples = copy.deepcopy(original)
            mutate(self.samples[-1])
            with self.assertRaises(ValueError): self.evaluate()

    def test_unfrozen_baseline_and_observation_gaps_refused(self):
        self.policy["baselineFrozenAtEpoch"] = 2000
        with self.assertRaises(ValueError): self.evaluate()
        self.policy["baselineFrozenAtEpoch"] = 1701
        del self.samples[1]
        with self.assertRaises(ValueError): self.evaluate()

    def test_old_preinstall_passes_do_not_supply_candidate_exposure(self):
        for row in self.samples[-1]["statistics"]["system_wide_unique_passes"]["passes"]:
            row["started_at"] -= 700000
        self.assertEqual(self.checks()["detections"], "inconclusive")

    def test_tolerance_is_explicit_bounded_and_does_not_allow_zero_detections(self):
        self.policy["yieldTolerance"] = .06
        with self.assertRaises(ValueError): self.evaluate()
        self.policy["yieldTolerance"] = .05
        for row in self.samples[-1]["statistics"]["system_wide_unique_passes"]["passes"]:
            row.update(detected=False, tracked=False)
        self.assertEqual(self.checks()["detections"], "rejected")

    def test_real_retained_cli_receipt_passes_monitor_and_revokes_success_on_failure(self):
        root = Path(self.directory.name)
        evidence_dir = root / "evidence"
        evidence_dir.mkdir()
        for item in list(root.iterdir()):
            if item.is_file(): item.rename(evidence_dir / item.name)
        self.archive = Evidence(evidence_dir, 1)
        services = {name: {"path": "/release/bin/" + name, "sha256": "a" * 64}
                    for name in ("vectorwarp-api.service", "vectorwarp-processor.service")}
        self.binding.update(schema=3, candidateRunId=1, candidateRunAttempt=1, monitorMinimumFreeBytes=1)
        self.binding["nodes"]["site"].update(release="/release", hostIdSha256="b" * 64,
            packageFilename="vectorwarp-test.deb", acceleration="cpu", clutterAcceleration="cpu",
            runtimeExecutables=services, runtimeModules={key: {} for key in services},
            endpoint="http://localhost", ssh=None, disk={"path": "/var/lib", "minimumFreeBytes": 1})
        self.evaluate()
        binding_raw = R.encoded(self.binding)
        self.inputs["bindingSha256"] = R.sha256(binding_raw)
        args = SimpleNamespace(binding=root / "binding.json", policy=root / "policy.json",
            inputs=root / "inputs.json", evidence_dir=evidence_dir, output=root / "receipt.json")
        args.binding.write_bytes(binding_raw)
        args.policy.write_bytes(R.encoded(self.policy))
        args.inputs.write_bytes(R.encoded(self.inputs))
        with patch.object(R.time, "time", return_value=2600):
            self.assertEqual(R.run(args), 0)
        receipt = R.decode(args.output.read_bytes())
        self.assertEqual(monitor.verify_regression(receipt,
            {"sha256": self.inputs["bindingSha256"], "expected": self.binding}, 2600, 2000), [])
        self.inputs["snapshots"] = ["0" * 64]
        args.inputs.write_bytes(R.encoded(self.inputs))
        with patch.object(R.time, "time", return_value=2600), self.assertRaises(ValueError):
            R.run(args)
        self.assertEqual(R.decode(args.output.read_bytes())["error"], "evaluation-in-progress-or-failed")


if __name__ == "__main__":
    unittest.main()
