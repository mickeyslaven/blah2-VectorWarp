#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <limits>
#include <stdexcept>

namespace blah2 {
enum class ClutterTwiddlePolicy { lut, native };
inline ClutterTwiddlePolicy clutterTwiddlePolicy(const char* setting) {
  if (!setting) return ClutterTwiddlePolicy::lut;
  if (std::strcmp(setting, "lut") == 0) return ClutterTwiddlePolicy::lut;
  if (std::strcmp(setting, "native") == 0) return ClutterTwiddlePolicy::native;
  throw std::invalid_argument("BLAH2_GPU_CLUTTER_TWIDDLES must be lut or native");
}
// Keep the default LUT-only: isolated stage timing cannot admit native clutter
// safely under shared wideband/CAF/temporal contention. Native twiddles require
// an explicit override until whole-bank performance qualification exists.
// Numerical qualification and every hard allocation cap remain unchanged.
template<class Attempt>
uint32_t selectClutterBatch(uint32_t channels, const char* setting, Attempt attempt) {
  if (!channels || channels > 8)
    throw std::invalid_argument("Invalid GPU clutter construction channel count");
  const auto policy = clutterTwiddlePolicy(setting);
  for (uint32_t batch = channels; batch > 0; --batch) {
    if (attempt(batch, policy == ClutterTwiddlePolicy::lut)) return batch;
  }
  return 0;
}
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
// Producers which write directly into that workspace use the same padding
// after all active samples have been generated and accepted.
template <class Sample>
void padClutterBatch(Sample* destination, size_t capacity, size_t count) {
  if (!destination || !count || count > capacity)
    throw std::invalid_argument("Invalid GPU clutter batch input");
  std::fill(destination + count, destination + capacity, Sample{});
}

template <class Sample>
void copyClutterBatch(Sample* destination, size_t capacity,
    const Sample* source, size_t count) {
  if (!destination || !source || !count || count > capacity)
    throw std::invalid_argument("Invalid GPU clutter batch input");
  std::copy_n(source, count, destination);
  padClutterBatch(destination, capacity, count);
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
// One shared queue fence, never more than one outstanding clutter submission.
// Vulkan-independent so exception/fence sequencing is tested without a driver.
class ClutterSubmission {
  bool pending_ = false, poisoned_ = false;
public:
  ClutterSubmission() = default;
  ClutterSubmission(const ClutterSubmission&) = delete;
  ClutterSubmission& operator=(const ClutterSubmission&) = delete;
  void ensureIdle() const {
    if (pending_ || poisoned_)
      throw std::logic_error("GPU clutter submission is pending or poisoned");
  }
  // A failed enqueue can leave device state uncertain. Teardown must drain
  // even if the API never reported a successful submission in that case.
  bool needsDrain() const noexcept { return pending_ || poisoned_; }
  template<class Enqueue> void enqueue(Enqueue&& enqueueOperation) {
    ensureIdle();
    try { enqueueOperation(); pending_ = true; }
    catch (...) { poisoned_ = true; throw; }
  }
  template<class Wait, class Reset> void complete(Wait&& wait, Reset&& reset) {
    if (!pending_ || poisoned_)
      throw std::logic_error("GPU clutter completion has no healthy submission");
    try {
      wait();             // Never reset a fence while its command is pending.
      reset();
      pending_ = false;   // Reset failure must not advertise a reusable fence.
    } catch (...) { poisoned_ = true; throw; }
  }
  template<class Host, class Complete> void overlap(Host&& host, Complete&& completeOperation) {
    if (!pending_ || poisoned_)
      throw std::logic_error("GPU clutter overlap has no healthy submission");
    std::exception_ptr hostFailure;
    try { host(); } catch (...) { hostFailure = std::current_exception(); }
    // Drain/reset even after allocation, numerical, or non-standard host
    // exceptions. A device completion fault takes precedence: it must not be
    // downgraded to a recoverable numerical rejection with an unsafe fence.
    completeOperation();
    ensureIdle();
    if (hostFailure) std::rethrow_exception(hostFailure);
  }
};
}
