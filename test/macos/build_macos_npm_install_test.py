#!/usr/bin/env python3
"""Regression coverage for npm 11 installation in an absolute macOS artifact."""
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "script" / "build-macos.sh"
API = ROOT / "api"


class BuildMacosNpmInstallTest(unittest.TestCase):
    def test_npm11_installs_staged_api_at_an_absolute_artifact_path(self):
        npm_version = subprocess.check_output(["npm", "--version"], text=True).strip()
        self.assertEqual(npm_version.split(".", 1)[0], "11", npm_version)

        script = SCRIPT.read_text()
        self.assertIn('cd "$OUTPUT_DIR/api"', script)
        self.assertNotIn('--prefix "$OUTPUT_DIR/api"', script)

        with tempfile.TemporaryDirectory(prefix="vectorwarp npm11 artifact ") as temporary:
            artifact_api = Path(temporary).resolve() / "artifact" / "api"
            shutil.copytree(API, artifact_api,
                            ignore=shutil.ignore_patterns("node_modules", "*.test.js"))
            subprocess.run(["npm", "ci", "--omit=dev", "--ignore-scripts",
                            "--no-audit", "--no-fund"], cwd=artifact_api,
                           check=True, text=True, capture_output=True, timeout=120)
            package = json.loads((artifact_api / "package.json").read_text())
            lockfile = json.loads((artifact_api / "package-lock.json").read_text())
            self.assertEqual(lockfile["packages"][""]["name"], package["name"])
            self.assertTrue((artifact_api / "node_modules" / "express").is_dir())


if __name__ == "__main__":
    unittest.main()
