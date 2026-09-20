#pragma once
#include <complex>
#include <cstdint>
#include <memory>

struct GpuBlockedCorrelationTiming {
  uint32_t gpuBlocks = 0;
  uint32_t cpuBlocks = 0;
  double convertMs = 0;
  double flushMs = 0;
  double submitMs = 0;
  double submitToFenceMs = 0;
  double fenceWaitMs = 0;
  double invalidateMs = 0;
  double mappedCopyMs = 0;
  double finiteCheckMs = 0;
  double fp64MergeMs = 0;
};

// Fixed-geometry benchmark backend for the first 68 of 272 correlation blocks.
// It fails hard on every GPU error and never substitutes CPU work.
class GpuBlockedCorrelation {
  struct Impl;
  std::unique_ptr<Impl> impl_;
public:
  GpuBlockedCorrelation(uint32_t samples, uint32_t taps, uint32_t fftLength);
  ~GpuBlockedCorrelation();
  GpuBlockedCorrelation(const GpuBlockedCorrelation&) = delete;
  GpuBlockedCorrelation& operator=(const GpuBlockedCorrelation&) = delete;
  void start(const std::complex<double>* reference,
             const std::complex<double>* surveillance);
  GpuBlockedCorrelationTiming finish(std::complex<double>* autocorrelation,
                                     std::complex<double>* crosscorrelation);
  void abort() noexcept;
  static constexpr uint32_t gpu_blocks() { return 68; }
  static constexpr uint32_t total_blocks() { return 272; }
};
