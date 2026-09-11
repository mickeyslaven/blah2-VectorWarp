"""Small offline checks: no receiver, GPU or benchmark process is started."""
import csv
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

from summarize import numeric_difference, percentile, summarize


class ToolsTests(unittest.TestCase):
    def test_matrix_rejects_invalid_arguments_before_output(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary)/"not-created"
            base = [sys.executable, str(Path(__file__).with_name("run_matrix.py")),
                "--root", temporary, "--output", str(output), "--recording", "absent.mchq",
                "--profile", "absent.json"]
            for options in (["--sha256", "bad"],
                            ["--sha256", "a"*64, "--devices", "../bad"],
                            ["--sha256", "a"*64, "--expected-frames", "0"]):
                result = subprocess.run(base+options, capture_output=True, text=True)
                self.assertEqual(result.returncode, 2)
                self.assertFalse(output.exists())

    def test_comparison_and_percentile(self):
        self.assertEqual(numeric_difference({"id": "a", "delay": 3}, {"id": "b", "delay": 3}), 0)
        self.assertEqual(numeric_difference([1], []), float("inf"))
        self.assertAlmostEqual(percentile([1, 3, 5], .95), 4.8)

    def test_no_guard_bypass_by_nonfinite_limits(self):
        with tempfile.TemporaryDirectory() as directory:
            for invalid in ("nan", "inf", "-1", "0"):
                result = subprocess.run([sys.executable, str(Path(__file__).with_name("guard.py")),
                    "--log", str(Path(directory)/"never.jsonl"), "--max-celsius", invalid,
                    "--", "not-a-real-command"], capture_output=True, text=True)
                self.assertEqual(result.returncode, 2)
                self.assertIn("finite and positive", result.stderr)
                self.assertFalse((Path(directory)/"never.jsonl").exists())

    def test_summary_requires_complete_outputs_and_baseline(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            label = "pair-upstream-cpu-auto-r1"
            (directory/"manifest.json").write_text(json.dumps({"host": "test", "config": {"sample_rate": 10, "cpi": .2}}))
            (directory/f"{label}.summary.json").write_text(json.dumps({"frames": 1,
                "samples_per_channel": 2, "startup_ms": 1}))
            stages = ("read_ms", "extract_ms", "reference_ms", "spectrum_ms", "clutter_ms",
                "ambiguity_ms", "detection_ms", "tracker_ms", "json_ms", "pipeline_ms")
            with (directory/f"{label}.frames.csv").open("w") as stream:
                writer = csv.DictWriter(stream, fieldnames=stages)
                writer.writeheader(); writer.writerow(dict.fromkeys(stages, 1))
            (directory/f"{label}.thermal.jsonl").write_text(json.dumps({"gpu": {"nvidia": "40, 12, 300, 9, 1000"}})+"\n")
            (directory/f"{label}.outputs.jsonl").write_text("")
            with self.assertRaisesRegex(RuntimeError, "output count"):
                summarize(directory)
            (directory/f"{label}.outputs.jsonl").write_text(json.dumps({"detections": [], "tracks": []})+"\n")
            result = summarize(directory)[0]
            self.assertEqual(result["output_frames_compared"], 1)
            self.assertIsNone(result["steady_mean_ms"])
            self.assertEqual(result["peak_host_nvidia_vram_bytes"], 300*2**20)


if __name__ == "__main__":
    unittest.main()
