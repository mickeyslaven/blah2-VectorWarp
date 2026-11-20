#!/usr/bin/env python3
"""Authenticate same-source main CI before native signing or tap publication.

This checks hosted software evidence only. GPU qualification, exact package
identity, the 24-hour soak and explicit publication approval remain separate.
"""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess

REPOSITORY = "mickeyslaven/blah2-VectorWarp"
WORKFLOWS = ("ci.yml", "macos.yml", "macos-standalone.yml", "homebrew.yml", "kraken-bundled.yml")


def require(condition, message):
    if not condition:
        raise ValueError(message)


def select_run(runs, workflow, source, exclude_run_id=""):
    require(re.fullmatch(r"[0-9a-f]{40}", source), "invalid source commit")
    require(workflow in WORKFLOWS, "unexpected release workflow")
    candidates = [run for run in runs
                  if run.get("head_sha") == source and run.get("head_branch") == "main"
                  and run.get("event") in ("push", "workflow_dispatch")
                  and str(run.get("id")) != str(exclude_run_id)]
    require(candidates, f"{workflow}: no main run for exact source {source}")
    for run in candidates:
        require(type(run.get("id")) is int and run["id"] > 0
                and type(run.get("run_number")) is int and run["run_number"] > 0
                and type(run.get("run_attempt")) is int and run["run_attempt"] > 0,
                f"{workflow}: missing run identity")
        require(run.get("path") == f".github/workflows/{workflow}"
                and run.get("repository", {}).get("full_name") == REPOSITORY
                and run.get("head_repository", {}).get("full_name") == REPOSITORY,
                f"{workflow}: workflow or repository mismatch")
    # Never hide a newer failure/pending run behind an earlier green result.
    latest = max(candidates, key=lambda run: (run["run_number"], run["run_attempt"]))
    require(latest.get("status") == "completed" and latest.get("conclusion") == "success",
            f"{workflow}: latest same-source main run {latest['id']} is "
            f"{latest.get('status')}/{latest.get('conclusion')}")
    return {"workflow": workflow, "runId": latest["id"], "runAttempt": latest["run_attempt"],
            "url": f"https://github.com/{REPOSITORY}/actions/runs/{latest['id']}"}


def collect(source, exclude_run_id=""):
    require(re.fullmatch(r"[0-9a-f]{40}", source), "invalid source commit")
    receipts = []
    failures = []
    for workflow in WORKFLOWS:
        endpoint = (f"repos/{REPOSITORY}/actions/workflows/{workflow}/runs"
                    f"?head_sha={source}&branch=main&per_page=100")
        response = subprocess.run(["gh", "api", endpoint], check=True, capture_output=True,
                                  text=True, timeout=30)
        report = json.loads(response.stdout)
        require(isinstance(report.get("workflow_runs"), list), "invalid workflow API response")
        # A bounded query is sufficient for normal release activity. Refuse a
        # truncated history rather than infer the newest trusted run from it.
        require(type(report.get("total_count")) is int
                and report["total_count"] <= len(report["workflow_runs"]),
                f"{workflow}: bounded run history is incomplete")
        try:
            receipts.append(select_run(report["workflow_runs"], workflow, source, exclude_run_id))
        except ValueError as error:
            failures.append(str(error))
    require(not failures, "Release CI is incomplete:\n" + "\n".join(failures))
    return {"schema": 1, "repository": REPOSITORY, "sourceCommit": source, "workflows": receipts}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-sha", default=os.environ.get("GITHUB_SHA", ""))
    parser.add_argument("--exclude-run-id", default=os.environ.get("GITHUB_RUN_ID", ""))
    parser.add_argument("--output", type=Path, default=Path("release-ci-provenance.json"))
    args = parser.parse_args()
    require(os.environ.get("GITHUB_REPOSITORY", REPOSITORY) == REPOSITORY,
            "release CI must use the canonical repository")
    receipt = collect(args.source_sha, args.exclude_run_id)
    args.output.write_text(json.dumps(receipt, indent=2, sort_keys=True) + "\n")
    print(f"All {len(receipt['workflows'])} hosted workflows passed for {args.source_sha}")


if __name__ == "__main__":
    main()
