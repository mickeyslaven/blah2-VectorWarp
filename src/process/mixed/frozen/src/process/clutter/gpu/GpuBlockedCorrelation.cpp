#include "GpuBlockedCorrelation.h"
#include "GpuBlockedCorrelationMath.h"
// Give this second in-process Vulkan helper instantiation private symbols. The
// FIR implementation includes the pinned helper in its original blah2 namespace.
#define blah2 vectorwarp_corr_gpu
#include "hardware-vulkan-support.h"
#undef blah2
#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
using Float = std::complex<float>;
using Complex = std::complex<double>;
using Clock = std::chrono::steady_clock;
double elapsed(Clock::time_point first) {
  return std::chrono::duration<double, std::milli>(Clock::now() - first).count();
}
constexpr uint32_t kSamples = vectorwarp_gpu_corr_bench::samples;
constexpr uint32_t kTaps = vectorwarp_gpu_corr_bench::taps;
constexpr uint32_t kLength = vectorwarp_gpu_corr_bench::length;
constexpr uint32_t kHop = vectorwarp_gpu_corr_bench::hop;
constexpr uint32_t kGpuBlocks = vectorwarp_gpu_corr_bench::gpuBlocks;
constexpr uint32_t kTotalBlocks = vectorwarp_gpu_corr_bench::totalBlocks;
constexpr uint32_t kCurrentSamples = vectorwarp_gpu_corr_bench::currentSamples;
constexpr uint32_t kReferenceSamples = vectorwarp_gpu_corr_bench::referenceSamples;

void barrier(VkCommandBuffer command, VkPipelineStageFlags from,
             VkPipelineStageFlags to, VkAccessFlags source, VkAccessFlags target) {
  VkMemoryBarrier memory{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
  memory.srcAccessMask = source;
  memory.dstAccessMask = target;
  vkCmdPipelineBarrier(command, from, to, 0, 1, &memory, 0, nullptr, 0, nullptr);
}

constexpr char packShader[] = R"glsl(#version 450
layout(local_size_x=128) in;
layout(binding=0) readonly buffer Raw { vec2 rawData[]; };
layout(binding=1) writeonly buffer Packed { vec2 packedData[]; };
layout(push_constant) uniform P {uint count;uint fft;uint blocks;uint hop;int taps;} p;
void main(){
 uint i=gl_GlobalInvocationID.x+gl_GlobalInvocationID.y*gl_NumWorkGroups.x*128;
 if(i>=p.count)return;
 uint lane=(i/p.fft)%3, block=i/(3*p.fft), position=i%p.fft;
 uint source=block*p.hop+position;
 uint history=uint(p.taps)-1;
 if(lane==0) packedData[i]=rawData[source];
 else if(position<history) packedData[i]=vec2(0.0);
 else if(lane==1) packedData[i]=rawData[source];
 else packedData[i]=rawData[p.blocks*p.hop+history+block*p.hop+position-history];
})glsl";
constexpr char exactProductsShader[] = R"glsl(#version 450
layout(local_size_x=128) in;
layout(binding=0) readonly buffer In { vec2 inputData[]; };
layout(binding=1) writeonly buffer Out { vec2 outputData[]; };
layout(push_constant) uniform P {uint count;uint fft;uint blocks;uint unused;int unused2;} p;
void main(){
 uint i=gl_GlobalInvocationID.x+gl_GlobalInvocationID.y*gl_NumWorkGroups.x*128;
 if(i>=p.count)return;
 uint block=i/p.fft, bin=i%p.fft;
 vec2 h=inputData[(3*block)*p.fft+bin];
 vec2 x=inputData[(3*block+1)*p.fft+bin];
 vec2 y=inputData[(3*block+2)*p.fft+bin];
 outputData[(2*block)*p.fft+bin]=vec2(x.x*h.x+x.y*h.y,x.y*h.x-x.x*h.y);
 outputData[(2*block+1)*p.fft+bin]=vec2(y.x*h.x+y.y*h.y,y.y*h.x-y.x*h.y);
})glsl";
constexpr char gatherShader[] = R"glsl(#version 450
layout(local_size_x=128) in;
layout(binding=0) readonly buffer In { vec2 inputData[]; };
layout(binding=1) writeonly buffer Out { vec2 outputData[]; };
layout(push_constant) uniform P {uint count;uint fft;uint taps;uint unused;int unused2;} p;
void main(){
 uint i=gl_GlobalInvocationID.x+gl_GlobalInvocationID.y*gl_NumWorkGroups.x*128;
 if(i>=p.count)return;
 uint lane=i/p.taps, lag=i%p.taps;
 outputData[i]=inputData[lane*p.fft+lag];
})glsl";
}

