"""Source-only guard for the aggregate GPU quota; does not compile or spawn workers."""
from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[2]


class GpuLaneBudgetContract(unittest.TestCase):
    def test_host_work_overlap_is_explicit_and_default_precedes_submission(self):
        source = (ROOT / "src/process/ambiguity/GpuProcess.cpp").read_text()
        self.assertIn('std::getenv("BLAH2_GPU_CLUTTER_HOST_WORK_OVERLAP")', source)
        self.assertIn('return value && std::strcmp(value, "1") == 0;', source)
        self.assertIn("const bool clutterHostWorkOverlap_", source)
        process = source.split("bool processClutterFrameWithHostWork(", 1)[1]
        process = process.split("\n  void process(", 1)[0]
        before = process.index("if (hostWork && !clutterHostWorkOverlap_) hostWork();")
        submit = process.index("sendMessage(child_->fd(), request);")
        overlap = process.index("if (hostWork && clutterHostWorkOverlap_)")
        self.assertLess(before, submit)
        self.assertLess(submit, overlap)
        self.assertLess(overlap, process.index("receiveMessage(child_->fd(), remainingMs"))

    def test_both_allocators_consume_the_same_combined_quota(self):
        for relative in ("src/process/ambiguity/VulkanBackend.cpp",
                         "core/VulkanBackend.private.cpp"):
            with self.subTest(backend=relative):
                source = (ROOT / relative).read_text()
                self.assertRegex(source, re.compile(
                    r"const auto policy = gpu_memory::allocationPolicy\("
                    r"candidate\.info\.memoryBytes,\s*"
                    r'std::getenv\("BLAH2_GPU_ALLOCATION_LIMIT_BYTES"\)\);'))
                self.assertRegex(source, re.compile(
                    r"deviceAllocations\.try_emplace\(device, policy\.bytes,\s*"
                    r"memoryProperties\(candidate\.physical\), policy\.separateHostHeaps\);"))

    def test_override_cannot_add_a_second_host_allowance(self):
        source = (ROOT / "src/process/ambiguity/GpuMemory.h").read_text()
        self.assertIn("AllocationPolicy policy{heapBudget(heapBytes), bounded == nullptr}", source)
        self.assertIn("policy.bytes = std::min(policy.bytes, requested)", source)
        self.assertIn("requested > (maximumDeviceBytes - value) / 10", source)
        self.assertIn("++count > 20", source)
        self.assertIn("if (!count || !requested)", source)

    def test_portable_workers_share_memory_budget_and_use_local_transactions(self):
        source = (ROOT / "src/process/pipeline/CanonicalRadar.cpp").read_text()
        self.assertNotIn("!residentFrontend && !residentAttempted", source)
        self.assertIn("shouldAttemptPortableFrontend(config.accelerationMode", source)
        self.assertEqual(source.count("initializePortableAcceleration();"), 3)
        self.assertIn("gpu_worker_lifetime::replacementAvailable()", source)
        self.assertIn("laneIndex = qualificationLaneIndex", source)
        self.assertIn("shouldAttemptResidentFrontend(", source)
        self.assertIn("!fmReferenceFilter && !atscPilotProjector", source)
        self.assertIn("config.stages.clutter && residentRequired", source)
        self.assertIn("use auto or gpu", source)
        self.assertRegex(source, re.compile(
            r"options\.allocationLimitBytes = budget->allocationLimit\(laneIndex,\s*"
            r"gpuProcessMappingBytes\(geometry\)\)"))
        self.assertIn("createGpuProcess(geometry, device, options)", source)
        self.assertIn("return leaseGpuLane(std::move(worker), budget)", source)
        lease = (ROOT / "src/process/ambiguity/GpuLaneLease.h").read_text()
        self.assertIn("std::mutex transaction_;", lease)
        self.assertEqual(lease.count("std::unique_lock<std::mutex> lock(transaction_);"), 1)
        self.assertEqual(lease.count("transaction([&]"), 4)
        self.assertLess(lease.index("std::unique_lock<std::mutex> lock(transaction_);"),
                        lease.index("stream_profile::WorkScope scope("))
        for submission in ("backend_->process(reference, surveillance, output)",
                           "dynamic_cast<GpuFrameBackend&>(*backend_).processFrame()",
                           "dynamic_cast<GpuClutterFrameBackend&>(*backend_).processClutterFrame()"):
            self.assertIn(submission, lease)
        self.assertNotIn("process_loaded_cpi", lease)

    def test_resident_replacement_waits_for_real_worker_exit(self):
        transport = (ROOT / "src/process/ambiguity/GpuWorkerTransport.private.h").read_text()
        self.assertIn('resident{"resident","blah2-gpu-resident-frame","",true}', transport)
        self.assertIn("if(retainReservation)++gpu_worker_lifetime::pendingReaps()", transport)
        self.assertIn("if(retainReservation&&(result==process||(result<0&&errno==ECHILD)))--gpu_worker_lifetime::pendingReaps()", transport)
        source = (ROOT / "src/process/pipeline/CanonicalRadar.cpp").read_text()
        self.assertIn("residentDevice = selectResidentGpuDevice", source)
        self.assertNotIn("config_.accelerationDevice = selectResidentGpuDevice", source)
        self.assertIn('config_.accelerationMode == "auto" && !backendFactory && residentBuilt', source)
        cmake = (ROOT / "cmake/GpuAcceleration.cmake").read_text()
        self.assertIn("target_compile_definitions(blah2MultiIlluminatorRadar PRIVATE VECTORWARP_RESIDENT_GPU=1)", cmake)


if __name__ == "__main__":
    unittest.main()
