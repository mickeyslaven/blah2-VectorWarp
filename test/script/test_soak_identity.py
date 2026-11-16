"""Bounded identity fixtures; never inspect live nodes or invoke systemctl."""
import importlib.util
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("soak_identity", ROOT / "tools/vectorwarp_soak_identity.py")
IDENTITY = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(IDENTITY)


class ArtifactIdentityTests(unittest.TestCase):
    def test_digest_binds_bytes_names_modes_and_symlink_targets(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "source"
            source.write_text("first")
            initial = IDENTITY.artifact_hash(root)
            source.write_text("second")
            self.assertNotEqual(initial, IDENTITY.artifact_hash(root))
            source.write_text("first")
            self.assertEqual(initial, IDENTITY.artifact_hash(root))
            source.chmod(0o700)
            self.assertNotEqual(initial, IDENTITY.artifact_hash(root))
            renamed = root / "renamed"
            before = IDENTITY.artifact_hash(root)
            source.rename(renamed)
            self.assertNotEqual(before, IDENTITY.artifact_hash(root))
            link = root / "link"
            link.symlink_to("renamed")
            before = IDENTITY.artifact_hash(root)
            link.unlink()
            link.symlink_to("./renamed")
            self.assertNotEqual(before, IDENTITY.artifact_hash(root))

    def test_external_symlink_fails_closed(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary) / "artifact"
            root.mkdir()
            outside = Path(temporary) / "outside"
            outside.write_text("not part of artifact")
            (root / "external").symlink_to(outside)
            with self.assertRaises(ValueError):
                IDENTITY.artifact_hash(root)

    def test_empty_artifact_fails_closed(self):
        with tempfile.TemporaryDirectory() as temporary:
            with self.assertRaises(ValueError):
                IDENTITY.artifact_hash(Path(temporary))

    def test_unreadable_subtree_is_not_silently_omitted(self):
        with tempfile.TemporaryDirectory() as temporary, \
                patch.object(IDENTITY.os, "scandir", side_effect=PermissionError("unreadable")):
            with self.assertRaises(PermissionError):
                IDENTITY.artifact_hash(Path(temporary))


if __name__ == "__main__":
    unittest.main()
