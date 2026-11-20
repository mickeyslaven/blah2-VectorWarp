#pragma once

#include <cmath>
#include <complex>
#include <cstdint>
#include <array>
#include <stdexcept>

namespace blah2::channelizer {

// Absolute NCO phase in cycles, without first rounding a uint64 sample index
// to double. Integer-Hz lane offsets admit an exact modular integer product;
// the product of two reduced uint32 values fits uint64. Fractional offsets
// retain the exact binary value of offsetHz: multiply its 53-bit significand
// by sample, divide that integer by rate, then keep only the fractional bits.
// Dividing offset/rate before modular accumulation would amplify the division
// rounding error by the absolute sample index. Four uint32 limbs also avoid a
// non-portable __int128 dependency on Windows/MSVC.
inline double mixerPhaseCycles(uint64_t sample, double offsetHz, uint32_t rateHz) {
  if (!rateHz || !std::isfinite(offsetHz) || std::abs(offsetHz) > rateHz)
    throw std::invalid_argument("Invalid absolute channelizer phase coordinates");
  if (std::trunc(offsetHz) == offsetHz) {
    const uint64_t magnitude = uint64_t(std::abs(offsetHz));
    const uint64_t remainder = ((sample % rateHz) * (magnitude % rateHz)) % rateHz;
    return (offsetHz > 0 ? -1.0 : 1.0) * double(remainder) / rateHz;
  }
  int exponent = 0;
  const uint64_t significand = uint64_t(std::ldexp(
    std::frexp(std::abs(offsetHz), &exponent), 53));
  const int shift = 53 - exponent; // Positive for every fractional offset.
  const std::array<uint32_t, 2> a{{uint32_t(sample), uint32_t(sample >> 32)}};
  const std::array<uint32_t, 2> b{{uint32_t(significand), uint32_t(significand >> 32)}};
  std::array<uint32_t, 4> quotient{};
  for (unsigned i = 0; i < 2; ++i) {
    uint64_t carry = 0;
    for (unsigned j = 0; j < 2; ++j) {
      const uint64_t product = uint64_t(a[i]) * b[j] + quotient[i + j] + carry;
      quotient[i + j] = uint32_t(product);
      carry = product >> 32;
    }
    quotient[i + 2] = uint32_t(carry);
  }
  uint64_t remainder = 0;
  for (int i = 3; i >= 0; --i) {
    const uint64_t dividend = (remainder << 32) | quotient[i];
    quotient[i] = uint32_t(dividend / rateHz);
    remainder = dividend % rateHz;
  }
  double cycles = std::ldexp(double(remainder) / rateHz, -shift);
  for (int i = 0; i < 4; ++i) {
    const int bits = shift - 32 * i;
    if (bits <= 0) break;
    const uint32_t limb = bits >= 32 ? quotient[i] :
      quotient[i] & ((uint32_t(1) << bits) - 1);
    cycles += std::ldexp(double(limb), 32 * i - shift);
  }
  // Rounding an exact value just below one to one is the same phase as zero.
  return (offsetHz > 0 ? -1.0 : 1.0) * std::fmod(cycles, 1.0);
}

inline std::complex<double> mixerTurn(uint64_t sample, double offsetHz, uint32_t rateHz) {
  return std::polar(1.0, 2*std::acos(-1.0)*mixerPhaseCycles(sample, offsetHz, rateHz));
}

} // namespace blah2::channelizer
