#pragma once
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <stdexcept>

namespace vectorwarp_gpu_corr_bench {
using Complex = std::complex<double>;
using Float = std::complex<float>;
constexpr uint32_t samples = 1000000;
constexpr uint32_t taps = 410;
constexpr uint32_t length = 4096;
constexpr uint32_t hop = length - taps + 1;
constexpr uint32_t gpuBlocks = 68;
constexpr uint32_t totalBlocks = (samples + hop - 1) / hop;
constexpr uint32_t currentSamples = gpuBlocks * hop;
constexpr uint32_t referenceSamples = currentSamples + taps - 1;
static_assert(totalBlocks == 272 && currentSamples == 250716 &&
              referenceSamples == 251125);

inline void packSources(const Complex* x, const Complex* y, Float* destination) {
  if (!x || !y || !destination)
    throw std::invalid_argument("Invalid GPU correlation source storage");
  for (uint32_t i = 0; i < taps-1; ++i)
    destination[i] = Float(x[samples-(taps-1)+i]);
  for (uint32_t i = 0; i < currentSamples; ++i)
    destination[taps-1+i] = Float(x[i]);
  for (uint32_t i = 0; i < currentSamples; ++i)
    destination[referenceSamples+i] = Float(y[i]);
}

inline void validateLags(const Float* lags) {
  if (!lags) throw std::invalid_argument("Invalid GPU correlation lag storage");
  for (uint64_t i = 0; i < uint64_t(gpuBlocks)*2*taps; ++i)
    if (!std::isfinite(lags[i].real()) || !std::isfinite(lags[i].imag()))
      throw std::runtime_error("Nonfinite GPU blocked correlation lag");
}

inline void sumLags(const Float* lags, Complex* sumA, Complex* sumB) {
  if (!lags || !sumA || !sumB)
    throw std::invalid_argument("Invalid GPU correlation merge storage");
  std::fill_n(sumA, taps, Complex{});
  std::fill_n(sumB, taps, Complex{});
  for (uint32_t block = 0; block < gpuBlocks; ++block)
    for (uint32_t lag = 0; lag < taps; ++lag) {
      sumA[lag] += std::conj(Complex(lags[(uint64_t(block)*2)*taps+lag]));
      sumB[lag] += Complex(lags[(uint64_t(block)*2+1)*taps+lag]);
    }
}
}
