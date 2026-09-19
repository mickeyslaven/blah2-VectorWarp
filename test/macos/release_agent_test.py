#!/usr/bin/env python3
"""Offline release-agent policy and source-reuse checks."""
import importlib.util
from pathlib import Path
import tempfile
import unittest

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

    def test_incomplete_linux_matrix_refused(self):
        number = "0.2.0"
        names = [f"vectorwarp_{number}-1_{distro}_{arch}.deb"
                 for distro in ("ubuntu22.04", "ubuntu24.04", "ubuntu26.04", "debian13")
                 for arch in ("amd64", "arm64")]
        names += [f"vectorwarp-{number}-1.fc44.{arch}.rpm" for arch in ("x86_64", "aarch64")]
        assets = [{"name": name} for name in names]
        assets += [{"name": name} for name in ("SHA256SUMS", "SHA256SUMS.asc", "package-manifest.json")]
        agent.required_linux_assets({"assets": assets}, "v0.2.0")
        with self.assertRaisesRegex(ValueError, "complete Linux draft"):
            agent.required_linux_assets({"assets": assets[:-1]}, "v0.2.0")

    def test_state_is_idempotent_and_bound_to_commit(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "state.json"
            self.assertEqual(agent.read_state(path, "a" * 40)["stage"], "new")
            agent.save_state(path, {"commit": "a" * 40, "stage": "published"})
            self.assertEqual(agent.read_state(path, "a" * 40)["stage"], "published")
            with self.assertRaisesRegex(ValueError, "another commit"):
                agent.read_state(path, "b" * 40)


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
            self.assertEqual(inputs.parse_header_versions(path)["cpp-httplib"], "0.54.1")
            path.write_text(path.read_text().replace("0.54.1", "0.56.0"))
            with self.assertRaisesRegex(ValueError, "new source review"):
                inputs.parse_header_versions(path)


if __name__ == "__main__":
    unittest.main()
