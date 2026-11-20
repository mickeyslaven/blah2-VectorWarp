#!/usr/bin/env python3
"""Current-input approval, byte identity and notice coverage; no signing/network."""
import copy
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import tarfile
import tempfile
import types
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("reviewed_release_inputs",
    ROOT / "script/prepare-macos-release-inputs.py")
inputs = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(inputs)
SOURCE = "a" * 40


class CurrentInputReview(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.approval = self.root / "review.json"
        self.files = {}
        for name in ("runtime-arm64.json", "runtime-x86_64.json", "inventory.json",
                     "headers.txt", "formula.rb", "api-package-lock.json", "node-runtime.env", "notices.json"):
            path = self.root / name
            path.write_text(name)
            self.files[name] = path
        self.record = {"schema": 1, "purpose": inputs.REVIEW_PURPOSE, "source_id": SOURCE,
                       "run_id": "123", "version": "1.2.3",
                       "files": {name: inputs.file_record(path) for name, path in self.files.items()},
                       "sources": {}}
        self.write_review()

    def write_review(self):
        self.approval.write_text(json.dumps(self.record))
        self.approval.chmod(0o600)

    def verify(self, **kwargs):
        values = dict(path=self.approval, source_id=SOURCE, run_id="123", version="1.2.3",
                      files=self.files, sources=None)
        values.update(kwargs)
        return inputs.reviewed_inputs(**values)

    def test_exact_current_inputs_have_one_review_without_historical_baseline(self):
        value = self.verify()
        self.assertEqual(value["record"], self.record)
        self.assertEqual(value["raw"], self.approval.read_bytes())
        self.assertEqual(value["metadata"]["sha256"], inputs.sha256(self.approval))

    def test_each_input_identity_is_required_and_immutable(self):
        for name, path in self.files.items():
            original = path.read_bytes()
            path.write_bytes(original + b"changed")
            with self.subTest(name=name), self.assertRaisesRegex(ValueError, "bytes differ"):
                self.verify()
            path.write_bytes(original)
        for key, value in (("source_id", "b" * 40), ("run_id", "124"), ("version", "1.2.4")):
            with self.subTest(key=key), self.assertRaises(ValueError):
                self.verify(**{key: value})
        for name in self.files:
            changed = copy.deepcopy(self.record)
            del changed["files"][name]
            self.approval.write_text(json.dumps(changed))
            with self.subTest(name=name), self.assertRaises(ValueError): self.verify()
        self.write_review()
        self.files["extra"] = self.approval
        with self.assertRaises(ValueError): self.verify()

    def test_owner_only_receipt_and_duplicate_key_rules(self):
        for mode in (0o644, 0o640, 0o666):
            self.approval.chmod(mode)
            with self.subTest(mode=mode), self.assertRaises(ValueError): self.verify()
        self.approval.chmod(0o600)
        link = self.root / "linked-review"; link.symlink_to(self.approval)
        with self.assertRaises(ValueError): self.verify(path=link)
        for raw in (b'{"schema":1,"schema":1}', b"[]", b"\xff", b"{"):
            with self.subTest(raw=raw), self.assertRaises(ValueError): inputs.review_json(raw)
        self.approval.write_bytes(b" " * (inputs.REVIEW_MAX_BYTES + 1))
        with self.assertRaises(ValueError): self.verify()

    def test_public_sources_are_exact_regular_offline_inputs(self):
        sources = self.root / "sources"; sources.mkdir()
        source = sources / "dependency-1.tar.gz"; source.write_bytes(b"reviewed upstream")
        self.record["sources"] = {source.name: inputs.file_record(source)}
        self.write_review()
        self.verify(sources=sources)
        source.write_bytes(b"unreviewed upstream")
        with self.assertRaises(ValueError): self.verify(sources=sources)
        source.unlink(); source.symlink_to(self.files["headers.txt"])
        with self.assertRaises(ValueError): self.verify(sources=sources)

    def test_architecture_guard_forbids_transition_fallback_and_network_fetch(self):
        source = (ROOT / "script/prepare-macos-release-inputs.py").read_text()
        for retired in ("BASELINE_SOURCE_ID", "PROXY_ADDR_REVIEW", "reconstructed_baseline",
                        "urlopen", "git fetch", "reviewed_build_changes"):
            self.assertNotIn(retired, source)
        self.assertEqual(source.count("def verify_inputs("), 1)


class CurrentNoticeCoverage(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.notices = self.root / "notices"; self.notices.mkdir()
        self.native = self.notices / "native-LICENSE"; self.native.write_bytes(b"native license")
        self.runtimes, self.inventories = {}, {}
        for arch in inputs.package.ARCHES:
            runtime = self.root / arch
            package = runtime / "api/node_modules/demo"; package.mkdir(parents=True)
            (package / "package.json").write_text(json.dumps({"name": "demo", "version": "2", "license": "MIT"}))
            (package / "LICENSE").write_text("API license")
            (runtime / "api/package-lock.json").write_text(json.dumps({"packages": {
                "": {}, "node_modules/demo": {"version": "2"},
                "node_modules/dev-only": {"version": "1", "dev": True}}}))
            (runtime / "standalone.json").write_text(json.dumps({"source_id": SOURCE}))
            self.runtimes[arch] = runtime
            self.inventories[arch] = {"components": [{"notice_files": [
                {"path": "LICENSE", "sha256": inputs.sha256(self.native)}]}]}
        self.index = {"source_id": SOURCE, "api_packages": [{"name": "demo", "version": "2", "license": "MIT",
            "notice_files_in_each_runtime": ["api/node_modules/demo/LICENSE"]}],
            "input_notices": [{"file": "native-LICENSE", "sha256": inputs.sha256(self.native)}]}

    def verify(self):
        (self.notices / "INDEX.json").write_text(json.dumps(self.index))
        inputs.verify_notice_coverage(self.notices, self.runtimes, self.inventories)

    def test_current_complete_coverage_for_both_architectures(self):
        self.verify()

    def test_missing_or_changed_package_identity_notice_and_native_bytes_fail(self):
        original = copy.deepcopy(self.index)
        for change in (lambda: self.index.update(api_packages=[]),
                       lambda: self.index["api_packages"][0].update(version="old"),
                       lambda: self.index["api_packages"][0].update(license="wrong"),
                       lambda: self.index["api_packages"][0].update(notice_files_in_each_runtime=["../outside"]),
                       lambda: self.index.update(input_notices=[]),
                       lambda: self.index.update(source_id="b" * 40)):
            self.index = copy.deepcopy(original); change()
            with self.assertRaises(ValueError): self.verify()
        self.index = original
        self.native.write_text("changed")
        with self.assertRaises(ValueError): self.verify()

    def test_each_runtime_must_ship_its_notice(self):
        (self.runtimes["x86_64"] / "api/node_modules/demo/LICENSE").unlink()
        with self.assertRaises(ValueError): self.verify()

    def test_current_pair_approval_cannot_bypass_candidate_provenance_or_source_pins(self):
        self.verify()
        checkout = self.root / "checkout"; (checkout / "api").mkdir(parents=True)
        (checkout / "packaging").mkdir()
        (checkout / "api/package-lock.json").write_bytes((self.runtimes["arm64"] / "api/package-lock.json").read_bytes())
        (checkout / "packaging/node-runtime.env").write_text("NODE_VERSION=99.1.2\nNODE_SOURCE_SHA256=" + "a" * 64 + "\n")
        (self.notices / "notices.json").write_text("{}")
        approval = self.root / "review.json"
        args = types.SimpleNamespace(checkout=checkout, notices=self.notices, input_manifest=approval,
                                     sources=None, version="1.2.3", run_id="123", **self.runtimes)
        for arch, runtime in self.runtimes.items():
            (runtime / "standalone.json").write_text(json.dumps({"source_id": SOURCE, "node_version": "99.1.2"}))
            inventory = self.root / (arch + "-inventory.json")
            inventory.write_text(json.dumps({**self.inventories[arch], "source_id": SOURCE, "arch": arch,
                                              "runtime_manifest_sha256": inputs.sha256(runtime / "standalone.json")}))
            setattr(args, arch + "_inventory", inventory)
            for suffix, content in (("closure", "reviewed closure"), ("cpp_formula", "reviewed formula"),
                    ("header_versions", "\n".join(name + " 1" for name in sorted(inputs.HEADER_INPUT_NAMES)))):
                path = self.root / (arch + "-" + suffix); path.write_text(content)
                setattr(args, arch + "_" + suffix, path)
        def approve():
            record = {"schema": 1, "purpose": inputs.REVIEW_PURPOSE, "source_id": SOURCE,
                      "version": args.version, "run_id": args.run_id, "sources": {},
                      "files": {name: inputs.file_record(path) for name, path in inputs.input_files(args, self.runtimes).items()}}
            approval.write_text(json.dumps(record)); approval.chmod(0o600)
        closure = {"files": {"node-source.tar.gz": {"sha256": "a" * 64}}, "formula_inputs": {"formulas": [
            {"id": name + "|1", "formula": name, "version": "1", "usage": ["compile-header"],
             "recipe": {"sha256": inputs.sha256(args.arm64_cpp_formula)}} for name in inputs.HEADER_INPUT_NAMES]}}
        approve()
        with mock.patch.object(inputs.package, "audit_runtime", side_effect=lambda path, arch: inputs.read_json(path / "standalone.json")), \
                mock.patch.object(inputs.package, "validated_notices"), \
                mock.patch.object(inputs.subprocess, "check_output", side_effect=lambda command, **unused: SOURCE if "rev-parse" in command else ""), \
                mock.patch.object(inputs, "verify_source_closure", return_value=closure):
            self.assertEqual(inputs.verify_inputs(args)[1], SOURCE)
            inventory = inputs.read_json(args.x86_64_inventory)
            args.x86_64_inventory.write_text(json.dumps({**inventory, "arch": "arm64"})); approve()
            with self.assertRaisesRegex(ValueError, "candidate runtime"): inputs.verify_inputs(args)
            args.x86_64_inventory.write_text(json.dumps(inventory)); approve()
            closure["files"]["node-source.tar.gz"]["sha256"] = "b" * 64
            with self.assertRaisesRegex(ValueError, "Node version or corresponding source"): inputs.verify_inputs(args)
            closure["files"]["node-source.tar.gz"]["sha256"] = "a" * 64
            with mock.patch.object(inputs.subprocess, "check_output", side_effect=[SOURCE, " M source.cpp"]):
                with self.assertRaisesRegex(ValueError, "not clean"): inputs.verify_inputs(args)
            approval.unlink()
            with self.assertRaises((ValueError, OSError)): inputs.verify_inputs(args)


class CurrentSourceArchive(unittest.TestCase):
    def test_node_source_and_runtime_follow_current_reviewed_pin_not_a_fixed_version(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "node.env"
            path.write_text("NODE_VERSION=99.2.3\nNODE_SOURCE_SHA256=" + "a" * 64 + "\n")
            manifest = {"node_version": "99.2.3"}
            closure = {"files": {"node-source.tar.gz": {"sha256": "a" * 64}}}
            inputs.verify_node_source(path, manifest, closure)
            for changed_manifest, changed_closure in (({}, closure), (manifest, {}),
                    ({"node_version": "24.22.1"}, closure)):
                with self.assertRaises(ValueError):
                    inputs.verify_node_source(path, changed_manifest, changed_closure)
            path.write_text(path.read_text() + "NODE_VERSION=99.2.3\n")
            with self.assertRaisesRegex(ValueError, "duplicate"):
                inputs.verify_node_source(path, manifest, closure)

    def test_archive_binds_current_manifest_inputs_and_is_independently_readable(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary); output = root / "output"; output.mkdir()
            source = root / "inventory.json"; source.write_bytes(b"inventory")
            review_file = root / "approved.json"
            record = {"schema": 1, "purpose": inputs.REVIEW_PURPOSE, "source_id": SOURCE,
                      "run_id": "123", "version": "1.2.3",
                      "files": {"inventory.json": inputs.file_record(source)}, "sources": {}}
            raw = json.dumps(record).encode()
            review_file.write_bytes(raw); review_file.chmod(0o600)
            (output / "input-review.json").write_bytes(raw)
            args = types.SimpleNamespace(checkout=root, input_manifest=review_file, run_id="123",
                                         version="1.2.3", sources=None)
            review = inputs.reviewed_inputs(review_file, SOURCE, "123", "1.2.3",
                                            {"inventory.json": source}, None)
            def git_archive(*unused, **kwargs): kwargs["stdout"].write(b"first-party source")
            with mock.patch.object(inputs, "input_files", return_value={"inventory.json": source}), \
                    mock.patch.object(inputs.subprocess, "run", side_effect=git_archive):
                archive = inputs.source_archive(args, SOURCE, {}, output, review)
            with tarfile.open(archive) as value:
                members = {item.name: value.extractfile(item).read() for item in value}
            prefix = "VectorWarp-corresponding-source/"
            self.assertEqual(members[prefix + "input-review.json"], raw)
            self.assertEqual(members[prefix + "inventory.json"], b"inventory")
            manifest = json.loads(members[prefix + "MANIFEST.json"])
            for name, item in manifest["files"].items():
                self.assertEqual(hashlib.sha256(members[prefix + name]).hexdigest(), item["sha256"])
            self.assertFalse((output / "VectorWarp-current-source.tar.gz").exists())
            self.assertNotIn("baseline_source_id", manifest)

    def test_archive_size_bound(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "source.tar.gz"
            with path.open("wb") as stream: stream.truncate(inputs.GITHUB_RELEASE_ASSET_LIMIT)
            with self.assertRaisesRegex(ValueError, "GitHub requires release assets"):
                inputs.validate_release_asset_size(path)

    def test_preparation_failure_cleans_only_new_output(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            notices = root / "notices"; notices.mkdir()
            (notices / "notices.json").write_text("{}")
            review_path = root / "review.json"; review_path.write_text("{}")
            args = types.SimpleNamespace(output=root / "output", checkout=root / "checkout",
                                         notices=notices, input_manifest=review_path, sources=None)
            review = {"raw": b"{}", "record": {"files": {
                "notices-manifest.json": {"sha256": inputs.sha256(notices / "notices.json")}}}}
            with mock.patch.object(inputs, "verify_inputs", return_value=({}, SOURCE, review)), \
                    mock.patch.object(inputs, "input_files", return_value={}), \
                    mock.patch.object(inputs.package, "validated_notices"), \
                    mock.patch.object(inputs, "source_archive", side_effect=ValueError("archive failed")):
                with self.assertRaisesRegex(ValueError, "archive failed"): inputs.prepare(args)
            self.assertFalse(args.output.exists())
            self.assertEqual(review_path.read_text(), "{}")
            self.assertEqual((notices / "notices.json").read_text(), "{}")


if __name__ == "__main__":
    unittest.main()
