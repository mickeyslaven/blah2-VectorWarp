"""Bounded identity fixtures; never inspect live nodes or invoke systemctl."""
import importlib.util
import os
from pathlib import Path
import shutil
import tempfile
import unittest
from unittest.mock import MagicMock, patch

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("soak_identity", ROOT / "tools/vectorwarp_soak_identity.py")
IDENTITY = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(IDENTITY)


class ArtifactIdentityTests(unittest.TestCase):
    def test_process_cwd_uses_declared_plan_only_when_proc_access_is_denied(self):
        proc, declared = MagicMock(), Path('/declared/service/cwd')
        proc.__truediv__.return_value.resolve.side_effect = PermissionError('ptrace denied')
        self.assertEqual(IDENTITY.process_cwd(proc, declared), declared)
        executable = MagicMock()
        executable.resolve.return_value = Path('/usr/bin/node')
        self.assertEqual(IDENTITY.process_executable(proc, executable),
                         (Path('/usr/bin/node'), 'service-plan'))
        proc.__truediv__.return_value.resolve.side_effect = FileNotFoundError('process disappeared')
        with self.assertRaises(FileNotFoundError):
            IDENTITY.process_cwd(proc, declared)
        with self.assertRaises(FileNotFoundError):
            IDENTITY.process_executable(proc, executable)

    def fixture(self, root):
        root.chmod(0o755)
        release = root / "candidate"
        for name in ("bin/blah2", "api/server.js"):
            file = release / name
            file.parent.mkdir(parents=True, exist_ok=True)
            file.write_text("candidate")
        (release / "bin/blah2").chmod(0o755)
        config, node = root / "config.yml", root / "node"
        config.write_text("site configuration")
        node.write_text("runtime")
        def command(arguments):
            return "{ path=" + arguments[0] + " ; argv[]=" + " ".join(arguments) + " ; ignore_errors=no ; pid=0 ; }"
        units = {unit: {"ActiveState": "inactive", "WorkingDirectory": str(release),
            "User": "vectorwarp-api" if "api" in unit else "vectorwarp",
            "ExecStart": command([str(node), str(release / "api/server.js"), str(config)]
                if "api" in unit else [str(release / "bin/blah2"), "--config", str(config)]),
            "Environment": "LD_LIBRARY_PATH=" + str(release / "bin")} for unit in IDENTITY.UNITS}
        return release, config, node, units

    def test_activation_checks_effective_api_processor_and_loader_paths(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            release, config, node, units = self.fixture(root)
            with self.assertRaisesRegex(ValueError, "provenance"):
                IDENTITY.preflight(release, config, units)
            legacy = IDENTITY.preflight(release, config, units, allow_legacy=True)
            self.assertEqual(legacy["artifactHashProvenance"], "legacy-unpinned")
            module = release / "bin/blah2-gpu-resident-vulkan.so"
            module.write_bytes(b"installed resident module")
            for key, name in IDENTITY.NATIVE_HASH_PATHS.items():
                if not (release / name).exists():
                    (release / name).write_bytes(key.encode())
            manifest = release / ".vectorwarp-build"
            pins = IDENTITY.installed_hashes(release)
            declared = "".join(f"{key}={value}\n" for key, value in pins.items())
            manifest.write_text(f"processor_sha256={pins['processor_sha256']}\n"
                                "resident_module_sha256=" + "a" * 64 + "\n")
            with self.assertRaisesRegex(ValueError, "incomplete"):
                IDENTITY.preflight(release, config, units, allow_legacy=True)
            manifest.write_text(declared)
            self.assertEqual(IDENTITY.preflight(release, config, units)["verifiedDeclaredHashes"], pins)
            module.write_bytes(b"different pre-launch resident module")
            with self.assertRaisesRegex(ValueError, "installed bytes: resident_module_sha256"):
                IDENTITY.preflight(release, config, units)
            module.write_bytes(b"installed resident module")
            (release / "bin/blah2").write_bytes(b"wrong processor")
            with self.assertRaisesRegex(ValueError, "installed bytes: processor_sha256"):
                IDENTITY.preflight(release, config, units)
            (release / "bin/blah2").write_bytes(b"candidate")
            for key in ("gpu_worker_sha256", "portable_module_sha256", "wideband_module_sha256"):
                target = release / IDENTITY.NATIVE_HASH_PATHS[key]
                original = target.read_bytes()
                target.write_bytes(b"wrong native member")
                with self.subTest(key=key), self.assertRaisesRegex(ValueError, "installed bytes: " + key):
                    IDENTITY.preflight(release, config, units)
                target.write_bytes(original)
            for text in (declared + f"resident_module_sha256={pins['resident_module_sha256']}\n",
                         "resident_module_sha256=source-or-malformed-digest\n"):
                manifest.write_text(text)
                with self.assertRaisesRegex(ValueError, "invalid or duplicate"):
                    IDENTITY.preflight(release, config, units)
            manifest.write_text("source_commit=legacy-without-installed-byte-pins\n")
            self.assertEqual(IDENTITY.check_declared_hashes(release, allow_legacy=True), {})
            with self.assertRaisesRegex(ValueError, "incomplete"):
                IDENTITY.check_declared_hashes(release)
            manifest.write_text(declared)
            proc_root = root / "proc"
            for pid, executable, arguments, descendants in (
                    (1, node, [str(node), str(release / "api/server.js"), str(config)], ""),
                    (2, release / "bin/blah2", [str(release / "bin/blah2"), "--config", str(config)], "3"),
                    (3, release / "bin/blah2-gpu-worker", [], "")):
                proc = proc_root / str(pid)
                (proc / "task" / str(pid)).mkdir(parents=True)
                (proc / "task" / str(pid) / "children").write_text(descendants)
                (proc / "cmdline").write_bytes(("\0".join(arguments) + "\0").encode())
                (proc / "environ").write_bytes(b"")
                (proc / "maps").write_text("")
                (proc / "stat").write_text(str(pid) + " (fixture) " + " ".join(["1"] * 20))
                (proc / "exe").symlink_to(executable)
                (proc / "cwd").symlink_to(release)
                (proc / "root").symlink_to("/")
            for pid, unit in enumerate(IDENTITY.UNITS, 1):
                units[unit].update(ActiveState="active", MainPID=str(pid),
                                   ExecMainStartTimestampMonotonic="1")
            with patch.object(IDENTITY, "Path", side_effect=lambda value:
                              proc_root if value == "/proc" else Path(value)):
                runtime = IDENTITY.preflight(release, config, units, check_immutable=False)
                worker = str(release / "bin/blah2-gpu-worker")
                self.assertEqual(runtime["runtimeChildren"][IDENTITY.UNITS[1]],
                                 [{"pid": 3, "path": worker, "startTicks": "1"}])
                self.assertEqual(runtime["runtimeModules"][IDENTITY.UNITS[1]][worker],
                                 pins["gpu_worker_sha256"])
                (proc_root / "1/environ").write_bytes(b"NODE_PATH=/old/code\0")
                with self.assertRaisesRegex(ValueError, "NODE_PATH"):
                    IDENTITY.preflight(release, config, units, check_immutable=False)
                (proc_root / "1/environ").write_bytes(b"")
                (proc_root / "3/exe").unlink()
                (proc_root / "3/exe").symlink_to(node)
                with self.assertRaisesRegex(ValueError, "child executable outside"):
                    IDENTITY.preflight(release, config, units, check_immutable=False)
            for values in units.values():
                values["ActiveState"] = "inactive"
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
            release, config, _, units = self.fixture(root)
            self.assertTrue(IDENTITY.preflight(release, config, units, allow_legacy=True)["coherentServicePlan"])
            release.chmod(0o500)
            with self.assertRaisesRegex(ValueError, "traversable"):
                IDENTITY.preflight(release, config, units, allow_legacy=True)
            release.chmod(0o755)
            (release / "api/server.js").chmod(0o600)
            with self.assertRaisesRegex(ValueError, "readable"):
                IDENTITY.preflight(release, config, units, allow_legacy=True)
            (release / "api/server.js").chmod(0o644)
            (release / "bin/blah2").chmod(0o644)
            with self.assertRaisesRegex(ValueError, "executable"):
                IDENTITY.preflight(release, config, units, allow_legacy=True)
            (release / "bin/blah2").chmod(0o755)
            units["vectorwarp-processor.service"]["User"] = "vectorwarp-api"
            self.assertTrue(IDENTITY.preflight(release, config, units, allow_legacy=True)["coherentServicePlan"])

    def test_effective_mount_overrides_are_refused_before_service_activation(self):
        with tempfile.TemporaryDirectory() as temporary:
            release, config, _, units = self.fixture(Path(temporary))
            for unit in IDENTITY.UNITS:
                for key in ("BindPaths", "BindReadOnlyPaths", "RootDirectory", "RootImage"):
                    with self.subTest(unit=unit, key=key):
                        units[unit][key] = "/old/api/server.js:" + str(release / "api/server.js")
                        with self.assertRaisesRegex(ValueError, key):
                            IDENTITY.preflight(release, config, units, allow_legacy=True)
                        del units[unit][key]

    def test_coordinator_preflight_preserves_masked_processor_and_refuses_role_drift(self):
        with tempfile.TemporaryDirectory() as temporary:
            release, config, _, units = self.fixture(Path(temporary))
            units[IDENTITY.UNITS[0]]["Environment"] = "VECTORWARP_NODE_ROLE=coordinator"
            units[IDENTITY.UNITS[1]] = {"ActiveState": "inactive", "LoadState": "masked", "MainPID": "0"}
            result = IDENTITY.preflight(release, config, units, allow_legacy=True)
            self.assertEqual(result["nodeRole"], "coordinator")
            self.assertEqual(result["runtimeExecutables"], {})
            for state, pid in (("active", "4"), ("activating", "0"), ("inactive", "4")):
                units[IDENTITY.UNITS[1]].update(ActiveState=state, MainPID=pid)
                with self.assertRaisesRegex(ValueError, "must not run"):
                    IDENTITY.preflight(release, config, units, allow_legacy=True)
            units[IDENTITY.UNITS[1]].update(ActiveState="inactive", MainPID="0")
            units[IDENTITY.UNITS[0]]["Environment"] = "VECTORWARP_NODE_ROLE=invalid"
            with self.assertRaisesRegex(ValueError, "invalid cohort node role"):
                IDENTITY.preflight(release, config, units, allow_legacy=True)
            units[IDENTITY.UNITS[0]]["Environment"] = ""
            with self.assertRaisesRegex(ValueError, "unprivileged"):
                IDENTITY.preflight(release, config, units, allow_legacy=True)

    def test_coordinator_probe_measures_process_root_code_and_config(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            release, config, node, units = self.fixture(root)
            pins = IDENTITY.installed_hashes(release)
            (release / ".vectorwarp-build").write_text("".join(f"{k}={v}\n" for k, v in pins.items()))
            api = units[IDENTITY.UNITS[0]]
            api.update(Environment="VECTORWARP_NODE_ROLE=coordinator", ActiveState="active",
                       MainPID="1", ExecMainStartTimestampMonotonic="999999999999999999")
            units[IDENTITY.UNITS[1]] = {"ActiveState": "inactive", "MainPID": "0"}
            proc = root / "proc/1"
            (proc / "task/1").mkdir(parents=True)
            (proc / "task/1/children").write_text("")
            (proc / "cmdline").write_bytes(("\0".join(map(str, (node, release / "api/server.js", config))) + "\0").encode())
            (proc / "environ").write_bytes(b"VECTORWARP_NODE_ROLE=coordinator\0")
            (proc / "maps").write_text("")
            (proc / "exe").symlink_to(node)
            (proc / "cwd").symlink_to(release)
            namespace_release = proc / "root" / release.relative_to("/")
            shutil.copytree(release, namespace_release)
            namespace_config = proc / "root" / config.relative_to("/")
            shutil.copy2(config, namespace_config)
            with patch.object(IDENTITY, "service_units", return_value=units), \
                    patch.object(IDENTITY, "Path", side_effect=lambda value:
                        root / "proc" if value == "/proc" else Path(value)):
                result = IDENTITY.probe(release, config, root)
                self.assertEqual(set(result["runtimeExecutables"]), {IDENTITY.UNITS[0]})
                self.assertEqual(result["nodeRole"], "coordinator")
                (proc / "environ").write_bytes(b"")
                with self.assertRaisesRegex(ValueError, "node role differs"):
                    IDENTITY.probe(release, config, root)
                (proc / "environ").write_bytes(b"VECTORWARP_NODE_ROLE=coordinator\0")
                overlay = namespace_release / "api/server.js"
                overlay.write_text("old bind-mounted API")
                with self.assertRaisesRegex(ValueError, "process-root artifact differs"):
                    IDENTITY.probe(release, config, root)
                shutil.copy2(release / "api/server.js", overlay)
                extra = namespace_release / "api/foreign.js"
                extra.write_text("extra namespace code")
                with self.assertRaisesRegex(ValueError, "process-root artifact differs"):
                    IDENTITY.probe(release, config, root)
                extra.unlink()
                namespace_config.write_text("foreign configuration")
                with self.assertRaisesRegex(ValueError, "process-root configuration differs"):
                    IDENTITY.probe(release, config, root)

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
