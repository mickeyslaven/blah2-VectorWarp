"""Bounded build-input extraction tests; never execute the SDRplay installer."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "script/prepare-sdrplay-build-sdk.sh"


class BuildSdkTests(unittest.TestCase):
    def test_explicit_license_consent_is_required(self):
        with tempfile.TemporaryDirectory(prefix="vectorwarp-sdk-consent-") as temporary:
            destination = Path(temporary) / "sdk"
            environment = {**os.environ, "VECTORWARP_SDRPLAY_BUILD_LICENSE_ACCEPTED": "false"}
            result = subprocess.run(["bash", str(SCRIPT), "--output-dir", str(destination)],
                                    env=environment, text=True, capture_output=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("explicit acceptance", result.stderr)
            self.assertFalse(destination.exists())

    @unittest.skipUnless(os.environ.get("VECTORWARP_SDRPLAY_BUILD_LICENSE_ACCEPTED") == "true",
                         "actual SDK extraction requires the maintainer's build-license approval")
    def test_both_architectures_extract_only_approved_build_inputs(self):
        for machine, sdk_arch in (("x86_64", "amd64"), ("aarch64", "arm64")):
            with self.subTest(machine=machine), tempfile.TemporaryDirectory(prefix="vectorwarp-sdk-") as temporary:
                root = Path(temporary)
                tools = root / "tools"
                tools.mkdir()
                uname = tools / "uname"
                uname.write_text(f"#!/bin/sh\nprintf '{machine}\\n'\n")
                uname.chmod(0o755)
                environment = {**os.environ, "PATH": f"{tools}:{os.environ['PATH']}",
                               "VECTORWARP_SDRPLAY_BUILD_LICENSE_ACCEPTED": "true"}
                destination = root / "sdk"
                command = ["bash", str(SCRIPT), "--output-dir", str(destination)]
                result = subprocess.run(command, env=environment, text=True, capture_output=True)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertTrue((destination / "inc/sdrplay_api.h").is_file())
                self.assertTrue((destination / sdk_arch / "libsdrplay_api.so.3.15").is_file())
                for path in destination.rglob("*"):
                    if path.is_file():
                        relative = path.relative_to(destination)
                        self.assertTrue((relative.parts[0] == "inc" and path.suffix == ".h") or
                                        str(relative) == f"{sdk_arch}/libsdrplay_api.so.3.15", relative)
                repeat = subprocess.run(command, env=environment, text=True, capture_output=True)
                self.assertNotEqual(repeat.returncode, 0, "Existing SDK must not be overwritten")


if __name__ == "__main__":
    unittest.main()
