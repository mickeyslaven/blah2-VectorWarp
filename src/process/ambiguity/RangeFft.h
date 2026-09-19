#pragma once

#include <cstdint>
#include <cstdlib>
#include <limits>
#include <stdexcept>
#include <string>

namespace blah2 {
// Extra zero padding changes FFT cost without changing the requested lags or
// Doppler bins. Share the selection with the benchmark's geometry inspection.
inline uint32_t selectedRangeFftLength(uint32_t minimum) {
  const char* mode = std::getenv("VECTORWARP_RANGE_FFT");
  if (mode && std::string(mode) != "default") {
    if (std::string(mode) != "power2")
      throw std::invalid_argument("VECTORWARP_RANGE_FFT must be default or power2");
    uint64_t padded = 1;
    while (padded < minimum) padded <<= 1;
    if (padded > std::numeric_limits<uint16_t>::max())
      throw std::invalid_argument("Power-of-two range FFT exceeds the processor limit");
    return static_cast<uint32_t>(padded);
  }
  return minimum;
}
}
