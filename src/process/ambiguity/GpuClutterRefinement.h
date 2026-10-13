#pragma once

#include "process/utility/FftwThreads.h"
#include <algorithm>
#include <climits>
#include <cmath>
#include <complex>
#include <cstdint>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <vector>

namespace blah2::clutter_refinement {
using Double = std::complex<double>;
using Float = std::complex<float>;

template<class T> bool finite(T value) {
  return std::isfinite(value.real()) && std::isfinite(value.imag());
}

// The isolated GPU module uses this lock for both the clutter baseline and
// macOS prime-plan constants. Completed plans execute outside the planner lock.
inline std::mutex& plannerMutex() { static std::mutex value; return value; }
struct FftFree { void operator()(Double* p) const { fftw_free(p); } };
using Buffer = std::unique_ptr<Double, FftFree>;
inline Buffer allocate(size_t count) {
  if (!count || count > SIZE_MAX / sizeof(Double))
    throw std::invalid_argument("GPU clutter CPU allocation overflow");
  Buffer result(static_cast<Double*>(fftw_malloc(count * sizeof(Double))));
  if (!result) throw std::bad_alloc();
  return result;
}
struct PlanFree {
  void operator()(fftw_plan_s* plan) const {
    std::lock_guard<std::mutex> lock(plannerMutex());
    fftw_destroy_plan(plan);
  }
};
using Plan = std::unique_ptr<fftw_plan_s, PlanFree>;
inline Plan makePlan(uint32_t length, Double* input, Double* output, int direction) {
  if (!length || length > INT_MAX || !input || !output)
    throw std::invalid_argument("GPU module CPU FFT geometry invalid");
  std::lock_guard<std::mutex> lock(plannerMutex());
  static const bool ready = fftw_init_threads() != 0;
  if (!ready) throw std::runtime_error("GPU module FFTW thread initialization failed");
  struct RestoreThreads {
    const int previous = blah2::fftw_planner_threads();
    ~RestoreThreads() { blah2::set_fftw_planner_threads(previous); }
  } restore;
  blah2::set_fftw_planner_threads(1);
  fftw_plan result = fftw_plan_dft_1d(int(length),
    reinterpret_cast<fftw_complex*>(input), reinterpret_cast<fftw_complex*>(output),
    direction, FFTW_ESTIMATE);
  if (!result) throw std::runtime_error("GPU module CPU FFT plan failed");
  // Return ownership only after the restoration/lock scopes unwind. No plan
  // deleter can recursively acquire the planner mutex during this function.
  return Plan(result);
}
inline Plan makePlan(uint32_t length, Double* data, int direction) {
  return makePlan(length, data, data, direction);
}

// Short FP64 overlap-save FIR for the initial GPU-solved coefficients. The
// reference and its block spectra are shared across one frame's channel batches.
class BlockedBaseline {
  uint32_t samples_, taps_, fft_ = 1024, hop_, blocks_;
  std::vector<Double> reference_, spectra_;
  Buffer work_, weights_;
  // Plans die before the referenced buffers, including on constructor failure.
  Plan forward_, inverse_, weightForward_;
  bool prepared_ = false;
public:
  BlockedBaseline(uint32_t samples, uint32_t taps)
    : samples_(samples), taps_(taps) {
    if (!samples || samples > INT_MAX || !taps || taps > samples)
      throw std::invalid_argument("GPU clutter CPU FIR geometry invalid");
    while (uint64_t(fft_) < uint64_t(taps) * 2) {
      if (fft_ > uint32_t(INT_MAX) / 2)
        throw std::invalid_argument("GPU clutter CPU FIR FFT too large");
      fft_ *= 2;
    }
    hop_ = fft_ - taps + 1;
    blocks_ = uint32_t((uint64_t(samples) + hop_ - 1) / hop_);
    const uint64_t cacheElements = uint64_t(blocks_) * fft_;
    if (uint64_t(samples_) + cacheElements + 2ULL * fft_ > SIZE_MAX / sizeof(Double))
      throw std::invalid_argument("GPU clutter CPU FIR cache overflow");
    reference_.resize(samples_); spectra_.resize(size_t(cacheElements));
    work_ = allocate(fft_); weights_ = allocate(fft_);
    forward_ = makePlan(fft_, work_.get(), FFTW_FORWARD);
    inverse_ = makePlan(fft_, work_.get(), FFTW_BACKWARD);
    weightForward_ = makePlan(fft_, weights_.get(), FFTW_FORWARD);
  }
  void invalidate() { prepared_ = false; }
  uint32_t fft() const { return fft_; }
  // Explicit buffers only: FFTW plan internals and GPU memory are separate.
  size_t storageBytes() const {
    return (reference_.size() + spectra_.size() + 2ULL * fft_) * sizeof(Double);
  }
  void prepare(const Float* reference, size_t count) {
    invalidate();
    if (!reference || count != samples_)
      throw std::invalid_argument("GPU clutter CPU reference shape differs");
    for (uint32_t i = 0; i < samples_; ++i) {
      if (!finite(reference[i])) throw std::domain_error("Nonfinite rounded clutter reference");
      reference_[i] = Double(reference[i]);
    }
    for (uint32_t block = 0; block < blocks_; ++block) {
      const int64_t first = int64_t(block) * hop_ - int64_t(taps_ - 1);
      const int64_t lower = std::max<int64_t>(0, first);
      const int64_t upper = std::min<int64_t>(samples_, first + fft_);
      std::fill_n(work_.get(), fft_, Double{});
      if (lower < upper)
        std::copy(reference_.begin() + lower, reference_.begin() + upper,
          work_.get() + lower - first);
      fftw_execute(forward_.get());
      std::copy_n(work_.get(), fft_, spectra_.data() + size_t(block) * fft_);
    }
    prepared_ = true;
  }
  void estimate(const Double* q, Double* output) {
    if (!prepared_ || !q || !output) throw std::logic_error("GPU clutter CPU FIR is not prepared");
    for (uint32_t tap = 0; tap < taps_; ++tap) {
      if (!finite(q[tap])) throw std::domain_error("Nonfinite initial clutter coefficient");
      weights_.get()[tap] = q[tap];
    }
    std::fill(weights_.get() + taps_, weights_.get() + fft_, Double{});
    fftw_execute(weightForward_.get());
    for (uint32_t block = 0; block < blocks_; ++block) {
      const auto* spectrum = spectra_.data() + size_t(block) * fft_;
      for (uint32_t i = 0; i < fft_; ++i) work_.get()[i] = spectrum[i] * weights_.get()[i];
      fftw_execute(inverse_.get());
      const uint32_t begin = block * hop_, count = std::min(hop_, samples_ - begin);
      for (uint32_t i = 0; i < count; ++i) {
        const Double value = work_.get()[taps_ - 1 + i] / double(fft_);
        if (!finite(value)) throw std::domain_error("Nonfinite FP64 clutter baseline");
        output[begin + i] = value;
      }
    }
  }
  void residual(const Float* input, const Double* q, const Double* linear,
      Float* output, double* residualPower = nullptr, double* inputPower = nullptr) const {
    if (!prepared_ || !input || !q || !linear || !output)
      throw std::logic_error("GPU clutter residual inputs invalid");
    for (uint32_t i = 0; i < samples_; ++i) {
      Double circular = linear[i];
      // Normal-equation correlation is circular. Add its wrap only for the
      // correction residual; the published clutter estimate remains linear.
      if (i < taps_ - 1)
        for (uint32_t j = i + 1; j < taps_; ++j)
          circular += q[j] * reference_[size_t(samples_) + i - j];
      const Float value(Double(input[i]) - circular);
      if (!finite(input[i]) || !finite(value))
        throw std::domain_error("Nonfinite rounded clutter residual");
      output[i] = value;
      if (residualPower) *residualPower += std::norm(Double(value));
      if (inputPower) *inputPower += std::norm(Double(input[i]));
    }
  }
};
} // namespace blah2::clutter_refinement
