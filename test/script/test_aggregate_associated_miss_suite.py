#!/usr/bin/env python3
"""Regression test for target-label-preserving exploratory aggregation."""
import json
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def result(job_id):
    method = {"target_score": 2.0, "margin_over_control_p99_db": -1.0,
              "controls": 9, "controls_exceeding_target": 3}
    return {"job_id": job_id, "family_trials": 15,
            "gain_treatments": {"raw_as_acquired": {
                "methods": {"spatial_glrt": method}}}}


with tempfile.TemporaryDirectory() as temporary:
    root = Path(temporary); raw = root / "results"; raw.mkdir()
    cohort = {"positives": [{"capture": {"file": "shared.mchq"}}],
              "misses": [
                  {"job_id": "clean", "event_id": "c", "pass_id": "c", "hex": "a",
                   "capture": {"file": "clean.mchq"}},
                  {"job_id": "overlap", "event_id": "o", "pass_id": "o", "hex": "b",
                   "capture": {"file": "shared.mchq"}}], "summary": {"miss_labels": 2}}
    (root / "cohort.json").write_text(json.dumps(cohort))
    for job in cohort["misses"]:
        (raw / f'{job["job_id"]}-result.json').write_text(json.dumps(result(job["job_id"])))
    output = root / "summary.json"
    subprocess.run(["python3", str(ROOT / "script" / "aggregate-associated-miss-suite.py"),
                    "--cohort", str(root / "cohort.json"), "--results", str(raw),
                    "--output", str(output)], check=True)
    summary = json.loads(output.read_text())
    assert summary["completed_labels"] == 2 and not summary["missing_labels"]
    assert {row["scope"] for row in summary["labels"]} == {"clean", "overlap_flagged"}
    assert all(row["raw_methods"][0]["raw_rank"] == 4 for row in summary["labels"])
    assert not summary["validation"]["plus_3db_validated"]
print("PASS associated exploratory aggregation preserves labels and validation guard")
