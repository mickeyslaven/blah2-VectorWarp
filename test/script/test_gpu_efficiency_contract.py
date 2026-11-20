"""No-build wiring and independent coordinate tests; not GPU qualification."""
import math
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


def offset(scale, newest, older, age):
    mean = .5 * newest + .5 * older
    rate = scale * mean
    displacement = rate * age
    fixed = math.floor(abs(displacement) * 65536 + .5)
    nearest = (fixed + 32768) // 65536
    return -nearest if displacement < 0 else nearest


class GpuEfficiencyContract(unittest.TestCase):
    @unittest.skipUnless(shutil.which("c++"), "C++ compiler required for the bounded VkFFT mock")
    def test_shared_fft_plan_precision_binding_and_failure_cleanup(self):
        source = (ROOT / "src/process/ambiguity/VulkanResources.private.h").read_text()
        plan = "struct NativePlan {" + source.split("struct NativePlan {", 1)[1].split(
            "#ifdef __APPLE__", 1)[0]
        fixture = r'''
#include <cassert>
#include <cstdint>
#include <stdexcept>
#include <string>
using VkCommandBuffer = uint64_t;
using VkFFTResult = int;
constexpr int VKFFT_SUCCESS = 0;
struct VkFFTApplication {};
struct VkFFTConfiguration {
  uint64_t FFTdim = 0, size[1]{}, numberBatches = 0, doublePrecision = 0, useLUT = 0;
  uint64_t *device = nullptr, *queue = nullptr, *fence = nullptr, *commandPool = nullptr;
  uint64_t *physicalDevice = nullptr, *bufferSize = nullptr, *buffer = nullptr;
  int isCompilerInitialized = 0;
};
struct VkFFTLaunchParams { VkCommandBuffer* commandBuffer = nullptr; uint64_t* buffer = nullptr; };
struct Context { uint64_t device = 1, queue = 2, fence = 3, pool = 4;
  struct { uint64_t physical = 5; } candidate; };
struct Buffer { uint64_t bytes = 4096, handle = 6; };
VkFFTConfiguration captured;
int initResult = 0, appendResult = 0, releases = 0, appendedDirection = 0;
uint64_t appendedBuffer = 0, appendedCommand = 0;
int initializeVkFFT(VkFFTApplication*, VkFFTConfiguration config) { captured = config; return initResult; }
void deleteVkFFT(VkFFTApplication*) { ++releases; }
int VkFFTAppend(VkFFTApplication*, int direction, VkFFTLaunchParams* params) {
  appendedDirection = direction; appendedBuffer = *params->buffer;
  appendedCommand = *params->commandBuffer; return appendResult;
}
void startupTrace(const char*) {}
void checkFft(int result) { if (result) throw std::runtime_error("mock VkFFT failure"); }
'''
        checks = r'''
int main() {
  Context context; Buffer buffer;
  for (bool fp64 : {false, true}) for (bool lut : {false, true}) {
    NativePlan plan(context, buffer, 29, 3, lut, fp64);
    assert(captured.FFTdim == 1 && captured.size[0] == 29 && captured.numberBatches == 3);
    assert(captured.doublePrecision == fp64 && captured.useLUT == lut);
    assert(captured.device == &context.device && captured.queue == &context.queue);
    assert(captured.fence == &context.fence && captured.commandPool == &context.pool);
    assert(captured.physicalDevice == &context.candidate.physical);
    assert(captured.bufferSize == &buffer.bytes && captured.buffer == nullptr);
    assert(captured.isCompilerInitialized == 1);
    for (int direction : {-1, 1}) {
      ++buffer.handle; plan.append(17, direction);
      assert(appendedDirection == direction && appendedCommand == 17 && appendedBuffer == buffer.handle);
    }
  }
  assert(releases == 4);
  initResult = 4;
  try { NativePlan failed(context, buffer, 29, 3); assert(false); }
  catch (const std::runtime_error&) {}
  assert(releases == 5);
  initResult = 0; appendResult = 9;
  try { NativePlan failed(context, buffer, 29, 3); failed.append(17, -1); assert(false); }
  catch (const std::runtime_error&) {}
  assert(releases == 6);
}
'''
        with tempfile.TemporaryDirectory(prefix="vectorwarp-fft-plan-") as directory:
            executable = str(Path(directory) / "test")
            subprocess.run([shutil.which("c++"), "-std=c++17", "-x", "c++", "-", "-o", executable],
                           input=fixture + plan + checks, text=True, check=True,
                           capture_output=True, timeout=20)
            subprocess.run([executable], check=True, capture_output=True, timeout=5)

    def test_exact_coordinate_factorization(self):
        axes = list(range(-8, 9))
        cases = 0
        for scale in (0, -.566, -.5, math.nextafter(-.5, 0),
                      math.nextafter(-.5, -1), -32):
            for depth in (4, 8):
                for radius in (0, 4, 16):
                    table = [0] * (len(axes) * (2 * radius + 1) * depth)
                    for row in range(len(axes)):
                        for dr in range(-radius, radius + 1):
                            for age in range(depth):
                                rr = row - dr * age
                                at = (row * (2 * radius + 1) + dr + radius) * depth + age
                                if 0 <= rr < len(axes):
                                    table[at] = offset(scale, axes[row], axes[rr], age)
                                    for column in (0, 1, 32, 64):
                                        for dc in range(-2, 3):
                                            old = column - dc * age - offset(scale, axes[row], axes[rr], age)
                                            self.assertEqual(column - dc * age - table[at], old)
                                            cases += 1
        self.assertGreater(cases, 100000)

    def test_setup_uses_same_gpu_arithmetic_not_host_offsets(self):
        source = (ROOT / "core/GpuPostprocess.private.h").read_text()
        self.assertIn("if(p.a==3u)", source)
        self.assertIn("fractionalDelayOffset(readD(94u),row,uint(rr),age)", source)
        trajectory = source.split("if(p.a==1u)", 1)[1].split("return;", 1)[0]
        self.assertNotIn("fractionalDelayOffset(", trajectory)
        self.assertNotIn("readD(94u)", trajectory)
        self.assertIn("path/delayWidth)*m[74]+age", trajectory)
        constructor, append = source.split(" void append(VkCommandBuffer command)", 1)
        self.assertIn("{offsetWords*2u,3,0,0,0}", constructor)
        self.assertNotIn("{offsetWords*2u,3,0,0,0}", append)
        self.assertIn("maxStorageBufferRange", constructor)

    def test_input_reset_keeps_immutable_offsets(self):
        source = (ROOT / "core/GpuPostprocess.private.h").read_text()
        reset = source.split(" void appendInputReset(", 1)[1].split(" void completeInputReset", 1)[0]
        self.assertNotIn("35*4", reset)
        self.assertNotIn("metadata_->bytes", reset)

    def test_resident_only_alias_and_lifetime_order(self):
        source = (ROOT / "core/VulkanBackend.private.cpp").read_text()
        self.assertIn("if(resident_)weights_=surveillance_;", source)
        self.assertIn("(resident_?lc:nc)*8", source)
        self.assertIn("elements -= n + 3*n*c", source)
        batch = source.split('check(vkBeginCommandBuffer(batch_,&begin)', 1)[1].split('check(vkEndCommandBuffer(batch_)', 1)[0]
        self.assertEqual(batch.count("appendCorrelations(batch_)"), 2)
        self.assertLess(batch.rindex("appendCorrelations(batch_)"), batch.index("appendCorrection(batch_)"))
        self.assertNotIn("appendCorrelations", batch.split("appendCorrection(batch_)", 1)[1])

    def test_native_baseline_is_tile_consumed_and_recomputed(self):
        source = (ROOT / "core/GpuNativeRefinement.private.h").read_text()
        members = source.split("class Refinement", 1)[1].split("public:", 1)[0]
        constructor = source.split("Refinement(Context&", 1)[1].split(
            "Buffer& borrowedReference", 1)[0]
        baseline = source.split("void appendBaseline(", 1)[1].split(
            "void appendResidual", 1)[0]
        merge = source.split("void appendMerge(", 1)[1].split("};", 1)[0]
        self.assertNotIn("baseline_", members)
        self.assertNotIn("uint64_t(samples)*channels*16", constructor)
        self.assertIn("residualFromWork_->append", baseline)
        self.assertIn("weightsPack_->append", merge)
        self.assertIn("weightPlan_->append", merge)
        self.assertIn("mergeFromWork_->append", merge)
        self.assertIn("Buffer& borrowedReference() { return *cache_; }", source)

    def test_recomputed_weight_fft_coverage_is_consistent(self):
        backend = (ROOT / "core/VulkanBackend.private.cpp").read_text()
        proof = (ROOT / "src/process/ambiguity/GpuResidentProof.cpp").read_text()
        self.assertIn("status[gpu_native::weightFfts]!=2*active", backend)
        self.assertIn("x.nativeStatus[9]==2*active", proof)

    def test_actual_admission_includes_tracker_and_cleans_before_retry(self):
        source = (ROOT / "core/GpuResidentFrame.private.h").read_text()
        attempts = source.split("for(uint32_t c=ceiling;c>0;--c)", 1)[1].split("if(!work_)", 1)[0]
        self.assertNotIn("if(required>budget-ambiguityBytes)", attempts)
        self.assertLess(attempts.index("make_unique<gpu_tracker::Stage>"), attempts.index("break;"))
        self.assertIn("tracker_.reset();work_.reset();", attempts)
        self.assertIn("clearRejection()", attempts)
        self.assertIn("required_total_at_least_bytes=", attempts)
        self.assertIn("heap_used_at_rejection=", attempts)

    def test_caps_remain_unchanged_and_diagnostics_are_lower_bounds(self):
        source = (ROOT / "src/process/ambiguity/GpuMemory.h").read_text()
        self.assertIn("maximumDeviceBytes = 2ULL << 30", source)
        self.assertIn("std::min(maximumDeviceBytes, heapBytes / 4)", source)
        self.assertIn("bytes>UINT64_MAX-used_?UINT64_MAX:used_+bytes", source)
        self.assertIn('reject(type,bytes,"device-budget")', source)
        self.assertIn('reject(type,bytes,"heap-budget")', source)
        self.assertIn('reject(type,bytes,"vulkan-allocation")', source)

    def test_native_regressions_compare_old_gpu_math_and_clutter_bytes(self):
        source = (ROOT / "test/gpu/TestGpuTbdFractional.cpp").read_text()
        self.assertIn("legacyTbd=std::make_unique<Kernel>", source)
        self.assertIn("std::memcmp(optimized.data(),observed.data()", source)
        self.assertIn("optimizedFlags==flags", source)
        self.assertIn("fixture.parity(h,-.566,8,8,16)", source)
        self.assertIn("std::memcmp(staging.mapped,expected.data()", source)
        self.assertIn("for(uint32_t active:{2u,1u})", source)


if __name__ == "__main__":
    unittest.main()
