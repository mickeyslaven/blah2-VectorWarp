"""Isolated replay safety/acceptance regressions; fake children and sinks only."""
import copy
import errno
import importlib.util
import io
import json
import os
from pathlib import Path
import re
import signal
import socket
import subprocess
import tempfile
import threading
import unittest
from unittest.mock import MagicMock, Mock, call, patch

SPEC = importlib.util.spec_from_file_location("isolated_iq_replay",
    Path(__file__).resolve().parents[2] / "script/run-isolated-iq-replay.py")
RUN = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(RUN)


def artifact_events(binding, parent=987654, worker=987655, role="portable"):
    common = {"schema": 1, "nonce": binding["nonce"], "pid": worker, "parent": parent}
    factory = next(iter(RUN.ARTIFACTS.FACTORIES[role]))
    abi, mode = RUN.ARTIFACTS.FACTORIES[role][factory]
    return [dict(common, event="worker-spawn", pid=parent, parent=1, workerPid=worker,
                 executablePath=binding["artifacts"]["worker"]["path"]),
            dict(common, event="worker-start", worker=binding["artifacts"]["worker"]),
            dict(common, event="module-attempt", role=role),
            dict(common, event="module-loaded", role=role, module=binding["artifacts"][role],
                 abi=abi, factory=factory, workerMode=mode, deviceRequest="test-device",
                 driverStatus={"available": True, "devices": [{"id": "test-device",
                     "vendorId": 123, "deviceId": 456, "driverVersionRaw": 789}]}),
            dict(common, event="backend-start", role=role, factory=factory, workerMode=mode)]


def artifact_log(events):
    return "".join(RUN.ARTIFACTS.PREFIX + json.dumps(item) + "\n" for item in events)


