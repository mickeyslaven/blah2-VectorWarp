#!/usr/bin/env python3
"""Focused offline Mach-O relocation/audit fixtures for standalone packaging."""
import importlib.util
import json
import os
from pathlib import Path
import platform
import shutil
import signal
import subprocess
import tempfile
import time
import unittest
from unittest import mock
from types import SimpleNamespace


ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("package_macos_standalone",
                                               ROOT / "script/package-macos-standalone.py")
packager = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(packager)


def local_arch():
    machine = platform.machine().lower()
    if machine in {"arm64", "aarch64"}:
        return "arm64"
    if machine in {"x86_64", "amd64"}:
        return "x86_64"
    raise unittest.SkipTest(f"unsupported test host architecture: {machine}")


def other_arch(architecture):
    return "x86_64" if architecture == "arm64" else "arm64"


class StandalonePackageTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="vectorwarp standalone package ")
        self.base = Path(self.temporary.name)
        self.runtime_index = 0

    def tearDown(self):
        self.temporary.cleanup()

    def compile(self, architecture, output, *sources, extra=()):
        command = ["xcrun", "--sdk", "macosx", "clang", "-Wall", "-Wextra", "-Werror",
                   "-arch", architecture, "-mmacosx-version-min=15.0", *map(str, sources),
                   *map(str, extra), "-o", str(output)]
        result = subprocess.run(command, text=True, capture_output=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def runtime(self, architecture, source_id="a" * 40):
        self.runtime_index += 1
        suffix = f"{architecture}-{self.runtime_index}"
        origin = self.base / f"origin-{suffix}"
        library = origin / "private libraries"
        binary = origin / "bin"
        library.mkdir(parents=True); binary.mkdir()
        dep_source = origin / "dep.c"; middle_source = origin / "middle.c"; main_source = origin / "main.c"
        dep = library / "libdep.dylib"; middle = library / "libmiddle.dylib"; executable = binary / "fixture"
        dep_source.write_text("int dep(void) { return 42; }\n")
        middle_source.write_text("int dep(void); int middle(void) { return dep(); }\n")
        main_source.write_text("int middle(void); int main(void) { return middle() == 42 ? 0 : 1; }\n")
        self.compile(architecture, dep, dep_source, extra=("-dynamiclib", "-Wl,-install_name," + str(dep)))
        self.compile(architecture, middle, middle_source, dep,
                     extra=("-dynamiclib", "-Wl,-install_name," + str(middle),
                            "-Wl,-rpath," + str(library)))
        self.compile(architecture, executable, main_source, middle,
            extra=("-Wl,-rpath," + str(library),))
        runtime = self.base / f"runtime-{suffix}"
        runtime.mkdir()
        relocator = packager.Relocator(runtime, architecture, [binary])
        relocator.copy_native(executable, runtime / "bin/fixture")
        relocator.relocate()
        count = packager.native_audit(runtime, architecture, "15.0")
        metadata = {"schema": 1, "arch": architecture, "source_id": source_id,
                    "minimum_os": "15.0", "native_files": count,
                    "files": packager.files_manifest(runtime)}
        (runtime / "standalone.json").write_text(json.dumps(metadata, sort_keys=True))
        return runtime, origin, library

    def add_standalone_script(self, runtime):
        script = runtime / "script/vectorwarp-standalone"
        script.parent.mkdir()
        script.write_text("#!/bin/sh\nexec \"$(dirname \"$0\")/../bin/fixture\" \"$@\"\n")
        script.chmod(0o755)
        metadata = json.loads((runtime / "standalone.json").read_text())
        metadata["files"] = packager.files_manifest(runtime)
        (runtime / "standalone.json").write_text(json.dumps(metadata, sort_keys=True))

    def inherited_rpath_runtime(self, architecture):
        self.runtime_index += 1
        suffix = f"inherited-{architecture}-{self.runtime_index}"
        origin = self.base / f"origin-{suffix}"
        library = origin / "private libraries"
        binary = origin / "bin"
        library.mkdir(parents=True); binary.mkdir()
        dep_source = origin / "dep.c"; middle_source = origin / "middle.c"; main_source = origin / "main.c"
        dep = library / "libdep.dylib"; middle = library / "libmiddle.dylib"; executable = binary / "fixture"
        dep_source.write_text("int dep(void) { return 42; }\n")
        middle_source.write_text("int dep(void); int middle(void) { return dep(); }\n")
        main_source.write_text("int middle(void); int main(void) { return middle() == 42 ? 0 : 1; }\n")
        self.compile(architecture, dep, dep_source,
                     extra=("-dynamiclib", "-Wl,-install_name,@rpath/libdep.dylib"))
        self.compile(architecture, middle, middle_source, dep,
                     extra=("-dynamiclib", "-Wl,-install_name," + str(middle)))
        self.compile(architecture, executable, main_source, middle,
                     extra=("-Wl,-rpath," + str(library),))
        runtime = self.base / f"runtime-{suffix}"
        runtime.mkdir()
        relocator = packager.Relocator(runtime, architecture, [binary])
        relocator.copy_native(executable, runtime / "bin/fixture")
        return relocator

    def test_relocation_closes_transitive_absolute_dependencies_after_origin_removal(self):
        architecture = local_arch()
        runtime, origin, library = self.runtime(architecture)
        executable = runtime / "bin/fixture"
        links, rpaths, _, _ = packager.commands(executable)
        self.assertFalse(rpaths)
        self.assertTrue(all(link.startswith(("@loader_path/", *packager.SYSTEM)) for link in links), links)
        self.assertGreaterEqual(len(list((runtime / "lib").glob("*.dylib"))), 2)
        shutil.rmtree(library)
        result = subprocess.run([str(executable)], text=True, capture_output=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertFalse(origin.exists() and library.exists())
        self.assertEqual(packager.native_audit(runtime, architecture, "15.0"), 3)

    def test_audit_rejects_tampering_wrong_arch_deployment_and_symlink_escape(self):
        architecture = local_arch()
        runtime, _, _ = self.runtime(architecture)
        self.assertEqual(packager.audit_runtime(runtime, architecture)["arch"], architecture)
        (runtime / "readme.txt").write_text("modified after manifest")
        with self.assertRaisesRegex(ValueError, "files changed"):
            packager.audit_runtime(runtime, architecture)
        (runtime / "readme.txt").unlink()
        metadata = json.loads((runtime / "standalone.json").read_text())
        metadata["files"] = packager.files_manifest(runtime)
        (runtime / "standalone.json").write_text(json.dumps(metadata))
        with self.assertRaisesRegex(ValueError, "wrong architecture"):
            packager.native_audit(runtime, other_arch(architecture), "15.0")
        with self.assertRaisesRegex(ValueError, "deployment target"):
            packager.native_audit(runtime, architecture, "14.0")
        escaped = runtime / "escape"
        escaped.symlink_to(self.base)
        with self.assertRaisesRegex(ValueError, "absolute symlink"):
            packager.safe_tree(runtime)

    def test_assembly_refuses_absent_and_mismatched_runtime_identities_before_output(self):
        architecture = local_arch()
        current, _, _ = self.runtime(architecture, "a" * 40)
        missing = self.base / "missing"
        arguments = SimpleNamespace(arm64=current if architecture == "arm64" else missing,
                                    x86_64=current if architecture == "x86_64" else missing,
                                    output=self.base / "absent-output", version="1.2.3")
        with self.assertRaises(FileNotFoundError):
            packager.assemble(arguments)
        self.assertFalse(arguments.output.exists())

        arm, _, _ = self.runtime("arm64", "a" * 40)
        intel, _, _ = self.runtime("x86_64", "b" * 40)
        arguments = SimpleNamespace(arm64=arm, x86_64=intel,
                                    output=self.base / "mismatch-output", version="1.2.3")
        with self.assertRaisesRegex(ValueError, "same source identity"):
            packager.assemble(arguments)
        self.assertFalse(arguments.output.exists())

    def test_assembly_builds_and_runs_a_universal_app_and_component_package(self):
        source_id = "c" * 40
        arm, _, _ = self.runtime("arm64", source_id)
        intel, _, _ = self.runtime("x86_64", source_id)
        self.add_standalone_script(arm)
        self.add_standalone_script(intel)
        output = self.base / "assembled output"
        app = output / "VectorWarp.app"
        original_run = packager.run

        def add_finder_metadata_after_compile(*command):
            result = original_run(*command)
            if command[0] == '/usr/bin/clang':
                # Reproduce metadata attached by Finder/file providers after
                # the generated app directory exists, before bundle signing.
                original_run('/usr/bin/xattr', '-wx', 'com.apple.FinderInfo',
                             '00' * 8 + '0400' + '00' * 22, app)
                original_run('/usr/bin/xattr', '-w', 'com.apple.ResourceFork',
                             'fixture', app / 'Contents/Resources/runtime/arm64/script/vectorwarp-standalone')
                original_run('/usr/bin/xattr', '-w', 'io.vectorwarp.keep', 'retained', app)
            return result

        with mock.patch.object(packager, 'run', side_effect=add_finder_metadata_after_compile):
            packager.assemble(SimpleNamespace(arm64=arm, x86_64=intel,
                                              output=output, version="1.2.3"))
        self.assertEqual(original_run('/usr/bin/xattr', '-p', 'io.vectorwarp.keep', app).strip(), 'retained')
        attributes = original_run('/usr/bin/xattr', '-r', app)
        self.assertNotIn('com.apple.FinderInfo', attributes)
        self.assertNotIn('com.apple.ResourceFork', attributes)
        self.assertNotIn('io.vectorwarp.keep', original_run('/usr/bin/xattr', '-r', arm))
        launcher = app / "Contents/MacOS/VectorWarp"
        package = output / "VectorWarp-universal-local.pkg"
        self.assertTrue(package.is_file())
        self.assertEqual(set(packager.run("/usr/bin/lipo", "-archs", launcher).split()),
                         set(packager.ARCHES))
        self.assertEqual(packager.audit_runtime(app / "Contents/Resources/runtime/arm64", "arm64")["source_id"], source_id)
        self.assertEqual(packager.audit_runtime(app / "Contents/Resources/runtime/x86_64", "x86_64")["source_id"], source_id)
        result = subprocess.run([str(launcher), "literal space", "$HOME", "--flag"],
                                text=True, capture_output=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        expanded = self.base / "expanded"
        subprocess.run(["/usr/sbin/pkgutil", "--expand", str(package), str(expanded)],
                       check=True, text=True, capture_output=True, timeout=20)
        package_info = next(expanded.rglob("PackageInfo"))
        package_info_text = package_info.read_text()
        self.assertIn('install-location="/Applications"', package_info_text)
        scripts = [path.name for path in expanded.rglob("Scripts/*")
                   if path.is_file() and not path.name.startswith("._")]
        self.assertTrue(set(scripts).issubset({"preinstall"}), scripts)

    def test_assembly_validates_and_embeds_notice_bundle(self):
        source_id = "e" * 40
        arm, _, _ = self.runtime("arm64", source_id)
        intel, _, _ = self.runtime("x86_64", source_id)
        notices = self.base / "notices"; notices.mkdir(); (notices / "LICENSES").mkdir()
        (notices / "LICENSES/example.txt").write_text("notice")
        manifest = {"schema": 1, "source_id": source_id,
          "runtime_manifest_sha256": {"arm64": packager.digest(arm / "standalone.json"), "x86_64": packager.digest(intel / "standalone.json")},
          "files": {"LICENSES/example.txt": packager.digest(notices / "LICENSES/example.txt")}}
        (notices / "notices.json").write_text(json.dumps(manifest))
        output = self.base / "notice-output"
        packager.assemble(SimpleNamespace(arm64=arm, x86_64=intel, output=output, version="1.2.3", notices_dir=notices))
        self.assertEqual((output / "VectorWarp.app/Contents/Resources/ThirdPartyNotices/LICENSES/example.txt").read_text(), "notice")
        self.assertEqual(packager.digest(arm / "standalone.json"), manifest["runtime_manifest_sha256"]["arm64"])
        self.assertEqual(packager.digest(intel / "standalone.json"), manifest["runtime_manifest_sha256"]["x86_64"])
        expanded = self.base / "notice-expanded"
        packager.run('/usr/sbin/pkgutil', '--expand-full',
                     output / 'VectorWarp-universal-local.pkg', expanded)
        exported = list(expanded.rglob('ThirdPartyNotices/LICENSES/example.txt'))
        self.assertEqual(len(exported), 1)
        self.assertEqual(exported[0].read_text(), 'notice')

        for case in ('source', 'runtime-hash', 'altered', 'extra', 'unsafe-path',
                     'root-symlink', 'file-symlink', 'directory-symlink',
                     'manifest-symlink', 'fifo'):
            with self.subTest(case=case):
                candidate = self.base / ('notices-' + case)
                shutil.copytree(notices, candidate)
                candidate_metadata = candidate / 'notices.json'
                value = json.loads(candidate_metadata.read_text())
                if case == 'source':
                    value['source_id'] = 'f' * 40
                elif case == 'runtime-hash':
                    value['runtime_manifest_sha256']['x86_64'] = '0' * 64
                elif case == 'unsafe-path':
                    value['files']['../outside'] = '0' * 64
                elif case == 'altered':
                    (candidate / 'LICENSES/example.txt').write_text('altered')
                elif case == 'extra':
                    (candidate / 'extra.txt').write_text('unexpected')
                elif case == 'root-symlink':
                    alias = self.base / 'notice-alias'
                    alias.symlink_to(candidate, target_is_directory=True)
                    candidate = alias
                elif case == 'file-symlink':
                    target = candidate / 'LICENSES/example.txt'
                    target.unlink()
                    target.symlink_to(notices / 'LICENSES/example.txt')
                elif case == 'directory-symlink':
                    (candidate / 'escape').symlink_to(notices, target_is_directory=True)
                elif case == 'manifest-symlink':
                    candidate_metadata.unlink()
                    candidate_metadata.symlink_to(notices / 'notices.json')
                elif case == 'fifo':
                    os.mkfifo(candidate / 'special')
                if case != 'manifest-symlink':
                    candidate_metadata.write_text(json.dumps(value))
                rejected_output = self.base / ('rejected-' + case)
                with self.assertRaisesRegex(ValueError, 'notices'):
                    packager.assemble(SimpleNamespace(arm64=arm, x86_64=intel,
                        output=rejected_output, version='1.2.3', notices_dir=candidate))
                self.assertFalse(rejected_output.exists(), 'invalid notices created output')
        with self.assertRaisesRegex(ValueError, 'overlap'):
            packager.assemble(SimpleNamespace(arm64=arm, x86_64=intel,
                output=notices / 'overlap', version='1.2.3', notices_dir=notices))
        self.assertFalse((notices / 'overlap').exists())

    def test_relocator_resolves_a_dylib_dependency_through_parent_rpath(self):
        """A copied dylib's @rpath inherits the executable's original runpath."""
        relocator = self.inherited_rpath_runtime(local_arch())
        relocator.relocate()

    def test_assembly_treats_equivalent_minimum_os_spellings_as_equal(self):
        """15.0 and 15.0.0 name the same deployment target."""
        arm, _, _ = self.runtime("arm64", "d" * 40)
        intel, _, _ = self.runtime("x86_64", "d" * 40)
        metadata = json.loads((intel / "standalone.json").read_text())
        metadata["minimum_os"] = "15.0.0"
        metadata["files"] = packager.files_manifest(intel)
        (intel / "standalone.json").write_text(json.dumps(metadata, sort_keys=True))
        packager.assemble(SimpleNamespace(arm64=arm, x86_64=intel,
                                          output=self.base / "normalized", version="1.2.3"))

    def test_assembly_rejects_output_nested_in_an_input_runtime(self):
        arm, _, _ = self.runtime("arm64", "e" * 40)
        intel, _, _ = self.runtime("x86_64", "e" * 40)
        arguments = SimpleNamespace(arm64=arm, x86_64=intel,
                                    output=arm / "nested-output", version="1.2.3")
        with self.assertRaisesRegex(ValueError, "output and input trees must not overlap"):
            packager.assemble(arguments)
        self.assertFalse(arguments.output.exists())

    def test_preinstall_refuses_a_running_app_without_mutating_it(self):
        volume = self.base / "mounted volume"
        probe = volume / "Applications/VectorWarp.app/Contents/MacOS/probe"
        probe.parent.mkdir(parents=True)
        source = self.base / "pause.c"
        source.write_text("#include <unistd.h>\nint main(void) { for (;;) pause(); }\n")
        self.compile(local_arch(), probe, source)
        before = probe.read_bytes()
        process = subprocess.Popen([str(probe)])
        preinstall = ROOT / "packaging/macos/preinstall"
        try:
            for _ in range(20):
                listing = subprocess.run(["/bin/ps", "-axo", "comm="], text=True,
                                         capture_output=True, check=True).stdout
                if str(probe) in listing:
                    break
                time.sleep(0.05)
            else:
                self.fail("owned probe did not appear in ps command paths")
            blocked = subprocess.run(["/bin/sh", str(preinstall), "dummy.pkg", "/Applications", str(volume)],
                                     text=True, capture_output=True, timeout=10)
            self.assertNotEqual(blocked.returncode, 0)
            self.assertIn("VectorWarp is running", blocked.stderr)
            self.assertIn("stop", blocked.stderr)
            self.assertIsNone(process.poll())
            self.assertEqual(probe.read_bytes(), before)
        finally:
            if process.poll() is None:
                process.send_signal(signal.SIGTERM)
                process.wait(timeout=10)
        clear = subprocess.run(["/bin/sh", str(preinstall), "dummy.pkg", "/Applications", str(volume)],
                               text=True, capture_output=True, timeout=10)
        self.assertEqual(clear.returncode, 0, clear.stdout + clear.stderr)


if __name__ == "__main__":
    unittest.main()
