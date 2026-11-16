#pragma once

#include <vulkan/vulkan.h>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace blah2 {

// Disabled by default. Opt in for at most 64 frames per pipeline with
// BLAH2_GPU_CLUTTER_PROFILE_FRAMES=N. Queries are recorded only in diagnostic
// pipelines, and unsupported timestamps degrade to host timing only.
class GpuClutterProfile {
public:
  enum Host : unsigned { ReferenceCopy, ReferenceEnqueue, ReferenceWaitTail, ReferenceOverlapWall,
    Factor, BaselinePrepare, Covariance,
    InputCopy, InitialWait, InitialSolve, Baseline, Residual, ResidualCopy,
    ResidualWait, CorrectionSolve, WeightCopy, FinishWait, Merge, HostCount };
  using Clock = std::chrono::steady_clock;
  class Scope {
    double* elapsed_;
    Clock::time_point start_;
  public:
    explicit Scope(double* elapsed) : elapsed_(elapsed) {
      if (elapsed_) start_ = Clock::now();
    }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
    ~Scope() noexcept {
      if (elapsed_) *elapsed_ +=
        std::chrono::duration<double, std::milli>(Clock::now()-start_).count();
    }
  };
private:
  VkDevice device_;
  VkQueryPool pool_ = VK_NULL_HANDLE;
  uint32_t bits_ = 0, limit_ = 0, frame_ = 0, hostSeen_ = 0, gpuSeen_ = 0;
  float period_;
  bool active_ = false;
  uint32_t samples_, bins_, channels_;
  Clock::time_point start_;
  std::array<double, HostCount> host_{};
  std::array<double, 4> gpu_{};
public:
  GpuClutterProfile(VkPhysicalDevice physical, VkDevice device, uint32_t queue,
      float period, uint32_t samples, uint32_t bins, uint32_t channels)
    : device_(device), period_(period), samples_(samples), bins_(bins), channels_(channels) {
    const char* value = std::getenv("BLAH2_GPU_CLUTTER_PROFILE_FRAMES");
    if (!value || !*value) return;
    for (const char* p = value; *p; ++p) {
      if (*p < '0' || *p > '9' || limit_ > 6)
        throw std::invalid_argument("BLAH2_GPU_CLUTTER_PROFILE_FRAMES must be 0..64");
      limit_ = limit_ * 10 + unsigned(*p-'0');
    }
    if (limit_ > 64)
      throw std::invalid_argument("BLAH2_GPU_CLUTTER_PROFILE_FRAMES must be 0..64");
    if (!limit_) return;
    uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, nullptr);
    std::vector<VkQueueFamilyProperties> properties(count);
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, properties.data());
    if (queue >= count || !std::isfinite(period_) || period_ <= 0) return;
    bits_ = properties[queue].timestampValidBits;
    if (!bits_ || bits_ > 64) { bits_ = 0; return; }
    VkQueryPoolCreateInfo info{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    info.queryType = VK_QUERY_TYPE_TIMESTAMP; info.queryCount = 6;
    if (vkCreateQueryPool(device_, &info, nullptr, &pool_) != VK_SUCCESS) {
      pool_ = VK_NULL_HANDLE; bits_ = 0;
    }
  }
  GpuClutterProfile(const GpuClutterProfile&) = delete;
  GpuClutterProfile& operator=(const GpuClutterProfile&) = delete;
  ~GpuClutterProfile() { if (pool_) vkDestroyQueryPool(device_, pool_, nullptr); }
  void beginFrame() {
    active_ = frame_ < limit_;
    if (!active_) return;
    ++frame_; hostSeen_ = gpuSeen_ = 0; host_.fill(0); gpu_.fill(0);
    start_ = Clock::now();
  }
  Scope scope(Host stage) {
    if (active_) hostSeen_ |= uint32_t(1) << stage;
    return Scope(active_ ? &host_[stage] : nullptr);
  }
  void recordBegin(VkCommandBuffer command, uint32_t phase) const {
    if (!pool_) return;
    vkCmdResetQueryPool(command, pool_, phase*2, 2);
    vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, pool_, phase*2);
  }
  void recordEnd(VkCommandBuffer command, uint32_t phase) const {
    if (pool_) vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
      pool_, phase*2+1);
  }
  // Caller has already waited on the submission fence. Never add another
  // blocking wait or turn missing profiling evidence into a DSP rejection.
  void collect(uint32_t phase, uint32_t stage) {
    if (!active_ || !pool_) return;
    uint64_t ticks[2]{};
    const auto result = vkGetQueryPoolResults(device_, pool_, phase*2, 2,
      sizeof(ticks), ticks, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT);
    gpuSeen_ |= uint32_t(1) << stage;
    if (result != VK_SUCCESS) { gpu_[stage] = -1; return; }
    const uint64_t mask = bits_ == 64 ? UINT64_MAX : (uint64_t(1) << bits_)-1;
    if (gpu_[stage] >= 0)
      gpu_[stage] += double((ticks[1]-ticks[0]) & mask) * double(period_) / 1e6;
  }
  void finish() noexcept {
    if (!active_) return;
    active_ = false;
    try {
      static constexpr const char* names[] = {"reference_copy", "reference_enqueue",
        "reference_wait_tail", "reference_overlap_wall", "factor",
        "baseline_prepare", "covariance_fp64", "input_copy", "initial_wait", "initial_solve", "baseline",
        "residual", "residual_copy", "residual_wait", "correction_solve", "weight_copy",
        "finish_wait", "merge"};
      static constexpr const char* gpuNames[] = {"reference_fft", "initial_corr", "residual_corr", "correction_fir"};
      std::ostringstream line;
      line << "GPU_CLUTTER_PROFILE frame=" << frame_ << " limit=" << limit_
        << " samples=" << samples_ << " bins=" << bins_ << " channels=" << channels_
        << " reference_overlap=host_fp64_and_gpu_fft timings=wall_nonadditive"
        << " timestamp_bits=" << bits_ << " host_total_ms="
        << std::chrono::duration<double, std::milli>(Clock::now()-start_).count();
      for (unsigned i = 0; i < HostCount; ++i)
        if (hostSeen_ & (uint32_t(1) << i)) line << " host_" << names[i] << "_ms=" << host_[i];
      for (unsigned i = 0; i < gpu_.size(); ++i)
        if (gpuSeen_ & (uint32_t(1) << i)) line << " gpu_" << gpuNames[i] << "_ms=" << gpu_[i];
      line << '\n'; std::cerr << line.str();
    } catch (...) { /* Diagnostics must not change frame acceptance. */ }
  }
};
} // namespace blah2
