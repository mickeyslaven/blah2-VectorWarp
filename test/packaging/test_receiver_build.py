#!/usr/bin/env python3
"""Source-only acceptance for selectable live receiver build artifacts."""

from __future__ import annotations

import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
BUILD_SCRIPT = ROOT / "script/build-native.sh"
INSTALL_SCRIPT = ROOT / "script/install-native.sh"
RECEIVERS = {
    "kraken": ("Kraken", ("OFF", "OFF", "OFF")),
    "rspduo": ("RspDuo,Kraken", ("ON", "OFF", "OFF")),
    "usrp": ("Usrp,Kraken", ("OFF", "ON", "OFF")),
    "hackrf": ("HackRF,Kraken", ("OFF", "OFF", "ON")),
    "all": ("RspDuo,Usrp,HackRF,Kraken", ("ON", "ON", "ON")),
}


def executable(path: Path, body: str = "#!/bin/sh\nexit 0\n") -> None:
    path.write_text(body, encoding="utf-8")
    path.chmod(0o755)


class ReceiverBuildContractTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="vectorwarp-receivers-")
        self.temp = Path(self.temporary.name)
        self.tools = self.temp / "tools"
        self.tools.mkdir()
        executable(self.tools / "node", "#!/bin/sh\nprintf '24\\n'\n")
        executable(self.tools / "npm")
        executable(self.tools / "uname", "#!/bin/sh\nprintf '%s\\n' \"${FAKE_UNAME:-x86_64}\"\n")
        for name in ("cmake", "git", "curl", "tar", "zip", "unzip", "c++", "ninja"):
            executable(self.tools / name)
        executable(self.tools / "pkg-config", """#!/bin/sh
printf '%s\\n' "$*" >>"$PKG_CONFIG_LOG"
exit 0
""")
        self.dependencies = self.temp / "deps"
        (self.dependencies / "vcpkg/.git").mkdir(parents=True)
        executable(self.dependencies / "vcpkg/vcpkg")
        self.sdrplay = self.temp / "sdrplay"
        (self.sdrplay / "include").mkdir(parents=True)
        (self.sdrplay / "lib").mkdir()
        (self.sdrplay / "include/sdrplay_api.h").write_text("fixture\n", encoding="utf-8")
        (self.sdrplay / "lib/libsdrplay_api.so.3.15").write_text("fixture\n", encoding="utf-8")

    def tearDown(self):
        self.temporary.cleanup()

    def test_manifests_pin_the_gcc_compatible_rapidjson_snapshot(self):
        for relative in ("lib/vcpkg.json", "lib/vcpkg-kraken.json"):
            with self.subTest(manifest=relative):
                manifest = json.loads((ROOT / relative).read_text(encoding="utf-8"))
                dependency = next(item for item in manifest["dependencies"]
                                  if item["name"] == "rapidjson")
                override = next(item for item in manifest["overrides"]
                                if item["name"] == "rapidjson")
                self.assertEqual(dependency, {
                    "name": "rapidjson", "version>=": "2023-07-17"
                })
                self.assertEqual(override, {
                    "name": "rapidjson", "version-date": "2023-07-17"
                })

    def build_environment(self, with_uhd: bool) -> dict:
        uhd_tools = self.temp / "uhd-tools"
        if with_uhd:
            uhd_tools.mkdir(exist_ok=True)
            executable(uhd_tools / "uhd_config_info")
        environment = os.environ.copy()
        paths = [str(self.tools)]
        if with_uhd:
            paths.append(str(uhd_tools))
        paths.append(environment.get("PATH", "/usr/bin:/bin"))
        environment.update({
            "PATH": os.pathsep.join(paths),
            "PKG_CONFIG_LOG": str(self.temp / "pkg-config.log"),
            "BLAH2_SDRPLAY_INCLUDE_DIR": str(self.sdrplay / "include"),
            "BLAH2_SDRPLAY_LIBRARY": str(self.sdrplay / "lib/libsdrplay_api.so.3.15"),
        })
        return environment

    def cmake_fixture_arguments(self) -> list[str]:
        packages = self.temp / "cmake-packages"
        includes = self.temp / "cmake-includes"
        for package, target in (("asio", "asio::asio"), ("ryml", "ryml::ryml"),
                                ("httplib", "httplib::httplib")):
            directory = packages / package
            directory.mkdir(parents=True)
            (directory / f"{package}Config.cmake").write_text(
                f"add_library({target} INTERFACE IMPORTED)\n", encoding="utf-8")
        armadillo = packages / "Armadillo"
        armadillo.mkdir()
        (armadillo / "ArmadilloConfig.cmake").write_text(
            "set(Armadillo_FOUND TRUE)\n", encoding="utf-8")
        uhd = packages / "UHD"
        uhd.mkdir()
        (uhd / "UHDConfig.cmake").write_text(
            "set(UHD_FOUND TRUE)\nset(UHD_INCLUDE_DIRS \"\")\nset(UHD_LIBRARIES \"\")\n",
            encoding="utf-8")
        (uhd / "UHDConfigVersion.cmake").write_text("""set(PACKAGE_VERSION "4.8.0.0")
set(PACKAGE_VERSION_COMPATIBLE TRUE)
if(PACKAGE_FIND_VERSION VERSION_EQUAL PACKAGE_VERSION)
  set(PACKAGE_VERSION_EXACT TRUE)
endif()
""", encoding="utf-8")
        (includes / "rapidjson").mkdir(parents=True)
        (includes / "rapidjson/allocators.h").write_text("fixture\n", encoding="utf-8")
        return [
            "-DCMAKE_FIND_PACKAGE_PREFER_CONFIG=ON",
            f"-Dasio_DIR={packages / 'asio'}", f"-Dryml_DIR={packages / 'ryml'}",
            f"-Dhttplib_DIR={packages / 'httplib'}", f"-DArmadillo_DIR={armadillo}",
            f"-DUHD_DIR={uhd}", f"-DRAPIDJSON_INCLUDE_DIRS={includes}",
            "-DBUILD_TESTING=OFF", "-DBLAH2_GPU=OFF",
        ]

    def test_cmake_legacy_kraken_and_usrp_only_configurations_are_decoupled(self):
        cmake = shutil.which("cmake")
        if not cmake:
            self.skipTest("cmake is not installed in this bounded test environment")
        common = [cmake, "-S", str(ROOT), *self.cmake_fixture_arguments()]
        legacy_build = self.temp / "cmake-legacy-kraken"
        result = subprocess.run([
            *common, "-B", str(legacy_build), "-DBLAH2_KRAKEN_ONLY=ON",
            f"-DBLAH2_OUTPUT_DIR={legacy_build / 'bin'}",
        ], text=True, capture_output=True, check=False)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        cache = (legacy_build / "CMakeCache.txt").read_text(encoding="utf-8")
        for name in ("RSPDUO", "USRP", "HACKRF"):
            self.assertIn(f"BLAH2_ENABLE_{name}:BOOL=OFF", cache)
        legacy_flags = "\n".join(path.read_text(encoding="utf-8")
                                 for path in legacy_build.rglob("flags.make"))
        self.assertIn("BLAH2_KRAKEN_ONLY=1", legacy_flags)

        usrp_build = self.temp / "cmake-usrp"
        result = subprocess.run([
            # An explicit new option may override the legacy Kraken-only default.
            *common, "-B", str(usrp_build), "-DBLAH2_KRAKEN_ONLY=ON",
            "-DBLAH2_ENABLE_RSPDUO=OFF", "-DBLAH2_ENABLE_USRP=ON",
            "-DBLAH2_ENABLE_HACKRF=OFF", f"-DBLAH2_OUTPUT_DIR={usrp_build / 'bin'}",
            # These deliberately absent paths prove USRP does not load SDRplay.
            f"-DBLAH2_SDRPLAY_INCLUDE_DIR={self.temp / 'absent/include'}",
            f"-DBLAH2_SDRPLAY_LIBRARY={self.temp / 'absent/libsdrplay.so'}",
        ], text=True, capture_output=True, check=False)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        flags = "\n".join(path.read_text(encoding="utf-8") for path in usrp_build.rglob("flags.make"))
        self.assertIn("BLAH2_ENABLE_USRP=1", flags)
        self.assertNotIn("BLAH2_ENABLE_RSPDUO=1", flags)
        self.assertNotIn("BLAH2_KRAKEN_ONLY=1", flags)

    def test_each_backend_selects_only_its_dependencies_and_compile_flags(self):
        for backend, (receivers, flags) in RECEIVERS.items():
            with self.subTest(backend=backend):
                log = self.temp / "pkg-config.log"
                if log.exists():
                    log.unlink()
                environment = self.build_environment(backend in {"usrp", "all"})
                if backend == "usrp":
                    environment["BLAH2_SDRPLAY_INCLUDE_DIR"] = str(self.temp / "absent/include")
                    environment["BLAH2_SDRPLAY_LIBRARY"] = str(self.temp / "absent/libsdrplay.so")
                result = subprocess.run([
                    "bash", str(BUILD_SCRIPT), "--backend", backend, "--gpu", "off",
                    "--dry-run", "--jobs", "1", "--deps-dir", str(self.dependencies),
                    "--build-dir", str(self.temp / f"build-{backend}"),
                ], cwd=ROOT, env=environment, text=True, capture_output=True, check=False)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertIn(f"receivers={receivers}", result.stdout)
                for name, flag in zip(("RSPDUO", "USRP", "HACKRF"), flags):
                    self.assertIn(f"-DBLAH2_ENABLE_{name}={flag}", result.stdout)
                self.assertIn("cmake -G Ninja", result.stdout)
                self.assertNotIn("VCPKG_FORCE_SYSTEM_BINARIES", result.stdout)
                calls = log.read_text(encoding="utf-8")
                self.assertEqual("libhackrf" in calls, backend in {"hackrf", "all"})
                self.assertEqual("BLAH2_SDRPLAY_INCLUDE_DIR" in result.stdout,
                                 backend in {"rspduo", "all"})

        invalid = subprocess.run([
            "bash", str(BUILD_SCRIPT), "--backend", "airspy", "--preflight"
        ], cwd=ROOT, text=True, capture_output=True, check=False)
        self.assertNotEqual(invalid.returncode, 0)
        self.assertIn("kraken, rspduo, usrp, hackrf or all", invalid.stderr)

    def test_non_x86_vcpkg_configure_uses_required_system_tools(self):
        for architecture in ("aarch64", "arm64", "armv7l", "s390x", "ppc64le", "riscv64"):
            with self.subTest(architecture=architecture):
                environment = self.build_environment(False)
                environment["FAKE_UNAME"] = architecture
                result = subprocess.run([
                    "bash", str(BUILD_SCRIPT), "--backend", "kraken", "--gpu", "off",
                    "--dry-run", "--jobs", "1", "--deps-dir", str(self.dependencies),
                    "--build-dir", str(self.temp / f"build-{architecture}"),
                ], cwd=ROOT, env=environment, text=True, capture_output=True, check=False)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertIn("env VCPKG_FORCE_SYSTEM_BINARIES=1 cmake -G Ninja", result.stdout)

    def make_artifact(self, backend: str, compiled_receivers: str | None) -> Path:
        artifact = self.temp / f"artifact-{backend}-{len(list(self.temp.glob('artifact-*')))}"
        for directory in ("bin", "api", "html", "config-examples", "systemd", "libexec"):
            (artifact / directory).mkdir(parents=True, exist_ok=True)
        executable(artifact / "bin/blah2")
        (artifact / "api/server.js").write_text("fixture\n", encoding="utf-8")
        (artifact / "html/index.html").write_text("fixture\n", encoding="utf-8")
        for name in ("config.yml", "config-kraken.yml", "config-usrp.yml", "config-hackrf.yml"):
            shutil.copy2(ROOT / "config" / name, artifact / "config-examples" / name)
        for name in ("vectorwarp-api.service.in", "vectorwarp-processor.service.in",
                     "vectorwarp-restart.service.in", "vectorwarp.sysusers",
                     "vectorwarp.tmpfiles", "vectorwarp.sudoers.in"):
            shutil.copy2(ROOT / "contrib/systemd" / name, artifact / "systemd" / name)
        for name in ("vectorwarp-restart", "vectorwarp-wait-api.js"):
            shutil.copy2(ROOT / "script" / name, artifact / "libexec" / name)
        lines = ["build_id=receiver-test", f"backend={backend}"]
        if compiled_receivers is not None:
            lines.append(f"compiled_receivers={compiled_receivers}")
        (artifact / ".vectorwarp-build").write_text("\n".join(lines) + "\n", encoding="utf-8")
        return artifact

    def install(self, artifact: Path, stage: Path, *extra: str) -> subprocess.CompletedProcess:
        return subprocess.run([
            "bash", str(INSTALL_SCRIPT), "--artifact", str(artifact),
            "--destdir", str(stage), *extra,
        ], cwd=ROOT, text=True, capture_output=True, check=False)

    def test_installer_renders_the_validated_compiled_receiver_manifest(self):
        for backend, (receivers, _) in RECEIVERS.items():
            with self.subTest(backend=backend):
                # Order is not semantic; the installer emits one canonical list.
                supplied = ",".join(reversed(receivers.split(",")))
                artifact = self.make_artifact(backend, supplied)
                stage = self.temp / f"stage-{backend}"
                result = self.install(artifact, stage)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                unit = (stage / "usr/lib/systemd/system/vectorwarp-api.service").read_text(
                    encoding="utf-8")
                self.assertIn(f'Environment="BLAH2_RECEIVER_TYPES={receivers}"', unit)

        legacy = self.make_artifact("kraken", None)
        legacy_stage = self.temp / "stage-legacy"
        result = self.install(legacy, legacy_stage)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('Environment="BLAH2_RECEIVER_TYPES=Kraken"',
                      (legacy_stage / "usr/lib/systemd/system/vectorwarp-api.service").read_text(
                          encoding="utf-8"))

    def test_installer_rejects_unknown_duplicate_or_inaccurate_receiver_lists(self):
        for compiled in ("Usrp,Airspy,Kraken", "Usrp,Usrp,Kraken", "Usrp", ""):
            with self.subTest(compiled=compiled):
                artifact = self.make_artifact("usrp", compiled)
                result = self.install(artifact, self.temp / "unused-stage", "--no-systemd", "--preflight")
                self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        missing = self.make_artifact("usrp", None)
        result = self.install(missing, self.temp / "unused-stage", "--no-systemd", "--preflight")
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("lacks compiled_receivers", result.stderr)


if __name__ == "__main__":
    unittest.main()
