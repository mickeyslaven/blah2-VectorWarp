#!/usr/bin/env python3
import hashlib
import importlib.util
import json
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("provenance", ROOT / "script" / "collect-macos-standalone-provenance.py")
provenance = importlib.util.module_from_spec(spec)
spec.loader.exec_module(provenance)


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


class StandaloneProvenanceTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.cellar = self.root / "Cellar"
        self.keg = self.cellar / "fixture" / "1.0"
        self.source = self.keg / "lib" / "fixture.dylib"
        self.source.parent.mkdir(parents=True)
        self.source.write_bytes(b"fixture-native-input")
        recipe = self.keg / ".brew" / "fixture.rb"
        recipe.parent.mkdir()
        recipe.write_text('url "https://example.invalid/fixture.tar.gz"\nsha256 "' + "a" * 64 + '"\n')
        (self.keg / "INSTALL_RECEIPT.json").write_text('{"private_path":"/Users/example"}')
        (self.keg / "sbom.spdx.json").write_text('{"packages":[]}')
        (self.keg / "COPYING.RUNTIME").write_text("fixture notice")
        self.runtime = self.root / "runtime"
        self.runtime.mkdir()

    def tearDown(self):
        self.temp.cleanup()

    def manifest(self, dependencies):
        (self.runtime / "standalone.json").write_text(json.dumps({
            "schema": 1, "source_id": "a" * 40, "source_revision": "a" * 40,
            "arch": "arm64", "minimum_os": "15.0", "dependencies": dependencies,
        }))

    def dependency(self, **changes):
        value = {"formula": "fixture", "version": "1.0", "file": "lib/fixture.dylib", "sha256": digest(self.source)}
        value.update(changes)
        return value

    def collect(self):
        return provenance.collect(self.runtime, self.cellar, self.root / "notices")

    def test_valid_input_binds_manifest_and_sanitizes_records(self):
        self.manifest([self.dependency()])
        result = self.collect()
        self.assertEqual(result["status"], "inventory-complete")
        self.assertEqual(result["arch"], "arm64")
        self.assertEqual(result["bindings"][0]["status"], "matched")
        self.assertEqual(result["components"][0]["input_sha256"], digest(self.source))
        self.assertTrue((self.root / "notices" / "fixture-1.0" / "COPYING.RUNTIME").is_file())
        serialized = json.dumps(result)
        self.assertNotIn(str(self.root), serialized)
        self.assertNotIn("private_path", serialized)

    def test_hash_mismatch_is_an_explicit_nonblocking_gap(self):
        self.manifest([self.dependency(sha256="0" * 64)])
        result = self.collect()
        self.assertEqual(result["status"], "inventory-complete-with-gaps")
        self.assertEqual(result["bindings"][0]["status"], "hash-mismatch")
        self.assertIn("staged dependency hash", result["gaps"][0]["reason"])

    def test_missing_receipt_and_escape_are_explicit_gaps(self):
        (self.keg / "INSTALL_RECEIPT.json").unlink()
        self.manifest([self.dependency(), self.dependency(file="../outside", sha256="0" * 64)])
        result = self.collect()
        self.assertEqual(result["status"], "inventory-complete-with-gaps")
        reasons = "\n".join(item["reason"] for item in result["gaps"])
        self.assertIn("missing INSTALL_RECEIPT.json", reasons)
        self.assertIn("dependency metadata is invalid", reasons)

    def test_hostile_manifest_path_is_not_reflected_in_public_gap(self):
        hostile = "/Users/private-user/secret-runtime.dylib"
        self.manifest([self.dependency(file=hostile)])
        result = self.collect()
        self.assertEqual(result["gaps"], [{"index": 0, "reason": "dependency metadata is invalid"}])
        self.assertNotIn(hostile, json.dumps(result))


if __name__ == "__main__":
    unittest.main()
