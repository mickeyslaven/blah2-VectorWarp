#pragma once

#include <algorithm>
#include <cstdint>
#include <stdexcept>

namespace vectorwarp_clutter {

// Preserve WienerHopf's dest[i] = src[(i - delay) mod samples] convention
// without a division in the hot copy loop.
template <class Container, class Value>
void copy_rotation(const Container& source, uint32_t samples, int32_t delay,
                   Value* destination)
{
  if (!samples || source.size() < samples || !destination)
    throw std::invalid_argument("Invalid clutter rotation span");
  const int64_t start = (-int64_t(delay)) % int64_t(samples);
  uint32_t position = static_cast<uint32_t>(start < 0 ? start + samples : start);
  for (uint32_t copied = 0; copied < samples;) {
    const uint32_t count = std::min(samples - copied, samples - position);
    std::copy_n(source.begin() + position, count, destination + copied);
    copied += count;
    position = 0;
  }
}

// Bounded contiguous-input variant for shared-memory CPIs.  Keep the same
// rotation convention as the container overload above.
template <class Value>
void copy_rotation(const Value* source, uint32_t sourceSamples, uint32_t samples,
                   int32_t delay, Value* destination)
{
  if (!source || !samples || sourceSamples < samples || !destination)
    throw std::invalid_argument("Invalid clutter rotation span");
  const int64_t start = (-int64_t(delay)) % int64_t(samples);
  uint32_t position = static_cast<uint32_t>(start < 0 ? start + samples : start);
  for (uint32_t copied = 0; copied < samples;) {
    const uint32_t count = std::min(samples - copied, samples - position);
    std::copy_n(source + position, count, destination + copied);
    copied += count;
    position = 0;
  }
}

}  // namespace vectorwarp_clutter
