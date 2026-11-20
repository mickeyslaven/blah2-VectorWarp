"""Bounded identity fixtures; never inspect live nodes or invoke systemctl."""
import importlib.util
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("soak_identity", ROOT / "tools/vectorwarp_soak_identity.py")
IDENTITY = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(IDENTITY)


class ArtifactIdentityTests(unittest.TestCase):
    def test_activation_checks_effective_api_processor_and_loader_paths(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            root.chmod(0o755)
            release = root / "candidate"
            for name in ("bin/blah2", "api/server.js"):
                file = release / name
                file.parent.mkdir(parents=True, exist_ok=True)
                file.write_text("candidate")
            (release / "bin/blah2").chmod(0o755)
            config = root / "config.yml"
            config.write_text("site configuration")
            node = root / "node"
            node.write_text("runtime")
            def command(arguments):
                return "{ path=" + arguments[0] + " ; argv[]=" + " ".join(arguments) + " ; ignore_errors=no ; pid=0 ; }"
            units = {unit: {"ActiveState": "inactive", "WorkingDirectory": str(release),
                "User": "vectorwarp-api" if "api" in unit else "vectorwarp",
                "ExecStart": command([str(node), str(release / "api/server.js"), str(config)]
                    if "api" in unit else [str(release / "bin/blah2"), "--config", str(config)]),
                "Environment": "LD_LIBRARY_PATH=" + str(release / "bin")} for unit in IDENTITY.UNITS}
            self.assertTrue(IDENTITY.preflight(release, config, units)["coherentServicePlan"])
            api = units["vectorwarp-api.service"]
            old = api["WorkingDirectory"]
            api["WorkingDirectory"] = str(root)
            with self.assertRaisesRegex(ValueError, "API entrypoint"):
                IDENTITY.preflight(release, config, units)
            api["WorkingDirectory"] = old
            for value in ("LD_LIBRARY_PATH=" + str(root), "LD_PRELOAD=/old/library.so",
                          "NODE_OPTIONS=--require=/old/bootstrap.js"):
                api["Environment"] = value
                with self.subTest(value=value), self.assertRaises(ValueError):
                    IDENTITY.preflight(release, config, units)

    def test_preflight_rejects_candidate_modes_unusable_by_service_users(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            root.chmod(0o755)
            release = root / "candidate"
            for name in ("bin/blah2", "api/server.js"):
                file = release / name
                file.parent.mkdir(parents=True, exist_ok=True)
                file.write_text("candidate")
            (release / "bin/blah2").chmod(0o755)
            config = root / "config.yml"
            config.write_text("site configuration")
            node = root / "node"
            node.write_text("runtime")
            def command(arguments):
                return "{ path=" + arguments[0] + " ; argv[]=" + " ".join(arguments) + " ; ignore_errors=no ; pid=0 ; }"
            units = {unit: {"ActiveState": "inactive", "User": "vectorwarp-api" if "api" in unit else "vectorwarp",
                "WorkingDirectory": str(release), "Environment": "",
                "ExecStart": command([str(node), str(release / "api/server.js"), str(config)]
                    if "api" in unit else [str(release / "bin/blah2"), "--config", str(config)])}
                for unit in IDENTITY.UNITS}
            self.assertTrue(IDENTITY.preflight(release, config, units)["coherentServicePlan"])
            release.chmod(0o500)
            with self.assertRaisesRegex(ValueError, "traversable"):
                IDENTITY.preflight(release, config, units)
            release.chmod(0o755)
            (release / "api/server.js").chmod(0o600)
            with self.assertRaisesRegex(ValueError, "readable"):
                IDENTITY.preflight(release, config, units)
            (release / "api/server.js").chmod(0o644)
            (release / "bin/blah2").chmod(0o644)
            with self.assertRaisesRegex(ValueError, "executable"):
                IDENTITY.preflight(release, config, units)
            (release / "bin/blah2").chmod(0o755)
            units["vectorwarp-processor.service"]["User"] = "vectorwarp-api"
            self.assertTrue(IDENTITY.preflight(release, config, units)["coherentServicePlan"])

    def test_loaded_module_binds_disk_inode_and_rejects_other_cohort(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            release = root / "candidate"
            (release / "bin").mkdir(parents=True)
            module = release / "bin/libblah2-capture-core.so.1"
            module.write_bytes(b"native cohort")
            proc = root / "proc"
            proc.mkdir()
            (proc / "map_files").mkdir()
            info = module.stat()
            def write_map(path, inode=info.st_ino, device=None):
                major, minor = device or (os.major(info.st_dev), os.minor(info.st_dev))
                (proc / "maps").write_text(
                    f"1000-2000 r-xp 0 {major:x}:{minor:x} {inode} {path}\n")
            write_map(module)
            self.assertEqual(IDENTITY.loaded_modules(proc, release), {str(module): IDENTITY.file_hash(module)})
            write_map(module, info.st_ino + 1)
            with self.assertRaisesRegex(ValueError, "inode"):
                IDENTITY.loaded_modules(proc, release)
            os.link(module, proc / "map_files/1000-2000")
            write_map(module, device=(os.major(info.st_dev), os.minor(info.st_dev) + 1))
            self.assertEqual(IDENTITY.loaded_modules(proc, release),
                             {str(module): IDENTITY.file_hash(module)})
            old = root / module.name
            old.write_bytes(b"old cohort")
            write_map(old)
            with self.assertRaisesRegex(ValueError, "outside"):
                IDENTITY.loaded_modules(proc, release)

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

    def test_in_place_api_replacement_after_startup_cannot_match_cached_code(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "server.js"
            source.write_text("old cached API")
            before = max(root.stat().st_ctime_ns, source.stat().st_ctime_ns)
            IDENTITY.artifact_hash(root, latest_change_ns=before)
            source.write_text("replacement API")
            with self.assertRaisesRegex(ValueError, "after API/processor startup"):
                IDENTITY.artifact_hash(root, latest_change_ns=before)

    def test_unreadable_subtree_is_not_silently_omitted(self):
        with tempfile.TemporaryDirectory() as temporary, \
                patch.object(IDENTITY.os, "scandir", side_effect=PermissionError("unreadable")):
            with self.assertRaises(PermissionError):
                IDENTITY.artifact_hash(Path(temporary))


if __name__ == "__main__":
    unittest.main()
