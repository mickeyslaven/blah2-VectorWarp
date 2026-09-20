#!/usr/bin/env python3
"""Policy checks for tag-specific reviewed macOS source-input receipts."""
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location(
    "reviewed_release_inputs", ROOT / "script/prepare-macos-release-inputs.py")
inputs = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(inputs)


class ReviewedBuildChangesTest(unittest.TestCase):
    guarded = ["CMakeLists.txt"]

    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        self.checkout = self.root / "checkout"
        self.checkout.mkdir()
        self.git("init")
        self.git("config", "user.email", "review@example.invalid")
        self.git("config", "user.name", "Review Test")
        (self.checkout / "CMakeLists.txt").write_text("project(VectorWarp)\n")
        self.git("add", "CMakeLists.txt")
        self.git("commit", "-m", "candidate")
        self.source_id = self.git("rev-parse", "HEAD")
        self.receipt = self.root / "review.json"

    def tearDown(self):
        self.temporary.cleanup()

    def git(self, *args):
        return subprocess.check_output(["git", "-C", str(self.checkout), *args], text=True).strip()

    def record(self, *, source_id=None, baseline_source_id=None, files=None):
        content = (self.checkout / "CMakeLists.txt").read_bytes()
        return {"schema": 1, "purpose": "first-party-build-only",
                "source_id": source_id or self.source_id,
                "baseline_source_id": baseline_source_id or inputs.BASELINE_SOURCE_ID,
                "files": files if files is not None else {
                    "CMakeLists.txt": hashlib.sha256(content).hexdigest()}}

    def write_receipt(self, record, path=None, mode=0o600):
        path = path or self.receipt
        path.write_text(json.dumps(record, sort_keys=True) + "\n")
        path.chmod(mode)
        return path

    def verify(self, record=None, path=None):
        return inputs.reviewed_build_changes(
            self.checkout, self.source_id, self.guarded, path or self.write_receipt(record or self.record()))

    def test_accepts_exact_candidate_cmake_review(self):
        review = self.verify()
        self.assertEqual(review["record"], self.record())
        self.assertEqual(review["raw"], self.receipt.read_bytes())
        self.assertEqual(review["metadata"]["source_id"], self.source_id)
        self.assertEqual(review["metadata"]["baseline_source_id"], inputs.BASELINE_SOURCE_ID)
        self.assertEqual(review["metadata"]["archive_member"],
                         "VectorWarp-corresponding-source/build-source-review.json")

    def test_rejects_missing_extra_and_wrong_file_entries(self):
        with self.subTest("missing"):
            with self.assertRaisesRegex(ValueError, "exactly match"):
                self.verify(self.record(files={}))
        with self.subTest("extra"):
            record = self.record()
            record["files"]["cmake/Extra.cmake"] = "a" * 64
            with self.assertRaisesRegex(ValueError, "exactly match"):
                self.verify(record)
        with self.subTest("wrong_hash"):
            record = self.record(files={"CMakeLists.txt": "a" * 64})
            with self.assertRaisesRegex(ValueError, "differs from candidate Git blob"):
                self.verify(record)

    def test_rejects_wrong_candidate_or_baseline(self):
        with self.subTest("candidate"):
            with self.assertRaisesRegex(ValueError, "does not bind"):
                self.verify(self.record(source_id="a" * 40))
        with self.subTest("baseline"):
            with self.assertRaisesRegex(ValueError, "does not bind"):
                self.verify(self.record(baseline_source_id="b" * 40))

    def test_rejects_unsafe_paths_and_non_cmake_guarded_changes(self):
        record = self.record(files={"vendor/unreviewed.c": "a" * 64})
        self.write_receipt(record)
        with self.assertRaisesRegex(ValueError, "cannot approve non-CMake"):
            inputs.reviewed_build_changes(self.checkout, self.source_id, ["vendor/unreviewed.c"], self.receipt)
        record = self.record(files={"cmake/../outside.cmake": "a" * 64})
        with self.assertRaisesRegex(ValueError, "exactly match|unsafe path"):
            self.verify(record)

    def test_rejects_nonowner_mode_and_symlink(self):
        self.write_receipt(self.record(), mode=0o644)
        with self.assertRaisesRegex(ValueError, "owner-only regular"):
            self.verify(path=self.receipt)
        target = self.root / "target.json"
        self.write_receipt(self.record(), target)
        link = self.root / "link.json"
        os.symlink(target.name, link)
        with self.assertRaisesRegex(ValueError, "owner-only regular"):
            self.verify(path=link)


class BaselineNoticeValidationTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name) / "notices"
        self.root.mkdir()
        self.original_license_sha = inputs.CPP_LICENSE_SHA256
        inputs.CPP_LICENSE_SHA256 = hashlib.sha256(b"reviewed license\n").hexdigest()
        self.write_baseline()

    def tearDown(self):
        inputs.CPP_LICENSE_SHA256 = self.original_license_sha
        self.temporary.cleanup()

    def write_baseline(self):
        license_path = self.root / "embedded/cpp-httplib/LICENSE"
        license_path.parent.mkdir(parents=True, exist_ok=True)
        license_path.write_bytes(b"reviewed license\n")
        index = {"source_id": inputs.BASELINE_SOURCE_ID,
                 "input_notices": [{"file": "embedded/cpp-httplib/LICENSE",
                                     "sha256": inputs.CPP_LICENSE_SHA256}]}
        (self.root / "INDEX.json").write_text(json.dumps(index, sort_keys=True))
        files = {path.relative_to(self.root).as_posix(): inputs.sha256(path)
                 for path in self.root.rglob("*") if path.is_file()}
        (self.root / "notices.json").write_text(json.dumps(
            {"schema": 1, "source_id": inputs.BASELINE_SOURCE_ID, "files": files}, sort_keys=True))

    def test_validates_complete_baseline_notice_identity(self):
        self.assertIsNone(inputs.validated_baseline_notices(self.root))

    def test_rejects_changed_notice_content_or_index(self):
        (self.root / "embedded/cpp-httplib/LICENSE").write_bytes(b"changed\n")
        with self.assertRaisesRegex(ValueError, "content differs"):
            inputs.validated_baseline_notices(self.root)
        self.write_baseline()
        index_path = self.root / "INDEX.json"
        index = json.loads(index_path.read_text())
        index["source_id"] = "a" * 40
        index_path.write_text(json.dumps(index, sort_keys=True))
        manifest_path = self.root / "notices.json"
        manifest = json.loads(manifest_path.read_text())
        manifest["files"] = {path.relative_to(self.root).as_posix(): inputs.sha256(path)
                             for path in self.root.rglob("*")
                             if path.is_file() and path.name != "notices.json"}
        manifest_path.write_text(json.dumps(manifest, sort_keys=True))
        with self.assertRaisesRegex(ValueError, "notice index"):
            inputs.validated_baseline_notices(self.root)


if __name__ == "__main__":
    unittest.main()
