#pragma once

// Shared tracker progression for native, portable and resident processing.
// Capture lineage and publication provenance remain separate from this clock.
#include <algorithm>
#include <cstdint>
#include <stdexcept>

namespace blah2::tracker_clock {
inline uint64_t signalTimeMilliseconds(uint64_t endSample, uint32_t rate) {
  if (!rate || endSample / rate > UINT64_MAX / 1000)
    throw std::invalid_argument("Capture sample time cannot be represented");
  const uint64_t whole = (endSample / rate) * 1000;
  const uint64_t fraction = ((endSample % rate) * 1000) / rate;
  if (whole > UINT64_MAX - fraction)
    throw std::overflow_error("Capture sample time exhausted");
  return whole + fraction;
}

// Source gaps add elapsed samples; missing metadata and source epoch changes
// never import UTC or move a tracker backwards. Unknown frames advance by
// their actual accepted sample count, without manufacturing capture lineage.
class SignalProgressClock {
  uint64_t elapsedSamples_ = 0, sourceEpoch_ = 0, sourceEnd_ = 0,
    unpositionedSamples_ = 0;
  bool haveSource_ = false;
public:
  uint64_t advance(uint64_t samples, uint32_t rate, bool positioned = false,
      uint64_t epoch = 0, uint64_t endSample = 0) {
    if (!samples || !rate || (positioned && endSample < samples))
      throw std::invalid_argument("Invalid tracker sample progression");
    // An exactly repeated positioned frame is still a duplicate, not fresh
    // evidence. Metadata recovery after unknown frames is a different case.
    if (positioned && haveSource_ && epoch == sourceEpoch_ &&
        endSample == sourceEnd_ && !unpositionedSamples_)
      return signalTimeMilliseconds(elapsedSamples_, rate);
    uint64_t step = samples;
    if (positioned && haveSource_ && epoch == sourceEpoch_ && endSample > sourceEnd_) {
      const auto delta = endSample - sourceEnd_;
      if (delta > unpositionedSamples_)
        step = std::max(step, delta - unpositionedSamples_);
    }
    if (elapsedSamples_ > UINT64_MAX - step ||
        (!positioned && unpositionedSamples_ > UINT64_MAX - samples))
      throw std::overflow_error("Tracker sample progression exhausted");
    const auto next = elapsedSamples_ + step;
    const auto result = signalTimeMilliseconds(next, rate);
    elapsedSamples_ = next;
    if (positioned) {
      sourceEpoch_ = epoch; sourceEnd_ = endSample;
      unpositionedSamples_ = 0; haveSource_ = true;
    } else unpositionedSamples_ += samples;
    return result;
  }
};
} // namespace blah2::tracker_clock