class IsolatedReplay(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.binary = self.root / "fake-binary"
        self.binary.write_bytes(b"not executable: Popen is always mocked")
        (self.root / "blah2-gpu-worker").write_bytes(b"mock worker artifact")
        for name, _ in RUN.ARTIFACTS.MODULES.values():
            (self.root / name).write_bytes(b"mock module artifact " + name.encode())
        self.iq = self.root / "fake-iq"
        self.iq.write_bytes(b"unit-test IQ pin, never replayed")
        self.profile = {"configuration_complete": True,
            "capture": {"source_receiver": "arbitrary-adapter", "channel_count": 2,
                        "sample_rate_hz": 100, "frequency_hz": 1000,
                        "reference_channel": 1, "surveillance_channels": [0], "array_geometry": None},
            "process": {"performance": {"acceleration": "cpu", "fft_threads": 3},
                "data": {"cpi": .5, "buffer": 8}, "multi_illuminator": {
                "enabled": True, "illuminators": [{"id": name, "enabled": True,
                    "output_sample_rate_hz": 20} for name in ("one", "two", "three")]},
                "detection": {"live_temporal_shadow": {"enabled": True}},
                "evidence": {"causal_funnel": {"enabled": True}}}}
        self.profile_path = self.root / "profile.json"
        RUN.write_json(self.profile_path, self.profile)
        self.metadata = self.root / "metadata.json"
        digest = RUN.PROFILE.file_sha256(self.iq)
        RUN.write_json(self.metadata, {"dataSha256": digest})
        self.flags = ["--binary", str(self.binary), "--binary-sha256",
            RUN.PROFILE.file_sha256(self.binary), "--recording", str(self.iq), "--sha256", digest,
            "--metadata", str(self.metadata), "--profile", str(self.profile_path),
            "--output", str(self.root / "run"), "--expected-cpis", "8",
            "--expected-lane", "one", "--expected-lane", "two", "--expected-lane", "three",
            "--expected-input-path", "contiguous_double",
            "--expected-samples", "403", "--expected-trailing-samples", "3", "--timeout", "0.1"]
        self.args = RUN.parse_args(self.flags)
        self.statuses = [{"input": "replay", "state": "complete", "error": "", "loops": 0,
                         "positionSamples": 403, "totalSamples": 403, "trailingSamples": 3,
                         "captureStopped": True, "captureDroppedSamples": [0, 0],
                         "captureBacklogSamples": [0, 0]}]
        self.products = {"timing": [], "illuminator": []}
        for index in range(8):
            self.products["timing"].append({"nCpi": index + 1, "captureDroppedSamples": [0, 0],
                "captureSamplePositionAvailable": True, "captureEpoch": 0,
                "captureFirstEpochSample": index * 50, "captureEndEpochSample": (index + 1) * 50})
            for name in self.args.expected_lane:
                self.products["illuminator"].append({"id": name,
                    "capture_sample_position_available": True, "capture_epoch": 0,
                    "capture_first_input_sample": index * 50, "capture_end_input_sample": (index + 1) * 50,
                    "input_processing": {"path": "contiguous_double", "staging_copied_samples": 0,
                        "staging_widened_samples": 20, "deque_materializations": 0,
                        "deque_materialized_bytes": 0, "deque_materialization_ms": 0,
                        "contiguous_fp64_samples": 20, "contiguous_fp64_bytes_written": 320,
                        "contiguous_allocations": int(index == 0)}})
        terminal = copy.deepcopy(self.products["timing"][-1])
        terminal.update(captureTerminal=True, captureBoundaryDiscardedSamples=[0, 0],
                        captureStopDiscardedSamples=[0, 0])
        self.products["timing"].append(terminal)
        self.status = Mock(port=34567, overflow=False)
        self.status.snapshot.side_effect = lambda: copy.deepcopy(self.statuses)
        self.sinks = Mock()
        self.sinks.ports.return_value = {name: 35000 + index for index, name in enumerate(RUN.SESSION.PORT_NAMES)}
        self.sinks.truncated = set()
        self.sinks.drained = {name: threading.Event() for name in ("illuminator", "timing")}
        for event in self.sinks.drained.values():
            event.set()
        self.child = Mock(pid=987654, stdout=io.BytesIO(b"fake child log\n"), returncode=0)
        self.child.poll.return_value = None

    def execute(self):
        self.sinks.payloads = {name: bytearray("\n".join(json.dumps(row) for row in values).encode())
                               for name, values in self.products.items()}
        self.sinks.bytes = {name: len(value) for name, value in self.sinks.payloads.items()}
        def launch(*args, **kwargs):
            if self.args.acceleration != "cpu":
                binding = RUN.ARTIFACTS.prepare(self.binary, True)
                binding["nonce"] = kwargs["env"][RUN.ARTIFACTS.ENV + "NONCE"]
                events = artifact_events(binding)
                if hasattr(self, "artifact_transform"):
                    events = self.artifact_transform(events)
                with socket.socket(fileno=os.dup(kwargs["pass_fds"][0])) as channel:
                    for event in events:
                        channel.send(artifact_log([event]).encode(), socket.MSG_DONTWAIT)
            return self.child
        with patch.object(RUN.SESSION, "StatusServer", return_value=self.status), \
             patch.object(RUN.SESSION, "SinkGroup", return_value=self.sinks), \
             patch.object(RUN.SESSION.subprocess, "Popen", side_effect=launch) as start, \
             patch.object(RUN.SESSION, "group_alive", return_value=False), \
             patch.object(RUN.SESSION, "stop_group", return_value=False) as stop:
            result = RUN.run(self.args)
        return result, start, stop

    def test_binary_replacement_after_spawn_cannot_pass_replay(self):
        original = RUN.ARTIFACTS.validate
        def replaced(*args, **kwargs):
            self.binary.write_bytes(b"replacement executable")
            return original(*args, **kwargs)
        with patch.object(RUN.ARTIFACTS, "validate", side_effect=replaced):
            result, _, _ = self.execute()
        self.assertFalse(result["passed"])
        self.assertIn("pinned artifact changed", result["error"])

    def test_gpu_replay_rejects_missing_early_artifact_receipts(self):
        self.args.acceleration = "gpu"
        self.artifact_transform = lambda events: []
        result, _, _ = self.execute()
        self.assertFalse(result["passed"])
        self.assertIn("missing GPU worker artifact receipt", result["error"])

    def test_artifact_looking_stderr_cannot_replace_private_receipts(self):
        self.args.acceleration = "gpu"
        self.artifact_transform = lambda events: []
        self.child.stdout = io.BytesIO(artifact_log(artifact_events(
            RUN.ARTIFACTS.prepare(self.binary, True))).encode())
        result, _, _ = self.execute()
        self.assertFalse(result["passed"])
        self.assertIn("missing GPU worker artifact receipt", result["error"])

    def test_gpu_replay_rejects_module_receipt_after_backend_started(self):
        self.args.acceleration = "gpu"
        self.artifact_transform = lambda events: events[:3] + [events[4], events[3]]
        result, _, _ = self.execute()
        self.assertFalse(result["passed"])
        self.assertIn("backend construction preceded module receipt", result["error"])

    def test_complete_eight_cpi_three_lane_receipt_and_isolation(self):
        # Exercise a directory alias on Linux too: Darwin's /var temporary
        # directory is an alias of /private/var, and launch pins are canonical.
        alias = self.root / "directory-alias"
        alias.symlink_to(self.root, target_is_directory=True)
        self.binary = alias / self.binary.name
        self.args.binary = self.binary
        result, start, stop = self.execute()
        self.assertTrue(result["passed"], result)
        self.assertEqual(result["acceptance"]["capture_cpis"], 8)
        self.assertEqual(result["acceptance"]["terminal_records"], 1)
        self.assertEqual(set(result["acceptance"]["lanes"]), {"one", "two", "three"})
        command = start.call_args.args[0]
        self.assertEqual(command, [str(self.binary.resolve(strict=True)), "-c", str(self.args.output / "config.json")])
        self.assertTrue(start.call_args.kwargs["start_new_session"])
        self.assertEqual(start.call_args.kwargs["cwd"], self.args.output)
        self.assertEqual(start.call_args.kwargs["env"]["OMP_NUM_THREADS"], "1")
        stop.assert_not_called()
        self.child.send_signal.assert_called_once_with(signal.SIGTERM)
        self.assertEqual(result["shutdown"], "graceful-eof-stop")
        config = json.loads((self.args.output / "config.json").read_text())
        self.assertEqual(config["network"]["ip"], "127.0.0.1")
        self.assertEqual(config["network"]["ports"]["api"], self.status.port)
        self.assertEqual(config["capture"]["device"]["type"], "arbitrary-adapter")
        self.assertFalse(config["capture"]["replay"]["loop"])
        self.assertFalse(config["save"]["iq"])
        self.assertEqual(config["process"], self.profile["process"])
        self.assertEqual(result["process_config_sha256"], result["profile_process_sha256"])
        for name in ("receipt.json", "status.json", "processor.log", "illuminator.raw",
                     "illuminator.json", "timing.raw", "timing.json"):
            self.assertTrue((self.args.output / name).is_file(), name)

    def test_exact_nonzero_replay_start_is_configured_and_receipted(self):
        self.args.start_sample = 7
        self.args.recording_samples = 410
        self.statuses[0]["positionSamples"] = 410
        self.statuses[0]["totalSamples"] = 410
        result, _, _ = self.execute()
        self.assertTrue(result["passed"], result)
        config = json.loads((self.args.output / "config.json").read_text())
        self.assertEqual(config["capture"]["replay"]["start_sample"], 7)
        self.assertEqual(result["start_sample"], 7)
        self.assertEqual(result["recording_samples"], 410)

    def test_replay_start_sample_arithmetic_fails_before_launch(self):
        self.args.start_sample = 7
        self.args.recording_samples = 409
        with self.assertRaisesRegex(ValueError, "start/segment/recording"):
            self.execute()

    def test_hash_mismatch_never_starts_child(self):
        for key in ("sha256", "binary_sha256"):
            with self.subTest(pin=key):
                self.args.output = self.root / key
                old = getattr(self.args, key)
                setattr(self.args, key, "0" * 64)
                result, start, _ = self.execute()
                self.assertFalse(result["passed"])
                self.assertIn("SHA-256 mismatch", result["error"])
                start.assert_not_called()
                setattr(self.args, key, old)

    def test_existing_output_refused_without_overwrite(self):
        self.args.output.mkdir()
        sentinel = self.args.output / "precious"
        sentinel.write_text("preserve")
        with self.assertRaises(FileExistsError):
            self.execute()
        self.assertEqual(sentinel.read_text(), "preserve")

    def test_timeout_and_early_exit_preserve_evidence_and_stop_own_child(self):
        self.statuses[0]["state"] = "playing"
        for early in (False, True):
            with self.subTest(early_exit=early):
                self.args.output = self.root / str(early)
                self.child.stdout = io.BytesIO(b"bounded failure log")
                self.child.poll.return_value = 7 if early else None
                result, _, stop = self.execute()
                self.assertFalse(result["passed"])
                self.assertIn("exited" if early else "deadline", result["error"])
                stop.assert_called_with(self.child)
                self.assertTrue((self.args.output / "status.json").is_file())

    def test_truncated_sink_cannot_pass(self):
        self.sinks.truncated.add("illuminator")
        result, _, _ = self.execute()
        self.assertFalse(result["passed"])
        self.assertEqual(result["truncated_sinks"], ["illuminator"])

    def test_bad_expectations_fail_before_child(self):
        self.args.expected_samples += 1
        result, start, _ = self.execute()
        self.assertFalse(result["passed"])
        self.assertIn("sample arithmetic", result["error"])
        start.assert_not_called()

    def test_missing_lane_or_cpi_fails(self):
        self.products["illuminator"].pop()
        result, _, _ = self.execute()
        self.assertFalse(result["passed"])
        self.assertIn("three: 7 frames != 8", result["acceptance"]["errors"])

    def test_terminal_loss_is_not_hidden_by_eight_good_cpis(self):
        self.products["timing"][-1]["captureDroppedSamples"] = [4, 4]
        result, _, _ = self.execute()
        self.assertFalse(result["passed"])
        self.assertIn("terminal capture accounting contains unexplained loss", result["acceptance"]["errors"])

    def test_counter_and_eof_regressions_fail_closed(self):
        counters = self.products["illuminator"][3]["input_processing"]
        counters["staging_copied_samples"] = 20
        counters["deque_materializations"] = 1
        counters["contiguous_allocations"] = 1
        del counters["contiguous_fp64_samples"]
        final = dict(self.statuses[0], state="stopped", trailingSamples=0,
                     captureStopped=True, captureBacklogSamples=[0, 0], captureDroppedSamples=[0, 0])
        self.statuses.append(final)
        self.products["timing"][1]["captureDroppedSamples"] = [0, 4]
        self.products["timing"][2]["captureFirstEpochSample"] += 1
        result, _, _ = self.execute()
        errors = "\n".join(result["acceptance"]["errors"])
        self.assertFalse(result["passed"])
        for name in ("staging_copied_samples", "deque_materializations", "contiguous_allocations",
                     "contiguous_fp64_samples", "trailingSamples", "capture drops", "sample identity"):
            self.assertIn(name, errors)

    def borrowed_fixture(self):
        """Reproduce the retained 8-CPI, 3-lane CPU replay contract."""
        self.args.expected_input_path = ["native_float_borrowed"]
        self.products["timing"].pop()  # native replay deliberately emits no terminal timing
        for frame in self.products["illuminator"]:
            frame["input_processing"] = dict.fromkeys(frame["input_processing"], 0)
            frame["input_processing"]["path"] = "native_float_borrowed"

        def terminal_post(timeout):
            self.assertGreater(timeout, 0)
            self.assertEqual(self.statuses[-1]["state"], "complete")
            self.statuses.append(dict(self.statuses[-1], state="stopped", captureStopped=True,
                                      captureBacklogSamples=[0, 0], captureDroppedSamples=[0, 0]))
            return 0

        self.child.wait.side_effect = terminal_post

    def test_borrowed_cpu_complete_then_stopped_without_terminal_timing(self):
        self.borrowed_fixture()
        result, _, stop = self.execute()
        self.assertTrue(result["passed"], result)
        self.assertEqual(result["eof_complete_status"]["state"], "complete")
        self.assertEqual(result["acceptance"]["terminal_records"], 0)
        self.assertTrue(result["acceptance"]["terminal_capture_status"])
        self.assertEqual(self.statuses[-1]["state"], "stopped")
        self.child.send_signal.assert_called_once_with(signal.SIGTERM)
        stop.assert_not_called()

    def test_borrowed_path_does_not_permit_widening_deques_or_missing_counters(self):
        self.borrowed_fixture()
        counters = self.products["illuminator"][0]["input_processing"]
        for name in ("staging_widened_samples", "contiguous_fp64_samples", "contiguous_fp64_bytes_written",
                     "deque_materializations", "contiguous_allocations"):
            counters[name] = 1
        del counters["staging_copied_samples"]
        result, _, _ = self.execute()
        self.assertFalse(result["passed"])
        errors = "\n".join(result["acceptance"]["errors"])
        for name in ("staging_widened_samples", "contiguous_fp64_samples", "contiguous_fp64_bytes_written",
                     "deque_materializations", "contiguous_allocations", "staging_copied_samples"):
            self.assertIn(name, errors)

    def test_borrowed_path_requires_explicit_admission(self):
        self.borrowed_fixture()
        self.args.expected_input_path = ["contiguous_double", "partial_contiguous_double"]
        result, _, _ = self.execute()
        self.assertFalse(result["passed"])
        self.assertIn("unexpected input path", "\n".join(result["acceptance"]["errors"]))

    def test_unknown_path_expectation_fails_before_start(self):
        self.args.expected_input_path = ["guessed-path"]
        with self.assertRaisesRegex(ValueError, "supported path"):
            self.execute()

    def test_missing_terminal_stopped_status_does_not_pass(self):
        self.borrowed_fixture()
        self.child.wait.side_effect = None  # EOF complete alone is not a terminal capture receipt
        self.statuses[0].pop("captureStopped")
        result, _, _ = self.execute()
        self.assertFalse(result["passed"])
        self.assertIn("missing/lossy terminal stopped-capture status", result["error"])

    def test_stopped_without_preceding_complete_does_not_pass(self):
        self.borrowed_fixture()
        self.statuses[0]["state"] = "stopped"
        result, _, stop = self.execute()
        self.assertFalse(result["passed"])
        self.assertIn("deadline", result["error"])
        self.child.send_signal.assert_not_called()
        stop.assert_called_with(self.child)

    def test_shutdown_deadline_keeps_failure_evidence_and_cleans_private_group(self):
        self.child.wait.side_effect = subprocess.TimeoutExpired("fake", .1)
        result, _, stop = self.execute()
        self.assertFalse(result["passed"])
        self.assertIn("timed out", result["error"])
        self.child.send_signal.assert_called_once_with(signal.SIGTERM)
        stop.assert_called_with(self.child)
        self.assertTrue((self.args.output / "status.json").is_file())

    def test_natural_exit_after_complete_needs_no_stop_signal(self):
        self.child.poll.return_value = 0
        result, _, stop = self.execute()
        self.assertTrue(result["passed"], result)
        self.assertEqual(result["shutdown"], "natural-exit")
        self.child.send_signal.assert_not_called()
        stop.assert_not_called()

    def test_auto_cli_and_config_preserve_clean_recovered_error(self):
        self.args = RUN.parse_args(self.flags + ["--acceleration", "auto", "--timeout", "10",
                                                "--error-stall-seconds", "0.01"])
        complete = copy.deepcopy(self.statuses[0])
        self.statuses = [dict(complete, state="error", positionSamples=0, error="GPU stage rejected")]
        snapshots = 0

        def advance():
            nonlocal snapshots
            snapshots += 1
            if snapshots == 2:
                self.statuses.append(dict(complete, state="playing", positionSamples=100))
            elif snapshots == 3:
                self.statuses.append(complete)
            return copy.deepcopy(self.statuses)

        self.status.snapshot.side_effect = advance
        result, _, stop = self.execute()
        self.assertTrue(result["passed"], result)
        self.assertEqual(result["acceptance"]["recovered_error_statuses"], 1)
        self.assertEqual(result["acceleration"], "auto")
        config = json.loads((self.args.output / "config.json").read_text())
        self.assertEqual(config["process"]["performance"]["acceleration"], "auto")
        stop.assert_not_called()

    def test_persistent_error_fails_early_and_retains_all_failure_evidence(self):
        self.args.acceleration = "auto"
        self.args.timeout = 30
        self.args.error_stall_seconds = .01
        self.statuses[0].update(state="error", positionSamples=100, error="GPU unavailable")
        result, _, stop = self.execute()
        self.assertFalse(result["passed"])
        self.assertIn("persistent capture error", result["error"])
        self.assertLess(result["elapsed_seconds"], self.args.timeout)
        self.assertEqual(result["capture_error_stall"]["position_samples"], 100)
        self.assertGreaterEqual(result["capture_error_stall"]["no_progress_seconds"], .01)
        stop.assert_called_with(self.child)
        self.child.send_signal.assert_not_called()  # failure cleanup, not successful EOF shutdown
        for name in ("receipt.json", "status.json", "processor.log", "illuminator.raw", "timing.raw"):
            self.assertTrue((self.args.output / name).is_file(), name)

    def test_error_after_historical_complete_is_not_accepted_as_current_eof(self):
        self.args.acceleration = "auto"
        self.args.timeout = 30
        self.args.error_stall_seconds = .01
        self.statuses.append(dict(self.statuses[0], state="error", error="late capture failure"))
        result, _, _ = self.execute()
        self.assertFalse(result["passed"])
        self.assertIn("persistent capture error", result["error"])
        self.assertNotIn("eof_complete_status", result)

    def test_explicit_gpu_does_not_silently_accept_recovered_error(self):
        self.args.acceleration = "gpu"
        self.statuses.insert(0, dict(self.statuses[0], state="error", positionSamples=0,
                                    error="requested GPU failed"))
        result, _, _ = self.execute()
        self.assertFalse(result["passed"])
        self.assertIn("explicit-mode error", "\n".join(result["acceptance"]["errors"]))

    def test_invalid_error_stall_bounds_fail_before_child(self):
        for value in (0, -1, 61, float("nan"), float("inf")):
            with self.subTest(value=value):
                self.args.error_stall_seconds = value
                with self.assertRaisesRegex(ValueError, "error stall limit"):
                    self.execute()

    def test_bounded_log_drains_but_never_grows_past_limit(self):
        logger = RUN.SESSION.BoundedLog(io.BytesIO(b"x" * 1000), self.root / "log", 17)
        logger.copy()
        self.assertTrue(logger.overflow.is_set())
        self.assertEqual((self.root / "log").stat().st_size, 17)

    def test_stop_signals_only_private_child_group(self):
        child = Mock(pid=987654)
        child.wait.side_effect = [subprocess.TimeoutExpired("fake", 1), 0]
        with patch.object(RUN.SESSION.os, "killpg") as kill:
            self.assertTrue(RUN.SESSION.stop_group(child, grace=.01))
        kill.assert_has_calls([call(987654, signal.SIGTERM), call(987654, 0),
                               call(987654, signal.SIGKILL)])

    def test_exited_group_leader_does_not_hide_worker(self):
        child = Mock(pid=987654)
        with patch.object(RUN.SESSION.os, "killpg") as kill:
            self.assertTrue(RUN.SESSION.stop_group(child, grace=0))
        kill.assert_any_call(987654, signal.SIGKILL)

    def test_existing_sink_primitive_drains_and_reports_truncation(self):
        sink = object.__new__(RUN.SESSION.SinkGroup)
        sink.stop = threading.Event()
        sink.drained = {"timing": threading.Event()}
        sink.bytes = {"timing": 0}
        sink.payloads = {"timing": bytearray()}
        sink.payload_limits = {"timing": 3}
        sink.truncated = set()
        connection = MagicMock()
        connection.recv.side_effect = [b"12345", b""]
        connection.__enter__.return_value = connection
        listener = Mock()
        listener.accept.side_effect = [(connection, None), OSError()]
        sink._serve("timing", listener)
        self.assertEqual(sink.payloads["timing"], b"123")
        self.assertEqual(sink.bytes["timing"], 5)
        self.assertEqual(sink.truncated, {"timing"})
        self.assertTrue(sink.drained["timing"].is_set())


class CaptureErrorPersistence(unittest.TestCase):
    def test_forward_progress_restarts_stall_clock_even_while_error_is_reported(self):
        watch = RUN.SESSION.CaptureErrorWatch(5)
        status = {"state": "error", "positionSamples": 100, "error": "fallback pending"}
        self.assertIsNone(watch.observe(status, 0))
        self.assertIsNone(watch.observe(dict(status, positionSamples=101), 4))
        self.assertIsNone(watch.observe(dict(status, positionSamples=101), 8))
        failure = watch.observe(dict(status, positionSamples=101), 9)
        self.assertEqual(failure["no_progress_seconds"], 5)

    def test_backward_or_missing_positions_and_changed_messages_do_not_fake_progress(self):
        watch = RUN.SESSION.CaptureErrorWatch(5)
        self.assertIsNone(watch.observe({"state": "error", "positionSamples": 100}, 0))
        self.assertIsNone(watch.observe({"state": "error", "positionSamples": 99}, 3))
        failure = watch.observe({"state": "error", "error": "different message"}, 5)
        self.assertEqual(failure["highest_position_samples"], 100)
        self.assertEqual(failure["no_progress_seconds"], 5)

    def test_recovery_clears_error_window_and_a_new_error_gets_its_own_grace(self):
        watch = RUN.SESSION.CaptureErrorWatch(5)
        error = {"state": "error", "positionSamples": 100}
        self.assertIsNone(watch.observe(error, 0))
        self.assertIsNone(watch.observe({"state": "playing", "positionSamples": 110}, 6))
        self.assertIsNone(watch.observe(error, 7))
        self.assertIsNone(watch.observe(error, 11))
        self.assertIsNone(watch.observe({"state": "complete", "positionSamples": 200}, 12))


class GpuArtifactBinding(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.binary = self.root / "blah2"
        for name in ("blah2", "blah2-gpu-worker", *(item[0] for item in RUN.ARTIFACTS.MODULES.values())):
            (self.root / name).write_bytes(name.encode())
        self.binding = RUN.ARTIFACTS.prepare(self.binary, True)
        self.events = artifact_events(self.binding)

    def validate(self, events=None):
        return RUN.ARTIFACTS.validate(self.binding,
            artifact_log(self.events if events is None else events), 987654)

    def test_exact_runtime_cohort_excludes_unattempted_resident_module(self):
        result = self.validate()
        self.assertEqual([value["role"] for value in result["loaded"]], ["portable"])
        (self.root / "blah2-gpu-resident-vulkan.so").write_bytes(b"unused replacement")
        self.assertTrue(self.validate()["passed"])

    def test_resident_and_temporal_interfaces_share_one_pinned_module(self):
        resident = artifact_events(self.binding, role="resident")
        temporal = artifact_events(self.binding, worker=987656, role="resident")
        temporal[3].update(abi=1, factory="blah2_gpu_create_temporal", workerMode="--temporal-v1")
        temporal[4].update(factory="blah2_gpu_create_temporal", workerMode="--temporal-v1")
        result = self.validate(resident + temporal)
        self.assertEqual([item["abi"] for item in result["loaded"]], [18, 1])
        self.assertEqual(result["loaded"][0]["module"], result["loaded"][1]["module"])
        for field, value in (("abi", 18), ("factory", "blah2_gpu_create_resident"),
                             ("workerMode", "--resident-v5"), ("factory", None)):
            broken = copy.deepcopy(temporal)
            broken[3][field] = value
            with self.subTest(field=field), self.assertRaisesRegex(ValueError, "unexpected module"):
                self.validate(broken)
        broken = copy.deepcopy(temporal)
        broken[4]["workerMode"] = "--resident-v5"
        with self.assertRaisesRegex(ValueError, "backend factory or worker mode differs"):
            self.validate(broken)

    def test_resident_binding_matches_native_module_abi(self):
        header = (Path(__file__).parents[2] / "src/process/ambiguity/GpuResidentBackend.h").read_text()
        match = re.search(r"namespace blah2::resident\s*\{.*?constexpr unsigned moduleAbi\s*=\s*(\d+)\s*;",
                          header, re.DOTALL)
        self.assertIsNotNone(match)
        expected = int(match.group(1))
        self.assertEqual(RUN.ARTIFACTS.MODULES["resident"][1], expected)
        self.assertEqual(RUN.ARTIFACTS.FACTORIES["resident"]["blah2_gpu_create_resident"][0], expected)

    def test_module_and_worker_replacement_are_rejected(self):
        for role in ("processor", "worker", "portable"):
            with self.subTest(role=role):
                path = Path(self.binding["artifacts"][role]["path"])
                original = path.read_bytes()
                replacement = path.with_suffix(".replacement")
                replacement.write_bytes(original)  # Same hash, different loaded image/inode.
                replacement.replace(path)
                with self.assertRaisesRegex(ValueError, "pinned artifact changed"):
                    self.validate()
                self.binding = RUN.ARTIFACTS.prepare(self.binary, True)
                self.events = artifact_events(self.binding)

    def test_missing_worker_receipt_is_rejected_even_with_another_healthy_child(self):
        missing = artifact_events(self.binding, worker=987656)[:1]
        with self.assertRaisesRegex(ValueError, "spawned GPU worker lacks"):
            self.validate(self.events + missing)
        with self.assertRaisesRegex(ValueError, "missing GPU worker"):
            self.validate([])

    def test_missing_and_late_module_receipts_fail_closed(self):
        for indices in ((0, 1, 2), (0, 1, 2, 4), (0, 1, 2, 4, 3), (0, 2, 3, 4)):
            with self.subTest(indices=indices), self.assertRaises(ValueError):
                self.validate([self.events[index] for index in indices])

    def test_unexpected_path_hash_abi_device_and_launch_nonce_are_rejected(self):
        for field, value in (("abi", 99), ("deviceRequest", "another-device"),
                             ("nonce", "another-launch"), ("driverStatus", {})):
            events = copy.deepcopy(self.events)
            events[3][field] = value
            with self.subTest(field=field), self.assertRaises(ValueError):
                self.validate(events)
        for field, value in (("path", "/different/module.so"), ("sha256", "0" * 64)):
            events = copy.deepcopy(self.events)
            events[3]["module"][field] = value
            with self.subTest(field=field), self.assertRaisesRegex(ValueError, "unexpected module"):
                self.validate(events)

    def test_worker_can_report_before_parent_spawn_record(self):
        self.assertTrue(self.validate(self.events[1:] + self.events[:1])["passed"])

    def test_environment_cannot_inherit_another_launch_pin(self):
        env = RUN.ARTIFACTS.environment(self.binding,
            {"VECTORWARP_GPU_ARTIFACT_RESIDENT_PATH": "wrong", "UNRELATED": "retained",
             "VECTORWARP_GPU_ARTIFACT_NONCE": "stale", "VECTORWARP_GPU_ARTIFACT_CHANNEL": "hostile"})
        self.assertEqual(env["UNRELATED"], "retained")
        self.assertEqual(env["VECTORWARP_GPU_ARTIFACT_RESIDENT_PATH"],
                         self.binding["artifacts"]["resident"]["path"])
        self.assertEqual(env["VECTORWARP_GPU_ARTIFACT_NONCE"], self.binding["nonce"])
        self.assertNotIn("VECTORWARP_GPU_ARTIFACT_CHANNEL", env)

    def channel(self, config=None):
        config = config or {"capture": {"replay": {"state": True, "format": "blah2"}}}
        channel = RUN.ARTIFACTS.ReceiptChannel(self.binding, config)
        self.addCleanup(channel.close)
        return channel, config

    def test_private_channel_passes_fresh_explicit_replay_authorization(self):
        channel, config = self.channel()
        authorization = config["capture"]["replay"]["artifacts"]
        self.assertEqual(authorization, {"enabled": True, "confirmation": RUN.ARTIFACTS.CONFIRMATION,
            "descriptor": channel.pass_fds[0], "nonce": self.binding["nonce"]})
        self.assertFalse(channel.sender.get_inheritable())
        for event in self.events:
            channel.sender.send(artifact_log([event]).encode(), socket.MSG_DONTWAIT)
        channel.launched()
        self.assertTrue(RUN.ARTIFACTS.validate(self.binding, channel.finish(), 987654)["passed"])

    def test_live_or_unsupported_replay_cannot_authorize_artifacts(self):
        for state, format in ((False, "blah2"), (1, "blah2"), (True, "unrecognized"), (True, "raw")):
            with self.subTest(state=state, format=format), self.assertRaisesRegex(ValueError, "offline replay"):
                self.channel({"capture": {"replay": {"state": state, "format": format}}})

    def test_datagram_peer_reset_preserves_receipts_and_fail_closed_validation(self):
        for events in (self.events, [], self.events[:1]):
            with self.subTest(events=len(events)):
                receiver, sender = Mock(), Mock()
                sender.fileno.return_value = 42
                receiver.recv.side_effect = [artifact_log([event]).encode() for event in events] + [
                    ConnectionResetError("closed datagram peer")]
                with patch.object(RUN.ARTIFACTS.socket, "socketpair", return_value=(receiver, sender)), \
                     patch.object(RUN.ARTIFACTS.select, "select", return_value=([], [], [])):
                    channel, _ = self.channel()
                    output = channel.finish()
                self.assertEqual(output, artifact_log(events))
                if events == self.events:
                    self.assertTrue(RUN.ARTIFACTS.validate(self.binding, output, 987654)["passed"])
                    with patch.object(RUN.ARTIFACTS, "verify_files", side_effect=ValueError("pinned artifact changed")):
                        with self.assertRaisesRegex(ValueError, "pinned artifact changed"):
                            RUN.ARTIFACTS.validate(self.binding, output, 987654)
                else:
                    with self.assertRaisesRegex(ValueError, "missing GPU worker"):
                        RUN.ARTIFACTS.validate(self.binding, output, 987654)

    def test_peer_reset_cannot_erase_malformed_receipt_or_hide_other_socket_errors(self):
        for payloads, error in (([b"invalid\n", ConnectionResetError("peer closed")], "malformed"),
                                ([OSError("unrelated socket failure")], "unrelated socket failure"),
                                ([OSError(errno.ENOBUFS, "No buffer space available")], "No buffer space available")):
            receiver, sender = Mock(), Mock()
            sender.fileno.return_value = 42
            receiver.recv.side_effect = payloads
            with patch.object(RUN.ARTIFACTS.socket, "socketpair", return_value=(receiver, sender)), \
                 patch.object(RUN.ARTIFACTS.select, "select", return_value=([], [], [])):
                channel, _ = self.channel()
                with self.assertRaisesRegex(ValueError, error): channel.finish()

    def test_private_channel_rejects_hostile_and_oversize_datagrams(self):
        for payload in (b"not json\n", b"VECTORWARP_GPU_ARTIFACT {}\nsecond line\n",
                        b"VECTORWARP_GPU_ARTIFACT \xff\n"):
            with self.subTest(payload=payload[:32]):
                channel, _ = self.channel()
                channel.sender.send(payload, socket.MSG_DONTWAIT)
                with self.assertRaises(ValueError): channel.finish()
        # Darwin can reject MAX_RECEIPT+1 at send() with EMSGSIZE before the
        # collector sees it. Inject that exact receive result to exercise the
        # unchanged production bound independently of the kernel datagram cap.
        receiver, sender = Mock(), Mock()
        sender.fileno.return_value = 42
        receiver.recv.side_effect = [b"x" * (RUN.ARTIFACTS.MAX_RECEIPT + 1),
                                    ConnectionResetError("closed datagram peer")]
        with patch.object(RUN.ARTIFACTS.socket, "socketpair", return_value=(receiver, sender)), \
             patch.object(RUN.ARTIFACTS.select, "select", return_value=([], [], [])):
            channel, _ = self.channel()
            with self.assertRaisesRegex(ValueError, "exceeded its bound"): channel.finish()
        self.assertEqual(channel.records, [])

    def test_receipt_retention_is_bounded(self):
        with patch.object(RUN.ARTIFACTS, "MAX_RECEIPTS", 2):
            channel, _ = self.channel()
            for _ in range(3): channel.sender.send(artifact_log([self.events[0]]).encode(), socket.MSG_DONTWAIT)
            with self.assertRaisesRegex(ValueError, "exceeded its bound"): channel.finish()
            self.assertLessEqual(len(channel.records), 2)

    def assert_stalled_sender_backpressure(self, sender):
        payload = b"x" * 128  # Fit beneath Darwin's per-datagram kernel ceiling.
        sent = 0
        with self.assertRaises(OSError) as raised:
            for _ in range(10000):
                self.assertEqual(sender.send(payload, socket.MSG_DONTWAIT), len(payload))
                sent += 1
        self.assertIn(raised.exception.errno, {errno.EAGAIN, errno.EWOULDBLOCK, errno.ENOBUFS})
        self.assertGreater(sent, 0)  # An initial EMSGSIZE/error is not queue backpressure.

    def test_stalled_runner_datagram_send_fails_without_waiting(self):
        # Exercise the exact nonblocking transport contract without any GPU or
        # C++ build: a receiver that never drains cannot stall its producer.
        sender, receiver = socket.socketpair(socket.AF_UNIX, socket.SOCK_DGRAM)
        self.addCleanup(sender.close); self.addCleanup(receiver.close)
        sender.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 4096)
        self.assert_stalled_sender_backpressure(sender)
        # Deterministically exercise Darwin ENOBUFS and Linux EAGAIN on either
        # host. This assertion is test-only; collector errors remain fatal.
        for number in {errno.EAGAIN, errno.EWOULDBLOCK, errno.ENOBUFS}:
            with self.subTest(backpressure=number):
                fake = Mock(send=Mock(side_effect=[128, OSError(number, "queue full")]))
                self.assert_stalled_sender_backpressure(fake)
        for responses in ([128, OSError(errno.EMSGSIZE, "oversize")],
                          [128, OSError(errno.EPIPE, "closed")],
                          [OSError(errno.ENOBUFS, "no successful send")], [127]):
            with self.subTest(invalid=responses), self.assertRaises(AssertionError):
                self.assert_stalled_sender_backpressure(Mock(send=Mock(side_effect=responses)))

    def test_modules_are_resolved_beside_the_actual_symlinked_worker(self):
        actual = self.root / "actual"
        actual.mkdir()
        (actual / "blah2-gpu-worker").write_bytes(b"resolved worker")
        (actual / "blah2-gpu-vulkan.so").write_bytes(b"resolved module")
        alias = self.root / "actual-alias"
        alias.symlink_to(actual, target_is_directory=True)
        (self.root / "blah2-gpu-worker").unlink()
        (self.root / "blah2-gpu-worker").symlink_to(alias / "blah2-gpu-worker")
        binding = RUN.ARTIFACTS.prepare(self.binary, True)
        self.assertEqual(binding["artifacts"]["worker"]["path"], str((alias / "blah2-gpu-worker").resolve(strict=True)))
        self.assertEqual(binding["artifacts"]["portable"]["path"], str((alias / "blah2-gpu-vulkan.so").resolve(strict=True)))
        self.assertNotIn("resident", binding["artifacts"])

    def test_loader_receipts_precede_factory_call_without_an_abi_change(self):
        root = Path(__file__).resolve().parents[2]
        worker = (root / "src/process/ambiguity/GpuWorker.cpp").read_text()
        loader = (root / "src/process/ambiguity/GpuArtifactBinding.h").read_text()
        self.assertLess(loader.index('emit("module-loaded"'), loader.index('emit("backend-start"'))
        self.assertLess(loader.index('emit("backend-start"'), loader.index('return reinterpret_cast<Create>'))
        for role in ("wideband", "resident"):
            self.assertIn(f"module->factory<blah2::{role}::Create>", worker.replace("wideband_gpu", "wideband"))
        self.assertIn("module->factory<blah2::GpuCreate>", worker)
        self.assertNotIn("dlopen(", worker)


if __name__ == "__main__":
    unittest.main()
