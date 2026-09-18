#pragma once

#include <algorithm>
#include <complex>
#include <cstdint>
#include <stdexcept>
#include <fftw3.h>

namespace vectorwarp_clutter {
using Complex = std::complex<double>;

// Fill the three FFT lanes for one circular-correlation overlap-save block.
inline void correlation_block(const Complex* x, const Complex* y,
                              uint32_t samples, uint32_t taps,
                              uint32_t length, uint64_t begin,
                              fftw_plan plan, Complex* buffer)
{
  if (!x || !y || !buffer || !plan || !samples || !taps || taps > samples ||
      taps > length || begin >= samples)
    throw std::invalid_argument("Invalid clutter correlation block");
  const uint32_t hop = length - taps + 1;
  const uint32_t valid = uint32_t(std::min<uint64_t>(hop, samples - begin));
  Complex* history = buffer;
  Complex* currentX = history + length;
  Complex* currentY = currentX + length;
  uint32_t position = uint32_t((begin + samples - (taps - 1)) % samples);
  for (uint32_t copied = 0; copied < length;) {
    const uint32_t count = std::min(length - copied, samples - position);
    std::copy(x + position, x + position + count, history + copied);
    copied += count;
    position = 0;
  }
  std::fill(currentX, currentX + taps - 1, Complex{});
  std::fill(currentY, currentY + taps - 1, Complex{});
  std::copy(x + begin, x + begin + valid, currentX + taps - 1);
  std::copy(y + begin, y + begin + valid, currentY + taps - 1);
  std::fill(currentX + taps - 1 + valid, currentX + length, Complex{});
  std::fill(currentY + taps - 1 + valid, currentY + length, Complex{});
  fftw_execute_dft(plan, reinterpret_cast<fftw_complex*>(buffer),
                   reinterpret_cast<fftw_complex*>(buffer));
}

inline void accumulate_correlation(const Complex* buffer, uint32_t length,
                                   Complex* autocorrelation,
                                   Complex* crosscorrelation)
{
  if (!buffer || !autocorrelation || !crosscorrelation || !length)
    throw std::invalid_argument("Invalid clutter correlation accumulation");
  const Complex* history = buffer;
  const Complex* currentX = history + length;
  const Complex* currentY = currentX + length;
  for (uint32_t i = 0; i < length; ++i) {
    const Complex reference = std::conj(history[i]);
    autocorrelation[i] += currentX[i] * reference;
    crosscorrelation[i] += currentY[i] * reference;
  }
}
}  // namespace vectorwarp_clutter
