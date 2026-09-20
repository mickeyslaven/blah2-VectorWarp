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
        self.original_baseline = inputs.BASELINE_SOURCE_ID
        (self.checkout / "CMakeLists.txt").write_text("project(VectorWarp)\n")
        self.git("add", "CMakeLists.txt")
        self.git("commit", "-m", "baseline")
        self.primary_branch = self.git("branch", "--show-current")
        self.baseline_id = self.git("rev-parse", "HEAD")
        inputs.BASELINE_SOURCE_ID = self.baseline_id
        (self.checkout / "CMakeLists.txt").write_text("project(VectorWarpReviewed)\n")
        self.git("add", "CMakeLists.txt")
        self.git("commit", "-m", "reviewed build change")
        self.reviewed_source_id = self.git("rev-parse", "HEAD")
        self.source_id = self.reviewed_source_id
        self.receipt = self.root / "review.json"

    def tearDown(self):
        inputs.BASELINE_SOURCE_ID = self.original_baseline
        self.temporary.cleanup()

    def git(self, *args):
        return subprocess.check_output(["git", "-C", str(self.checkout), *args], text=True).strip()

    def record(self, *, source_id=None, baseline_source_id=None, files=None):
        content = subprocess.check_output(["git", "-C", str(self.checkout), "show",
                                           self.reviewed_source_id + ":CMakeLists.txt"])
        return {"schema": 1, "purpose": "first-party-build-only",
                "source_id": source_id or self.reviewed_source_id,
                "baseline_source_id": baseline_source_id or inputs.BASELINE_SOURCE_ID,
                "files": files if files is not None else {
                    "CMakeLists.txt": hashlib.sha256(content).hexdigest()}}

    def commit(self, message):
        self.git("add", "-A")
        self.git("commit", "-m", message)
        self.source_id = self.git("rev-parse", "HEAD")

    def write_receipt(self, record, path=None, mode=0o600):
        path = path or self.receipt
        path.write_text(json.dumps(record, sort_keys=True) + "\n")
        path.chmod(mode)
        return path

    def verify(self, record=None, path=None):
        changed = self.git("diff", "--name-only", self.baseline_id, self.source_id).splitlines()
        guarded = [name for name in changed if any(name == prefix or name.startswith(prefix)
                                                   for prefix in inputs.SENSITIVE_PATHS)]
        return inputs.reviewed_build_changes(
            self.checkout, self.source_id, guarded, path or self.write_receipt(record or self.record()))

    def test_accepts_exact_candidate_cmake_review(self):
        review = self.verify()
        self.assertEqual(review["record"], self.record())
        self.assertEqual(review["raw"], self.receipt.read_bytes())
        self.assertEqual(review["metadata"]["source_id"], self.source_id)
        self.assertNotIn("reviewed_source_id", review["metadata"])
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
            with self.assertRaisesRegex(ValueError, "differs from (reviewed|candidate) Git blob"):
                self.verify(record)

    def test_rejects_wrong_candidate_or_baseline(self):
        with self.subTest("candidate"):
            with self.assertRaisesRegex(ValueError, "reviewed source|does not bind"):
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
        with self.assertRaisesRegex(ValueError, "exactly match|unsafe path|guarded paths differ"):
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

    def test_accepts_descendant_with_identical_reviewed_build_inputs(self):
        (self.checkout / "README.txt").write_text("later source-only change\n")
        self.commit("later non-build change")
        review = self.verify()
        self.assertEqual(review["metadata"]["source_id"], self.source_id)
        self.assertEqual(review["metadata"]["reviewed_source_id"], self.reviewed_source_id)
        self.assertEqual(review["raw"], self.receipt.read_bytes())

    def test_rejects_new_or_changed_guarded_candidate_path(self):
        (self.checkout / "cmake").mkdir()
        (self.checkout / "cmake/New.cmake").write_text("set(NEW_INPUT 1)\n")
        self.commit("new CMake input")
        with self.assertRaisesRegex(ValueError, "guarded paths differ"):
            self.verify()

    def test_rejects_changed_reviewed_candidate_blob(self):
        (self.checkout / "CMakeLists.txt").write_text("project(ChangedAgain)\n")
        self.commit("change reviewed CMake input")
        with self.assertRaisesRegex(ValueError, "candidate Git blob"):
            self.verify()

    def test_rejects_nonancestor_and_sensitive_non_cmake_delta(self):
        self.git("checkout", "-b", "sibling", self.baseline_id)
        (self.checkout / "CMakeLists.txt").write_text("project(VectorWarpReviewed)\n")
        self.commit("sibling equivalent review")
        with self.assertRaisesRegex(ValueError, "not an ancestor"):
            self.verify()
        self.git("checkout", self.primary_branch)
        (self.checkout / "script").mkdir()
        (self.checkout / "script/build-macos.sh").write_text("#!/bin/sh\n")
        self.commit("change guarded script")
        with self.assertRaisesRegex(ValueError, "cannot approve non-CMake"):
            self.verify()

    def test_rejects_missing_reviewed_blob(self):
        self.git("rm", "CMakeLists.txt")
        self.commit("remove reviewed CMake input")
        with self.assertRaisesRegex(ValueError, "absent from candidate"):
            self.verify()

    def test_rejects_tag_object_and_incomplete_guarded_argument(self):
        self.git("tag", "-a", "review-tag", "-m", "not a commit identity")
        tag_id = self.git("rev-parse", "review-tag")
        with self.assertRaisesRegex(ValueError, "identify a commit directly"):
            self.verify(self.record(source_id=tag_id))
        (self.checkout / "cmake").mkdir()
        (self.checkout / "cmake/Hidden.cmake").write_text("set(NEW_INPUT 1)\n")
        self.commit("additional guarded file")
        self.write_receipt(self.record())
        with self.assertRaisesRegex(ValueError, "guarded paths differ"):
            inputs.reviewed_build_changes(self.checkout, self.source_id, ["CMakeLists.txt"], self.receipt)


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
