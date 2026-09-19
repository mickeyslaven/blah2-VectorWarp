#!/usr/bin/env python3
"""Offline CMS transfer tests using a minimal audited Mach-O runtime."""
import importlib.util
import io
import json
from pathlib import Path
import platform
import subprocess
import tarfile
import tempfile
import unittest
from types import SimpleNamespace

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("transfer_macos_runtime",
                                               ROOT / "script/transfer-macos-runtime.py")
transfer = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(transfer)


def local_arch():
    machine = platform.machine().lower()
    if machine in {"arm64", "aarch64"}:
        return "arm64"
    if machine in {"x86_64", "amd64"}:
        return "x86_64"
    raise unittest.SkipTest(f"unsupported architecture: {machine}")


class StandaloneTransferTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="vectorwarp transfer ")
        self.base = Path(self.temporary.name)
        self.index = 0
        self.key = self.base / "recipient-key.pem"
        self.certificate = self.base / "recipient-cert.pem"
        result = subprocess.run(["/usr/bin/openssl", "req", "-x509", "-newkey", "rsa:2048",
                                 "-nodes", "-keyout", self.key, "-out", self.certificate,
                                 "-subj", "/CN=VectorWarp transfer test", "-days", "1"],
                                text=True, capture_output=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.key.chmod(0o600)

    def tearDown(self):
        self.temporary.cleanup()

    def runtime(self):
        self.index += 1
        root = self.base / f"runtime-{self.index}"
        binary = root / "bin/fixture"
        binary.parent.mkdir(parents=True)
        source = self.base / "fixture.c"
        source.write_text("int main(void) { return 0; }\n")
        result = subprocess.run(["xcrun", "--sdk", "macosx", "clang", "-Wall", "-Wextra", "-Werror",
                                 "-arch", local_arch(), "-mmacosx-version-min=15.0", source, "-o", binary],
                                text=True, capture_output=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        transfer.packager.run("/usr/bin/codesign", "--force", "--sign", "-", binary)
        tool = root / "node_modules/js-yaml/bin/tool"
        tool.parent.mkdir(parents=True)
        tool.write_text("#!/bin/sh\nexit 0\n")
        tool.chmod(0o755)
        npm_bin = root / "node_modules/.bin"
        npm_bin.mkdir()
        (npm_bin / "tool").symlink_to("../js-yaml/bin/tool")
        count = transfer.packager.native_audit(root, local_arch(), "15.0")
        metadata = {"schema": 1, "arch": local_arch(), "source_id": "a" * 40,
                    "minimum_os": "15.0", "native_files": count,
                    "files": transfer.packager.files_manifest(root)}
        (root / "standalone.json").write_text(json.dumps(metadata, sort_keys=True))
        return root

    def encrypt(self, runtime):
        cipher = self.base / f"runtime-{self.index}.cms"
        metadata = self.base / f"runtime-{self.index}.cms.json"
        transfer.encrypt(SimpleNamespace(runtime=runtime, certificate=self.certificate,
                                         output=cipher, metadata=metadata))
        return cipher, metadata

    def decrypt_args(self, cipher, metadata, output, **expected):
        return SimpleNamespace(input=cipher, metadata=metadata, key=self.key,
                               certificate=self.certificate, output=output,
                               expected_source_id=expected.get("source_id", "a" * 40),
                               expected_arch=expected.get("arch", local_arch()),
                               expected_run_id=expected.get("run_id", "local"))

    def test_cms_round_trip_audits_and_restores_runtime(self):
        runtime = self.runtime()
        cipher, metadata_path = self.encrypt(runtime)
        metadata = json.loads(metadata_path.read_text())
        self.assertNotIn(str(self.base), metadata_path.read_text())
        self.assertEqual(metadata["ciphertext_sha256"], transfer.sha256(cipher))
        restored = self.base / "restored"
        transfer.decrypt(self.decrypt_args(cipher, metadata_path, restored))
        self.assertEqual(transfer.packager.audit_runtime(restored, local_arch())["source_id"], "a" * 40)
        self.assertEqual((restored / "node_modules/.bin/tool").readlink(),
                         Path("../js-yaml/bin/tool"))
        result = subprocess.run([str(restored / "bin/fixture")], text=True, capture_output=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_tampered_ciphertext_and_existing_target_are_rejected(self):
        cipher, metadata = self.encrypt(self.runtime())
        cipher.write_bytes(cipher.read_bytes() + b"tamper")
        with self.assertRaisesRegex(ValueError, "ciphertext digest"):
            transfer.decrypt(self.decrypt_args(cipher, metadata, self.base / "tampered"))
        cipher, metadata = self.encrypt(self.runtime())
        existing = self.base / "existing"
        existing.mkdir()
        with self.assertRaisesRegex(ValueError, "refusing to replace"):
            transfer.decrypt(self.decrypt_args(cipher, metadata, existing))
        self.key.chmod(0o644)
        with self.assertRaisesRegex(ValueError, "owner-only"):
            transfer.decrypt(self.decrypt_args(cipher, metadata, self.base / "exposed-key"))

    def test_decrypt_requires_trusted_source_architecture_and_run_identity(self):
        cipher, metadata = self.encrypt(self.runtime())
        for name, value, message in (("source_id", "b" * 40, "source identity"),
                                     ("arch", "x86_64" if local_arch() == "arm64" else "arm64", "architecture"),
                                     ("run_id", "12345", "GitHub run")):
            with self.subTest(name=name), self.assertRaisesRegex(ValueError, message):
                transfer.decrypt(self.decrypt_args(cipher, metadata, self.base / f"wrong-{name}",
                                                    **{name: value}))

    def test_safe_extract_rejects_traversal_before_writing_outside_destination(self):
        archive = self.base / "unsafe.tar"
        payload = b"blocked"
        with tarfile.open(archive, "w") as stream:
            member = tarfile.TarInfo("../outside")
            member.size = len(payload)
            stream.addfile(member, io.BytesIO(payload))
        destination = self.base / "destination"
        with self.assertRaisesRegex(ValueError, "archive member escapes runtime"):
            transfer.safe_extract(archive, destination)
        self.assertFalse((self.base / "outside").exists())

    def test_safe_extract_rejects_canonical_duplicates_and_escaping_symlink_chains(self):
        duplicate = self.base / "duplicate.tar"
        with tarfile.open(duplicate, "w") as stream:
            for name in ("a/../same", "same"):
                member = tarfile.TarInfo(name)
                member.size = 1
                stream.addfile(member, io.BytesIO(b"x"))
        with self.assertRaisesRegex(ValueError, "duplicate archive member"):
            transfer.safe_extract(duplicate, self.base / "duplicate-output")

        escape = self.base / "chain.tar"
        with tarfile.open(escape, "w") as stream:
            nested = tarfile.TarInfo("nested")
            nested.type = tarfile.DIRTYPE
            stream.addfile(nested)
            first = tarfile.TarInfo("nested/first")
            first.type = tarfile.SYMTYPE
            first.linkname = "second"
            stream.addfile(first)
            second = tarfile.TarInfo("nested/second")
            second.type = tarfile.SYMTYPE
            second.linkname = "../../outside"
            stream.addfile(second)
        with self.assertRaisesRegex(ValueError, "escapes runtime"):
            transfer.safe_extract(escape, self.base / "chain-output")
        self.assertFalse((self.base / "outside").exists())


if __name__ == "__main__":
    unittest.main()
