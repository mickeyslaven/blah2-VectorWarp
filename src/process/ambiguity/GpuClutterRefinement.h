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

// Check the complete bounded buffer without an early-exit branch per sample.
// The reduction permits portable SIMD while retaining NaN/Inf rejection under
// the normal IEEE compiler contract (fast-math is intentionally not enabled).
template<class T> bool finiteRange(const T* values, size_t count) {
  unsigned valid = 1;
  for (size_t i = 0; i < count; ++i) {
    valid &= unsigned(std::isfinite(values[i].real()));
    valid &= unsigned(std::isfinite(values[i].imag()));
  }
  return valid != 0;
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
  std::vector<Double> reference_, spectra_, covariance_;
  Buffer work_, weights_;
  // Plans die before the referenced buffers, including on constructor failure.
  Plan forward_, inverse_, weightForward_;
  bool prepared_ = false, covarianceReady_ = false;
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
    if (uint64_t(samples_) + cacheElements + 2ULL * fft_ + taps_ > SIZE_MAX / sizeof(Double))
      throw std::invalid_argument("GPU clutter CPU FIR cache overflow");
    reference_.resize(samples_); spectra_.resize(size_t(cacheElements)); covariance_.resize(taps_);
    work_ = allocate(fft_); weights_ = allocate(fft_);
    forward_ = makePlan(fft_, work_.get(), FFTW_FORWARD);
    inverse_ = makePlan(fft_, work_.get(), FFTW_BACKWARD);
    weightForward_ = makePlan(fft_, weights_.get(), FFTW_FORWARD);
  }
  void invalidate() { prepared_ = covarianceReady_ = false; }
  uint32_t fft() const { return fft_; }
  // Explicit buffers only: FFTW plan internals and GPU memory are separate.
  size_t storageBytes() const {
    return (reference_.size() + spectra_.size() + covariance_.size() + 2ULL * fft_) * sizeof(Double);
  }
  void prepare(const Float* reference, size_t count) {
    invalidate();
    if (!reference || count != samples_)
      throw std::invalid_argument("GPU clutter CPU reference shape differs");
    unsigned valid = 1;
    for (uint32_t i = 0; i < samples_; ++i) {
      valid &= unsigned(std::isfinite(reference[i].real()));
      valid &= unsigned(std::isfinite(reference[i].imag()));
      reference_[i] = Double(reference[i]);
    }
    if (!valid) throw std::domain_error("Nonfinite rounded clutter reference");
    for (uint32_t block = 0; block < blocks_; ++block) {
      const int64_t first = int64_t(block) * hop_ - int64_t(taps_ - 1);
      const int64_t lower = std::max<int64_t>(0, first);
      const int64_t upper = std::min<int64_t>(samples_, first + fft_);
      const uint32_t destination = uint32_t(lower - first);
      const uint32_t copied = uint32_t(upper - lower);
      // Interior overlap-save windows overwrite the entire FFT input. Avoid
      // clearing those buffers first; only the bounded frame-edge padding is
      // semantically zero. This preserves every FFT input bit while removing
      // one full-buffer store from almost every block on every backend.
      std::fill_n(work_.get(), destination, Double{});
      if (copied)
        std::copy_n(reference_.begin() + lower, copied,
          work_.get() + destination);
      std::fill_n(work_.get() + destination + copied,
        fft_ - destination - copied, Double{});
      fftw_execute(forward_.get());
      std::copy_n(work_.get(), fft_, spectra_.data() + size_t(block) * fft_);
    }
    prepared_ = true;
  }
  const std::vector<Double>& covariance() {
    if (!prepared_) throw std::logic_error("GPU clutter FP64 covariance is not prepared");
    if (covarianceReady_) return covariance_;
    // Each cached window X contains taps-1 history samples followed by a
    // disjoint valid tail Y. Masking that history out of Y counts each anchor
    // exactly once. For 0<=k<taps, IFFT(Y*conj(X))/fft therefore yields
    // sum_{i=k}^{N-1} x[i]*conj(x[i-k]), not an overlap-weighted covariance.
    // Reuse both existing scratch buffers and their plans; no full-CPI FFT.
    std::fill_n(work_.get(), fft_, Double{});
    for (uint32_t block = 0; block < blocks_; ++block) {
      const uint32_t begin = block * hop_, count = std::min(hop_, samples_ - begin);
      // The masked tail overwrites [taps-1, taps-1+count). Clear only the
      // history prefix and a possible short final-block suffix instead of
      // writing zeros over values immediately replaced by the copy.
      std::fill_n(weights_.get(), taps_ - 1, Double{});
      std::copy_n(reference_.data() + begin, count, weights_.get() + taps_ - 1);
      std::fill_n(weights_.get() + taps_ - 1 + count,
        fft_ - (taps_ - 1 + count), Double{});
      fftw_execute(weightForward_.get());
      const auto* spectrum = spectra_.data() + size_t(block) * fft_;
      for (uint32_t i = 0; i < fft_; ++i)
        work_.get()[i] += weights_.get()[i] * std::conj(spectrum[i]);
    }
    fftw_execute(inverse_.get());
    for (uint32_t lag = 0; lag < taps_; ++lag) {
      Double value = work_.get()[lag] / double(fft_);
      // Complete circular correlation at the frame boundary. Only the first
      // lag anchors wrap; internal block boundaries already include history.
      for (uint32_t i = 0; i < lag; ++i)
        value += reference_[i] * std::conj(reference_[size_t(samples_) + i - lag]);
      if (!finite(value)) {
        invalidate();
        throw std::domain_error("Nonfinite FP64 clutter covariance");
      }
      covariance_[lag] = value;
    }
    // Positive-lag first column R(row,col)=r[row-col], where
    // r[k]=sum_i x[i]*conj(x[(i-k) mod N]). Already a sum, not divided by N:
    // equivalent to full-CPI IFFT(abs(FFT(x))^2)/N in the CPU WienerHopf oracle.
    covarianceReady_ = true;
    return covariance_;
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
      unsigned valid = 1;
      for (uint32_t i = 0; i < count; ++i) {
        const Double value = work_.get()[taps_ - 1 + i] / double(fft_);
        valid &= unsigned(std::isfinite(value.real()));
        valid &= unsigned(std::isfinite(value.imag()));
        output[begin + i] = value;
      }
      if (!valid) throw std::domain_error("Nonfinite FP64 clutter baseline");
    }
  }
  // Produce the retained FP64 baseline and the FP32 correction residual in
  // one overlap-save traversal. The later correction merge still consumes
  // the complete baseline; this only avoids rereading it in a second full-CPI
  // host pass.
  void estimateResidual(const Float* input, const Double* q, Double* output,
      Float* residualOutput) {
    if (!prepared_ || !input || !q || !output || !residualOutput)
      throw std::logic_error("GPU clutter fused baseline inputs invalid");
    for (uint32_t tap = 0; tap < taps_; ++tap) {
      if (!finite(q[tap])) throw std::domain_error("Nonfinite initial clutter coefficient");
      weights_.get()[tap] = q[tap];
    }
    std::fill(weights_.get() + taps_, weights_.get() + fft_, Double{});
    fftw_execute(weightForward_.get());
    unsigned residualValid = 1;
    for (uint32_t block = 0; block < blocks_; ++block) {
      const auto* spectrum = spectra_.data() + size_t(block) * fft_;
      for (uint32_t i = 0; i < fft_; ++i)
        work_.get()[i] = spectrum[i] * weights_.get()[i];
      fftw_execute(inverse_.get());
      const uint32_t begin = block * hop_, count = std::min(hop_, samples_ - begin);
      unsigned baselineValid = 1;
      for (uint32_t i = 0; i < count; ++i) {
        const uint32_t sample = begin + i;
        const Double value = work_.get()[taps_ - 1 + i] / double(fft_);
        baselineValid &= unsigned(std::isfinite(value.real()));
        baselineValid &= unsigned(std::isfinite(value.imag()));
        output[sample] = value;
        Double circular = value;
        if (sample < taps_ - 1)
          for (uint32_t tap = sample + 1; tap < taps_; ++tap)
            circular += q[tap] * reference_[size_t(samples_) + sample - tap];
        const Float residual(Double(input[sample]) - circular);
        residualValid &= unsigned(std::isfinite(residual.real()));
        residualValid &= unsigned(std::isfinite(residual.imag()));
        residualOutput[sample] = residual;
      }
      if (!baselineValid)
        throw std::domain_error("Nonfinite FP64 clutter baseline");
    }
    if (!residualValid)
      throw std::domain_error("Nonfinite rounded clutter residual");
  }
  void residual(const Float* input, const Double* q, const Double* linear,
      Float* output, double* residualPower = nullptr, double* inputPower = nullptr) const {
    if (!prepared_ || !input || !q || !linear || !output)
      throw std::logic_error("GPU clutter residual inputs invalid");
    const uint32_t edge = std::min(samples_, taps_ - 1);
    // Circular correction exists only at the short frame edge. Keep the
    // multi-tap sum there and leave the millions of ordinary samples in a
    // contiguous loop with no per-sample boundary or diagnostics branches.
    // The arithmetic and FP64-to-FP32 rounding of every output are unchanged.
    if (!residualPower && !inputPower) {
      unsigned valid = 1;
      for (uint32_t i = 0; i < edge; ++i) {
        Double circular = linear[i];
        for (uint32_t j = i + 1; j < taps_; ++j)
          circular += q[j] * reference_[size_t(samples_) + i - j];
        const Float value(Double(input[i]) - circular);
        valid &= unsigned(std::isfinite(value.real()));
        valid &= unsigned(std::isfinite(value.imag()));
        output[i] = value;
      }
      for (uint32_t i = edge; i < samples_; ++i) {
        const Float value(Double(input[i]) - linear[i]);
        valid &= unsigned(std::isfinite(value.real()));
        valid &= unsigned(std::isfinite(value.imag()));
        output[i] = value;
      }
      if (!valid) throw std::domain_error("Nonfinite rounded clutter residual");
      return;
    }
    unsigned valid = 1;
    for (uint32_t i = 0; i < edge; ++i) {
      Double circular = linear[i];
      // Normal-equation correlation is circular. Add its wrap only for the
      // correction residual; the published clutter estimate remains linear.
      for (uint32_t j = i + 1; j < taps_; ++j)
        circular += q[j] * reference_[size_t(samples_) + i - j];
      const Float value(Double(input[i]) - circular);
      valid &= unsigned(std::isfinite(value.real()));
      valid &= unsigned(std::isfinite(value.imag()));
      output[i] = value;
      if (residualPower) *residualPower += std::norm(Double(value));
      if (inputPower) *inputPower += std::norm(Double(input[i]));
    }
    for (uint32_t i = edge; i < samples_; ++i) {
      const Float value(Double(input[i]) - linear[i]);
      valid &= unsigned(std::isfinite(value.real()));
      valid &= unsigned(std::isfinite(value.imag()));
      output[i] = value;
      if (residualPower) *residualPower += std::norm(Double(value));
      if (inputPower) *inputPower += std::norm(Double(input[i]));
    }
    if (!valid) throw std::domain_error("Nonfinite rounded clutter residual");
  }
};
} // namespace blah2::clutter_refinement
