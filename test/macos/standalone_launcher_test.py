#!/usr/bin/env python3
"""Offline architecture/argument tests for the universal app-bundle launcher."""
import os
from pathlib import Path
import platform
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "packaging/macos/launcher.c"


def runtime_arch():
    machine = platform.machine().lower()
    if machine in {"arm64", "aarch64"}:
        return "arm64"
    if machine in {"x86_64", "amd64"}:
        return "x86_64"
    raise unittest.SkipTest(f"unsupported local test architecture: {machine}")


class StandaloneLauncherTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="vectorwarp standalone ")
        self.root = Path(self.temporary.name)
        self.app = self.root / "VectorWarp Test.app"
        self.launcher = self.app / "Contents/MacOS/VectorWarp"
        self.launcher.parent.mkdir(parents=True)
        self.record = self.root / "arguments.txt"
        self.compile_launcher()

    def tearDown(self):
        self.temporary.cleanup()

    def compile_launcher(self):
        result = subprocess.run([
            "xcrun", "--sdk", "macosx", "clang", "-Wall", "-Wextra", "-Werror",
            "-arch", "arm64", "-arch", "x86_64", "-mmacosx-version-min=15.0",
            "-o", str(self.launcher), str(SOURCE)], text=True, capture_output=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def install_runtime(self, architecture=runtime_arch()):
        script = self.app / "Contents/Resources/runtime" / architecture / "script/vectorwarp-standalone"
        script.parent.mkdir(parents=True)
        script.write_text("#!/bin/sh\n: \"${VECTORWARP_LAUNCHER_RECORD:?}\"\n: > \"$VECTORWARP_LAUNCHER_RECORD\"\nfor argument do printf '%s\\n' \"$argument\" >> \"$VECTORWARP_LAUNCHER_RECORD\"; done\n")
        script.chmod(0o755)
        return script

    def run_launcher(self, executable=None, *arguments):
        env = {"VECTORWARP_LAUNCHER_RECORD": str(self.record)}
        return subprocess.run([str(executable or self.launcher), *arguments], text=True,
                              capture_output=True, env=env, timeout=10)

    def recorded_arguments(self):
        if not self.record.exists():
            return []
        return self.record.read_text().splitlines()

    def test_universal_binary_contains_both_required_architectures(self):
        result = subprocess.run(["lipo", "-archs", str(self.launcher)], text=True, capture_output=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(set(result.stdout.split()), {"arm64", "x86_64"})

    def test_local_arch_dispatch_and_literal_arguments(self):
        self.install_runtime()
        arguments = ("open", "literal space", "$HOME", ";not-a-command", "--flag=value")
        result = self.run_launcher(None, *arguments)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.recorded_arguments(), list(arguments))

    def test_symlink_launch_resolves_the_real_bundle_and_ignores_only_finder_psn(self):
        self.install_runtime()
        alias = self.root / "VectorWarp alias"
        alias.symlink_to(self.launcher)
        result = self.run_launcher(alias, "-psn_0_12345", "start", "-psn_not-a-command", "--literal")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.recorded_arguments(), ["start", "--literal"])

    def test_no_arguments_reaches_runtime_for_its_default_open_flow(self):
        self.install_runtime()
        result = self.run_launcher()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.recorded_arguments(), [])

    def test_missing_local_arch_payload_fails_without_fallback(self):
        other = "x86_64" if runtime_arch() == "arm64" else "arm64"
        self.install_runtime(other)
        result = self.run_launcher(None, "start")
        self.assertEqual(result.returncode, 127)
        self.assertIn(f"required {runtime_arch()} runtime payload is unavailable", result.stderr)
        self.assertFalse(self.record.exists())


if __name__ == "__main__":
    unittest.main()
