#!/usr/bin/env python3
"""Artifact outcome/ownership regressions: no builds, network or live installs."""
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import tempfile
import types
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("artifact_plan", ROOT / "script/artifact-plan.py")
plan = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(plan)


class ArtifactPlanTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)

    def put(self, name, content=b"fixture", mode=0o755):
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(content)
        path.chmod(mode)
        return path

    def artifact(self, backend="all"):
        for paths in plan.PLAN["required"].values():
            for name in paths: self.put(name)
        for name in ("usrp", "hackrf"): self.put(f"bin/blah2-receiver-{name}.so")
        for name in plan.PLAN["notices"]: self.put("licenses/" + name + ".txt")
        for name in ("receiver-source/rspduo/kit.json", "systemd/vectorwarp-sdrplay-build.service.in",
                     "libexec/vectorwarp-build-sdrplay.py"): self.put(name)
        compiled, local = plan.profile(backend)
        source_commit = "a" * 40
        manifest = self.put(".vectorwarp-build", (
            f"build_id=stable\nsource_commit={source_commit}\nbackend={backend}\ncompiled_receivers={','.join(compiled)}\n"
            f"local_build_receivers={','.join(local)}\ntest_only={str(backend == 'open-test').lower()}\ngpu=AUTO\n").encode())
        self.put("BUILD-RECEIPT.json", json.dumps({"schema": 1, "version": "stable",
            "sourceCommit": source_commit, "package": "native-candidate"}).encode(), 0o644)
        manifest.write_text(manifest.read_text() + "".join(f"{key}={value}\n"
            for key, value in plan.IDENTITY["installed_hashes"](self.root).items()))
        return manifest

    def test_package_receipt_must_match_native_manifest(self):
        self.artifact()
        receipt = self.root / "BUILD-RECEIPT.json"
        good = receipt.read_bytes()
        for value in ({"schema": 1, "version": "other", "sourceCommit": "a" * 40,
                       "package": "native-candidate"},
                      {"schema": 1, "version": "stable", "sourceCommit": "b" * 40,
                       "package": "native-candidate"},
                      {"schema": 1, "version": "stable", "sourceCommit": "a" * 40,
                       "package": "source"}):
            receipt.write_text(json.dumps(value))
            with self.assertRaisesRegex(ValueError, "receipt disagrees"):
                plan.validate(self.root, "package")
        receipt.write_bytes(good)
        self.assertEqual(plan.validate(self.root, "package")["build_id"], "stable")

    def test_current_build_install_and_package_accept_same_receiver_contract(self):
        for backend in ("all", "open-test"):
            self.artifact(backend)
            for level in ("build", "install", "package"):
                with self.subTest(backend=backend, level=level):
                    value = plan.validate(self.root, level, backend, backend == "open-test")
                    self.assertEqual(value["backend"], backend)
        self.assertEqual(plan.profile("all"), (["Usrp", "HackRF", "Kraken"], ["RspDuo"]))
        self.assertEqual(plan.profile("all", True), (["RspDuo", "Usrp", "HackRF", "Kraken"], []))

    def test_manifest_hostility_fails_before_install(self):
        manifest = self.artifact()
        good = manifest.read_bytes()
        for mutation in (good + b"backend=all\n", good + b"test_only=false\n",
                         good.replace(b"Usrp,HackRF,Kraken", b"Usrp,Usrp,Kraken"),
                         good.replace(b"RspDuo", b"HackRF"),
                         good.replace(b"build_id=stable", b"build_id=../../outside"),
                         good.replace(b"backend=all", b"backend=unknown"),
                         good.replace(b"test_only=false", b"test_only=true")):
            manifest.write_bytes(mutation)
            with self.assertRaises(ValueError): plan.validate(self.root, "install")
        manifest.write_bytes(good)
        mixed = self.put("bin/blah2-mixed-worker")
        with self.assertRaisesRegex(ValueError, "historical mixed"): plan.validate(self.root, "install")
        mixed.unlink(); mixed.symlink_to("absent")
        with self.assertRaisesRegex(ValueError, "historical mixed"): plan.validate(self.root, "install")

    def test_package_and_install_reject_stale_or_partial_native_hashes(self):
        manifest = self.artifact()
        original = manifest.read_text()
        module = self.root / "bin/blah2-gpu-resident-vulkan.so"
        module.write_bytes(b"a different resident build")
        for level in ("install", "package"):
            with self.subTest(level=level), self.assertRaisesRegex(ValueError, "installed bytes"):
                plan.validate(self.root, level)
        module.write_bytes(b"fixture")
        manifest.write_text("\n".join(line for line in original.splitlines()
                                    if not line.startswith("gpu_worker_sha256=")) + "\n")
        for level in ("install", "package"):
            with self.subTest(level=level), self.assertRaisesRegex(ValueError, "incomplete"):
                plan.validate(self.root, level)
        manifest.write_text("\n".join(line for line in original.splitlines()
                                    if not line.split("=", 1)[0].endswith("_sha256")) + "\n")
        self.assertEqual(plan.validate(self.root, "install")["artifact_hash_provenance"], "legacy-unpinned")
        with self.assertRaisesRegex(ValueError, "incomplete"):
            plan.validate(self.root, "package")

    def test_install_only_keeps_existing_pre_manifest_compatibility(self):
        self.artifact()
        for backend in ("all", "kraken"):
            with self.subTest(backend=backend):
                self.put(".vectorwarp-build", f"build_id=old\nbackend={backend}\n".encode())
                value = plan.validate(self.root, "install")
                self.assertEqual(value["compiled_receivers"], ",".join(plan.profile(backend)[0]))
                self.assertEqual(value["test_only"], "false")
                with self.assertRaises(ValueError): plan.validate(self.root, "package")
        self.put(".vectorwarp-build", b"build_id=old\nbackend=usrp\n")
        with self.assertRaises(ValueError): plan.validate(self.root, "install")

    def test_package_requires_explicit_test_only_marker_before_install_defaults(self):
        for backend in ("all", "open-test"):
            with self.subTest(backend=backend):
                manifest = self.artifact(backend)
                test_only = backend == "open-test"
                self.assertEqual(plan.validate(self.root, "package", test_only=test_only)["backend"], backend)
                manifest.write_bytes(manifest.read_bytes().replace(
                    f"test_only={str(test_only).lower()}\n".encode(), b""))
                with self.assertRaisesRegex(ValueError, "explicit receiver and test-only manifest"):
                    plan.validate(self.root, "package", test_only=test_only)
                if test_only:
                    with self.assertRaisesRegex(ValueError, "explicitly marked test-only"):
                        plan.validate(self.root, "install")
                else:
                    self.assertEqual(plan.validate(self.root, "install")["test_only"], "false")

    def test_release_refuses_missing_members_test_bins_vendor_sdk_and_nonexecutable(self):
        self.artifact()
        for name in plan.PLAN["required"]["package"]:
            path = self.root / name
            raw, mode = path.read_bytes(), path.stat().st_mode & 0o777
            path.unlink()
            with self.subTest(name=name), self.assertRaises(ValueError): plan.validate(self.root, "package")
            self.put(name, raw, mode)
        for name in ("bin/testDetection", "vendor/sdrplay_api.h", "vendor/libSDRplay.so"):
            path = self.put(name)
            with self.subTest(name=name), self.assertRaises(ValueError): plan.validate(self.root, "package")
            path.unlink()
        (self.root / "bin/blah2").chmod(0o644)
        with self.assertRaises(ValueError): plan.validate(self.root, "install")

    def test_staging_preserves_bytes_members_modes_and_rolls_back_a_failed_member(self):
        source, output = self.root / "source", self.root / "out"
        source.mkdir()
        for src, dst, mode, *unused in plan.PLAN["stage"]["common"] + plan.PLAN["stage"]["linux"]:
            path = source / src
            if mode is None:
                path.mkdir(parents=True, exist_ok=True)
                (path / "keep").write_bytes(src.encode())
            else:
                path.parent.mkdir(parents=True, exist_ok=True); path.write_bytes(src.encode())
        for name in ("api/drop.test.js", "api/node_modules/dependency", "docs/benchmarks/run.log",
                     "docs/benchmarks/keep.md"):
            path = source / name; path.parent.mkdir(parents=True, exist_ok=True); path.write_text(name)
        plan.stage(source, output, "linux")
        self.assertEqual((output / "libexec/vectorwarp-quiesce").read_bytes(), b"packaging/deb/preinst")
        self.assertEqual((output / "libexec/vectorwarp-check-node-version").stat().st_mode & 0o777, 0o644)
        self.assertEqual((output / "libexec/vectorwarp-restart").stat().st_mode & 0o777, 0o755)
        self.assertTrue((output / "config-examples/config.yml").is_file())
        self.assertTrue((output / "docs/benchmarks/keep.md").is_file())
        for name in ("api/drop.test.js", "api/node_modules", "docs/benchmarks/run.log"):
            self.assertFalse((output / name).exists())
        with mock.patch.object(plan.shutil, "copytree", side_effect=OSError("copy failure")):
            with self.assertRaises(OSError): plan.stage(source, output, "linux")
        self.assertEqual((output / "api/keep").read_bytes(), b"api")
        self.assertFalse(list(output.rglob(".artifact-*")))
        for unsafe in (source, source / "api", self.root):
            with self.assertRaises(ValueError): plan.stage(source, unsafe, "linux")

    def test_detached_replay_support_is_a_complete_isolated_cohort(self):
        plan.stage(ROOT, self.root / "support", "replay-support")
        result = subprocess.run(["python3", "-I", str(self.root / "support/processor_replay_test.py"), "--help"],
            env={**os.environ, "VECTORWARP_REPLAY_SESSION": str(self.root / "support/replay_session.py")},
            capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_failed_publication_and_failed_restore_preserve_old_member_for_recovery(self):
        source, output = self.root / "source", self.root / "out"
        (source / "api").mkdir(parents=True); (source / "api/keep").write_text("new")
        (output / "api").mkdir(parents=True); (output / "api/keep").write_text("old")
        original = Path.rename
        def rename(path, destination):
            if path.name in ("next", "previous"):
                raise OSError("injected publish/restore failure")
            return original(path, destination)
        with mock.patch.object(Path, "rename", rename):
            with self.assertRaises(OSError): plan.stage(source, output, "linux")
        recovery = list(output.glob(".artifact-*/previous/keep"))
        self.assertEqual(len(recovery), 1)
        self.assertEqual(recovery[0].read_text(), "old")

    def test_one_lifecycle_for_both_architectures_and_package_formats(self):
        for architecture in ("x64", "arm64"):
            for kind, distro in (("deb", "debian13"), ("rpm", "fedora44")):
                with self.subTest(architecture=architecture, format=kind):
                    (self.root / "dist").mkdir(exist_ok=True)
                    for old in (self.root / "dist").glob("*"): old.unlink()
                    self.put("dist/test." + kind)
                    calls = []
                    def command(argv, **kwargs):
                        calls.append((argv, kwargs))
                        if argv[0] == "curl": Path(argv[-1]).write_bytes(b"reviewed Node")
                    pins = {"NODE_VERSION": "99.1.2", "NODE_BASE_URL": "https://node.invalid/v99.1.2",
                            f"NODE_{architecture.upper()}_SHA256": hashlib.sha256(b"reviewed Node").hexdigest()}
                    args = types.SimpleNamespace(node_arch=architecture, format=kind, distro=distro, version="1.2.3")
                    with mock.patch.object(plan, "ROOT", self.root), mock.patch.object(plan, "key_values", return_value=pins), \
                            mock.patch.object(plan.subprocess, "run", side_effect=command):
                        plan.native_package(args)
                    self.assertEqual([item[0][0] for item in calls], ["curl", "tar", "bash", "bash", "bash"])
                    self.assertIn("--test", calls[2][0])
                    self.assertEqual(calls[3][0][-3:], [distro, "--output-dir", "dist"])
                    self.assertIn(f"linux-{architecture}.tar.xz", calls[0][0][-3])
                    self.assertFalse(Path(calls[0][0][-1]).exists())

    def test_no_parallel_profiles_stagers_or_embedded_dependency_pins(self):
        for filename in ("build-native.sh", "build-macos.sh", "install-native.sh", "package-native.sh"):
            source = (ROOT / "script" / filename).read_text()
            self.assertIn("artifact-plan.py", source)
            self.assertNotIn("case \"$BACKEND\" in", source)
            self.assertNotIn("expected_receivers=", source)
            self.assertNotIn("run cp -a \"$SOURCE_DIR/api\"", source)
        for path in (ROOT / ".github/workflows").glob("*.yml"):
            source = path.read_text()
            for pin in plan.PLAN["dependencies"].values(): self.assertFalse(pin["commit"] in source, path.name)
            self.assertFalse("node-version: " + plan.key_values(ROOT / plan.PLAN["node_pins"])["NODE_VERSION"] in source, path.name)

    def test_offline_dependencies_never_clone_or_fetch_and_verify_exact_revision(self):
        directory = self.root / "cache"
        with mock.patch.object(plan.subprocess, "run") as run:
            with self.assertRaisesRegex(ValueError, "absent"):
                plan.checkout("VkFFT", directory, offline=True)
            run.assert_not_called()
        (directory / ".git").mkdir(parents=True)
        with mock.patch.object(plan.subprocess, "run", return_value=types.SimpleNamespace(returncode=1)) as run:
            with self.assertRaisesRegex(ValueError, "lacks pinned"):
                plan.checkout("VkFFT", directory, offline=True)
            self.assertEqual(run.call_count, 1)
            self.assertIn("cat-file", run.call_args[0][0])
        commit = plan.PLAN["dependencies"]["VkFFT"]["commit"]
        with mock.patch.object(plan.subprocess, "run", return_value=types.SimpleNamespace(returncode=0)) as run, \
                mock.patch.object(plan.subprocess, "check_output", return_value=commit + "\n"):
            plan.checkout("VkFFT", directory, offline=True)
            self.assertEqual(run.call_args[0][0][-3:], ("checkout", "--detach", commit))
        with mock.patch.object(plan.subprocess, "run", return_value=types.SimpleNamespace(returncode=0)), \
                mock.patch.object(plan.subprocess, "check_output", return_value="wrong"):
            with self.assertRaisesRegex(ValueError, "pinned commit"):
                plan.checkout("VkFFT", directory, offline=True)

    def test_reviewed_matrix_rejects_cross_os_architecture_and_codename(self):
        for item in plan.matrix()["include"]:
            spec = plan.PLAN["linux_distros"][item["distro"]]
            command = ["python3", str(ROOT / "script/artifact-plan.py"), "target", item["distro"],
                       "--format", item["format"], "--node-arch", item["node_arch"],
                       "--os-id", spec["id"], "--os-version", spec["version"],
                       "--os-codename", spec["codename"]]
            accepted = subprocess.run(command, capture_output=True, text=True, timeout=5)
            self.assertEqual(accepted.returncode, 0, accepted.stderr)
            self.assertEqual(accepted.stdout.split()[-1], item["arch"])
            for field, wrong in (("--os-id", "foreign"), ("--os-version", "0"),
                                  ("--format", "rpm" if item["format"] == "deb" else "deb")):
                changed = list(command); changed[changed.index(field) + 1] = wrong
                self.assertNotEqual(subprocess.run(changed, capture_output=True, timeout=5).returncode, 0)

    def test_node_archive_mismatch_stops_before_extraction_build_or_install(self):
        calls = []
        def command(argv, **unused):
            calls.append(argv); Path(argv[-1]).write_bytes(b"unreviewed")
        args = types.SimpleNamespace(node_arch="x64", format="deb", distro="debian13", version="1.2.3")
        with mock.patch.object(plan.subprocess, "run", side_effect=command):
            with self.assertRaisesRegex(ValueError, "reviewed pin"):
                plan.native_package(args)
        self.assertEqual(len(calls), 1)
        self.assertFalse(Path(calls[0][-1]).exists())


if __name__ == "__main__":
    unittest.main()
