#!/usr/bin/env python3
"""Offline policy tests for the trusted-main Homebrew tap publisher."""
import hashlib
import importlib.util
import json
from pathlib import Path
import shutil
import tempfile
import unittest
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "script" / "publish-homebrew-tap.py"
SPEC = importlib.util.spec_from_file_location("publish_homebrew_tap", SCRIPT)
publisher = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(publisher)

COMMIT = "a" * 40
NEXT_COMMIT = "b" * 40
DIGEST = "c" * 64


class Result:
    def __init__(self, stdout="", returncode=0):
        self.stdout = stdout
        self.returncode = returncode


class HomebrewPublishTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.files = self.root / "candidate"
        self.source = self.root / "source"
        self.source.mkdir()
        self.write_receipt(self.files, COMMIT)
        self.environment = {
            "GITHUB_ACTIONS": "true",
            "GITHUB_REPOSITORY": publisher.SOURCE_REPO,
            "GITHUB_REF": "refs/heads/main",
            "GITHUB_EVENT_NAME": "push",
            "GITHUB_SHA": COMMIT,
        }

    def tearDown(self):
        self.temp.cleanup()

    def write_receipt(self, root, commit, version="0.1.7", revision=1):
        for name in publisher.FILES:
            path = root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(f"{name} for {commit}\n", encoding="utf-8")
        receipt = {
            "source_commit": commit,
            "source_url": f"https://github.com/{publisher.SOURCE_REPO}/archive/{commit}.tar.gz",
            "source_sha256": DIGEST,
            "version": version,
            "revision": revision,
            "files": {name: hashlib.sha256((root / name).read_bytes()).hexdigest()
                      for name in publisher.FILES},
        }
        (root / publisher.RECEIPT).write_text(json.dumps(receipt), encoding="utf-8")
        return receipt

    def test_receipt_requires_exact_hashed_regular_files(self):
        self.assertEqual(publisher.read_receipt(self.files)["source_commit"], COMMIT)
        (self.files / "Formula/vectorwarp.rb").write_text("tampered\n")
        with self.assertRaisesRegex(ValueError, "does not match"):
            publisher.read_receipt(self.files)
        self.write_receipt(self.files, COMMIT)
        receipt = json.loads((self.files / publisher.RECEIPT).read_text())
        receipt["files"].pop("README.md")
        (self.files / publisher.RECEIPT).write_text(json.dumps(receipt))
        with self.assertRaisesRegex(ValueError, "exactly"):
            publisher.read_receipt(self.files)

    def test_context_rejects_pr_fork_branch_and_sha_mismatch(self):
        for key, value in (("GITHUB_EVENT_NAME", "pull_request"),
                           ("GITHUB_REPOSITORY", "fork/vectorwarp"),
                           ("GITHUB_REF", "refs/heads/topic"),
                           ("GITHUB_SHA", NEXT_COMMIT),
                           ("GITHUB_ACTIONS", "false")):
            with self.subTest(key=key):
                environment = dict(self.environment)
                environment[key] = value
                with self.assertRaisesRegex(ValueError, "trusted main"):
                    publisher.context(environment, COMMIT)

    def test_same_sha_is_idempotent_without_write_or_push(self):
        work = self.root / "work"
        calls = []

        def command(args, cwd=None, check=True):
            calls.append(tuple(args))
            if args[:3] == ["git", "rev-parse", "HEAD"]:
                return Result(f"{COMMIT}\n")
            if args == ["git", "ls-remote", publisher.SOURCE_REMOTE, "refs/heads/main"]:
                return Result(f"{COMMIT}\trefs/heads/main\n")
            if args[:2] == ["git", "clone"]:
                shutil.copytree(self.files, work)
                return Result()
            if args == ["git", "show-ref", "--verify", "--quiet", "refs/remotes/origin/main"]:
                return Result(returncode=0)
            if args == ["git", "checkout", "main"]:
                return Result()
            self.fail(f"unexpected mocked command: {args}")

        with patch.dict(publisher.os.environ, self.environment, clear=True), \
             patch.object(publisher, "command", side_effect=command), \
             patch("builtins.print"):
            publisher.publish(self.source, self.files, work, COMMIT)
        forbidden = {"add", "commit", "push", "reset", "fetch"}
        self.assertFalse(any(forbidden.intersection(command) for command in calls), calls)
        self.assertIn(("git", "show-ref", "--verify", "--quiet", "refs/remotes/origin/main"), calls)
        self.assertIn(("git", "checkout", "main"), calls)

    def test_first_publication_bootstraps_empty_tap_then_pushes_and_reads_back(self):
        work = self.root / "work"
        tap_commit = "d" * 40
        calls = []

        def command(args, cwd=None, check=True):
            calls.append(tuple(args))
            if args == ["git", "rev-parse", "HEAD"]:
                return Result(f"{COMMIT if cwd == self.source else tap_commit}\n")
            if args == ["git", "ls-remote", publisher.SOURCE_REMOTE, "refs/heads/main"]:
                return Result(f"{COMMIT}\trefs/heads/main\n")
            if args[:2] == ["git", "clone"]:
                work.mkdir()
                return Result()
            if args == ["git", "show-ref", "--verify", "--quiet", "refs/remotes/origin/main"]:
                return Result(returncode=1)
            if args == ["git", "ls-remote", "--heads", publisher.TAP_REMOTE]:
                return Result()
            if args == ["git", "symbolic-ref", "HEAD", "refs/heads/main"]:
                return Result()
            if args[0:2] == ["git", "push"]:
                return Result(returncode=0)
            if args == ["git", "ls-remote", publisher.TAP_REMOTE, "refs/heads/main"]:
                return Result(f"{tap_commit}\trefs/heads/main\n")
            if args[0] == "git" and any(item in args for item in ("add", "commit")):
                return Result()
            self.fail(f"unexpected mocked command: {args}")

        with patch.dict(publisher.os.environ, self.environment, clear=True), \
             patch.object(publisher, "command", side_effect=command), \
             patch("builtins.print"):
            publisher.publish(self.source, self.files, work, COMMIT)
        self.assertIn(("git", "symbolic-ref", "HEAD", "refs/heads/main"), calls)
        self.assertEqual(sum(call[:2] == ("git", "push") for call in calls), 1)
        self.assertEqual(publisher.read_receipt(work)["source_commit"], COMMIT)

    def test_rejected_push_with_competitor_receipt_refuses_without_second_push(self):
        work = self.root / "work"
        competitor = self.root / "competitor"
        self.write_receipt(competitor, NEXT_COMMIT, version="0.1.8")
        calls = []

        def command(args, cwd=None, check=True):
            calls.append(tuple(args))
            if args == ["git", "rev-parse", "HEAD"]:
                return Result(f"{COMMIT}\n")
            if args == ["git", "ls-remote", publisher.SOURCE_REMOTE, "refs/heads/main"]:
                return Result(f"{COMMIT}\trefs/heads/main\n")
            if args[:2] == ["git", "clone"]:
                work.mkdir()
                return Result()
            if args == ["git", "show-ref", "--verify", "--quiet", "refs/remotes/origin/main"]:
                return Result(returncode=0)
            if args == ["git", "checkout", "main"]:
                return Result()
            if args[0:2] == ["git", "push"]:
                return Result(returncode=1)
            if args[:2] == ["git", "fetch"]:
                return Result()
            if args[:3] == ["git", "reset", "--hard"]:
                for child in work.iterdir():
                    if child.is_dir():
                        shutil.rmtree(child)
                    else:
                        child.unlink()
                shutil.copytree(competitor, work, dirs_exist_ok=True)
                return Result()
            if args[:2] == ["git", "merge-base"]:
                return Result(returncode=1)
            if args[0] == "git" and any(item in args for item in ("add", "commit")):
                return Result()
            self.fail(f"unexpected mocked command: {args}")

        with patch.dict(publisher.os.environ, self.environment, clear=True), \
             patch.object(publisher, "command", side_effect=command):
            with self.assertRaisesRegex(ValueError, "non-ancestor"):
                publisher.publish(self.source, self.files, work, COMMIT)
        self.assertEqual(sum(call[:2] == ("git", "push") for call in calls), 1)

    def test_advanced_main_refuses_before_clone_write_or_push(self):
        work = self.root / "work"
        calls = []

        def command(args, cwd=None, check=True):
            calls.append(tuple(args))
            if args[:3] == ["git", "rev-parse", "HEAD"]:
                return Result(f"{COMMIT}\n")
            if args[:2] == ["git", "ls-remote"]:
                return Result(f"{NEXT_COMMIT}\trefs/heads/main\n")
            self.fail(f"publisher performed an operation after main advanced: {args}")

        with patch.dict(publisher.os.environ, self.environment, clear=True), \
             patch.object(publisher, "command", side_effect=command):
            with self.assertRaisesRegex(ValueError, "main advanced"):
                publisher.publish(self.source, self.files, work, COMMIT)
        self.assertFalse(work.exists())
        self.assertFalse(any({"clone", "add", "commit", "push"}.intersection(call) for call in calls))

    def test_unexpected_artifact_rejected_before_git_operations(self):
        (self.files / "unexpected.txt").write_text("no")
        with patch.dict(publisher.os.environ, self.environment, clear=True), \
             patch.object(publisher, "command") as command:
            with self.assertRaisesRegex(ValueError, "Unexpected files"):
                publisher.publish(self.source, self.files, self.root / "work", COMMIT)
        command.assert_not_called()

    def test_nonancestor_and_rollback_are_refused(self):
        previous = publisher.read_receipt(self.files)
        candidate = dict(previous, source_commit=NEXT_COMMIT)
        with self.assertRaisesRegex(ValueError, "non-ancestor"):
            publisher.validate_update(candidate, previous, lambda old, new: False)
        candidate = dict(candidate, version="0.1.6")
        with self.assertRaisesRegex(ValueError, "rollback"):
            publisher.validate_update(candidate, previous, lambda old, new: True)

    def test_workflow_keeps_publish_isolated_and_after_real_arm_checks(self):
        workflow = (ROOT / ".github/workflows/homebrew.yml").read_text(encoding="utf-8")
        self.assertIn("permissions:\n  contents: read", workflow)
        self.assertGreaterEqual(workflow.count("persist-credentials: false"), 3)
        self.assertIn("environment: homebrew-publish", workflow)
        self.assertIn("concurrency:", workflow)
        self.assertIn("runs-on: macos-15", workflow)
        self.assertIn("brew install --build-from-source mickeyslaven/vectorwarp/vectorwarp", workflow)
        self.assertIn("brew test mickeyslaven/vectorwarp/vectorwarp", workflow)
        self.assertIn("needs: [policy, formula]", workflow)
        self.assertIn("name: reviewed-homebrew-tap", workflow)
        publish = workflow.split("  publish:\n", 1)[1]
        self.assertIn("github.repository == 'mickeyslaven/blah2-VectorWarp'", publish)
        self.assertIn("github.ref == 'refs/heads/main'", publish)
        self.assertIn("github.event_name == 'push' || github.event_name == 'workflow_dispatch'", publish)
        self.assertIn("secrets.HOMEBREW_TAP_DEPLOY_KEY", publish)
        self.assertIn("StrictHostKeyChecking=yes", publish)
        self.assertIn("UserKnownHostsFile=", publish)
        self.assertNotIn("HOMEBREW_TAP_TOKEN", publish)
        policy = workflow.split("  formula:\n", 1)[0]
        self.assertNotIn("HOMEBREW_TAP_DEPLOY_KEY", policy)


if __name__ == "__main__":
    unittest.main()
