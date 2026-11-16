#!/usr/bin/env python3
"""Build policy checks only: no compiler, GPU, SDR, or installed service needed."""
import pathlib
import shutil
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]


@unittest.skipUnless(shutil.which("cmake"), "CMake is required")
class GpuPlatformPolicyTest(unittest.TestCase):
    def policy(self, system, gpu="AUTO", advanced="AUTO"):
        with tempfile.TemporaryDirectory(prefix="vectorwarp-gpu-policy-") as temp:
            script = pathlib.Path(temp) / "policy.cmake"
            script.write_text(
                f'include("{ROOT}/cmake/GpuPlatformPolicy.cmake")\n'
                'blah2_gpu_platform_policy("${platform}" "${gpu}" "${advanced}" resident resident_reason wideband wideband_reason)\n'
                'message(STATUS "resident=${resident}; resident_reason=${resident_reason}; wideband=${wideband}; wideband_reason=${wideband_reason}")\n'
            )
            return subprocess.run(
                ["cmake", f"-Dplatform={system}", f"-Dgpu={gpu}",
                 f"-Dadvanced={advanced}", "-P", str(script)],
                capture_output=True, text=True, timeout=10,
            )

    def test_linux_native_modules_require_runtime_qualification(self):
        for gpu, advanced in (("AUTO", "AUTO"), ("ON", "AUTO"), ("AUTO", "ON")):
            with self.subTest(gpu=gpu, advanced=advanced):
                result = self.policy("Linux", gpu, advanced)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertIn("resident=TRUE", result.stdout)
                self.assertIn("wideband=TRUE", result.stdout)
                self.assertIn("runtime-qualified shaderFloat64", result.stdout)
                self.assertIn("runtime-qualified FP32", result.stdout)

    def test_macos_builds_fp32_wideband_without_claiming_residency(self):
        for gpu in ("AUTO", "ON"):
            with self.subTest(gpu=gpu):
                result = self.policy("Darwin", gpu)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertIn("resident=FALSE", result.stdout)
                self.assertIn("resident FP64 arithmetic proof is Linux-only", result.stdout)
                self.assertIn("wideband=TRUE", result.stdout)
                self.assertIn("runtime-qualified FP32", result.stdout)

    def test_required_native_module_rejects_unsupported_platforms(self):
        for system in ("Windows", "FreeBSD"):
            with self.subTest(system=system):
                result = self.policy(system, "ON", "ON")
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("BLAH2_GPU_ADVANCED=ON cannot be satisfied", result.stderr)
                self.assertIn(f"unsupported on {system}", result.stderr)

    def test_cpu_and_stage_only_builds_are_explicit(self):
        for system, gpu, advanced, reason in (
            ("Linux", "OFF", "AUTO", "BLAH2_GPU=OFF"),
            ("Darwin", "OFF", "AUTO", "BLAH2_GPU=OFF"),
            ("Linux", "ON", "OFF", "BLAH2_GPU_ADVANCED=OFF"),
        ):
            with self.subTest(system=system, gpu=gpu, advanced=advanced):
                result = self.policy(system, gpu, advanced)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertIn("resident=FALSE", result.stdout)
                self.assertIn("wideband=FALSE", result.stdout)
                self.assertIn(reason, result.stdout)
        result = self.policy("Linux", "OFF", "ON")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("disabled by BLAH2_GPU=OFF", result.stderr)

    def test_invalid_option_values_fail_configuration(self):
        for gpu, advanced, option in (
            ("on", "AUTO", "BLAH2_GPU"),
            ("ON", "required", "BLAH2_GPU_ADVANCED"),
        ):
            result = self.policy("Linux", gpu, advanced)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn(f"{option} must be AUTO, ON or OFF", result.stderr)


