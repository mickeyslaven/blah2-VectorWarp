#!/usr/bin/env python3
"""Operational acceptance fixtures are tests, never release evidence."""
import copy
import json
from pathlib import Path
import subprocess
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
            "statistics_window": {"retained_since": end - 600000, "ended_at": end,
                "requested_started_at": 0, "complete": False},
            "config_match": True, "config_revision": identity["configSha256"],
            "loaded_revision": identity["configSha256"], "evidence_status": "healthy", "tracking_status": "healthy",
            "timing": {"cpi_ms": 400, "maximum_completed_cycle_ms": 450,
                "maximum_completion_interval_ms": 500, "backlog_ms": 0, "drops": [0],
                "retired_cpis": 0, "retired_samples": 0},
            "channels": [{"id": "primary", "operational_track_window": window}]}
        return {"schema": "vectorwarp-operational-release-snapshot-v1", "epoch": epoch,
            "nodes": {"site": identity}, "statistics": {"schema": "vectorwarp-network-statistics-v1",
                "updated_at": end, "nodes": [node], "evaluation": {"policy_compatible": True,
                    "policy_hashes": ["3" * 64]}, "statistics_window": {"alignment_pending": False,
                        "started_at": end - 600000},
                "system_wide_unique_passes": {"scope": "all-nodes-all-channels-deduplicated",
                    "window": "common-retained-finalized-passes", "alignment_pending": False,
                    "passes_complete": True, "window_started_at": end - 600000, "passes": passes}}}

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

    def test_real_http_federation_transport_preserves_acceptance_provenance(self):
        # Synthetic observations traverse the production encoder, bounded HTTP
        # reader, node compactor and network deduplicator before this evaluator.
        program = r'''
const http = require('node:http'), fs = require('node:fs');
const {federationAssociationPassTransport} = require('./api/evidence-statistics');
const {probeJson, compactNodeStatistics, combineNodeStatistics} = require('./api/node-federation');
const input = JSON.parse(fs.readFileSync(0, 'utf8'));
let payload;
const server = http.createServer((req, res) => res.end(JSON.stringify(payload)));
(async () => {
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  try {
    for (const sample of input.samples) {
      const end = sample.epoch * 1000, old = sample.statistics.nodes[0];
      const rows = sample.statistics.system_wide_unique_passes.passes.map((row, index) => ({
        ...row, hex: row.identity, pass_id: `pass-${index}`, channel: 'primary',
        site_epoch: '1'.repeat(64), evaluation_contract_hash: '2'.repeat(64),
        evaluation_policy_hash: '3'.repeat(64), timing_policy: 'provisional',
        sensitivity_epoch_id: '5'.repeat(64), sensitivity_epoch_mixed: false,
        decision_grade: false, eligible_cpis: 6, association_eligible_cpis: 5,
        detected_cpis: 4, tracked_cpis: 3, outcome: 'tracked'}));
      rows.push({...rows[0], hex: 'surplus', pass_id: 'surplus'});
      rows.push({...rows[0], hex: 'anchor', pass_id: 'anchor',
        started_at: end - 700000, finalized_at: end - 500000});
      if (input.mode === 'mixed') rows[0].sensitivity_epoch_mixed = true;
      if (input.mode === 'changed') rows[0].evaluation_policy_hash = '4'.repeat(64);
      payload = {statistics_window: old.statistics_window,
        transport: federationAssociationPassTransport(rows)};
      if (input.mode === 'legacy') {
        payload.transport.rows = payload.transport.rows.map(row => row.slice(0, 10));
        delete payload.transport.dictionaries.provenance;
        payload.transport.fields = payload.transport.fields.slice(0, 10);
      }
      const row = payload.transport.rows[0], identity = payload.transport.dictionaries.provenance?.[row[11]];
      if (input.mode === 'null-flags') row[9] = null;
      if (input.mode === 'missing-flags') delete row[9];
      if (input.mode === 'fractional-flags') row[9] = 7.5;
      if (input.mode === 'missing-contract') identity[1] = null;
      if (input.mode === 'malformed-site') identity[0] = {};
      if (input.mode === 'malformed-timing') identity[3] = 'guessed';
      if (input.mode === 'missing-sensitivity') identity[4] = null;
      if (input.mode === 'missing-association') row[12] = null;
      if (input.mode === 'removed-column12') row.pop();
      if (input.mode === 'wrong-column-contract') payload.transport.fields[12] = 'eligible_cpis';
      if (input.mode === 'null-row') payload.transport.rows[0] = null;
      if (input.mode === 'empty-row') payload.transport.rows[0] = '';
      if (input.mode === 'whitespace-identity') { row[0] = ' '; row[1] = ''; }
      if (input.mode === 'erased-eligible-row') { row[9] = 0; row[6] = 0; }
      const response = await probeJson(`http://127.0.0.1:${server.address().port}`, '/passes', 2500, 1024 * 1024);
      const capture = {available: true, epoch: '0', discontinuities: '0', requalifications: '0',
        retiredCpis: '0', retiredSamples: '0', lanes: [{acceptedSamples: '1', droppedSamples: '0', discardedSamples: '0'}]};
      const node = compactNodeStatistics({id: 'site'}, {radar: 'receiving', processorFresh: true,
        processor: {state: 'live'}, configRevision: old.config_revision, loadedRevision: old.loaded_revision,
        featureDiagnostics: {processing: {configuredCpiMs: 500}},
        trackingPerformance: {status: 'healthy'}, evidenceHealth: {status: 'healthy'}},
        {cpi: 400, capture_backlog_ms: 500, captureDroppedSamples: [0],
          processingCycle: {processing: {maximumMs: 450}, completion_interval: {maximumMs: 500}},
          cycleEvidence: {schema: 'vectorwarp-processing-cycles-v1', anchorMonoNs: '1', anchorUtcNs: '1',
            fault: '', records: [{sequence: '1', startMonoNs: '2', endMonoNs: '3', drainBatches: '0',
              outcome: 'completed', captureBefore: capture, captureAfter: capture}]}},
        {channels: [{selected_channel: 'primary', operational_track_window: old.channels[0].operational_track_window,
          association_statistics: {evaluation_policy_hash: '3'.repeat(64)}}]}, response.value);
      Date.now = () => end;
      sample.statistics = combineNodeStatistics([node]);
    }
    process.stdout.write(JSON.stringify(input.samples));
  } finally { server.close(); }
})().catch(error => { console.error(error); process.exitCode = 1; });
'''
        original = copy.deepcopy(self.samples)
        for mode in ('normal', 'legacy', 'mixed', 'changed', 'null-flags', 'missing-flags',
                     'fractional-flags', 'missing-contract', 'malformed-site', 'malformed-timing',
                     'missing-sensitivity', 'missing-association', 'removed-column12', 'wrong-column-contract',
                     'null-row', 'empty-row', 'whitespace-identity', 'erased-eligible-row'):
            with self.subTest(mode=mode):
                result = subprocess.run(['node', '-e', program],
                    cwd=Path(__file__).resolve().parents[2],
                    input=json.dumps({'samples': original, 'mode': mode}).encode(),
                    capture_output=True, timeout=30)
                if mode not in ('normal', 'legacy', 'mixed', 'changed'):
                    self.assertNotEqual(result.returncode, 0)
                    self.assertIn(b'Invalid federation pass provenance batch', result.stderr)
                    self.assertEqual(result.stdout, b'', 'partial good rows must not reach acceptance')
                    continue
                self.assertEqual(result.returncode, 0, result.stderr)
                self.samples = json.loads(result.stdout)
                if mode == 'normal':
                    self.assertEqual(set(self.checks().values()), {'accepted'})
                else:
                    with self.assertRaisesRegex(ValueError, 'mixed or incompatible pass provenance'):
                        self.evaluate()

    def test_no_traffic_is_inconclusive(self):
        self.samples[-1]["statistics"]["system_wide_unique_passes"]["passes"] = []
        self.assertEqual(self.checks()["detections"], "inconclusive")
        self.assertFalse(self.evaluate()["regressionDetected"])

    def test_short_retention_and_late_network_alignment_cannot_pass(self):
        stats = self.samples[-1]["statistics"]
        stats["nodes"][0]["statistics_window"]["retained_since"] = 2595000
        self.assertEqual(self.checks()["detections"], "inconclusive",
                         'five seconds of retained passes cannot certify600seconds')
        stats["nodes"][0]["statistics_window"]["retained_since"] = 2000000
        stats["statistics_window"]["started_at"] = 2595000
        stats["system_wide_unique_passes"]["window_started_at"] = 2595000
        self.assertEqual(self.checks()["detections"], "inconclusive")

    def test_stale_retained_tail_cannot_shorten_required_600_second_exposure(self):
        stats = self.samples[-1]["statistics"]
        stats["system_wide_unique_passes"]["passes"] = self.sample(
            2570, self.identity)["statistics"]["system_wide_unique_passes"]["passes"]
        for stale_ms, expected in ((30000, "inconclusive"), (1, "inconclusive"), (0, "accepted")):
            with self.subTest(stale_ms=stale_ms):
                stats["nodes"][0]["statistics_window"]["ended_at"] = 2600000 - stale_ms
                checks = self.checks()
                self.assertEqual(checks["detections"], expected)
                self.assertEqual(checks["measurement-backed-track-continuity"], expected)
                self.assertFalse(self.evaluate()["regressionDetected"])

    def test_pending_alignment_without_traffic_is_inconclusive_not_success(self):
        stats = self.samples[-1]["statistics"]
        stats["statistics_window"].update(alignment_pending=True, started_at=None)
        stats["system_wide_unique_passes"].update(alignment_pending=True, window_started_at=None, passes=[])
        self.assertEqual(self.checks()["detections"], "inconclusive")
        self.assertFalse(self.evaluate()["regressionDetected"])

    def test_missing_or_inconsistent_retention_fails_closed(self):
        stats = self.samples[-1]["statistics"]
        window = stats["nodes"][0]["statistics_window"]
        window["complete"] = True
        with self.assertRaises(ValueError): self.evaluate()
        window["complete"] = False
        del window["retained_since"]
        with self.assertRaises(ValueError): self.evaluate()

    def test_multi_channel_retention_must_cover_every_channel(self):
        node = self.samples[-1]["statistics"]["nodes"][0]
        leaf = copy.deepcopy(node["statistics_window"])
        node["statistics_window"] = {"complete": False, "retained_since": 2000000,
            "channels": [{**leaf, "channel": "primary"}]}
        self.assertEqual(self.checks()["detections"], "accepted")
        node["statistics_window"]["channels"][0]["channel"] = "unrelated"
        with self.assertRaises(ValueError): self.evaluate()

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
                       lambda row: row["statistics"]["nodes"][0]["timing"].update(retired_cpis=None),
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