struct GpuBlockedCorrelation::Impl {
  std::shared_ptr<vectorwarp_corr_gpu::Instance> owner;
  std::unique_ptr<vectorwarp_corr_gpu::Context> context;
  std::unique_ptr<vectorwarp_corr_gpu::Buffer> raw, input, output, lags;
  std::unique_ptr<vectorwarp_corr_gpu::Plan> forwards, backwards;
  std::unique_ptr<vectorwarp_corr_gpu::Kernel> pack, products, gather;
  VkCommandBuffer command = VK_NULL_HANDLE;
  bool active = false;
  bool poisoned = false;
  Clock::time_point submitted{};
  GpuBlockedCorrelationTiming timing{};
  std::vector<Float> cached;
  std::vector<Complex> sumA, sumB;

  Impl(uint32_t samples, uint32_t taps, uint32_t fftLength) {
    if (samples != kSamples || taps != kTaps || fftLength != kLength)
      throw std::invalid_argument("GPU blocked correlation requires exactly 1M samples, 410 taps, and FFT4096");
    owner = std::make_shared<vectorwarp_corr_gpu::Instance>();
    auto devices = vectorwarp_corr_gpu::enumerate(*owner);
    const auto found = std::find_if(devices.begin(), devices.end(), [](const auto& candidate) {
      return candidate.properties.vendorID == 5348 &&
        candidate.info.name.find("V3D") != std::string::npos;
    });
    if (found == devices.end()) throw std::runtime_error("GPU blocked correlation requires actual Pi V3D");
    context = std::make_unique<vectorwarp_corr_gpu::Context>(owner, *found);
    auto& ctx = *context;
    constexpr auto mappedRequired = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT |
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
    constexpr auto mappedPreferred = VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
      VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
    raw = std::make_unique<vectorwarp_corr_gpu::Buffer>(ctx,
      uint64_t(kReferenceSamples + kCurrentSamples) * sizeof(Float),
      mappedRequired, mappedPreferred);
    input = std::make_unique<vectorwarp_corr_gpu::Buffer>(ctx,
      uint64_t(kGpuBlocks) * 3 * kLength * sizeof(Float),
      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    output = std::make_unique<vectorwarp_corr_gpu::Buffer>(ctx,
      uint64_t(kGpuBlocks) * 2 * kLength * sizeof(Float),
      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    lags = std::make_unique<vectorwarp_corr_gpu::Buffer>(ctx,
      uint64_t(kGpuBlocks) * 2 * kTaps * sizeof(Float),
      mappedRequired, mappedPreferred);
    forwards = std::make_unique<vectorwarp_corr_gpu::Plan>(ctx, *input,
      kLength, kGpuBlocks * 3, true);
    backwards = std::make_unique<vectorwarp_corr_gpu::Plan>(ctx, *output,
      kLength, kGpuBlocks * 2, true);
    pack = std::make_unique<vectorwarp_corr_gpu::Kernel>(ctx, packShader,
      std::vector<vectorwarp_corr_gpu::Buffer*>{raw.get(), input.get()});
    products = std::make_unique<vectorwarp_corr_gpu::Kernel>(ctx, exactProductsShader,
      std::vector<vectorwarp_corr_gpu::Buffer*>{input.get(), output.get()});
    gather = std::make_unique<vectorwarp_corr_gpu::Kernel>(ctx, gatherShader,
      std::vector<vectorwarp_corr_gpu::Buffer*>{output.get(), lags.get()});
    VkCommandBufferAllocateInfo allocation{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocation.commandPool = ctx.pool;
    allocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocation.commandBufferCount = 1;
    vectorwarp_corr_gpu::check(vkAllocateCommandBuffers(ctx.device, &allocation, &command),
      "blocked correlation command allocation");
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    vectorwarp_corr_gpu::check(vkBeginCommandBuffer(command, &begin),
      "blocked correlation command begin");
    barrier(command, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
      VK_ACCESS_HOST_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
    pack->append(command, {kGpuBlocks * 3 * kLength, kLength, kGpuBlocks, kHop, int32_t(kTaps)});
    barrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
      VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
    forwards->append(command, -1);
    barrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
      VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
    products->append(command, {kGpuBlocks * kLength, kLength, kGpuBlocks, 0, 0});
    barrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
      VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
    backwards->append(command, 1);
    barrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
      VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
    gather->append(command, {kGpuBlocks * 2 * kTaps, kLength, kTaps, 0, 0});
    barrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
      VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT);
    vectorwarp_corr_gpu::check(vkEndCommandBuffer(command),
      "blocked correlation command end");
    cached.resize(uint64_t(kGpuBlocks) * 2 * kTaps);
    sumA.resize(kTaps);
    sumB.resize(kTaps);
    std::cerr << "GPU_BLOCKED_CORRELATION status=active backend=vulkan device="
      << found->info.name << " gpu_blocks=" << kGpuBlocks
      << " cpu_blocks=" << kTotalBlocks-kGpuBlocks
      << " raw_properties=" << raw->properties
      << " lag_properties=" << lags->properties << " readback=cached\n";
  }

  ~Impl() {
    abort();
    if (context && command)
      vkFreeCommandBuffers(context->device, context->pool, 1, &command);
  }

  void start(const Complex* x, const Complex* y) {
    if (poisoned || active || !x || !y)
      throw std::runtime_error("Invalid GPU blocked correlation ownership");
    timing = {};
    timing.gpuBlocks = kGpuBlocks;
    timing.cpuBlocks = kTotalBlocks-kGpuBlocks;
    const auto begun = Clock::now();
    auto* destination = static_cast<Float*>(raw->mapped);
    vectorwarp_gpu_corr_bench::packSources(x, y, destination);
    const auto converted = Clock::now();
    raw->flush(0, raw->bytes);
    const auto flushed = Clock::now();
    timing.convertMs = std::chrono::duration<double,std::milli>(converted-begun).count();
    timing.flushMs = std::chrono::duration<double,std::milli>(flushed-converted).count();
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &command;
    const auto beforeSubmit = Clock::now();
    const VkResult status = vkQueueSubmit(context->queue, 1, &submit, context->fence);
    timing.submitMs = elapsed(beforeSubmit);
    if (status != VK_SUCCESS) {
      poisoned = true;
      vectorwarp_corr_gpu::check(status, "blocked correlation submit");
    }
    submitted = beforeSubmit;
    active = true;
  }

  GpuBlockedCorrelationTiming finish(Complex* a, Complex* b) {
    if (poisoned || !active || !a || !b)
      throw std::runtime_error("GPU blocked correlation finish without submission");
    try {
      const auto beforeWait = Clock::now();
      vectorwarp_corr_gpu::check(vkWaitForFences(context->device, 1,
        &context->fence, VK_TRUE, 5000000000ULL), "blocked correlation fence wait");
      const auto completed = Clock::now();
      timing.fenceWaitMs = std::chrono::duration<double,std::milli>(completed-beforeWait).count();
      timing.submitToFenceMs = std::chrono::duration<double,std::milli>(completed-submitted).count();
      vectorwarp_corr_gpu::check(vkResetFences(context->device, 1, &context->fence),
        "blocked correlation fence reset");
      active = false;
      const auto beforeInvalidate = Clock::now();
      lags->invalidate(0, lags->bytes);
      const auto invalidated = Clock::now();
      std::memcpy(cached.data(), lags->mapped, lags->bytes);
      const auto copied = Clock::now();
      timing.invalidateMs = std::chrono::duration<double,std::milli>(invalidated-beforeInvalidate).count();
      timing.mappedCopyMs = std::chrono::duration<double,std::milli>(copied-invalidated).count();
      vectorwarp_gpu_corr_bench::validateLags(cached.data());
      const auto checked = Clock::now();
      vectorwarp_gpu_corr_bench::sumLags(cached.data(), sumA.data(), sumB.data());
      const auto merged = Clock::now();
      timing.finiteCheckMs = std::chrono::duration<double,std::milli>(checked-copied).count();
      timing.fp64MergeMs = std::chrono::duration<double,std::milli>(merged-checked).count();
      for (uint32_t lag = 0; lag < kTaps; ++lag) {
        a[lag] += sumA[lag];
        b[lag] += sumB[lag];
      }
      return timing;
    } catch (...) {
      poisoned = true;
      throw;
    }
  }

  void abort() noexcept {
    if (!active || !context) return;
    // Correlation can still be in flight if CPU FFT/solve raises. Drain the
    // submission before the mapped buffers or command storage are reused.
    // A stuck V3D operation fails the benchmark rather than silently running
    // CPU or waiting forever on vkDeviceWaitIdle.
    const VkResult status = vkWaitForFences(context->device, 1,
      &context->fence, VK_TRUE, 5000000000ULL);
    if (status != VK_SUCCESS ||
        vkResetFences(context->device, 1, &context->fence) != VK_SUCCESS) {
      std::cerr << "GPU_BLOCKED_CORRELATION status=fatal phase=abort result="
        << status << '\n';
      std::terminate();
    }
    active = false;
  }
};

GpuBlockedCorrelation::GpuBlockedCorrelation(uint32_t samples, uint32_t taps,
    uint32_t fftLength)
  : impl_(std::make_unique<Impl>(samples, taps, fftLength)) {}
GpuBlockedCorrelation::~GpuBlockedCorrelation() = default;
void GpuBlockedCorrelation::start(const Complex* reference, const Complex* surveillance) {
  impl_->start(reference, surveillance);
}
GpuBlockedCorrelationTiming GpuBlockedCorrelation::finish(Complex* autocorrelation,
    Complex* crosscorrelation) {
  return impl_->finish(autocorrelation, crosscorrelation);
}
void GpuBlockedCorrelation::abort() noexcept { impl_->abort(); }
