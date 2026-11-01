#!/usr/bin/env python3
"""Policy checks for tag-specific reviewed macOS source-input receipts."""
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from unittest import mock

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

    def test_rejects_unsafe_guarded_paths(self):
        for path in ("cmake/../outside.cmake", "vendor/unreviewed.c",
                     "third_party/unreviewed.c"):
            with self.subTest(path=path):
                record = self.record(files={path: "a" * 64})
                self.write_receipt(record)
                with self.assertRaisesRegex(ValueError, "unsafe guarded build path"):
                    inputs.reviewed_build_changes(
                        self.checkout, self.source_id, [path], self.receipt)

    def test_accepts_exact_review_of_other_guarded_build_inputs(self):
        (self.checkout / "cmake").mkdir()
        (self.checkout / "cmake/ReceiverCohort.h.in").write_text("#define RECEIVER_COHORT 1\n")
        (self.checkout / "script").mkdir()
        (self.checkout / "script/build-macos.sh").write_text("#!/bin/sh\n")
        self.commit("review additional native build inputs")
        guarded = ["CMakeLists.txt", "cmake/ReceiverCohort.h.in", "script/build-macos.sh"]
        files = {}
        for path in guarded:
            content = subprocess.check_output(
                ["git", "-C", str(self.checkout), "show", self.source_id + ":" + path])
            files[path] = hashlib.sha256(content).hexdigest()
        record = self.record(source_id=self.source_id, files=files)
        review = inputs.reviewed_build_changes(
            self.checkout, self.source_id, guarded, self.write_receipt(record))
        self.assertEqual(review["record"], record)

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

    def test_rejects_nonancestor_and_unreviewed_sensitive_delta(self):
        self.git("checkout", "-b", "sibling", self.baseline_id)
        (self.checkout / "CMakeLists.txt").write_text("project(VectorWarpReviewed)\n")
        self.commit("sibling equivalent review")
        with self.assertRaisesRegex(ValueError, "not an ancestor"):
            self.verify()
        self.git("checkout", self.primary_branch)
        (self.checkout / "script").mkdir()
        (self.checkout / "script/build-macos.sh").write_text("#!/bin/sh\n")
        self.commit("change guarded script")
        with self.assertRaisesRegex(ValueError, "guarded paths differ"):
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


class FormulaSourceClaimTransitionTest(unittest.TestCase):
    @staticmethod
    def inventory(*components):
        return {"schema": 1, "components": list(components)}

    @staticmethod
    def component(name="example", version="1.0", source="a" * 64,
                  recipe="b" * 64, notice="c" * 64):
        return {
            "formula": name,
            "version": version,
            "input": f"lib/lib{name}.dylib",
            "formula_recipe": {"sha256": recipe, "source_sha256": [source]},
            "notice_files": [{"path": "LICENSE", "sha256": notice}],
            "copied_files": [{"path": f".brew/{name}.rb", "sha256": recipe}],
        }

    def test_accepts_dependency_removal(self):
        retained = self.component()
        removed = self.component(name="removed")
        self.assertIsNone(inputs.verify_source_claim_transition(
            self.inventory(retained), self.inventory(retained, removed), "arm64"))

    def test_rejects_added_dependency(self):
        retained = self.component()
        added = self.component(name="added")
        with self.assertRaisesRegex(ValueError, "new source review"):
            inputs.verify_source_claim_transition(
                self.inventory(retained, added), self.inventory(retained), "arm64")

    def test_rejects_changed_recipe_source_or_notice(self):
        prior = self.component()
        for field, value in (("recipe", "d" * 64),
                             ("source", "e" * 64),
                             ("notice", "f" * 64)):
            with self.subTest(field=field), self.assertRaisesRegex(
                    ValueError, "new source review"):
                inputs.verify_source_claim_transition(
                    self.inventory(self.component(**{field: value})),
                    self.inventory(prior), "arm64")

    def reviewed_uhd(self):
        review = inputs.VECTORWARP_UHD_REVIEW
        notices = [{"path": path, "sha256": digest} for path, digest in review["notices"]]
        copies = [{"path": f".brew/{review['formula']}.rb",
                   "sha256": review["formula_sha256"]}, *notices]
        return {
            "formula": review["formula"], "version": review["version"],
            "input": review["input"],
            "formula_recipe": {"sha256": review["formula_sha256"],
                               "source_sha256": [item[2] for item in review["sources"]]},
            "notice_files": notices, "copied_files": copies,
        }

    def test_accepts_exact_reviewed_uhd_replacement(self):
        legacy = self.component(name="uhd", version="4.10.0.0")
        self.assertIsNone(inputs.verify_source_claim_transition(
            self.inventory(self.reviewed_uhd()), self.inventory(legacy), "arm64"))

    def test_rejects_tampered_uhd_or_mpmd_runtime_dependencies(self):
        legacy = self.component(name="uhd", version="4.10.0.0")
        changed = self.reviewed_uhd()
        changed["formula_recipe"]["sha256"] = "0" * 64
        with self.assertRaisesRegex(ValueError, "new source review"):
            inputs.verify_source_claim_transition(
                self.inventory(changed), self.inventory(legacy), "arm64")
        with self.assertRaisesRegex(ValueError, "MPMD dependency stack"):
            inputs.verify_source_claim_transition(
                self.inventory(self.reviewed_uhd(), self.component(name="grpc")),
                self.inventory(legacy), "arm64")


