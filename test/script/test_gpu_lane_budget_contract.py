"""Source-only guard for the aggregate GPU quota; does not compile or spawn workers."""
from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[2]


class GpuLaneBudgetContract(unittest.TestCase):
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

    def test_portable_workers_reserve_mapping_and_share_device_only_lease(self):
        source = (ROOT / "src/process/analysis/MultiIlluminatorRadar.cpp").read_text()
        self.assertIn("!residentFrontend && !residentAttempted", source)
        self.assertRegex(source, re.compile(
            r"options\.allocationLimitBytes = budget->allocationLimit\(laneIndex,\s*"
            r"gpuProcessMappingBytes\(geometry\)\)"))
        self.assertIn("createGpuProcess(geometry, device, options)", source)
        self.assertIn("return leaseGpuLane(std::move(worker), budget)", source)
        lease = (ROOT / "src/process/ambiguity/GpuLaneLease.h").read_text()
        for submission in ("backend_->process(reference, surveillance, output)",
                           "dynamic_cast<GpuFrameBackend&>(*backend_).processFrame()",
                           "dynamic_cast<GpuClutterFrameBackend&>(*backend_).processClutterFrame()"):
            self.assertIn(submission, lease)
        self.assertNotIn("process_loaded_cpi", lease)


if __name__ == "__main__":
    unittest.main()
