#!/usr/bin/env python3
"""Offline trigger, retention and collector lifecycle checks; no SDR/perf needed."""
import json
import importlib.util
from pathlib import Path
import signal
import sys
import tempfile
import time
import unittest

from pi_spike_diagnostics import SpikeDiagnostics


class Recorder:
    def __init__(self):
        self.signals = []

    def poll(self):
        return None

    def send_signal(self, value):
        self.signals.append(value)


class SpikeTests(unittest.TestCase):
    def test_diagnostic_write_error_preserves_failure_evidence(self):
        from test_live_rspduo_check import CHECK, FAKE
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            binary = root / "fake-processor"
            binary.write_text(f"#!{sys.executable}\nMODE = 'success'\n" + FAKE)
            binary.chmod(0o755)
            observer = SpikeDiagnostics(root / "live", perf_command=[sys.executable, "-c",
                "import signal,time; signal.signal(signal.SIGUSR2, lambda *_: None); "
                "signal.signal(signal.SIGINT, lambda *_: exit(0)); time.sleep(10)"])
            observer._context = lambda _: None
            original = observer.observe
            def fail_write(frames, arrivals, states):
                if frames:
                    raise OSError("simulated diagnostic storage failure")
                original(frames, arrivals, states)
            observer.observe = fail_write
            result = CHECK.run_check(binary, .3, root / "live", observer=observer)
            self.assertFalse(result["ok"])
            self.assertTrue(any("simulated diagnostic storage failure" in reason for reason in result["failures"]))
            self.assertTrue((root / "live.evidence.json").is_file())
            self.assertIsNotNone(observer.perf.returncode)

    def test_profiler_death_rejects_soak_and_preserves_evidence(self):
        from test_live_rspduo_check import CHECK, FAKE
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            binary = root / "fake-processor"
            binary.write_text(f"#!{sys.executable}\nMODE = 'success'\n" + FAKE)
            binary.chmod(0o755)
            observer = SpikeDiagnostics(root / "live", perf_command=[
                sys.executable, "-c", "import time; time.sleep(.7); exit(3)"])
            observer._context = lambda _: None
            result = CHECK.run_check(binary, 1.5, root / "live", observer=observer)
            self.assertFalse(result["ok"])
            self.assertTrue(any("diagnostic" in reason for reason in result["failures"]))
            self.assertTrue(result["normalStop"])
            self.assertTrue((root / "live.evidence.json").is_file())

    def test_profiler_start_failure_never_launches_processor(self):
        spec = importlib.util.spec_from_file_location("live_check", Path(__file__).with_name("live-rspduo-check.py"))
        check = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(check)
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            marker = root / "opened-hardware"
            binary = root / "fake-processor"
            binary.write_text(f"#!{sys.executable}\nfrom pathlib import Path\nPath({str(marker)!r}).touch()\n")
            binary.chmod(0o755)
            observer = SpikeDiagnostics(root / "live", perf_command=[sys.executable, "-c", "exit(1)"])
            started = time.monotonic()
            with self.assertRaisesRegex(RuntimeError, "failed to start"):
                check.run_check(binary, .1, root / "live", observer=observer)
            self.assertLess(time.monotonic() - started, 3)
            self.assertFalse(marker.exists())
            self.assertTrue(all(stream.closed for stream in observer.files))

    def test_spike_threshold_stall_and_incremental_files(self):
        with tempfile.TemporaryDirectory() as directory:
            fake = Path(directory) / "perf.py"
            fake.write_text("import signal,time\nsignal.signal(signal.SIGUSR2, lambda *_: None)\n"
                            "signal.signal(signal.SIGINT, lambda *_: exit(0))\ntime.sleep(20)\n")
            observer = SpikeDiagnostics(Path(directory) / "live", stall_seconds=.15,
                                        perf_command=[sys.executable, str(fake)])
            observer._context = lambda _: None
            observer.start()
            observer.observe([{"nCpi": 1, "cpi": 749}, {"nCpi": 2, "cpi": 751}],
                             [time.monotonic(), time.monotonic()], [{"state": "live"}])
            time.sleep(.4)
            observer.close()
            events = [json.loads(line) for line in (observer.root / "events.jsonl").read_text().splitlines()]
            self.assertEqual([x["reason"] for x in events], ["large_cpi", "timing_stall"])
            self.assertTrue(events[0]["snapshotRequested"])
            self.assertFalse(events[1]["snapshotRequested"])
            self.assertEqual(len((observer.root / "frames.jsonl").read_text().splitlines()), 2)
            self.assertEqual(len((observer.root / "statuses.jsonl").read_text().splitlines()), 1)
            self.assertEqual(observer.summary()["errors"], [])
            self.assertEqual(observer.perf.returncode, 0)

    def test_normal_stop_suppresses_stall_trigger(self):
        with tempfile.TemporaryDirectory() as directory:
            observer = SpikeDiagnostics(Path(directory) / "live", stall_seconds=.05,
                perf_command=[sys.executable, "-c", "import signal,time; "
                    "signal.signal(signal.SIGINT, lambda *_: exit(0)); time.sleep(10)"])
            observer.start()
            observer.processor_stopping()
            observer.observe([{"nCpi": 1, "cpi": 350}], [time.monotonic()-.2], [])
            time.sleep(.2)
            observer.close()
            self.assertEqual((observer.root / "events.jsonl").read_text(), "")

    def test_snapshot_cap_and_failure_bypasses_cooldown(self):
        with tempfile.TemporaryDirectory() as directory:
            observer = SpikeDiagnostics(Path(directory) / "live")
            observer.root.mkdir()
            observer.perf = Recorder()
            observer._context = lambda _: None
            with (observer.root / "events.jsonl").open("w") as observer.events_stream:
                for _ in range(8):
                    observer._event({"reason": "harness_failure"})
            self.assertEqual(observer.snapshots, 6)
            self.assertEqual(observer.perf.signals, [signal.SIGUSR2] * 6)


if __name__ == "__main__":
    unittest.main()
