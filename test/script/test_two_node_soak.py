import importlib.util
import copy
import json
from pathlib import Path
import tempfile
import time
import unittest
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location(
    "two_node_soak", ROOT / "tools" / "vectorwarp_two_node_soak.py")
SOAK = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(SOAK)


def status(server="one"):
    return {"serverId": server, "build": {"commit": "abc"},
            "loadedRevision": "cfg", "configRevision": "cfg", "radar": "receiving",
            "processorFresh": True, "lastFrameAt": time.time() * 1000,
            "acceleration": {"active": "vulkan"},
            "clutterAcceleration": {"active": "cpu"}, "errors": [],
            "featureDiagnostics": {"recording": {"active": False, "requested": False,
                                                   "errorPresent": False}}}


def timing(cpi=10, drops=(0, 0), boundary=(0, 0)):
    return {"nCpi": cpi, "cpi": 400, "frame_age_ms": 500,
            "capture_backlog_ms": 500, "captureDroppedSamples": list(drops),
            "captureBoundaryDiscardedSamples": list(boundary),
            "captureStopDiscardedSamples": [0, 0], "inputDiscontinuity": False,
            "captureTerminal": False, "captureEpoch": 1}


def receiver_status(epoch="1", discontinuities="0", discarded="0", section="recovery"):
    value = status()
    receipt = {"discontinuities": discontinuities, "requalifications": discontinuities}
    if section == "recovery":
        receipt.update(discardedBlocks=discontinuities, discardedSamplesPerChannel=discarded)
        health = {"deviceTime": {"epoch": epoch}, "recovery": receipt}
    else:
        receipt.update(epoch=epoch, discardedSamples=[discarded, discarded])
        health = {"pairing": receipt}
    value["processor"] = {"receiverHealth": health}
    return value


