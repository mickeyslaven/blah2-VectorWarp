#!/usr/bin/env python3
"""Keep release-tag builds on disposable hosted Macs, not the signing Mac."""
from pathlib import Path
import unittest


WORKFLOW = (Path(__file__).resolve().parents[2] /
            ".github/workflows/macos-standalone.yml")


class StandaloneReleaseTriggerTest(unittest.TestCase):
    def test_main_merges_and_version_tags_build_both_hosted_architectures(self):
        source = WORKFLOW.read_text()
        triggers = source.split("permissions:", 1)[0]
        self.assertRegex(triggers, r"(?m)^  push:\n    branches: \[main\]\n    tags: \['v\*'\]$")
        self.assertIn("runner: macos-15\n            arch: arm64", source)
        self.assertIn("runner: macos-15-intel\n            arch: x86_64", source)
        self.assertNotRegex(source, r"(?i)runs-on:.*self.hosted")

    def test_release_transfer_remains_encrypted_and_conditional(self):
        source = WORKFLOW.read_text()
        self.assertIn("if: vars.VECTORWARP_STANDALONE_RECIPIENT_CERT != ''", source)
        self.assertIn("script/transfer-macos-runtime.py encrypt", source)
        self.assertIn("brew list --versions asio cpp-httplib eigen rapidjson vulkan-headers", source)
        self.assertIn("build/standalone-provenance/header-input-versions.txt", source)
        self.assertIn("$(brew --prefix cpp-httplib)/.brew/cpp-httplib.rb", source)
        self.assertIn("build/standalone-provenance/cpp-httplib-formula.rb", source)
        self.assertNotIn("script/sign-macos-standalone.py", source)
        self.assertNotIn("notarytool", source)


if __name__ == "__main__":
    unittest.main()
