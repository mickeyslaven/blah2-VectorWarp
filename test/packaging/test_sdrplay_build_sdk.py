"""Offline SDK-input policy tests. Fixtures are not vendor headers or libraries."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "script/prepare-sdrplay-build-sdk.sh"


class BuildSdkTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="vectorwarp-sdk-test-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.source = self.root / "user-sdk"
        self.source.mkdir()
        (self.source / "sdrplay_api.h").write_text("TEST FIXTURE ONLY\n")
        (self.source / "sdrplay_api_types.h").write_text("TEST FIXTURE ONLY\n")
        (self.source / "unrelated.h").write_text("DO NOT COPY\n")
        (self.source / "runtime.so").write_bytes(b"NOT A REAL LIBRARY")
        self.output = self.root / "prepared"
        self.environment = {key: value for key, value in os.environ.items()
                            if not key.startswith(("VECTORWARP_SDRPLAY_", "BLAH2_SDRPLAY_"))}
        self.environment.update(VECTORWARP_SDRPLAY_BUILD_LICENSE_ACCEPTED="true",
                                BLAH2_SDRPLAY_INCLUDE_DIR=str(self.source),
                                BLAH2_SDRPLAY_LIBRARY=str(self.source / "runtime.so"))

    def run_helper(self, **changes):
        return subprocess.run(["bash", str(SCRIPT), "--output-dir", str(self.output)],
                              env={**self.environment, **changes},
                              text=True, capture_output=True)

    def test_explicit_license_consent_is_required(self):
        result = self.run_helper(VECTORWARP_SDRPLAY_BUILD_LICENSE_ACCEPTED="false")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("explicit acceptance", result.stderr)
        self.assertFalse(self.output.exists())

    def test_external_installed_sdk_stages_only_allowed_files(self):
        for machine, sdk_arch in (("x86_64", "amd64"), ("aarch64", "arm64")):
            with self.subTest(machine=machine):
                self.output = self.root / ("prepared-" + sdk_arch)
                tools = self.root / ("tools-" + sdk_arch)
                tools.mkdir()
                uname = tools / "uname"
                uname.write_text("#!/bin/sh\nprintf '%s\\n' " + machine + "\n")
                uname.chmod(0o755)
                result = self.run_helper(PATH=str(tools) + ":" + self.environment["PATH"])
                self.assertEqual(result.returncode, 0, result.stderr)
                files = {str(file.relative_to(self.output)) for file in self.output.rglob("*") if file.is_file()}
                self.assertEqual(files, {"inc/sdrplay_api.h", "inc/sdrplay_api_types.h",
                                         sdk_arch + "/libsdrplay_api.so.3.15"})
                self.assertEqual(self.output.stat().st_mode & 0o777, 0o700)
                repeat = self.run_helper()
                self.assertNotEqual(repeat.returncode, 0)

    def test_missing_sdk_does_not_use_git_or_download(self):
        result = self.run_helper(BLAH2_SDRPLAY_INCLUDE_DIR=str(self.root / "absent"))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("https://sdrplay.com/hardware-api/", result.stderr)
        self.assertIn("No repository/history fallback", result.stderr)
        self.assertFalse(self.output.exists())

    def test_untrusted_archive_fails_checksum_before_staging(self):
        archive = self.root / "user-downloaded.run"
        archive.write_bytes(b"not the pinned vendor file")
        result = self.run_helper(VECTORWARP_SDRPLAY_SDK_ARCHIVE=str(archive))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("checksum mismatch", result.stderr)
        self.assertFalse(self.output.exists())

    def test_relative_sdk_paths_are_rejected(self):
        for values in ({"VECTORWARP_SDRPLAY_SDK_ARCHIVE": "vendor.run"},
                       {"BLAH2_SDRPLAY_LIBRARY": "runtime.so"}):
            result = self.run_helper(**values)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("absolute", result.stderr)
            self.assertFalse(self.output.exists())

    def test_repository_files_cannot_be_selected_as_sdk(self):
        result = self.run_helper(VECTORWARP_SDRPLAY_SDK_ARCHIVE=str(ROOT / "README.md"))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("outside the source tree", result.stderr)
        self.assertFalse(self.output.exists())

    def test_output_symlink_is_rejected(self):
        target = self.root / "untouched"
        target.mkdir()
        self.output.symlink_to(target, target_is_directory=True)
        result = self.run_helper()
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(list(target.iterdir()), [])

    def test_output_under_source_is_rejected_before_creation(self):
        self.output = ROOT / 'build' / 'forbidden-vendor-sdk-test'
        result = self.run_helper()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('outside the source tree', result.stderr)
        self.assertFalse(self.output.exists())

    def test_existing_output_must_already_be_private(self):
        self.output.mkdir(mode=0o755)
        result = self.run_helper()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('mode 0700', result.stderr)
        self.assertEqual(list(self.output.iterdir()), [])


if __name__ == "__main__":
    unittest.main()