class SoakEvaluation(unittest.TestCase):
    def test_completed_cycle_totals_survive_compaction_for_interval_averages(self):
        completed = {"scope": "completed_multi_consumer_cycles",
                     "lastCompletedNCpi": 9, "lastCompletedTimestamp": 1000,
                     "processing": {"samples": 9, "totalMs": 4500,
                                    "latestMs": 440, "meanMs": 500,
                                    "maximumMs": 610}}
        frame = {**timing(), "processingCycle": completed, "unretained": "large"}
        compact = SOAK.compact_timing(frame)
        self.assertEqual(compact["processingCycle"], completed)
        self.assertEqual(compact["cpi"], 400)
        self.assertNotIn("unretained", compact)

    def evaluate(self, stat=None, frame=None, previous=None):
        return SOAK.evaluate("node", stat or status(), frame or timing(),
                             {"release": "/release", "units": {}}, previous,
                             "vulkan", "cpu")

    def test_first_sample_rejects_preexisting_loss(self):
        failures, _ = self.evaluate(frame=timing(drops=(4, 4)))
        self.assertIn("node:preexisting-capture-loss", failures)

    def test_advancement_and_unchanged_counters_are_healthy(self):
        _, first = self.evaluate()
        first["epoch"] = time.time() - 5
        failures, observation = self.evaluate(frame=timing(cpi=20), previous=first)
        self.assertEqual(failures, [])
        self.assertEqual(observation["advancement"], 10)
        self.assertLess(observation["cadenceMs"], 600)

    def test_drop_delta_and_stall_fail(self):
        _, first = self.evaluate()
        first["epoch"] = time.time() - 5
        failures, _ = self.evaluate(frame=timing(cpi=10, drops=(1, 0)), previous=first)
        self.assertIn("node:cpi-not-advancing", failures)
        self.assertIn("node:capture-drops", failures)

    def test_cpi_counter_reset_invalidates_cohort_after_recovery(self):
        _, first = self.evaluate(frame=timing(cpi=100))
        first["epoch"] = time.time() - 5
        failures, _ = self.evaluate(frame=timing(cpi=20), previous=first)
        self.assertIn("node:capture-cpi-counter-reset", failures)
        started, rows = self.records([(0, []), (1, failures), (2, []), (12, [])], window=10)
        self.assertFalse(self.summary(started, rows[-1:])["startupAccepted"])

    def test_capture_counters_reject_javascript_rounding_and_keep_exact_strings(self):
        self.assertIsNone(SOAK.numeric_list([2**53, 0]))
        self.assertEqual(SOAK.numeric_list([str(2**64 - 1), "0"]), [2**64 - 1, 0])
        _, first = self.evaluate(frame=timing(drops=(str(2**53 + 1), "0")))
        first = json.loads(json.dumps(first))
        self.assertEqual(first["counters"]["drops"], [str(2**53 + 1), "0"])
        first["epoch"] = time.time() - 5
        failures, _ = self.evaluate(frame=timing(cpi=20, drops=(str(2**53 + 2), "0")),
                                   previous=first)
        self.assertIn("node:capture-drops", failures)
        self.assertNotIn("node:capture-drops-unverifiable", failures)

    def test_fresh_degraded_radar_is_not_healthy(self):
        failures, _ = self.evaluate(stat={**status(), "radar": "degraded"})
        self.assertIn("node:radar-not-receiving", failures)

    def test_restart_is_one_observed_boundary(self):
        _, first = self.evaluate()
        first["epoch"] = time.time() - 5
        failures, observation = self.evaluate(stat=status("two"), previous=first)
        self.assertIn("node:process-restarted", failures)
        self.assertTrue(observation["restarted"])

    def test_same_api_identity_cannot_hide_pid_restart_count_or_start_time_change(self):
        process = {"release": "/release", "bootId": "boot",
                   "units": {"vectorwarp-processor.service": {
                       "ActiveState": "active", "Result": "success", "MainPID": "100",
                       "NRestarts": "0", "ExecMainStartTimestampMonotonic": "123"}}}
        _, first = SOAK.evaluate("node", status(), timing(), process, None, "vulkan", "cpu")
        for key in ("MainPID", "NRestarts", "ExecMainStartTimestampMonotonic"):
            changed = copy.deepcopy(process)
            changed["units"]["vectorwarp-processor.service"][key] = "200"
            failures, _ = SOAK.evaluate("node", status(), timing(cpi=20), changed,
                                       first, "vulkan", "cpu")
            self.assertIn("node:process-restarted", failures)

    def test_capture_epoch_change_resets_even_with_same_process_and_zero_counters(self):
        _, first = self.evaluate(frame={**timing(), "captureEpoch": 1})
        first["epoch"] = time.time() - 5
        failures, _ = self.evaluate(frame={**timing(cpi=20), "captureEpoch": 2}, previous=first)
        self.assertIn("node:capture-epoch-reset", failures)

    def test_between_poll_receiver_recovery_is_sticky_despite_fresh_zero_drop_frame(self):
        for section in ("recovery", "pairing"):
            with self.subTest(section=section):
                _, first = self.evaluate(stat=receiver_status(section=section))
                first["epoch"] = time.time() - 5
                stat = receiver_status(epoch="2", discontinuities="1", discarded="4096",
                                       section=section)
                failures, observation = self.evaluate(stat=stat, frame=timing(cpi=20), previous=first)
                self.assertIn("node:capture-epoch-reset", failures)
                self.assertIn("node:capture-receiver-loss", failures)
                self.assertEqual(observation["receiverContinuity"]["counters"][
                    f"{section}.discontinuities"], ["1"])
                # The later clean sample and rolling-ledger eviction cannot
                # restore this cohort's zero-loss claim.
                observation["epoch"] = time.time() - 5
                later, _ = self.evaluate(stat=stat, frame=timing(cpi=30), previous=observation)
                self.assertEqual(later, [])
                started, rows = self.records([(0, []), (1, failures), (2, later), (12, [])], window=10)
                self.assertFalse(self.summary(started, rows[-1:])["startupAccepted"])

    def test_receiver_loss_before_first_poll_cannot_be_a_clean_baseline(self):
        for section in ("recovery", "pairing"):
            failures, _ = self.evaluate(stat=receiver_status(discontinuities="3", discarded="0",
                                                            section=section))
            self.assertIn("node:preexisting-capture-loss", failures)

    def test_cumulative_receiver_loss_is_detected_without_epoch_change(self):
        _, first = self.evaluate(stat=receiver_status())
        first["epoch"] = time.time() - 5
        failures, _ = self.evaluate(stat=receiver_status(discarded="1"),
                                   frame=timing(cpi=20), previous=first)
        self.assertIn("node:capture-receiver-loss", failures)
        self.assertNotIn("node:capture-epoch-reset", failures)

    def test_relative_sample_and_gain_epoch_capabilities_share_the_same_gate(self):
        for section in ("sampleTime", "gain"):
            stat = status()
            stat["processor"] = {"receiverHealth": {section: {"epoch": "1"}}}
            _, first = self.evaluate(stat=stat)
            first["epoch"] = time.time() - 5
            stat["processor"]["receiverHealth"][section]["epoch"] = "2"
            failures, _ = self.evaluate(stat=stat, frame=timing(cpi=20), previous=first)
            self.assertIn("node:capture-epoch-reset", failures)

    def test_exact_uint64_epochs_and_receiver_counters_survive_json_roundtrip(self):
        epoch, count = str(2**64 - 2), str(2**53 + 1)
        frame = {**timing(), "captureEpoch": 2**64, "eventTime": {"epoch": epoch}}
        _, first = self.evaluate(stat=receiver_status(epoch=epoch, discarded=count), frame=frame)
        first = json.loads(json.dumps(first))
        self.assertEqual(first["captureEpoch"], epoch)
        self.assertEqual(first["receiverContinuity"]["counters"][
            "recovery.discardedSamplesPerChannel"], [count])
        first["epoch"] = time.time() - 5
        frame.update(nCpi=20, eventTime={"epoch": str(2**64 - 1)})
        failures, _ = self.evaluate(stat=receiver_status(epoch=str(2**64 - 1),
            discarded=str(int(count) + 1)), frame=frame, previous=first)
        self.assertIn("node:capture-epoch-reset", failures)
        self.assertIn("node:capture-receiver-loss", failures)
        self.assertEqual(SOAK.compact_timing(frame)["eventTime"]["epoch"], str(2**64 - 1))

    def test_missing_inexact_and_malformed_epoch_fail_closed(self):
        for value in (None, False, -1, 1.5, 2**53, "01", "-1", str(2**64)):
            with self.subTest(value=value):
                failures, _ = self.evaluate(frame={**timing(), "captureEpoch": value})
                self.assertIn("node:capture-epoch-unverifiable", failures)

    def test_receiver_receipt_disappearance_reset_and_malformed_values_fail(self):
        _, first = self.evaluate(stat=receiver_status(discarded="10"))
        first["epoch"] = time.time() - 5
        failures, _ = self.evaluate(frame=timing(cpi=20), previous=first)
        self.assertIn("node:capture-receiver-unverifiable", failures)
        failures, _ = self.evaluate(stat=receiver_status(), frame=timing(cpi=20), previous=first)
        self.assertIn("node:capture-receiver-counter-reset", failures)
        for value in (None, False, "01", str(2**64), 2**53, []):
            with self.subTest(value=value):
                failures, _ = self.evaluate(stat=receiver_status(discarded=value),
                                           frame=timing(cpi=20), previous=first)
                self.assertIn("node:capture-receiver-unverifiable", failures)

    def test_process_restart_does_not_compare_epochs_or_receiver_counter_baselines(self):
        _, first = self.evaluate(stat=receiver_status(epoch="3", discarded="10"))
        stat = receiver_status(epoch="0")
        stat["serverId"] = "new-process"
        failures, _ = self.evaluate(stat=stat, frame={**timing(), "captureEpoch": 0}, previous=first)
        self.assertIn("node:process-restarted", failures)
        self.assertNotIn("node:capture-epoch-reset", failures)
        self.assertNotIn("node:capture-receiver-counter-reset", failures)

    def test_reviewed_cadence_limit_is_enforced(self):
        _, first = self.evaluate()
        first["epoch"] = time.time() - 5.5
        failures, _ = SOAK.evaluate("node", status(), timing(cpi=20),
                                   {"release": "/release", "units": {}}, first,
                                   "vulkan", "cpu", maximum_cadence_ms=500)
        self.assertIn("node:cadence", failures)

    def test_remote_release_is_not_fabricated(self):
        self.assertIsNone(SOAK.remote_process()["release"])

    def test_missing_remote_host_fails_before_probing_or_writing(self):
        for environment in ({}, {"VECTORWARP_SOAK_HP_HOST": "  "}):
            with tempfile.TemporaryDirectory() as temporary, \
                    patch.dict(SOAK.os.environ, environment, clear=True), \
                    patch.object(SOAK, "fetch") as fetch, \
                    patch.object(SOAK, "local_process") as probe:
                with self.assertRaisesRegex(ValueError, "VECTORWARP_SOAK_HP_HOST"):
                    SOAK.run_sample(Path(temporary))
                fetch.assert_not_called()
                probe.assert_not_called()
                self.assertEqual(list(Path(temporary).iterdir()), [])

    def test_configured_remote_host_is_used(self):
        with tempfile.TemporaryDirectory() as temporary, \
                patch.dict(SOAK.os.environ, {"VECTORWARP_SOAK_HP_HOST": " node-b.example.test "},
                           clear=True), \
                patch.object(SOAK, "local_process", return_value={}), \
                patch.object(SOAK, "fetch", side_effect=RuntimeError("offline fixture")) as fetch:
            self.assertEqual(SOAK.run_sample(Path(temporary)), 0)
            self.assertEqual([call.args[0] for call in fetch.call_args_list], [
                "http://127.0.0.1:3000/api/system/status",
                "http://node-b.example.test:3000/api/system/status"])

    def test_missing_or_malformed_cpi_and_capture_counters_fail_closed(self):
        _, previous = self.evaluate()
        previous["epoch"] = time.time() - 5
        for value in (None, True, -1, 1.5, float("nan"), float("inf"), "20"):
            frame = timing(cpi=20)
            if value is None:
                del frame["nCpi"]
            else:
                frame["nCpi"] = value
            with self.subTest(cpi=value):
                failures, _ = self.evaluate(frame=frame, previous=previous)
                self.assertIn("node:invalid-cpi-counter", failures)
        for key in ("captureDroppedSamples", "captureBoundaryDiscardedSamples",
                    "captureStopDiscardedSamples"):
            for value in (None, [], [0], [-1, 0], [False, 0], [0.5, 0],
                          [float("nan"), 0], [float("inf"), 0], "0,0"):
                frame = timing(cpi=20)
                if value is None:
                    del frame[key]
                else:
                    frame[key] = value
                with self.subTest(key=key, value=value):
                    failures, _ = self.evaluate(frame=frame, previous=previous)
                    self.assertIn("node:invalid-capture-counters", failures)

    def test_counter_regression_and_changed_channel_shape_fail(self):
        _, previous = self.evaluate(frame=timing(drops=(10, 10)))
        previous["epoch"] = time.time() - 5
        failures, _ = self.evaluate(frame=timing(cpi=20), previous=previous)
        self.assertIn("node:capture-drops-reset", failures)
        frame = timing(cpi=20, drops=(10, 10))
        for key in ("captureDroppedSamples", "captureBoundaryDiscardedSamples",
                    "captureStopDiscardedSamples"):
            frame[key].append(0)
        failures, _ = self.evaluate(frame=frame, previous=previous)
        for kind in ("drops", "boundary", "stop"):
            self.assertIn(f"node:capture-{kind}-reset", failures)

    def test_nonfinite_and_negative_timing_never_qualify(self):
        for key, failure in (("frame_age_ms", "stale-timing"),
                             ("capture_backlog_ms", "capture-backlog")):
            for value in (float("nan"), float("inf"), -1, False):
                frame = timing(); frame[key] = value
                failures, _ = self.evaluate(frame=frame)
                self.assertIn(f"node:{failure}", failures)

    def records(self, events, window=SOAK.DEFAULT_CLEAN_WINDOW_SECONDS):
        started = {"epoch": events[0][0]}
        rows = []
        for epoch, failures in events:
            row = {"epoch": epoch, "failures": list(failures), "nodes": {}}
            SOAK.advance_acceptance(started, row, window)
            rows.append(row)
        return started, rows

    def summary(self, started, rows):
        return SOAK.summarize([json.dumps(row) for row in rows], started, rows[-1])

    def test_summary_separates_startup_and_steady_state(self):
        started, rows = self.records([(0, ["hp:preexisting-capture-loss"]),
                                      (1, []), (11, [])], window=10)
        summary = self.summary(started, rows)
        self.assertFalse(summary["startupAccepted"])
        self.assertTrue(summary["steadyStateAccepted"])
        self.assertTrue(summary["diagnosticWindow"])
        self.assertEqual(summary["cleanWindow"]["policy"]["cleanWindowSeconds"], 10)

    def test_one_recovered_sample_never_accepts_steady_state(self):
        for failure in ("hp:unreachable", "hp:stale-frame", "hp:cadence",
                        "hp:invalid-cpi-counter", "hp:capture-drops"):
            started, rows = self.records([(0, []), (1, [failure]), (2, [])], window=1)
            summary = self.summary(started, rows)
            self.assertTrue(summary["latestHealthy"])
            self.assertEqual(summary["failedSamples"], 1)
            self.assertFalse(summary["steadyStateAccepted"])
            self.assertEqual(summary["cleanWindow"]["cleanSamples"], 1)

    def test_initial_unreachable_remains_startup_failure_after_clean_window(self):
        started, rows = self.records([(0, ["hp:unreachable"]), (1, []), (11, [])], window=10)
        summary = self.summary(started, rows)
        self.assertFalse(summary["startupAccepted"])
        self.assertTrue(summary["steadyStateAccepted"])

    def test_default_window_requires_ten_consecutive_minutes_of_observations(self):
        started, rows = self.records([(minute * 60, []) for minute in range(10)])
        self.assertFalse(self.summary(started, rows)["steadyStateAccepted"])
        last = {"epoch": 600, "failures": [], "nodes": {}}
        SOAK.advance_acceptance(started, last)
        rows.append(last)
        summary = self.summary(started, rows)
        self.assertTrue(summary["steadyStateAccepted"])
        self.assertFalse(summary["diagnosticWindow"])

    def test_inconclusive_exposure_never_accepts_but_can_start_a_fresh_clean_window(self):
        started, rows = self.records([(minute * 60, ["soak:insufficient-exposure"])
                                      for minute in range(12)])
        summary = self.summary(started, rows)
        self.assertTrue(summary["startupAccepted"])
        self.assertFalse(summary["steadyStateAccepted"])
        self.assertFalse(summary["releaseAccepted"])
        for minute in range(12, 23):
            row = {"epoch": minute * 60, "failures": [], "nodes": {}}
            SOAK.advance_acceptance(started, row)
            rows.append(row)
        self.assertTrue(self.summary(started, rows)["steadyStateAccepted"])

    def test_unobserved_gap_or_clock_rollback_breaks_clean_window(self):
        for last_epoch in (181, -1):
            if last_epoch < 0:
                events = [(2, []), (1, [])]
            else:
                events = [(0, []), (last_epoch, [])]
            started, rows = self.records(events, window=1)
            summary = self.summary(started, rows)
            self.assertIn("soak:observation-gap", summary["latestFailures"])
            self.assertFalse(summary["steadyStateAccepted"])

    def test_rolling_ledger_cannot_erase_startup_failures_or_clean_evidence(self):
        started, rows = self.records([(0, ["hp:capture-drops"]), (1, []), (11, [])], window=10)
        summary = self.summary(started, rows[-1:])
        self.assertFalse(summary["startupAccepted"])
        self.assertTrue(summary["steadyStateAccepted"])

    def test_changed_acceptance_policy_starts_a_fresh_window(self):
        started, rows = self.records([(0, []), (10, [])], window=10)
        self.assertTrue(self.summary(started, rows)["steadyStateAccepted"])
        row = {"epoch": 11, "failures": [], "nodes": {}}
        SOAK.advance_acceptance(started, row, clean_window_seconds=1)
        self.assertFalse(self.summary(started, [row])["steadyStateAccepted"])

    def test_invalid_window_duration_is_rejected(self):
        for value in (0, -1, float("inf"), float("nan"), False):
            with self.assertRaises(ValueError):
                SOAK.advance_acceptance({}, {"epoch": 1, "failures": []}, value)

    def binding(self):
        node = {"release": "/release", "artifactSha256": "a" * 64,
                "endpoint": "http://127.0.0.1:3000", "ssh": None,
                "disk": {"path": "/var/lib", "minimumFreeBytes": 200_000_000_000},
                "qualifications": {"radar-behavior": "6" * 64, "physical-gpu": "7" * 64},
                "configSha256": "c" * 64,
                "hostIdSha256": "e" * 64, "packageSha256": "f" * 64,
                "packageFilename": "vectorwarp_1.2.3_amd64.deb",
                "acceleration": "vulkan", "clutterAcceleration": "vulkan", "maximumCadenceMs": 600,
                "runtimeModules": {unit: {} for unit in
                                   ("vectorwarp-api.service", "vectorwarp-processor.service")},
                "runtimeExecutables": {
                    unit: {"path": "/release/bin/program", "sha256": "d" * 64}
                    for unit in ("vectorwarp-api.service", "vectorwarp-processor.service")}}
        return {"schema": 3, "sourceCommit": "b" * 40, "regressionPolicySha256": "1" * 64,
                "monitorMinimumFreeBytes": 200_000_000_000,
                "nodes": {"homeportal": node, "hp": {**node, "hostIdSha256": "2" * 64}}}

    def test_release_binding_requires_reviewed_nodes_and_complete_digests(self):
        with tempfile.TemporaryDirectory() as temporary:
            filename = Path(temporary) / "binding.json"
            value = self.binding()
            filename.write_text(json.dumps(value))
            self.assertEqual(SOAK.load_binding(str(filename)), value)
            for key in ("artifactSha256", "configSha256", "runtimeExecutables",
                        "acceleration", "clutterAcceleration", "hostIdSha256", "packageSha256",
                        "packageFilename", "runtimeModules", "maximumCadenceMs", "endpoint",
                        "ssh", "disk", "qualifications"):
                value = self.binding()
                del value["nodes"]["hp"][key]
                filename.write_text(json.dumps(value))
                with self.assertRaises(ValueError):
                    SOAK.load_binding(str(filename))
            value = self.binding()
            value["nodes"] = {}
            filename.write_text(json.dumps(value))
            with self.assertRaises(ValueError):
                SOAK.load_binding(str(filename))

    def test_source_config_artifact_and_executables_must_match(self):
        binding = self.binding()
        stat = status()
        stat.update(build={"commit": "b" * 40}, loadedRevision="c" * 64,
                    configRevision="c" * 64)
        process = dict(binding["nodes"]["hp"])
        process["disk"] = {"path": "/var/lib", "freeBytes": 300_000_000_000}
        self.assertEqual(SOAK.verify_binding("hp", stat, process, binding), [])
        for key in ("release", "artifactSha256", "configSha256", "runtimeExecutables",
                    "runtimeModules", "hostIdSha256"):
            bad = dict(process)
            del bad[key]
            self.assertIn(f"hp:artifact-binding:{key}",
                          SOAK.verify_binding("hp", stat, bad, binding))
        stat["loadedRevision"] = "old-config"
        self.assertIn("hp:config-binding", SOAK.verify_binding("hp", stat, process, binding))
        stat["build"] = {}
        self.assertIn("hp:source-binding", SOAK.verify_binding("hp", stat, process, binding))

    def test_unbound_or_short_window_never_accepts_a_release(self):
        started, rows = self.records([(minute * 60, []) for minute in range(1441)])
        self.assertTrue(self.summary(started, rows)["steadyStateAccepted"])
        self.assertFalse(self.summary(started, rows)["releaseAccepted"])
        started["releaseBinding"] = {"expected": self.binding()}
        self.assertTrue(self.summary(started, rows)["releaseAccepted"])
        started, rows = self.records([(0, []), (10, [])], window=10)
        started["releaseBinding"] = {"expected": self.binding()}
        self.assertFalse(self.summary(started, rows)["releaseAccepted"])

    def test_late_binding_failure_is_sticky_after_rolling_window(self):
        started, rows = self.records([(0, []), (1, ["hp:artifact-binding:artifactSha256"]),
                                      (2, []), (12, [])], window=10)
        self.assertFalse(self.summary(started, rows[-1:])["startupAccepted"])

    def test_regression_receipt_requires_exact_binding_policy_freshness_and_both_nodes(self):
        binding = {"expected": self.binding(), "sha256": "3" * 64}
        receipt = {"schema": 3, "bindingSha256": "3" * 64, "policySha256": "1" * 64,
                   "nodes": {name: {"retiredCpis": 0, "retiredSamples": 0,
                             "checks": {key: {"accepted": True, "status": "accepted", "evidenceSha256": "4" * 64}
                                        for key in SOAK.REGRESSION_CHECKS},
                             "qualifications": node["qualifications"]}
                             for name, node in binding["expected"]["nodes"].items()},
                   "evidenceSha256": "4" * 64,
                   "evaluationStartEpoch": 1, "evaluatedThroughEpoch": 100,
                   "regressionDetected": False}
        self.assertEqual(SOAK.verify_regression(receipt, binding, 100, 2), [])
        for malformed in (None, [], "receipt", {**receipt, "schema": 1},
                          {**receipt, "evidenceSha256": None}):
            self.assertEqual(SOAK.verify_regression(malformed, binding, 100, 2),
                             ["soak:regression-evidence-invalid"])
        for key in SOAK.REGRESSION_CHECKS:
            missing = copy.deepcopy(receipt)
            del missing["nodes"]["hp"]["checks"][key]
            self.assertEqual(SOAK.verify_regression(missing, binding, 100, 2),
                             ["soak:regression-evidence-invalid"])
            failed = copy.deepcopy(receipt)
            failed["nodes"]["hp"]["checks"][key]["accepted"] = False
            failed["nodes"]["hp"]["checks"][key]["status"] = "rejected"
            self.assertEqual(SOAK.verify_regression(failed, binding, 100, 2),
                             ["soak:regression-detected"])
            failed["nodes"]["hp"]["checks"][key]["status"] = "inconclusive"
            self.assertEqual(SOAK.verify_regression(failed, binding, 100, 2),
                             ["soak:insufficient-exposure"])
            failed["nodes"]["hp"]["checks"][key]["accepted"] = True
            self.assertEqual(SOAK.verify_regression(failed, binding, 100, 2),
                             ["soak:regression-evidence-invalid"])
        for key, value in (("bindingSha256", "5" * 64), ("policySha256", "5" * 64),
                           ("nodes", ["hp"]), ("evaluatedThroughEpoch", 101),
                           ("evaluationStartEpoch", 3), ("regressionDetected", None)):
            with self.subTest(key=key):
                self.assertTrue(SOAK.verify_regression({**receipt, key: value}, binding, 100, 2))
        self.assertTrue(SOAK.verify_regression(receipt, binding, 281, 2))
        self.assertEqual(SOAK.verify_regression({**receipt, "regressionDetected": True}, binding, 100, 2),
                         ["soak:regression-detected"])
        lost = copy.deepcopy(receipt)
        lost["nodes"]["hp"].update(retiredCpis=1, retiredSamples=512)
        self.assertEqual(SOAK.verify_regression(lost, binding, 100, 2), ["soak:regression-retired-work"])
        started, rows = self.records([(0, []), (1, ["soak:regression-detected"]),
                                      (2, []), (12, [])], window=10)
        self.assertFalse(self.summary(started, rows[-1:])["startupAccepted"])

    def test_changed_binding_and_invalid_state_preserve_old_evidence(self):
        for prior in ('not-json', json.dumps({"schema": 3, "epoch": 1, "previous": {},
                                              "releaseBinding": {"sha256": "old"}})):
            with self.subTest(prior=prior), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                binding_file = root / "binding.json"
                binding_file.write_text(json.dumps(self.binding()))
                state_file = root / "started.json"
                state_file.write_text(prior)
                environment = {"VECTORWARP_SOAK_BINDING": str(binding_file),
                               "VECTORWARP_SOAK_STATE": str(root),
                               "VECTORWARP_SOAK_HP_HOST": "node-b.example.test",
                               "VECTORWARP_SOAK_HP_SSH": "operator@hp"}
                with patch.dict(SOAK.os.environ, environment, clear=True), \
                        patch.object(SOAK, "measured_process") as probe:
                    with self.assertRaisesRegex(ValueError, "fresh state directory"):
                        SOAK.main()
                    probe.assert_not_called()
                self.assertEqual(state_file.read_text(), prior)

    def test_overlapping_sample_never_writes_the_ledger(self):
        with tempfile.TemporaryDirectory() as temporary, \
                patch.dict(SOAK.os.environ, {"VECTORWARP_SOAK_STATE": temporary}, clear=True), \
                patch.object(SOAK, "run_sample") as sample:
            with (Path(temporary) / ".lock").open("a") as guard:
                SOAK.fcntl.flock(guard, SOAK.fcntl.LOCK_EX | SOAK.fcntl.LOCK_NB)
                with self.assertRaises(BlockingIOError):
                    SOAK.main()
            sample.assert_not_called()

    def test_reviewed_topology_supports_one_two_and_many_mixed_nodes(self):
        for count in (1, 2, 3, 7):
            binding = self.binding()
            template = binding["nodes"]["homeportal"]
            binding["nodes"] = {}
            for index in range(count):
                node = copy.deepcopy(template)
                node.update(hostIdSha256=f"{index + 1:064x}", endpoint=f"http://node-{index}:3000",
                            ssh=f"operator@node-{index}")
                if index % 2:
                    node.update(acceleration="cpu", clutterAcceleration="cpu")
                    del node["qualifications"]["physical-gpu"]
                binding["nodes"][f"site-{index}"] = node
            with tempfile.TemporaryDirectory() as temporary:
                filename = Path(temporary) / "binding.json"
                filename.write_text(json.dumps(binding))
                self.assertEqual(SOAK.load_binding(filename), binding)
                with patch.dict(SOAK.os.environ, {"VECTORWARP_SOAK_BINDING": str(filename)}, clear=True), \
                        patch.object(SOAK, "measured_process", side_effect=RuntimeError("offline")) as probe:
                    SOAK.run_sample(Path(temporary))
                self.assertEqual(probe.call_count, count)
                self.assertEqual(set(json.loads((Path(temporary) / "latest.json").read_text())["performance"]),
                                 set(binding["nodes"]))

    def test_topology_hostile_inputs_fail_before_measurement(self):
        mutations = [lambda b: b.update(schema=2),
                     lambda b: b["nodes"]["hp"].update(hostIdSha256="e" * 64),
                     lambda b: b["nodes"]["hp"].update(ssh="-oProxyCommand=bad"),
                     lambda b: b["nodes"]["hp"].update(endpoint="http://user:secret@host"),
                     lambda b: b["nodes"]["hp"].update(endpoint="file:///etc/passwd"),
                     lambda b: b["nodes"]["hp"]["disk"].update(minimumFreeBytes=False),
                     lambda b: b["nodes"]["hp"]["disk"].update(path="relative"),
                     lambda b: b["nodes"]["hp"]["qualifications"].pop("physical-gpu")]
        for mutate in mutations:
            with tempfile.TemporaryDirectory() as temporary:
                binding = copy.deepcopy(self.binding()); mutate(binding)
                filename = Path(temporary) / "binding.json"
                filename.write_text(json.dumps(binding))
                with self.assertRaises(ValueError):
                    SOAK.load_binding(filename)
        with self.assertRaisesRegex(ValueError, "duplicate"):
            json.loads('{"nodes":{},"nodes":{}}', object_pairs_hook=SOAK.unique_object)

    def test_disk_reserve_is_measured_on_each_bound_host(self):
        binding = self.binding()
        stat = {**status(), "build": {"commit": "b" * 40},
                "loadedRevision": "c" * 64, "configRevision": "c" * 64}
        process = dict(binding["nodes"]["hp"])
        for measured in ({}, {"path": "/var/lib", "freeBytes": 199_999_999_999},
                         {"path": "/wrong", "freeBytes": 300_000_000_000},
                         {"path": "/var/lib", "freeBytes": float("inf")}):
            process["disk"] = measured
            self.assertIn("hp:disk-reserve", SOAK.verify_binding("hp", stat, process, binding))


if __name__ == "__main__":
    unittest.main()
