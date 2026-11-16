#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>

namespace blah2 {
// Rotate once at the frame boundary, then convert two contiguous iterator
// ranges. This is the same circular delay as (i-delay)%N, without a division
// and random-access deque lookup for every sample. INT32_MIN is widened
// before negation; delays spanning multiple frames retain their old meaning.
template <class Input, class Sample>
void copyClutterReference(const Input& source, Sample* destination,
    uint32_t samples, int32_t delay) {
  if (!samples || !destination || source.size() < samples)
    throw std::invalid_argument("Invalid GPU clutter reference input");
  int64_t first = (-int64_t(delay)) % int64_t(samples);
  if (first < 0) first += samples;
  const size_t tail = samples - size_t(first);
  std::copy_n(source.begin() + first, tail, destination);
  std::copy_n(source.begin(), size_t(first), destination + tail);
}

// The last batch reuses a full GPU workspace. Clear its unused channels so
// correlation/solve kernels cannot consume samples from the preceding batch.
template <class Sample>
void copyClutterBatch(Sample* destination, size_t capacity,
    const Sample* source, size_t count) {
  if (!destination || !source || !count || count > capacity)
    throw std::invalid_argument("Invalid GPU clutter batch input");
  std::copy_n(source, count, destination);
  std::fill(destination + count, destination + capacity, Sample{});
}

// Outputs remain candidates until every batch succeeds. The IPC caller must
// discard the entire candidate frame when this returns false.
template <class Sample, class Process>
bool processClutterBatches(uint32_t samples, uint32_t channels,
    uint32_t batchChannels, const Sample* input, size_t inputCount,
    Sample* output, size_t outputCount, Process process) {
  const uint64_t total = uint64_t(samples) * channels;
  if (!samples || !channels || channels > 8 || !batchChannels ||
      batchChannels > channels || total > std::numeric_limits<size_t>::max() ||
      !input || !output || inputCount != total || outputCount != total)
    throw std::invalid_argument("GPU clutter buffer dimensions changed");
  for (uint32_t first = 0; first < channels;) {
    const uint32_t active = std::min(batchChannels, channels - first);
    const size_t offset = size_t(first) * samples;
    const size_t count = size_t(active) * samples;
    if (!process(first, active, input + offset, count, output + offset)) return false;
    first += active;
  }
  return true;
}
}
