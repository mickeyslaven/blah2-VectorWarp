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
    "open-test": ("Usrp,HackRF,Kraken", ("OFF", "ON", "ON")),
    "kraken": ("Kraken", ("OFF", "OFF", "OFF")),
    "rspduo": ("RspDuo,Kraken", ("ON", "OFF", "OFF")),
    "usrp": ("Usrp,Kraken", ("OFF", "ON", "OFF")),
    "hackrf": ("HackRF,Kraken", ("OFF", "OFF", "ON")),
    "all": ("Usrp,HackRF,Kraken", ("OFF", "ON", "ON")),
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
        compiler = """#!/bin/sh
if [ "${1:-}" = -dumpmachine ]; then
  printf '%s\\n' "${FAKE_COMPILER_MACHINE:-x86_64-linux-gnu}"
fi
exit 0
"""
        executable(self.tools / "cc", compiler)
        executable(self.tools / "c++", compiler)
        for name in ("cmake", "git", "curl", "tar", "zip", "unzip", "ninja"):
            executable(self.tools / name)
        executable(self.tools / "pkg-config", """#!/bin/sh
printf '%s\\n' "$*" >>"$PKG_CONFIG_LOG"
if [ "$*" = "--exists ${FAKE_MISSING_PKG:-}" ]; then exit 1; fi
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
        (uhd / "UHDConfigVersion.cmake").write_text("""set(PACKAGE_VERSION "4.1.0.5")
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
        self.assertIn("BLAH2_MODULE_USRP=1", flags)
        self.assertNotIn("BLAH2_MODULE_RSPDUO=1", flags)
        self.assertNotIn("BLAH2_KRAKEN_ONLY=1", flags)
        # Radio SDKs are selected by their own module, never required just to
        # launch the core processor or replay a recording from another radio.
        main_flags = (usrp_build / "CMakeFiles/blah2.dir/flags.make").read_text()
        self.assertNotIn("BLAH2_MODULE_USRP", main_flags)
        main_link = (usrp_build / "CMakeFiles/blah2.dir/link.txt").read_text()
        self.assertNotIn("libuhd", main_link)
        self.assertNotIn("sdrplay", main_link)
        self.assertNotIn("libhackrf", main_link)

    def test_each_backend_selects_only_its_dependencies_and_compile_flags(self):
        for backend, (receivers, flags) in RECEIVERS.items():
            with self.subTest(backend=backend):
                log = self.temp / "pkg-config.log"
                if log.exists():
                    log.unlink()
                environment = self.build_environment(backend in {"open-test", "usrp", "all"})
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
                self.assertIn(f"-DBLAH2_LOCAL_BUILD_RSPDUO={'ON' if backend == 'all' else 'OFF'}", result.stdout)
                self.assertIn(
                    "env CMAKE_POLICY_VERSION_MINIMUM=3.5 cmake -G Ninja", result.stdout)
                self.assertNotIn("VCPKG_FORCE_SYSTEM_BINARIES", result.stdout)
                calls = log.read_text(encoding="utf-8")
                self.assertEqual("libhackrf" in calls, backend in {"open-test", "hackrf", "all"})
                self.assertEqual("libusb-1.0" in calls, backend in {"open-test", "hackrf", "all"})
                self.assertEqual("BLAH2_SDRPLAY_INCLUDE_DIR" in result.stdout,
                                 backend == "rspduo")

        invalid = subprocess.run([
            "bash", str(BUILD_SCRIPT), "--backend", "airspy", "--preflight"
        ], cwd=ROOT, text=True, capture_output=True, check=False)
        self.assertNotEqual(invalid.returncode, 0)
        self.assertIn("open-test, kraken, rspduo, usrp, hackrf or all", invalid.stderr)

    def test_open_test_preflight_needs_uhd_and_hackrf_but_not_sdrplay(self):
        environment = self.build_environment(True)
        environment["BLAH2_SDRPLAY_INCLUDE_DIR"] = str(self.temp / "absent/include")
        environment["BLAH2_SDRPLAY_LIBRARY"] = str(self.temp / "absent/libsdrplay.so")
        result = subprocess.run([
            "bash", str(BUILD_SCRIPT), "--backend", "open-test", "--gpu", "off",
            "--preflight", "--deps-dir", str(self.dependencies),
            "--build-dir", str(self.temp / "open-test-preflight"),
        ], cwd=ROOT, env=environment, text=True, capture_output=True, check=False)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("backend=open-test receivers=Usrp,HackRF,Kraken", result.stdout)

    def test_missing_hackrf_transitive_headers_fail_before_dependency_build(self):
        for backend in ("hackrf", "all"):
            with self.subTest(backend=backend):
                environment = self.build_environment(backend == "all")
                environment["FAKE_MISSING_PKG"] = "libusb-1.0"
                result = subprocess.run([
                    "bash", str(BUILD_SCRIPT), "--backend", backend, "--gpu", "off",
                    "--preflight", "--deps-dir", str(self.dependencies),
                    "--build-dir", str(self.temp / f"missing-usb-{backend}"),
                ], cwd=ROOT, env=environment, text=True, capture_output=True, check=False)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("libusb development files", result.stderr)
                self.assertNotIn("vcpkg_cmake_prefix", result.stdout)

    def test_non_x86_vcpkg_configure_uses_required_system_tools(self):
        for architecture in ("aarch64", "arm64", "armv7l", "s390x", "ppc64le", "riscv64"):
            with self.subTest(architecture=architecture):
                environment = self.build_environment(False)
                environment["FAKE_UNAME"] = architecture
                if architecture in {"aarch64", "arm64"}:
                    environment["FAKE_COMPILER_MACHINE"] = "aarch64-linux-gnu"
                elif architecture == "armv7l":
                    environment["FAKE_COMPILER_MACHINE"] = "arm-linux-gnueabihf"
                result = subprocess.run([
                    "bash", str(BUILD_SCRIPT), "--backend", "kraken", "--gpu", "off",
                    "--dry-run", "--jobs", "1", "--deps-dir", str(self.dependencies),
                    "--build-dir", str(self.temp / f"build-{architecture}"),
                ], cwd=ROOT, env=environment, text=True, capture_output=True, check=False)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                expected = ("env CMAKE_POLICY_VERSION_MINIMUM=3.5 "
                            "VCPKG_FORCE_SYSTEM_BINARIES=1")
                if architecture in {"aarch64", "arm64", "armv7l"}:
                    self.assertIn("native-compiler-aliases", result.stdout)
                    self.assertIn("-gcc", result.stdout)
                    self.assertIn("-g++", result.stdout)
                    self.assertIn("PATH=", result.stdout)
                    self.assertIn(f"{expected} PATH=", result.stdout)
                else:
                    self.assertIn(f"{expected} cmake -G Ninja", result.stdout)

    def test_arm_aliases_reject_a_non_native_compiler(self):
        environment = self.build_environment(False)
        environment.update({
            "FAKE_UNAME": "aarch64",
            "FAKE_COMPILER_MACHINE": "x86_64-linux-gnu",
        })
        result = subprocess.run([
            "bash", str(BUILD_SCRIPT), "--backend", "kraken", "--gpu", "off",
            "--dry-run", "--jobs", "1", "--deps-dir", str(self.dependencies),
            "--build-dir", str(self.temp / "build-wrong-compiler"),
        ], cwd=ROOT, env=environment, text=True, capture_output=True, check=False)
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("does not match host aarch64", result.stderr)

    def make_artifact(self, backend: str, compiled_receivers: str | None,
                      test_only: str | None = None) -> Path:
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
        for name in ("vectorwarp-restart", "vectorwarp-wait-api.js", "vectorwarp-activate-web"):
            shutil.copy2(ROOT / "script" / name, artifact / "libexec" / name)
        lines = ["build_id=receiver-test", f"backend={backend}"]
        if compiled_receivers is not None:
            lines.append(f"compiled_receivers={compiled_receivers}")
        if test_only is not None:
            lines.append(f"test_only={test_only}")
        if backend == "all":
            lines.append("local_build_receivers=RspDuo")
            (artifact / "receiver-source/rspduo").mkdir(parents=True)
            (artifact / "receiver-source/rspduo/kit.json").write_text('{"schema":1,"receiver":"RspDuo"}\n')
            shutil.copy2(ROOT / "script/vectorwarp-build-sdrplay.py", artifact / "libexec/vectorwarp-build-sdrplay.py")
            (artifact / "libexec/vectorwarp-build-sdrplay.py").chmod(0o755)
            shutil.copy2(ROOT / "contrib/systemd/vectorwarp-sdrplay-build.service.in", artifact / "systemd/vectorwarp-sdrplay-build.service.in")
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
                artifact = self.make_artifact(backend, supplied,
                                              "true" if backend == "open-test" else "false")
                stage = self.temp / f"stage-{backend}"
                result = self.install(artifact, stage)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                unit = (stage / "usr/lib/systemd/system/vectorwarp-api.service").read_text(
                    encoding="utf-8")
                self.assertIn(f'Environment="BLAH2_RECEIVER_TYPES={receivers}"', unit)
                if backend == "all":
                    self.assertIn('Environment="BLAH2_SDRPLAY_LOCAL_BUILD=true"', unit)
                    self.assertIn('Environment="BLAH2_LOCAL_BUILD_RECEIVER_TYPES=RspDuo"', unit)
                    self.assertTrue((stage / "opt/vectorwarp/libexec/vectorwarp-build-sdrplay").is_file())

        legacy = self.make_artifact("kraken", None)
        legacy_stage = self.temp / "stage-legacy"
        result = self.install(legacy, legacy_stage)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('Environment="BLAH2_RECEIVER_TYPES=Kraken"',
                      (legacy_stage / "usr/lib/systemd/system/vectorwarp-api.service").read_text(
                          encoding="utf-8"))

        unmarked = self.make_artifact("open-test", "Usrp,HackRF,Kraken")
        result = self.install(unmarked, self.temp / "open-test-unmarked", "--no-systemd", "--preflight")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("explicitly marked test-only", result.stderr)

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

    def test_stable_local_kit_contract_rejects_missing_or_spoofed_kit_metadata(self):
        artifact = self.make_artifact("all", "Usrp,HackRF,Kraken", "false")
        manifest = artifact / ".vectorwarp-build"
        manifest.write_text(manifest.read_text().replace("local_build_receivers=RspDuo", "local_build_receivers=HackRF"))
        result = self.install(artifact, self.temp / "bad-local-kit", "--no-systemd", "--preflight")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("invalid local_build_receivers", result.stderr)
        manifest.write_text(manifest.read_text().replace("local_build_receivers=HackRF\n", ""))
        result = self.install(artifact, self.temp / "missing-local-kit", "--no-systemd", "--preflight")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("three compiled receivers", result.stderr)

    def test_receiver_helper_staging_preserves_separate_root_policy(self):
        artifact = self.make_artifact('kraken', 'Kraken')
        shutil.copy2(ROOT / 'script/vectorwarp-receiver-helper.py', artifact / 'libexec/vectorwarp-receiver-helper')
        shutil.copy2(ROOT / 'script/vectorwarp-receiver-apt.py', artifact / 'libexec/vectorwarp-receiver-apt.py')
        shutil.copy2(ROOT / 'script/vectorwarp-receiver-dnf.py', artifact / 'libexec/vectorwarp-receiver-dnf.py')
        for name in ('vectorwarp-receiver.service.in', 'vectorwarp-receiver.socket', 'vectorwarp-receiver-policy.json.in'):
            shutil.copy2(ROOT / 'contrib/systemd' / name, artifact / 'systemd' / name)
        stage = self.temp / 'stage-management'
        policy_dir = stage / 'etc/vectorwarp-management'
        policy_dir.mkdir(parents=True)
        policy = policy_dir / 'receivers.json'
        sentinel = b'{"reviewed-local-policy":"preserve byte-for-byte"}\n'
        policy.write_bytes(sentinel)
        result = self.install(artifact, stage)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(policy.read_bytes(), sentinel)
        self.assertEqual(policy_dir.stat().st_mode & 0o777, 0o755)
        self.assertTrue((stage / 'opt/vectorwarp/libexec/vectorwarp-receiver-helper').is_file())
        self.assertTrue((stage / 'opt/vectorwarp/libexec/vectorwarp-activate-web').is_file())
        self.assertTrue((stage / 'opt/vectorwarp/libexec/vectorwarp-receiver-apt.py').is_file())
        self.assertTrue((stage / 'usr/lib/systemd/system/vectorwarp-api.service.wants/vectorwarp-receiver.socket').is_symlink())
        service = (stage / 'usr/lib/systemd/system/vectorwarp-receiver.service').read_text()
        self.assertIn('ExecStart=/usr/bin/python3 -I /opt/vectorwarp/libexec/vectorwarp-receiver-helper serve', service)
        self.assertNotIn('receiver-helper', (stage / 'etc/sudoers.d/vectorwarp').read_text())
        api_unit = (stage / 'usr/lib/systemd/system/vectorwarp-api.service').read_text()
        self.assertIn('/opt/vectorwarp/libexec/vectorwarp-receiver-helper', api_unit)
        self.assertIn('request-restart', api_unit)
        self.assertNotIn('/usr/bin/sudo', api_unit)
        self.assertNotIn('vectorwarp-restart.service', (stage / 'etc/sudoers.d/vectorwarp').read_text())
        fresh_stage = self.temp / 'stage-management-default'
        result = self.install(artifact, fresh_stage)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(json.loads((fresh_stage / 'etc/vectorwarp-management/receivers.json').read_text())['actions'], [])

    def test_package_web_activation_defers_running_broker_and_api(self):
        for hook in ('packaging/deb/postinst', 'packaging/rpm/vectorwarp.spec.in'):
            self.assertIn('/opt/vectorwarp/libexec/vectorwarp-activate-web', (ROOT / hook).read_text())
        executable(self.tools / 'systemctl', '''#!/bin/sh
printf '%s\\n' "$*" >> "$ACTIVATION_LOG"
case "$*" in
  'is-active --quiet vectorwarp-api.service') [ "$API_ACTIVE" = 1 ]; exit $? ;;
  'is-active --quiet vectorwarp-receiver.service') [ "$BROKER_ACTIVE" = 1 ]; exit $? ;;
  'enable vectorwarp-api.service'|'enable --now vectorwarp-api.service')
    [ "$ENABLE_FAIL" != 1 ]; exit $? ;;
  *) exit 99 ;;
esac
''')
        for api_active, broker_active, failure in ((0, 0, 0), (1, 0, 0), (0, 1, 0), (0, 0, 1)):
            with self.subTest(api=api_active, broker=broker_active, failure=failure):
                log = self.temp / 'activation.log'
                log.unlink(missing_ok=True)
                env = os.environ | {'PATH': f'{self.tools}:{os.environ["PATH"]}',
                                    'ACTIVATION_LOG': str(log), 'API_ACTIVE': str(api_active),
                                    'BROKER_ACTIVE': str(broker_active), 'ENABLE_FAIL': str(failure)}
                result = subprocess.run(['sh', str(ROOT / 'script/vectorwarp-activate-web')],
                                        env=env, text=True, capture_output=True, check=False)
                calls = log.read_text().splitlines()
                self.assertEqual(result.returncode == 0, failure == 0)
                if api_active or broker_active:
                    self.assertIn('enable vectorwarp-api.service', calls)
                    self.assertNotIn('enable --now vectorwarp-api.service', calls)
                    self.assertIn('Do not refresh while a receiver-management action is running', result.stderr)
                    self.assertIn('systemctl restart vectorwarp-receiver.service && systemctl restart vectorwarp-api.service', result.stderr)
                else:
                    self.assertIn('enable --now vectorwarp-api.service', calls)
                self.assertFalse(any('vectorwarp-processor.service' in call for call in calls))

    def test_receiver_policy_symlink_is_rejected_before_staging_any_release(self):
        artifact = self.make_artifact('kraken', 'Kraken')
        shutil.copy2(ROOT / 'script/vectorwarp-receiver-helper.py', artifact / 'libexec/vectorwarp-receiver-helper')
        shutil.copy2(ROOT / 'script/vectorwarp-receiver-apt.py', artifact / 'libexec/vectorwarp-receiver-apt.py')
        shutil.copy2(ROOT / 'script/vectorwarp-receiver-dnf.py', artifact / 'libexec/vectorwarp-receiver-dnf.py')
        for name in ('vectorwarp-receiver.service.in', 'vectorwarp-receiver.socket', 'vectorwarp-receiver-policy.json.in'):
            shutil.copy2(ROOT / 'contrib/systemd' / name, artifact / 'systemd' / name)
        stage = self.temp / 'stage-management-link'
        (stage / 'etc').mkdir(parents=True)
        (stage / 'etc/vectorwarp-management').symlink_to(self.temp)
        result = self.install(artifact, stage)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('must not be a symlink', result.stderr)
        self.assertFalse((stage / 'opt/vectorwarp/current').exists())


if __name__ == "__main__":
    unittest.main()