class DependencyTransitionReceiptTest(unittest.TestCase):
    def test_binds_exact_inventories_closures_and_claim_changes(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            inventories, priors, closures, headers, formulas = {}, {}, {}, {}, {}
            changes = {}
            for arch in inputs.package.ARCHES:
                prior = FormulaSourceClaimTransitionTest.inventory(
                    FormulaSourceClaimTransitionTest.component())
                candidate = FormulaSourceClaimTransitionTest.inventory(
                    FormulaSourceClaimTransitionTest.component(),
                    FormulaSourceClaimTransitionTest.component(name="added-" + arch))
                priors[arch] = root / f"prior-{arch}.json"
                inventories[arch] = root / f"inventory-{arch}.json"
                closures[arch] = root / f"closure-{arch}.tar.gz"
                headers[arch] = root / f"headers-{arch}.txt"
                formulas[arch] = root / f"cpp-{arch}.rb"
                priors[arch].write_text(json.dumps(prior))
                inventories[arch].write_text(json.dumps(candidate))
                closures[arch].write_bytes(("closure-" + arch).encode())
                headers[arch].write_text("asio 1.38.2\ncpp-httplib 0.63.0\nrapidjson 1.1.0\n")
                formulas[arch].write_text("formula fixture")
                changes[arch] = inputs.source_claim_changes(candidate, prior, arch)
            source_id, run_id = "d" * 40, "123"
            record = {"schema": 1, "purpose": inputs.DEPENDENCY_REVIEW_PURPOSE,
                "baseline_source_id": inputs.BASELINE_SOURCE_ID, "source_id": source_id,
                "run_id": run_id,
                "artifacts": {a: {"inventory_sha256": inputs.sha256(inventories[a]),
                                   "closure_sha256": inputs.sha256(closures[a]),
                                   "header_versions_sha256": inputs.sha256(headers[a]),
                                   "cpp_formula_sha256": inputs.sha256(formulas[a])}
                              for a in inputs.package.ARCHES},
                "changes": changes,
                "headers": {a: inputs.read_header_versions(headers[a])
                            for a in inputs.package.ARCHES}}
            receipt = root / "review.json"
            receipt.write_text(json.dumps(record, sort_keys=True))
            receipt.chmod(0o600)
            reviewed = inputs.reviewed_dependency_transition(
                receipt, source_id, run_id, inventories, closures, priors, headers, formulas)
            self.assertEqual(reviewed["record"], record)
            closures["arm64"].write_bytes(b"tampered")
            with self.assertRaisesRegex(ValueError, "exact candidate inputs"):
                inputs.reviewed_dependency_transition(
                    receipt, source_id, run_id, inventories, closures, priors, headers, formulas)

    def test_reviewed_headers_must_be_present_in_verified_closure(self):
        headers = {"asio": "1.38.2", "cpp-httplib": "0.63.0"}
        formula = {"id": "cpp-httplib|0.63.0", "usage": ["compile-header"],
                   "recipe": {"sha256": "0" * 64}}
        closure = {"formula_inputs": {"formulas": [formula]}}
        with tempfile.TemporaryDirectory() as directory:
            cpp = Path(directory) / "cpp.rb"
            cpp.write_text("formula fixture")
            with self.assertRaisesRegex(ValueError, "reviewed header input"):
                inputs.verify_reviewed_headers_in_closure(headers, cpp, closure, "arm64")

    def test_artifact_mutation_rejects_before_archival(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            values = {}
            for arch in inputs.package.ARCHES:
                for kind in ("inventory", "closure", "headers", "cpp"):
                    path = root / f"{arch}-{kind}"
                    path.write_text(f"{arch}-{kind}")
                    values[arch, kind] = path
            receipt_path = root / "dependency-source-review.json"
            receipt_path.write_text("reviewed")
            receipt_path.chmod(0o600)
            review = {"raw": b"reviewed", "record": {"artifacts": {
                arch: {"inventory_sha256": inputs.sha256(values[arch, "inventory"]),
                       "closure_sha256": inputs.sha256(values[arch, "closure"]),
                       "header_versions_sha256": inputs.sha256(values[arch, "headers"]),
                       "cpp_formula_sha256": inputs.sha256(values[arch, "cpp"])}
                for arch in inputs.package.ARCHES}}}
            args = mock.Mock(output=root)
            for arch in inputs.package.ARCHES:
                setattr(args, f"{arch}_inventory", values[arch, "inventory"])
                setattr(args, f"{arch}_header_versions", values[arch, "headers"])
                setattr(args, f"{arch}_cpp_formula", values[arch, "cpp"])
            public = root / "public-dependency-source"
            public.mkdir()
            persisted = public / "dependency-source-review.json"
            persisted.write_bytes(review["raw"])
            persisted.chmod(0o600)
            closures = {arch: values[arch, "closure"] for arch in inputs.package.ARCHES}
            inputs.verify_dependency_review_artifacts(args, closures, review)
            values["arm64", "inventory"].write_text("mutated")
            with self.assertRaisesRegex(ValueError, "changed before archival"):
                inputs.verify_dependency_review_artifacts(args, closures, review)


class VectorWarpUhdFormulaTest(unittest.TestCase):
    def test_formula_is_exact_reviewed_b200_build_without_mpmd(self):
        formula = ROOT / "packaging/homebrew/vectorwarp-uhd.rb"
        text = formula.read_text()
        self.assertEqual(inputs.sha256(formula),
                         inputs.VECTORWARP_UHD_REVIEW["formula_sha256"])
        self.assertIn("-DENABLE_B200=ON", text)
        self.assertIn("-DENABLE_MPMD=OFF", text)
        self.assertNotRegex(text, r'depends_on "(?:grpc|protobuf|abseil|c-ares|re2)"')


class ReviewedNodeRuntimeTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        (self.root / "packaging").mkdir()
        shutil.copyfile(ROOT / "packaging/node-runtime.env",
                        self.root / "packaging/node-runtime.env")
        self.staged_hashes = inputs.NODE_REVIEW["staged_binary_sha256"]
        inputs.NODE_REVIEW["staged_binary_sha256"] = {}
        self.runtimes = {}
        for arch in inputs.package.ARCHES:
            runtime = self.root / arch
            (runtime / "bin").mkdir(parents=True)
            (runtime / "bin/node").write_bytes((arch + " node").encode())
            inputs.NODE_REVIEW["staged_binary_sha256"][arch] = inputs.sha256(
                runtime / "bin/node")
            (runtime / "standalone.json").write_text(json.dumps({
                "node_version": inputs.NODE_REVIEW["version"],
                "dependencies": [{
                    "file": "node",
                    "sha256": inputs.NODE_REVIEW["binary_sha256"][arch],
                }],
            }))
            self.runtimes[arch] = runtime

    def tearDown(self):
        inputs.NODE_REVIEW["staged_binary_sha256"] = self.staged_hashes
        self.temporary.cleanup()

    def test_accepts_exact_pinned_node_runtime(self):
        self.assertIsNone(inputs.verify_node_runtimes(self.root, self.runtimes))

    def test_rejects_changed_node_binary_or_source_pin(self):
        (self.runtimes["arm64"] / "bin/node").write_bytes(b"tampered node")
        with self.assertRaisesRegex(ValueError, "Node provenance"):
            inputs.verify_node_runtimes(self.root, self.runtimes)
        (self.runtimes["arm64"] / "bin/node").write_bytes(b"arm64 node")
        value = json.loads((self.runtimes["arm64"] / "standalone.json").read_text())
        value["dependencies"][0]["sha256"] = "0" * 64
        (self.runtimes["arm64"] / "standalone.json").write_text(json.dumps(value))
        with self.assertRaisesRegex(ValueError, "Node provenance"):
            inputs.verify_node_runtimes(self.root, self.runtimes)
        shutil.copyfile(ROOT / "packaging/node-runtime.env",
                        self.root / "packaging/node-runtime.env")
        with (self.root / "packaging/node-runtime.env").open("a") as stream:
            stream.write("UNREVIEWED=1\n")
        with self.assertRaisesRegex(ValueError, "Node runtime or source inputs"):
            inputs.node_environment(self.root)


class ReviewedNpmLockTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.candidate = (ROOT / "api/package-lock.json").read_bytes()
        cls.baseline = inputs.reconstructed_baseline_npm_lock(cls.candidate)

    def test_accepts_only_reviewed_proxy_addr_transition(self):
        self.assertIsNone(inputs.reviewed_npm_lock_delta(
            self.baseline, self.candidate))

    def test_reconstructs_exact_reviewed_baseline_without_git_history(self):
        self.assertEqual(hashlib.sha256(self.baseline).hexdigest(),
                         inputs.BASELINE_NPM_LOCK_SHA256)

    def test_rejects_unreviewed_lock_change_even_with_matching_outer_hash(self):
        value = json.loads(self.candidate)
        value["packages"]["node_modules/proxy-addr"]["funding"]["url"] = \
            "https://example.invalid/unreviewed"
        mutated = json.dumps(value, separators=(",", ":")).encode()
        with mock.patch.object(inputs, "REVIEWED_NPM_LOCK_SHA256",
                               hashlib.sha256(mutated).hexdigest()):
            with self.assertRaisesRegex(ValueError, "proxy-addr lock entry"):
                inputs.reviewed_npm_lock_delta(self.baseline, mutated)

    def test_rejects_an_additional_package_change(self):
        value = json.loads(self.candidate)
        value["packages"]["node_modules/express"]["version"] = "0.0.0"
        mutated = json.dumps(value, separators=(",", ":")).encode()
        with mock.patch.object(inputs, "REVIEWED_NPM_LOCK_SHA256",
                               hashlib.sha256(mutated).hexdigest()):
            with self.assertRaisesRegex(ValueError, "unreviewed change"):
                inputs.reviewed_npm_lock_delta(self.baseline, mutated)


class ReboundNpmNoticeTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        self.baseline = self.root / "baseline"
        self.output = self.root / "output"
        self.node_license = self.root / "node-LICENSE"
        self.node_license.write_bytes(b"reviewed node license\n")
        self.uhd_notice_review = inputs.VECTORWARP_UHD_REVIEW["notices"]
        self.uhd_notices = {}
        reviewed_notices = []
        for index, (relative, _) in enumerate(self.uhd_notice_review):
            path = self.root / f"uhd-notice-{index}"
            path.write_bytes((relative + "\n").encode())
            self.uhd_notices[relative] = path
            reviewed_notices.append((relative, inputs.sha256(path)))
        inputs.VECTORWARP_UHD_REVIEW["notices"] = tuple(reviewed_notices)
        self.baseline.mkdir()
        index = {
            "source_id": inputs.BASELINE_SOURCE_ID,
            "api_packages": [{
                "name": "proxy-addr", "version": "2.0.7", "license": "MIT",
                "notice_files_in_each_runtime": [
                    "api/node_modules/proxy-addr/LICENSE"],
            }],
            "input_notices": [{
                "file": "embedded/cpp-httplib/LICENSE",
                "sha256": inputs.CPP_LICENSE_SHA256,
                "origin": "reviewed arm64 source",
            }],
        }
        (self.baseline / "INDEX.json").write_text(json.dumps(index))
        (self.baseline / "notices.json").write_text(json.dumps(
            {"schema": 1, "source_id": inputs.BASELINE_SOURCE_ID, "files": {}}))
        self.runtimes = {}
        for arch in inputs.package.ARCHES:
            runtime = self.root / arch
            package_dir = runtime / "api/node_modules/proxy-addr"
            package_dir.mkdir(parents=True)
            (package_dir / "package.json").write_text(json.dumps(
                {"name": "proxy-addr", "version": "2.0.8", "license": "MIT"}))
            (package_dir / "LICENSE").write_bytes(b"reviewed proxy license\n")
            (runtime / "standalone.json").write_text("{}")
            self.runtimes[arch] = runtime
        self.license_sha = inputs.PROXY_ADDR_REVIEW["license_sha256"]
        self.node_license_sha = inputs.NODE_REVIEW["license_sha256"]
        inputs.PROXY_ADDR_REVIEW["license_sha256"] = hashlib.sha256(
            b"reviewed proxy license\n").hexdigest()
        inputs.NODE_REVIEW["license_sha256"] = inputs.sha256(self.node_license)

    def tearDown(self):
        inputs.PROXY_ADDR_REVIEW["license_sha256"] = self.license_sha
        inputs.NODE_REVIEW["license_sha256"] = self.node_license_sha
        inputs.VECTORWARP_UHD_REVIEW["notices"] = self.uhd_notice_review
        self.temporary.cleanup()

    def test_rebinds_exact_runtime_notice_version(self):
        with mock.patch.object(inputs, "validated_baseline_notices"), \
                mock.patch.object(inputs.package, "validated_notices"):
            inputs.rebind_notices(
                self.baseline, self.output, "a" * 40, self.runtimes, self.node_license,
                self.uhd_notices)
        index = json.loads((self.output / "INDEX.json").read_text())
        record = next(item for item in index["api_packages"]
                      if item["name"] == "proxy-addr")
        self.assertEqual(record["version"], "2.0.8")
        node = next(item for item in index["input_notices"]
                    if item["file"].startswith("embedded/node-"))
        self.assertEqual(node["sha256"], inputs.NODE_REVIEW["license_sha256"])

    def test_rejects_runtime_notice_mismatch(self):
        (self.runtimes["arm64"] / "api/node_modules/proxy-addr/LICENSE").write_text(
            "different\n")
        with mock.patch.object(inputs, "validated_baseline_notices"), \
                mock.patch.object(inputs.package, "validated_notices"):
            with self.assertRaisesRegex(ValueError, "runtime identity or notice"):
                inputs.rebind_notices(
                    self.baseline, self.output, "a" * 40, self.runtimes, self.node_license,
                    self.uhd_notices)


if __name__ == "__main__":
    unittest.main()