"""Check install-guide commands without installing packages or starting services."""
from pathlib import Path
import re
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[2]
GUIDES = ("docs/INSTALL.md", "docs/SETUP.md", "docs/SDRPLAY_SETUP.md", "docs/PI_GPU_SETUP.md")


class InstallDocumentationTests(unittest.TestCase):
    def test_shell_examples_parse_without_execution(self):
        for relative in GUIDES:
            text = (ROOT / relative).read_text()
            for index, block in enumerate(re.findall(r"```(?:bash|sh)\n(.*?)```", text, re.S)):
                with self.subTest(guide=relative, block=index):
                    result = subprocess.run(["bash", "-n"], input=block,
                                            text=True, capture_output=True, timeout=10)
                    self.assertEqual(result.returncode, 0, result.stderr)

    def test_local_document_links_exist(self):
        for relative in GUIDES:
            document = ROOT / relative
            for target in re.findall(r"\]\(([^)]+)\)", document.read_text()):
                if "://" in target or target.startswith("#"):
                    continue
                with self.subTest(guide=relative, target=target):
                    self.assertTrue((document.parent / target.split("#")[0]).is_file())

    def test_source_instructions_use_supported_native_dependencies(self):
        for relative in ("docs/INSTALL.md", "docs/SETUP.md"):
            text = (ROOT / relative).read_text()
            with self.subTest(guide=relative):
                self.assertNotIn("libhackrf-devel", text)
                self.assertIn("hackrf-devel", text)
                self.assertIn("libusb1-devel", text)
                self.assertIn("libusb-1.0-0-dev", text)
                self.assertIn("python3-apt", text)
                self.assertIn("python3-libdnf5", text)
                self.assertIn("python3-rpm", text)
                self.assertIn("/usr/bin/node --version", text)
                self.assertIn("npm --version", text)
                self.assertIn("--backend", text)
                self.assertIn("--preflight", text)

    def test_default_startup_does_not_enable_boot_capture(self):
        install = (ROOT / "docs/INSTALL.md").read_text()
        self.assertIn("sudo systemctl start vectorwarp-api.service", install)
        self.assertNotIn("enable --now", install)
        self.assertIn("Save for later", install)
        self.assertIn("Save & Restart", install)
        for unit in re.findall(r"\bvectorwarp-[a-z-]+\.service\b", install):
            self.assertTrue((ROOT / "contrib/systemd" / (unit + ".in")).is_file())


if __name__ == "__main__":
    unittest.main()
