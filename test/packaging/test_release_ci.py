#!/usr/bin/env python3
"""Offline negative controls for authenticated same-source publication checks."""
import importlib.util
import json
from pathlib import Path
import subprocess
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("release_ci", ROOT / "script/verify-release-ci.py")
gate = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(gate)
SOURCE = "a" * 40


def run(workflow="ci.yml", **values):
    return {"id": 123, "run_number": 7, "run_attempt": 1, "head_sha": SOURCE,
            "head_branch": "main", "event": "push", "status": "completed", "conclusion": "success",
            "path": f".github/workflows/{workflow}",
            "repository": {"full_name": gate.REPOSITORY},
            "head_repository": {"full_name": gate.REPOSITORY}, **values}


class ReleaseCiTest(unittest.TestCase):
    def test_exact_successful_main_run_is_accepted(self):
        for event in ("push", "workflow_dispatch"):
            receipt = gate.select_run([run(event=event)], "ci.yml", SOURCE)
            self.assertEqual(receipt["runId"], 123)
            self.assertEqual(receipt["runAttempt"], 1)

    def test_wrong_source_branch_pr_fork_and_workflow_cannot_qualify(self):
        changes = ({"head_sha": "b" * 40}, {"head_branch": "topic"}, {"event": "pull_request"},
                   {"repository": {"full_name": "other/repo"}},
                   {"head_repository": {"full_name": "other/repo"}},
                   {"path": ".github/workflows/other.yml"}, {"run_attempt": 0})
        for change in changes:
            with self.subTest(change=change), self.assertRaises(ValueError):
                gate.select_run([run(**change)], "ci.yml", SOURCE)

    def test_latest_failure_pending_or_failed_retry_overrides_old_success(self):
        for values in ({"conclusion": "failure"}, {"status": "in_progress", "conclusion": None},
                       {"conclusion": "cancelled"}, {"conclusion": "skipped"}):
            with self.subTest(values=values), self.assertRaises(ValueError):
                gate.select_run([run(), run(id=124, run_number=8, **values)], "ci.yml", SOURCE)
        with self.assertRaises(ValueError):
            gate.select_run([run(), run(run_attempt=2, conclusion="failure")], "ci.yml", SOURCE)

    def test_current_publish_run_cannot_attest_itself(self):
        with self.assertRaises(ValueError):
            gate.select_run([run(event="workflow_dispatch")], "ci.yml", SOURCE, "123")
        receipt = gate.select_run([run(), run(id=124, run_number=8, status="in_progress")],
                                  "ci.yml", SOURCE, "124")
        self.assertEqual(receipt["runId"], 123)

    def test_complete_matrix_is_queried_without_success_filter(self):
        def query(command, **kwargs):
            workflow = command[2].split("/workflows/")[1].split("/")[0]
            self.assertIn(f"head_sha={SOURCE}&branch=main", command[2])
            self.assertNotIn("status=", command[2])
            return subprocess.CompletedProcess(command, 0, json.dumps(
                {"total_count": 1, "workflow_runs": [run(workflow)]}))
        with mock.patch.object(gate.subprocess, "run", side_effect=query) as request:
            receipt = gate.collect(SOURCE)
        self.assertEqual(request.call_count, 5)
        self.assertEqual({item["workflow"] for item in receipt["workflows"]}, set(gate.WORKFLOWS))

    def test_missing_or_truncated_history_fails_closed(self):
        for report in ({"total_count": 0, "workflow_runs": []},
                       {"total_count": 101, "workflow_runs": [run()]}):
            with self.subTest(report=report), mock.patch.object(gate.subprocess, "run",
                    return_value=subprocess.CompletedProcess([], 0, json.dumps(report))):
                with self.assertRaises(ValueError):
                    gate.collect(SOURCE)


if __name__ == "__main__":
    unittest.main()