class GpuBuildContractTest(unittest.TestCase):
    def test_production_has_no_pi_specific_frozen_dsp_pipeline(self):
        processor = (ROOT / "src/blah2.cpp").read_text()
        cmake = (ROOT / "cmake/GpuAcceleration.cmake").read_text()
        bench_cmake = (ROOT / "bench/CMakeLists.txt").read_text()
        for source in (processor, cmake, bench_cmake):
            for token in ("VECTORWARP_MIXED_AUTO", "pi4Host", "blah2MixedWorker",
                          "process/mixed/", "mixed::"):
                self.assertNotIn(token, source)
        self.assertIn("blah2::Acceleration acceleration(", processor)
        self.assertIn("acceleration.processClutter(", processor)
        self.assertIn("acceleration.process(", processor)
        for path in ("script/build-native.sh", "script/build-macos.sh",
                     "script/package-native.sh", "script/install-native.sh",
                     "packaging/pi/verify-pi4-image.sh"):
            source = (ROOT / path).read_text()
            self.assertIn("historical mixed DSP workers are not part of the unified", source)
            refusal = next(line for line in source.splitlines()
                           if "[[ ! -e $" in line and "/bin/blah2-mixed-worker" in line)
            self.assertIn("&& ! -L $", refusal)
        image = (ROOT / "packaging/pi/verify-pi4-image.sh").read_text()
        required_workers = next(line for line in image.splitlines()
                                if "required_elfs=" in line)
        self.assertNotIn("mixed", required_workers)
        for path in ("docs/MACOS.md", "docs/SDRPLAY_SETUP.md"):
            source = (ROOT / path).read_text()
            for receiver in ("cpu", "usrp", "hackrf", "rspduo", "kraken"):
                self.assertNotIn(f"--backend {receiver}", source)

    def test_runtime_gpu_receipts_use_exact_device_identity(self):
        portable = (ROOT / "src/process/ambiguity/Acceleration.cpp").read_text()
        wideband = (ROOT / "src/process/ambiguity/GpuWidebandModule.cpp").read_text()
        self.assertIn("status_.device = gpu_->device().id", portable)
        self.assertIn("deviceName()const override{return context_.candidate.info.id;}", wideband)
        self.assertIn('status_.gpuExecutionBackend = "vulkan"', portable)
        self.assertIn('clutterStatus_.gpuExecutionBackend = "vulkan"', portable)

    def test_portable_lane_transport_uses_exact_mapping_and_private_allocation_limit(self):
        transport = (ROOT / "src/process/ambiguity/GpuProcess.cpp").read_text()
        self.assertIn("uint64_t gpuProcessMappingBytes(const GpuGeometry& geometry)", transport)
        self.assertIn("const uint64_t bytes = Layout(geometry).bytes", transport)
        self.assertIn("bytes > UINT64_MAX - (uint64_t(page) - 1)", transport)
        self.assertIn("((bytes + uint64_t(page) - 1) / uint64_t(page)) * uint64_t(page)", transport)
        self.assertIn("if (options_.allocationLimitBytes)", transport)
        self.assertIn('rfind("BLAH2_GPU_ALLOCATION_LIMIT_BYTES=", 0)', transport)
        self.assertIn("argv, childEnvironmentPointers.data()", transport)
        self.assertNotIn('setenv("BLAH2_GPU_ALLOCATION_LIMIT_BYTES"', transport)
        fixture = (ROOT / "test/gpu/TestGpuProcess.cpp").read_text()
        self.assertIn("Portable mapping reservation lost physical page rounding", fixture)
        self.assertIn("Lane construction mutated the parent's environment", fixture)

    def test_auto_uses_correctness_and_health_not_comparative_speed(self):
        acceleration = (ROOT / "src/process/ambiguity/Acceleration.cpp").read_text()
        mixed = (ROOT / "src/process/mixed/MixedAutoPolicy.h").read_text()
        wideband = (ROOT / "src/process/ambiguity/GpuWidebandFallback.h").read_text()
        for source in (acceleration, mixed):
            self.assertNotIn(".95", source)
            self.assertNotIn("CPU is faster", source)
            self.assertNotIn("CPU clutter is faster", source)
            self.assertNotIn("slowWindows_", source)
        for metric in ("status_.cpuMs = median(cpuTimes_)",
                       "status_.gpuMs = median(gpuTimes_)",
                       "clutterStatus_.cpuMs = median(clutterCpuTimes_)",
                       "clutterStatus_.gpuMs = median(clutterGpuTimes_)"):
            self.assertIn(metric, acceleration)
        self.assertIn("GPU clutter accuracy check failed", acceleration)
        self.assertIn("Mixed full complex-map accuracy check failed", mixed)
        self.assertIn("Capture backlog grew during mixed processing", mixed)
        self.assertIn("gpuMilliseconds>deadlineMilliseconds", wideband)
        self.assertNotIn("cpuMilliseconds", wideband)
        # Clutter/FIR composition and ambiguity use the same admission class.
        fixture = (ROOT / "test/unit/process/ambiguity/TestAccelerationFallback.cpp").read_text()
        self.assertIn("Slower-but-correct GPU timing evidence", fixture)
        self.assertIn("AUTO rejected slower-but-correct GPU clutter or ambiguity", fixture)

    @unittest.skipUnless(shutil.which("cmake"), "CMake is required")
    def test_standalone_gpu_project_resolves_shared_socket_dependencies(self):
        with tempfile.TemporaryDirectory(prefix="vectorwarp-gpu-standalone-") as temp:
            result = subprocess.run([
                "cmake", "-S", str(ROOT / "test/gpu"), "-B", temp,
                "-DBLAH2_GPU=OFF", "-DBUILD_TESTING=ON"],
                capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_native_targets_and_capabilities_are_receiver_independent(self):
        cmake = (ROOT / "cmake/GpuAcceleration.cmake").read_text()
        conditions = []
        guarded_targets = set()
        for line in cmake.splitlines():
            line = line.strip()
            if line.startswith("if("):
                conditions.append(line)
            elif line.startswith("endif("):
                conditions.pop()
            for target in ("blah2GpuResidentVulkan", "blah2GpuWidebandVulkan"):
                if line.startswith(f"add_library({target} "):
                    guarded_targets.add(target)
                    expected = ("if(BLAH2_GPU_RESIDENT_ELIGIBLE)" if
                                target == "blah2GpuResidentVulkan" else
                                "if(BLAH2_GPU_WIDEBAND_ELIGIBLE)")
                    self.assertIn(expected, conditions)
                    self.assertFalse(any("KRAKEN" in item or "RECEIVER" in item
                                         or "ENABLE_USRP" in item or "ENABLE_RSPDUO" in item
                                         or "ENABLE_HACKRF" in item for item in conditions))
        self.assertEqual(guarded_targets, {"blah2GpuResidentVulkan", "blah2GpuWidebandVulkan"})
        for feature in ("RESIDENT", "WIDEBAND"):
            for target in ("blah2", "blah2GpuWorker"):
                self.assertIn(
                    f"target_compile_definitions({target} PRIVATE VECTORWARP_{feature}_GPU=1)",
                    cmake,
                )
        self.assertIn('OR BLAH2_GPU_ADVANCED STREQUAL "ON"', cmake)
        self.assertIn("GPU build capabilities:", cmake)

    def test_fp32_wideband_does_not_require_shader_float64(self):
        context = (ROOT / "core/VulkanBackend.private.cpp").read_text()
        wideband = (ROOT / "src/process/ambiguity/GpuWidebandModule.cpp").read_text()
        self.assertIn("Arithmetic { fp32, fp64 }", context)
        self.assertIn("arithmetic == Arithmetic::fp64 && !supported.shaderFloat64", context)
        self.assertIn("Context::Arithmetic::fp32", wideband)
        self.assertIn("VECTORWARP_GPU_FP32_MODULE", wideband)

    def test_wideband_transport_enforces_worker_descriptor_isolation(self):
        source = (ROOT / "src/process/ambiguity/GpuWidebandProcess.cpp").read_text()
        self.assertIn("POSIX_SPAWN_CLOEXEC_DEFAULT", source)
        self.assertIn("posix_spawn_file_actions_addinherit_np(&actions,fd)", source)
        self.assertIn("posix_spawn_file_actions_addclosefrom_np(&actions,5)", source)
        self.assertIn('opendir("/proc/self/fd")', source)
        self.assertLess(source.index("const bool standardOpen"), source.index("memory_=int(syscall"))
        self.assertIn("if(attributesReady)posix_spawnattr_destroy", source)
        fixture = (ROOT / "test/unit/process/ambiguity/TestGpuWidebandProcess.cpp").read_text()
        self.assertIn("F_DUPFD,100", fixture)
        self.assertIn("VW_MOCK_WIDEBAND_CLOSED_STD", fixture)

    def test_worker_dispatch_checks_compiled_modules_not_host_os(self):
        worker = (ROOT / "src/process/ambiguity/GpuWorker.cpp").read_text()
        self.assertIn("#ifdef VECTORWARP_RESIDENT_GPU", worker)
        self.assertIn("#ifdef VECTORWARP_WIDEBAND_GPU", worker)
        self.assertNotIn("#ifdef __linux__", worker)
        native = (ROOT / "core/VulkanBackend.private.cpp").read_text()
        self.assertIn('#if !defined(__linux__) && !defined(VECTORWARP_GPU_FP32_MODULE)', native)

    def test_policy_runs_on_linux_and_both_macos_release_architectures(self):
        command = "python3 test/packaging/test_gpu_platform_contract.py -v"
        for workflow in ("ci.yml", "release-packages.yml", "macos.yml"):
            source = (ROOT / ".github/workflows" / workflow).read_text()
            self.assertIn(command, source, workflow)
        macos = (ROOT / ".github/workflows/macos.yml").read_text()
        self.assertIn("runner: [macos-15, macos-15-intel]", macos)

    def test_physical_receipts_are_required_and_self_hosted_work_is_trusted(self):
        qualification = (ROOT / ".github/workflows/gpu-qualification.yml").read_text()
        self.assertIn("workflow_dispatch:", qualification)
        self.assertNotIn("  pull_request:", qualification)
        self.assertNotIn("  push:", qualification)
        self.assertIn("report.get('protected') is not True", qualification)
        self.assertIn("report['commit']['sha'] != os.environ['GITHUB_SHA']", qualification)
        for feature in ("FP32", "FP64"):
            self.assertIn(f"vars.VECTORWARP_GPU_{feature}_RUNNER_LABELS", qualification)
        self.assertIn("--backend all --gpu on", qualification)
        self.assertIn("max-parallel: 1", qualification)
        self.assertIn("--source-sha", qualification)
        gate = (ROOT / ".github/workflows/gpu-receipt-gate.yml").read_text()
        self.assertIn("'gpu-qualification.yml'", gate)
        self.assertIn("run['headSha'] == sha", gate)
        self.assertIn("run['event'] == 'workflow_dispatch'", gate)
        self.assertIn("run['conclusion'] == 'success'", gate)
        self.assertIn("verify_qualification_receipts.py", gate)
        release = (ROOT / ".github/workflows/release-packages.yml").read_text()
        self.assertIn("needs: [validate, qualified-linux-gpu]", release)
        for name in ("macos.yml", "macos-standalone.yml"):
            source = (ROOT / ".github/workflows" / name).read_text()
            self.assertIn("test/macos/standalone_driver_probe.py", source)
            self.assertNotIn("test/gpu/product_parity_test.py", source)
            self.assertIn("macos-15-intel", source)


if __name__ == "__main__":
    unittest.main()
