#pragma once

#include <cstdint>
#include <limits>
#include <stdexcept>

namespace blah2 {

// The length the clutter convolution is padded to.
//
// 1016064 = 2^8 * 3^4 * 7^2. Measured, not derived. The shipped geometry
// (fs 2e6, cpi 0.5, clutter delay -10..400) needs at least 1000411 points, and
// that factors as 269 * 3719, both prime, so FFTW falls back to Rader and the
// transform costs roughly three times what it should.
//
// This value was picked by timing every admissible length within 2% of the
// minimum on the target hardware. It came first out of fifteen candidates on
// four different Pi 5 boards, across both memory variants, at every thread
// count from one to four, idle and under load, by a margin of 2.25x to 3.8x
// over not padding. Best and median times agreed throughout.
//
// It is deliberately a constant rather than something derived. A formula was
// tried first and was worse than doing nothing: picking the *smallest*
// admissible length gives 1002375 = 3^6 * 5^3 * 11, which measured 1.54x
// SLOWER than unpadded on ARM at four threads while looking like a 1.40x win
// on x86. Transform cost is not predictable from the factorisation, so the
// only honest way to choose is to measure, and the only way to keep a measured
// answer stable is to write it down.
//
// Re-measure if the geometry, the FFTW build or the target SoC changes.
inline constexpr uint32_t kClutterFftLength = 1016064;

// The padded length to use for a convolution needing at least `minimum` points.
//
// Padding is only ever an optimisation: any length at or above the alias-free
// minimum produces the identical linear convolution, and only the first
// nSamples outputs are read. So the constant is used only where it genuinely
// covers the geometry without being wastefully larger, and anything else falls
// back to the unpadded length, which is always correct and is exactly what this
// code did before the constant existed.
inline uint32_t clutterFftLength(uint64_t minimum)
{
  constexpr uint64_t limit = std::numeric_limits<int>::max();
  if (!minimum || minimum > limit)
    throw std::invalid_argument("FFT length must fit a positive FFTW int");

  // Same 2% band the candidates were drawn from. At the shipped geometry the
  // constant sits 1.56% above the minimum, so it applies; change cpi or the
  // delay span far enough and it quietly stops applying rather than aliasing.
  if (kClutterFftLength >= minimum && kClutterFftLength <= minimum + minimum / 50)
    return kClutterFftLength;

  return static_cast<uint32_t>(minimum);
}

}
