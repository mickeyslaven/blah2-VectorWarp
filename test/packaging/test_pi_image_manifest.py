"""Offline tests for Pi 4 Imager metadata generated from a real XZ stream."""

import hashlib
import importlib.util
import json
import lzma
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "packaging/pi/generate-imager-manifest.py"
SPEC = importlib.util.spec_from_file_location("pi_image_manifest", SCRIPT)
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


class PiImageManifestTests(unittest.TestCase):
    def setUp(self):
        self.work = tempfile.TemporaryDirectory()
        self.addCleanup(self.work.cleanup)
        self.root = Path(self.work.name)
        # A compressed test image; no Pi OS bytes or external URLs are fetched.
        self.raw = (b"Pi 4 test partition data\x00" * 70000) + bytes(range(256))
        self.image = self.root / "vectorwarp-1.2.3-pi4.img.xz"
        self.image.write_bytes(lzma.compress(self.raw, format=lzma.FORMAT_XZ))
        self.output = self.root / "vectorwarp.rpi-imager-manifest"
        self.url = "https://downloads.example.test/releases/v1.2.3/" + self.image.name
        self.icon = "https://downloads.example.test/icons/vectorwarp.png"

    def generate(self, **changes):
        args = dict(image=self.image, url=self.url, icon_url=self.icon,
                    version="1.2.3", release_date="2026-09-19", output=self.output)
        args.update(changes)
        return MODULE.generate(**args)

    def test_hashes_sizes_pi4_only_and_systemd(self):
        manifest = self.generate()
        self.assertEqual(json.loads(self.output.read_text()), manifest)
        entry = manifest["os_list"][0]
        self.assertEqual(entry["image_download_size"], self.image.stat().st_size)
        self.assertEqual(entry["image_download_sha256"], hashlib.sha256(self.image.read_bytes()).hexdigest())
        self.assertEqual(entry["extract_size"], len(self.raw))
        self.assertEqual(entry["extract_sha256"], hashlib.sha256(self.raw).hexdigest())
        self.assertEqual(entry["devices"], ["pi4"])
        self.assertEqual(entry["architecture"], "armv8")
        self.assertEqual(entry["init_format"], "systemd")
        self.assertNotIn("capabilities", entry)
        self.assertEqual(manifest["imager"]["devices"], [{
            "name": "Raspberry Pi 4 Model B", "description": "Raspberry Pi 4 Model B",
            "tags": ["pi4"], "matching_type": "exclusive", "architecture": "armv8"}])
        self.assertNotIn("pi5", self.output.read_text())

        # Validate against the upstream schema when a local copy and the
        # optional jsonschema module are present; normal tests stay offline.
        schema_path = os.environ.get("RPI_IMAGER_SCHEMA")
        if schema_path:
            try:
                import jsonschema
            except ImportError:
                self.fail("RPI_IMAGER_SCHEMA requires the jsonschema module")
            else:
                jsonschema.validate(manifest, json.loads(Path(schema_path).read_text()))

    def test_truncated_or_invalid_xz_does_not_write_partial_manifest(self):
        self.output.write_text("prior manifest")
        good = self.image.read_bytes()
        for corrupt in (good[:-10], b"not an XZ stream", good + b"trailing"):
            with self.subTest(corrupt=corrupt[:8]):
                self.image.write_bytes(corrupt)
                with self.assertRaises(ValueError):
                    self.generate()
                self.assertEqual(self.output.read_text(), "prior manifest")
                self.assertEqual(list(self.root.glob(".vectorwarp.rpi-imager-manifest.*")), [])

    def test_credentials_and_mutable_url_forms_rejected(self):
        for url in (
            "https://user:secret@downloads.example.test/releases/v1.2.3/" + self.image.name,
            self.url + "?token=secret",
            "http://downloads.example.test/releases/v1.2.3/" + self.image.name,
            self.url + " ",
            "\thttps://downloads.example.test/releases/v1.2.3/" + self.image.name,
            self.url + "\x7f",
        ):
            with self.subTest(url=url), self.assertRaises(ValueError):
                self.generate(url=url)
            self.assertFalse(self.output.exists())
        with self.assertRaises(ValueError):
            self.generate(icon_url="https://user:secret@downloads.example.test/icon.png")
        with self.assertRaises(ValueError):
            self.generate(icon_url=self.icon + "\n")

    def test_cli_help_describes_metadata_only(self):
        result = subprocess.run([sys.executable, str(SCRIPT), "--help"],
                                check=True, capture_output=True, text=True)
        self.assertIn("does not build, flash, download, or publish", " ".join(result.stdout.split()))


if __name__ == "__main__":
    unittest.main()
