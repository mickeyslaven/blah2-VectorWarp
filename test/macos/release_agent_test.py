#!/usr/bin/env python3
"""Offline release-agent policy and source-reuse checks."""
import importlib.util
import io
import json
from pathlib import Path
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]


def load(name, file):
    spec = importlib.util.spec_from_file_location(name, ROOT / "script" / file)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


agent = load("macos_release_agent", "macos-release-agent.py")
inputs = load("macos_release_inputs", "prepare-macos-release-inputs.py")


class AgentPolicyTest(unittest.TestCase):
    def test_release_selection_requires_new_stable_version(self):
        releases = [{"tagName": "v0.1.9", "isPrerelease": False},
                    {"tagName": "v0.2.0", "isPrerelease": False},
                    {"tagName": "v0.2.1", "isPrerelease": True},
                    {"tagName": "main", "isPrerelease": False}]
        self.assertEqual(agent.choose_release(releases)["tagName"], "v0.2.0")
        self.assertIsNone(agent.choose_release(releases[:1]))

    def test_only_successful_push_at_exact_commit_supplies_runtime(self):
        sha = "a" * 40
        runs = [{"headSha": sha, "event": "pull_request", "conclusion": "success", "databaseId": 9},
                {"headSha": "b" * 40, "event": "push", "conclusion": "success", "databaseId": 8},
                {"headSha": sha, "event": "push", "conclusion": "failure", "databaseId": 7},
                {"headSha": sha, "event": "push", "conclusion": "success", "databaseId": 6}]
        self.assertEqual(agent.choose_run(runs, sha)["databaseId"], 6)
        self.assertIsNone(agent.choose_run(runs, "c" * 40))

    def linux_assets(self, number, *, bookworm=False):
        names = [f"vectorwarp_{number}-1_{distro}_{arch}.deb"
                 for distro in ("ubuntu22.04", "ubuntu24.04", "ubuntu26.04", "debian13")
                 for arch in ("amd64", "arm64")]
        if bookworm:
            names.append(f"vectorwarp_{number}-1_debian12_arm64.deb")
        names += [f"vectorwarp-{number}-1.fc44.{arch}.rpm" for arch in ("x86_64", "aarch64")]
        names += ["SHA256SUMS", "SHA256SUMS.asc", "package-manifest.json"]
        return {"assets": [{"name": name} for name in names]}

    def test_linux_matrix_requires_bookworm_arm64_for_new_stable_versions(self):
        for number in ("0.1.10", "0.1.11", "0.2.0"):
            with self.subTest(version=number):
                tag = "v" + number
                complete = self.linux_assets(number, bookworm=True)
                agent.required_linux_assets(complete, tag)
                with self.assertRaisesRegex(ValueError, "complete Linux draft"):
                    agent.required_linux_assets(self.linux_assets(number), tag)
                with self.assertRaisesRegex(ValueError, "complete Linux draft"):
                    agent.required_linux_assets({"assets": complete["assets"][:-1]}, tag)

    def test_legacy_v019_keeps_its_ten_package_contract(self):
        agent.required_linux_assets(self.linux_assets("0.1.9"), "v0.1.9")

    def test_state_is_idempotent_and_bound_to_commit(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "state.json"
            self.assertEqual(agent.read_state(path, "a" * 40)["stage"], "new")
            agent.save_state(path, {"commit": "a" * 40, "stage": "published"})
            self.assertEqual(agent.read_state(path, "a" * 40)["stage"], "published")
            with self.assertRaisesRegex(ValueError, "another commit"):
                agent.read_state(path, "b" * 40)

    def test_publication_accepts_generic_heading_but_requires_current_manifest(self):
        pkg = "vectorwarp-0.1.11-macos-universal.pkg"
        source = "vectorwarp-0.1.11-macos-corresponding-source.tar.gz"
        page = f"<h1>Install VectorWarp</h1><a>{pkg}</a><a>{source}</a>".encode()
        manifest = {"version": "0.1.11", "macos_package": {
            "version": "0.1.11", "filename": pkg, "source_archive": {"filename": source}}}
        with mock.patch.object(agent.urllib.request, "urlopen", side_effect=[
                io.BytesIO(page), io.BytesIO(json.dumps(manifest).encode())]):
            self.assertTrue(agent.verify_public("v0.1.11"))
        manifest["version"] = "0.1.9"
        with mock.patch.object(agent.urllib.request, "urlopen", side_effect=[
                io.BytesIO(page), io.BytesIO(json.dumps(manifest).encode())]):
            self.assertFalse(agent.verify_public("v0.1.11"))

    def test_public_release_receipt_carries_source_review(self):
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            package, archive = work / "package.pkg", work / "source.tar.gz"
            package.write_bytes(b"signed package fixture")
            archive.write_bytes(b"reviewed source fixture")
            review = {"sha256": "a" * 64, "source_id": "b" * 40}
            assets = agent.release_files({"team_id": "DJGHPX8T7R"},
                {"tag": "v0.1.11", "commit": "b" * 40}, work, package, archive,
                "00000000-0000-0000-0000-000000000000", review)
            self.assertEqual(json.loads(assets[-1].read_text())["build_source_review"], review)

    def test_cached_inputs_repeat_gates_using_trusted_tooling(self):
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            output = work / "release-inputs"
            output.mkdir()
            archive = output / "vectorwarp-0.1.11-macos-corresponding-source.tar.gz"
            archive.write_bytes(b"fixture archive")
            (output / "notices").mkdir()
            (output / "notices/notices.json").write_text("{}")
            runtimes = {arch: work / arch for arch in agent.ARCHES}
            for runtime in runtimes.values():
                runtime.mkdir()
                (runtime / "standalone.json").write_text("{}")
            candidate = {"tag": "v0.1.11", "commit": "b" * 40}
            review = {"metadata": {"sha256": "a" * 64, "source_id": candidate["commit"],
                                   "reviewed_source_id": "c" * 40}}
            receipt = {"schema": 1, "version": "0.1.11", "source_id": candidate["commit"],
                       "source_archive": archive.name, "source_archive_size": archive.stat().st_size,
                       "source_archive_sha256": agent.digest(archive),
                       "notices_sha256": agent.digest(output / "notices/notices.json"),
                       "build_source_review": review["metadata"]}
            (output / "receipt.json").write_text(json.dumps(receipt))
            settings = {key: work / key for key in ("checkout", "baseline_notices",
                "baseline_source_archive", "baseline_arm64_inventory", "baseline_x86_64_inventory")}
            helper = mock.Mock()
            helper.verify_inputs.return_value = (runtimes, candidate["commit"], review)
            with mock.patch.object(agent.importlib.util, "spec_from_file_location") as spec, \
                    mock.patch.object(agent.importlib.util, "module_from_spec", return_value=helper):
                result = agent.prepare_inputs(settings, candidate, work, work / "tag-source", runtimes, runtimes)
                self.assertEqual(result[2], review["metadata"])
                self.assertEqual(spec.call_args.args[1], settings["checkout"] / "script/prepare-macos-release-inputs.py")
                helper.prepare.assert_not_called()
                helper.package.validated_notices.assert_called_once()
                helper.verify_inputs.side_effect = ValueError("new source review required")
                with self.assertRaisesRegex(ValueError, "new source review required"):
                    agent.prepare_inputs(settings, candidate, work, work / "tag-source", runtimes, runtimes)
                helper.verify_inputs.side_effect = None
                for bad_review in (None, {"sha256": "c" * 64},
                        {**review["metadata"], "reviewed_source_id": "d" * 40},
                        {"sha256": "a" * 64, "source_id": candidate["commit"]}):
                    receipt["build_source_review"] = bad_review
                    (output / "receipt.json").write_text(json.dumps(receipt))
                    with self.assertRaisesRegex(ValueError, "release input receipt differs"):
                        agent.prepare_inputs(settings, candidate, work, work / "tag-source", runtimes, runtimes)


class SourceReuseTest(unittest.TestCase):
    def component(self, recipe="a" * 64):
        return {"formula": "fftw", "version": "3.3.11", "input": "lib/libfftw3.dylib",
                "formula_recipe": {"sha256": recipe, "source_sha256": ["b" * 64]},
                "notice_files": [{"path": "COPYING", "sha256": "c" * 64}]}

    def test_recipe_change_cannot_reuse_prior_source(self):
        prior = {"schema": 1, "components": [self.component()]}
        current = {"schema": 1, "components": [self.component("d" * 64)]}
        self.assertNotEqual(inputs.source_claims(prior), inputs.source_claims(current))
        with self.assertRaisesRegex(ValueError, "incomplete"):
            inputs.source_claims({"schema": 1, "components": [self.component("")]})

    def test_header_version_change_fails_closed(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "headers.txt"
            path.write_text("asio 1.38.2\ncpp-httplib 0.54.1\nrapidjson 1.1.0\n"
                            "vulkan-headers 1.4.357.0\neigen 5.0.1\n")
            self.assertEqual(inputs.parse_header_versions(path, "arm64")["cpp-httplib"], "0.54.1")
            path.write_text(path.read_text().replace("0.54.1", "0.53.1"))
            self.assertEqual(inputs.parse_header_versions(path, "x86_64")["cpp-httplib"], "0.53.1")
            path.write_text(path.read_text().replace("0.53.1", "0.56.0"))
            with self.assertRaisesRegex(ValueError, "new source review"):
                inputs.parse_header_versions(path, "arm64")

    def test_intel_formula_pins_reviewed_source(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "formula.rb"
            version, checksum = inputs.CPP_HEADER["x86_64"]
            formula = ("class CppHttplib < Formula\n"
                       f'  url "https://github.com/yhirose/cpp-httplib/archive/refs/tags/v{version}.tar.gz"\n'
                       f'  sha256 "{checksum}"\n'
                       '  license "MIT"\nend\n')
            path.write_text(formula)
            self.assertTrue(inputs.verify_cpp_formula(path, "x86_64").endswith("v0.53.1.tar.gz"))
            path.write_text(formula.replace(checksum, "a" * 64))
            with self.assertRaisesRegex(ValueError, "formula source"):
                inputs.verify_cpp_formula(path, "x86_64")


if __name__ == "__main__":
    unittest.main()
