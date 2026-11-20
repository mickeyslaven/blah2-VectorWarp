"""Tiny sealed synthetic snapshots; no GPU, host captures, or external truth."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

SCORER, FIXTURES = sys.argv[1:3]
sys.argv = sys.argv[:1]


class ScorerCliTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="resident-score-test-")
        self.addCleanup(self.temp.cleanup)
        self.directory = Path(self.temp.name)
        subprocess.run([FIXTURES, "--fixtures", str(self.directory)], check=True,
                       capture_output=True, text=True, timeout=10)
        self.paths = [str(self.directory / f"{i}.vwrps") for i in range(1, 9)]

    def run_score(self, extra=(), paths=None, success=True):
        result = subprocess.run([SCORER, *extra, *(paths or self.paths)],
                                capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 0 if success else 1, result.stdout + result.stderr)
        return [json.loads(line) for line in result.stdout.splitlines()]

    def manifest(self, points, identity):
        path = self.directory / "evaluation.json"
        path.write_text(json.dumps({"version": 1, "frames": [
            {"snapshot_and_lineage_sha256": identity, "points": points}]}))
        return ["--evaluation", str(path)]

    def test_unlabelled_warmup_and_hash_bound_target_control(self):
        records = self.run_score()
        self.assertEqual(records[-1]["record"], "complete")
        first, last = records[0], records[-2]
        self.assertEqual([arm["ready"] for arm in first["arms"]], [True] * 3 + [False] * 3)
        self.assertTrue(all(arm["ready"] for arm in last["arms"]))
        self.assertTrue(all(arm["evaluation_points"] is None for arm in last["arms"]))
        self.assertEqual(len(last["scorer_executable_sha256"]), 64)
        points = [{"kind": "target", "id": "synthetic-pass", "evidence": "synthetic test only",
                   "row": 3, "column": 4},
                  {"kind": "control", "id": "synthetic-control", "evidence": "synthetic test only",
                   "row": 3, "column": 0}]
        labelled = self.run_score(self.manifest(points, last["snapshot_and_lineage_sha256"]))[-2]
        for arm in labelled["arms"]:
            target, control = arm["evaluation_points"]
            self.assertTrue(target["available"])
            self.assertTrue(target["diagnostic_selected"])
            self.assertTrue(target["paired_baseline_selected"])
            self.assertFalse(control["diagnostic_selected"])
        self.assertIsNone(labelled["pfa"])
        self.assertIsNone(labelled["false_track_delta"])
        self.assertFalse(labelled["promotion_authorized"])
        self.assertFalse(labelled["gpu_execution"])

    def test_missing_sidecar_and_reordered_cohort_rejected(self):
        records = self.run_score(paths=[self.paths[1], self.paths[0]], success=False)
        self.assertEqual([row["record"] for row in records], ["failure"])
        Path(self.paths[0] + ".vwrl").unlink()
        records = self.run_score(success=False)
        self.assertIn("sidecar missing", records[-1]["reason"])

    def test_unknown_identity_bad_cell_and_conflicting_control_rejected(self):
        records = self.run_score()
        identity = records[-2]["snapshot_and_lineage_sha256"]
        point = {"kind": "control", "id": "c", "evidence": "synthetic", "row": 3, "column": 4}
        bad_identity = "0" * 64
        result = self.run_score(self.manifest([point], bad_identity), success=False)
        self.assertIn("absent from cohort", result[-1]["reason"])
        result = self.run_score(self.manifest([{**point, "row": 1000}], identity), success=False)
        self.assertIn("outside snapshot", result[-1]["reason"])
        result = self.run_score(self.manifest([point, {**point, "kind": "target"}], identity), success=False)
        self.assertIn("conflicting", result[-1]["reason"])


if __name__ == "__main__":
    unittest.main()
