#pragma once

#include <cstdint>

namespace rspduo_sequence {
// SDRplay's API specification states that firstSampleNum is divided by the
// decimation factor to make it relative to the output sample rate. Thus a
// contiguous callback advances by its delivered numSamples, not by a raw ADC
// count. uint32_t arithmetic intentionally wraps at the SDK counter rollover.
// Source: SDRplay Software Defined Radio API Specification, stream callback /
// DecimateControl section (https://www.sdrplay.com/docs/SDRplay_SDR_API_Specification.pdf).
constexpr bool continues(uint32_t expected, uint32_t actual)
{
  return expected == actual;
}

constexpr uint32_t next(uint32_t first, uint32_t samples)
{
  return first + samples;
}
}
