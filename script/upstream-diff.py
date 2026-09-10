#!/usr/bin/env python3
"""Write a review-only diff against the immutable upstream baseline pin.

The output intentionally contains only repository identities and Git's diff
metadata. It does not interpret commit messages as implemented capabilities.
"""
import argparse
import json
from pathlib import Path
import re
import subprocess
import sys


REPOSITORY = re.compile(r"[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+\Z")
COMMIT = re.compile(r"[0-9a-f]{40}\Z")


def git(*args, cwd):
    result = subprocess.run(["git", *args], cwd=cwd, text=True, capture_output=True, check=False)
    if result.returncode:
        raise RuntimeError(result.stderr.strip() or "git command failed")
    return result.stdout.strip()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--repository", type=Path, default=Path("."))
    args = parser.parse_args()
    baseline = json.loads(args.baseline.read_text())
    upstream = baseline.get("upstream_repository", "")
    upstream_commit = baseline.get("upstream_commit", "")
    integration_base = baseline.get("integration_base_commit", "")
    if not REPOSITORY.fullmatch(upstream) or not COMMIT.fullmatch(upstream_commit) or not COMMIT.fullmatch(integration_base):
        parser.error("baseline must contain a GitHub owner/repository and immutable 40-hex commits")
    repository = args.repository.resolve()
    try:
        git("cat-file", "-e", upstream_commit + "^{commit}", cwd=repository)
    except RuntimeError:
        # Fetch only the baseline object by its full hash; never compare to a
        # moving branch or retarget the checkout's configured remotes.
        git("fetch", "--no-tags", f"https://github.com/{upstream}.git", upstream_commit, cwd=repository)
        git("cat-file", "-e", upstream_commit + "^{commit}", cwd=repository)
    current = git("rev-parse", "HEAD", cwd=repository)
    stat = git("diff", "--stat", f"{upstream_commit}..{current}", cwd=repository)
    names = git("diff", "--name-status", f"{upstream_commit}..{current}", cwd=repository)
    text = "\n".join((
        "VectorWarp upstream-diff review artifact",
        "Purpose: review metadata only; this file makes no feature or test claims.",
        f"current_revision: {current}",
        f"upstream_repository: {upstream}",
        f"upstream_commit: {upstream_commit}",
        f"integration_base_commit: {integration_base}",
        "",
        "git diff --stat:", stat or "(no changes)",
        "",
        "git diff --name-status:", names or "(no changes)",
        "",
    ))
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(text)


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, RuntimeError, json.JSONDecodeError) as error:
        print(f"upstream-diff: {error}", file=sys.stderr)
        raise SystemExit(1)
