import importlib.util
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
            "captureTerminal": False}


class SoakEvaluation(unittest.TestCase):
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

    def test_restart_is_one_observed_boundary(self):
        _, first = self.evaluate()
        first["epoch"] = time.time() - 5
        failures, observation = self.evaluate(stat=status("two"), previous=first)
        self.assertIn("node:process-restarted", failures)
        self.assertTrue(observation["restarted"])

    def test_remote_release_is_not_fabricated(self):
        self.assertIsNone(SOAK.remote_process()["release"])

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

    def test_default_window_really_requires_24_hours_of_observations(self):
        started, rows = self.records([(minute * 60, []) for minute in range(1440)])
        self.assertFalse(self.summary(started, rows)["steadyStateAccepted"])
        last = {"epoch": 86400, "failures": [], "nodes": {}}
        SOAK.advance_acceptance(started, last)
        rows.append(last)
        summary = self.summary(started, rows)
        self.assertTrue(summary["steadyStateAccepted"])
        self.assertFalse(summary["diagnosticWindow"])

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
                "configSha256": "c" * 64,
                "runtimeExecutables": {
                    unit: {"path": "/release/bin/program", "sha256": "d" * 64}
                    for unit in ("vectorwarp-api.service", "vectorwarp-processor.service")}}
        return {"schema": 1, "sourceCommit": "b" * 40,
                "nodes": {"homeportal": node, "hp": node}}

    def test_release_binding_requires_both_nodes_and_complete_digests(self):
        with tempfile.TemporaryDirectory() as temporary:
            filename = Path(temporary) / "binding.json"
            value = self.binding()
            filename.write_text(json.dumps(value))
            self.assertEqual(SOAK.load_binding(str(filename)), value)
            for key in ("artifactSha256", "configSha256", "runtimeExecutables"):
                value = self.binding()
                del value["nodes"]["hp"][key]
                filename.write_text(json.dumps(value))
                with self.assertRaises(ValueError):
                    SOAK.load_binding(str(filename))
            value = self.binding()
            del value["nodes"]["hp"]
            filename.write_text(json.dumps(value))
            with self.assertRaises(ValueError):
                SOAK.load_binding(str(filename))

    def test_source_config_artifact_and_executables_must_match(self):
        binding = self.binding()
        stat = status()
        stat.update(build={"commit": "b" * 40}, loadedRevision="c" * 64,
                    configRevision="c" * 64)
        process = dict(binding["nodes"]["hp"])
        self.assertEqual(SOAK.verify_binding("hp", stat, process, binding), [])
        for key in ("release", "artifactSha256", "configSha256", "runtimeExecutables"):
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


if __name__ == "__main__":
    unittest.main()
