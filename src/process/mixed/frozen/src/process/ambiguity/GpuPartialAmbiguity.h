#pragma once
#include <complex>
#include <cstdint>
#include <deque>
#include <memory>
#include <vector>

struct GpuPartialAmbiguityTiming {
  double packSubmitMs=0;
  double submitToFenceMs=0;
  double fenceWaitMs=0;
  double readbackMs=0;
};

// Isolated exact-geometry benchmark path; no production acceleration ABI.
class GpuPartialAmbiguity {
  struct Impl;
  std::unique_ptr<Impl> impl_;
public:
  using Complex=std::complex<double>;
  GpuPartialAmbiguity(uint32_t samples,uint32_t rows,uint32_t nCorr,
    uint32_t fft,uint32_t delays,int32_t delayMin,double dopplerMiddle);
  ~GpuPartialAmbiguity();
  void start(const std::deque<Complex>& reference,
    const std::deque<Complex>& surveillance,
    const Complex* rotatedReference=nullptr,
    const Complex* filteredSurveillance=nullptr,
    uint32_t fullSamples=0, int32_t rotation=0);
  void start_borrowed(const Complex* rotatedReference,
    const Complex* filteredSurveillance, uint32_t fullSamples, int32_t rotation);
  GpuPartialAmbiguityTiming finish(std::vector<Complex>& rangeRows);
  void abort() noexcept;
  static constexpr uint32_t gpuRows=75;
};
