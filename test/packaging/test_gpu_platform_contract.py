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
    def test_vulkan_resources_are_shared_but_backend_policy_stays_local(self):
        ambiguity = ROOT / "src/process/ambiguity"
        shared = (ambiguity / "VulkanResources.private.h").read_text()
        for marker in ("struct Buffer", "struct Kernel", "struct NativePlan", "multiplySource", "chirpPrepareSource"):
            self.assertTrue(marker in shared, f"canonical resource missing: {marker}")
        for path, action in ((ambiguity / "VulkanBackend.cpp", "using CPU"),
                             (ROOT / "core/VulkanBackend.private.cpp", "resident route stopped")):
            source = path.read_text()
            self.assertTrue('VulkanResources.private.h"' in source)
            self.assertTrue(f'memoryFailureAction = "{action}"' in source)
            for marker in ("struct Buffer", "struct Kernel", "struct NativePlan", "multiplySource =", "chirpPrepareSource ="):
                self.assertFalse(marker in source, f"duplicate resource returned: {marker}")
            for marker in ("struct Instance", "struct Context", "budgetAllocateMemory"):
                self.assertTrue(marker in source, f"backend policy moved: {marker}")

    def test_portable_factory_has_one_owner_and_advanced_modules_keep_identity(self):
        ambiguity = ROOT / "src/process/ambiguity"
        portable = (ambiguity / "VulkanBackend.cpp").read_text()
        private = (ROOT / "core/VulkanBackend.private.cpp").read_text()
        for marker in ("class VulkanBackend final", "blah2_gpu_create("):
            self.assertTrue(marker in portable, f"portable owner missing {marker}")
            self.assertFalse(marker in private, f"duplicate portable implementation: {marker}")
        for marker in ("blah2_gpu_driver_status", "blah2_gpu_devices"):
            self.assertTrue(marker in private, f"advanced module identity missing {marker}")
        worker = (ambiguity / "GpuWorker.cpp").read_text()
        for factory in ("blah2_gpu_create", "blah2_gpu_create_resident", "blah2_gpu_create_wideband"):
            self.assertTrue(f'"{factory}"' in worker, f"worker factory missing {factory}")

    def test_resident_private_pipeline_only_accepts_captured_reference(self):
        pipeline = (ROOT / "core/GpuResidentFrame.private.h").read_text()
        reference = (ROOT / "core/GpuReferenceSpectrum.private.h").read_text()
        for marker in ("ReferenceMode::supplied", "residentSuppliedReference",
                       "bool prepareReference(", "bool finishRadar(", "probePositiveLog"):
            self.assertFalse(marker in pipeline, f"unreachable private route returned: {marker}")
        self.assertIn("capturedArray=1, capturedDedicated=2", reference)
        self.assertTrue("GPU_RESIDENT_UNSUPPORTED reference mode" in pipeline)
        self.assertTrue("supplied_reference=0" in pipeline)  # Stable qualification field.
        self.assertFalse("positive-log-probe" in reference)

    def test_ready_reply_drain_and_resident_control_deadlines(self):
        ambiguity = ROOT / "src/process/ambiguity"
        policy = (ambiguity / "GpuWorkerLifetime.h").read_text()
        self.assertLess(policy.index("const int ready=poll"), policy.index("if(Clock::now()>=deadline)"))
        self.assertIn("std::max<int64_t>(0", policy)
        wideband = (ambiguity / "GpuWidebandProcess.cpp").read_text()
        resident = (ambiguity / "GpuResidentProcess.cpp").read_text()
        for source in (wideband, resident):
            self.assertIn("gpu_receive_deadline::waitReadable(fd,deadline", source)
            self.assertIn("MSG_DONTWAIT", source)
        self.assertIn("r=reply(pendingBegin_+std::chrono::milliseconds(options_.frameMs))", wideband)
        self.assertIn("const auto ready=reply(startupDeadline)", wideband)
        self.assertNotIn("1-ms floor", wideband)
        for operation in ("commit", "abort", "published", "restore", "resetInput"):
            start = resident.index(f"auto p=request(Op::{operation})")
            send = resident.index("transport::sendPacket(child_->fd(),p)", start)
            self.assertIn("deadline_=Clock::now()+std::chrono::milliseconds", resident[start:send])
        fixture = (ROOT / "test/gpu/TestGpuResidentProcess.cpp").read_text()
        for marker in ("Queued resident reply rejected after deadline", "Delayed control acknowledgments", '"commit-timeout"', '"published-timeout"'):
            self.assertIn(marker, fixture)

    def test_live_frame_diagnostics_are_inert_and_offline_output_is_deferred(self):
        ambiguity = ROOT / "src/process/ambiguity"
        policy = (ambiguity / "GpuWorkerLifetime.h").read_text()
        self.assertIn("appendEnvironment", policy)
        self.assertIn("environment.erase", policy)
        self.assertIn("gpu_worker_lifetime::expectedParent()!=getppid()", policy)
        self.assertIn("offline?parseLimit", policy)
        for filename in ("GpuWorkerTransport.private.h", "GpuWidebandProcess.cpp"):
            source = (ambiguity / filename).read_text()
            self.assertIn("gpu_offline_diagnostics::appendEnvironment(childEnvironment,options_.offlineTransportTrace)", source)
        profile = (ambiguity / "GpuClutterProfile.h").read_text()
        self.assertNotIn("std::cerr", profile)
        self.assertNotIn("std::getenv", profile)
        self.assertIn("gpu_offline_diagnostics::frameLimit()", profile)
        deferred = (ambiguity / "GpuDeferredDiagnostics.h").read_text()
        self.assertIn("void flush()noexcept{active_=false;}", deferred)
        self.assertIn("deferredOutput().append", deferred)
        self.assertIn("maximumBytes=64*65536", deferred)
        self.assertNotIn("active_?stream_:std::cerr", deferred)
        self.assertIn("context_.diagnostics.stream()<<std::setprecision(12)<<\"GPU_TRACKER_COMMIT", (ROOT / "core/GpuTracker.private.h").read_text())
        fixture = (ROOT / "test/gpu/TestFrameDiagnostics.cpp").read_text()
        self.assertIn("Blocked stderr delayed reply/object destruction", fixture)

    def test_wideband_generic_fp32_admission_is_not_a_sample_rate_policy(self):
        root = ROOT / "src/process/ambiguity"
        planner = (root / "GpuWidebandChannelizer.cpp").read_text()
        module = (root / "GpuWidebandModule.cpp").read_text()
        self.assertNotIn("2400000", planner)
        self.assertNotIn("2400000", module)
        self.assertIn("Context::Arithmetic::fp32", module)
        self.assertIn("maxStorageBufferRange", module)
        self.assertIn("layout_.totalBytes > (512ULL << 20)", planner)
        self.assertIn("VW_SECOND_OUTPUT_RATE", (ROOT / "test/gpu/TestGpuWidebandPhysical.cpp").read_text())

    def test_wideband_trace_policy_change_has_a_new_private_wire_version(self):
        transport = (ROOT / "src/process/ambiguity/GpuWidebandProcess.cpp").read_text()
        self.assertIn("constexpr uint32_t magic=0x42574702", transport)
        self.assertNotIn("0x42574701", transport)
        fixture = (ROOT / "test/unit/process/ambiguity/TestGpuWidebandProcess.cpp").read_text()
        self.assertIn('"legacy-header"', fixture)
        self.assertIn('"legacy-reply"', fixture)

    def test_all_worker_routes_bind_the_spawning_parent_without_rejecting_pid_one(self):
        root = ROOT / "src/process/ambiguity"
        for name in ("GpuWorkerTransport.private.h", "GpuWidebandProcess.cpp"):
            source = (root / name).read_text()
            self.assertIn("gpu_worker_lifetime::bindParent(childEnvironment)", source, name)
        for name in ("GpuProcess.cpp", "GpuResidentProcess.cpp", "GpuWidebandProcess.cpp"):
            source = (root / name).read_text()
            self.assertIn("gpu_worker_lifetime::protectWorker()", source, name)
            self.assertNotRegex(source, r"parent\s*==\s*1", name)
        for name, policy in (("GpuProcess.cpp", "portable"), ("GpuResidentProcess.cpp", "resident")):
            source = (root / name).read_text()
            self.assertIn('#include "GpuWorkerTransport.private.h"', source)
            self.assertIn("std::make_unique<gpu_worker_transport::Child>", source)
            self.assertIn("gpu_worker_transport::" + policy, source)
        self.assertFalse((root / "GpuResidentTransport.private.h").exists())
        helper = (root / "GpuWorkerLifetime.h").read_text()
        self.assertIn("getppid() != parent", helper)
        self.assertIn("PR_SET_PDEATHSIG", helper)
        self.assertIn("EVFILT_PROC", helper)
        tests = (ROOT / "cmake/GpuProcessTests.cmake").read_text()
        self.assertIn("NAME gpuWorkerLifetime", tests)
        self.assertIn("NAME gpuWorkerPidNamespace", tests)

    def test_retired_alternate_processor_sources_and_build_routes_are_absent(self):
        for path in ("src/process/mixed", "test/unit/process/mixed"):
            self.assertFalse(any(item.is_file() for item in (ROOT / path).rglob("*")), path)
        for path in ("CMakeLists.txt", "cmake/GpuAcceleration.cmake", "bench/Pipeline.cpp",
                     "src/blah2.cpp", "src/process/clutter/WienerHopf.cpp",
                     "src/process/ambiguity/Ambiguity.cpp", "src/process/ambiguity/Ambiguity.h"):
            source = (ROOT / path).read_text()
            for token in ("VECTORWARP_MIXED_AUTO", "process/mixed/", "blah2::mixed",
                          "testMixed", "mixedAutoPolicy", "mixedWorkerFaults",
                          "VECTORWARP_MIXED_FIR_BENCH", "VECTORWARP_GPU_BLOCKED_FULL_BENCH",
                          "VECTORWARP_GPU_PARTIAL_AMBIGUITY_BENCH"):
                self.assertNotIn(token, source, f"{path}: retired alternate processor {token}")
        source = (ROOT / "test/unit/process/ambiguity/TestAccelerationFallback.cpp").read_text()
        for fault in ('"nan-tail"', '"wrong-tail"'):
            self.assertIn(fault, source, "Retired full-map tail guards must live on the canonical path")

    def test_production_has_no_pi_specific_frozen_dsp_pipeline(self):
        processor = (ROOT / "src/blah2.cpp").read_text()
        cmake = (ROOT / "cmake/GpuAcceleration.cmake").read_text()
        bench_cmake = (ROOT / "bench/CMakeLists.txt").read_text()
        for source in (processor, cmake, bench_cmake):
            for token in ("VECTORWARP_MIXED_AUTO", "pi4Host", "blah2MixedWorker",
                          "process/mixed/", "mixed::"):
                self.assertNotIn(token, source)
        self.assertIn("blah2::Acceleration acceleration(", processor)
        pipeline = (ROOT / "src/process/pipeline/RadarPipeline.h").read_text()
        self.assertTrue("pipeline::processFrontend(" in processor)
        self.assertTrue("acceleration->processClutter(" in pipeline)
        self.assertTrue("acceleration->process(" in pipeline)
        for path in ("script/build-native.sh", "script/package-native.sh", "script/install-native.sh"):
            source = (ROOT / path).read_text()
            self.assertIn(' validate ', source)
            self.assertIn('artifact-plan.py', source)
        for path in ("script/build-macos.sh", "packaging/pi/verify-pi4-image.sh"):
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
        shared = (ROOT / "src/process/ambiguity/GpuWorkerTransport.private.h").read_text()
        self.assertIn("if (options_.allocationLimitBytes)", shared)
        self.assertIn('rfind("BLAH2_GPU_ALLOCATION_LIMIT_BYTES=", 0)', shared)
        self.assertIn("argv, childEnvironmentPointers.data()", shared)
        self.assertNotIn('setenv("BLAH2_GPU_ALLOCATION_LIMIT_BYTES"', shared)
        fixture = (ROOT / "test/gpu/TestGpuProcess.cpp").read_text()
        self.assertIn("Portable mapping reservation lost physical page rounding", fixture)
        self.assertIn("Lane construction mutated the parent's environment", fixture)

    def test_auto_uses_correctness_and_health_not_comparative_speed(self):
        acceleration = (ROOT / "src/process/ambiguity/Acceleration.cpp").read_text()
        wideband = (ROOT / "src/process/ambiguity/GpuWidebandFallback.h").read_text()
        for source in (acceleration, wideband):
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
        self.assertNotIn("gpuMilliseconds>deadlineMilliseconds", wideband)
        self.assertIn("!std::isfinite(gpuMilliseconds)", wideband)
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
        self.assertIn("posix_spawn_file_actions_addclosefrom_np(&actions,gpu_artifact_binding::active()?7:5)", source)
        self.assertIn("gpu_artifact_binding::spawnActions(actions,false)", source)
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
