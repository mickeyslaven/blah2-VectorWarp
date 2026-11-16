#pragma once

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>

namespace blah2::iq
{

constexpr std::size_t maxCanonicalChannels = 8;
constexpr std::size_t maxLineageIdentityBytes = 128;

// Receiver/OS-independent provenance for one coherent per-lane IQ block.
// This is an in-process value contract, not a byte-serializable worker ABI.
// Validate at ingress and retain it unchanged beside the corresponding IQ.
struct CanonicalLineage
{
  // captureId identifies an acquisition session globally; replay preserves
  // the original identity. All counters are scoped to that session. Zero is
  // a valid initial epoch/generation, never a synonym for "unknown".
  std::string captureId;
  std::optional<uint64_t> captureEpoch;
  std::optional<uint64_t> configurationEpoch;
  std::string laneId;
  std::optional<uint64_t> resetGeneration;
  // These identify the exact reference definition and calibration snapshot.
  // An explicitly uncalibrated stream needs its own nonempty identity.
  std::string referenceId;
  std::string calibrationId;

  // Half-open interval [sourceFirst, sourceEnd) consumed from this capture
  // epoch, in original input samples. FIR history may precede sourceFirst;
  // this interval must never be relabelled as the entire FIR support.
  uint64_t sourceFirst = 0;
  uint64_t sourceEnd = 0;
  uint64_t captureFrequencyHz = 0;
  uint64_t frequencyHz = 0;
  uint32_t inputRateHz = 0;
  uint32_t sampleRateHz = 0;
  uint32_t inputSamplesPerOutput = 1;
  uint32_t decimationPhase = 0;
  double groupDelayInputSamples = 0;

  // The kth output was emitted at firstInputSample + k * decimation.
  // Its nominal signal time is earlier by groupDelayInputSamples. Keep the
  // integer sample anchor separate from delay, including near UINT64_MAX.
  // Empty outputs use firstInputSample == 0 (outputSamples disambiguates it).
  uint64_t firstInputSample = 0;
  uint64_t outputSamples = 0;
  uint32_t channelCount = 0;
  // Payload order -> immutable physical input identity; never a role index.
  std::array<uint32_t, maxCanonicalChannels> channelIds{};
};

inline bool validLineageIdentity(const std::string& identity)
{
  if (identity.empty() || identity.size() > maxLineageIdentityBytes) return false;
  for (const unsigned char value : identity)
    if (value < 32 || value == 127) return false;
  return true;
}

inline void validateCanonicalLineage(const CanonicalLineage& value)
{
  if (!validLineageIdentity(value.captureId) ||
      !validLineageIdentity(value.laneId) ||
      !validLineageIdentity(value.referenceId) ||
      !validLineageIdentity(value.calibrationId))
    throw std::invalid_argument("Canonical IQ lineage requires bounded explicit identities");
  if (!value.captureEpoch || !value.configurationEpoch || !value.resetGeneration)
    throw std::invalid_argument("Canonical IQ lineage requires explicit epochs and reset generation");
  if (value.channelCount == 0 || value.channelCount > maxCanonicalChannels)
    throw std::invalid_argument("Canonical IQ channel count is outside the contract");
  for (uint32_t i = 0; i < value.channelCount; ++i)
    for (uint32_t j = 0; j < i; ++j)
      if (value.channelIds[i] == value.channelIds[j])
        throw std::invalid_argument("Canonical IQ physical channel identities must be unique");
  if (!value.captureFrequencyHz || !value.frequencyHz || !value.inputRateHz ||
      !value.sampleRateHz || !value.inputSamplesPerOutput ||
      uint64_t(value.sampleRateHz) * value.inputSamplesPerOutput != value.inputRateHz ||
      value.decimationPhase >= value.inputSamplesPerOutput)
    throw std::invalid_argument("Canonical IQ RF/rate/decimation metadata is inconsistent");
  if (!std::isfinite(value.groupDelayInputSamples) || value.groupDelayInputSamples < 0)
    throw std::invalid_argument("Canonical IQ FIR group delay must be finite and nonnegative");
  if (value.sourceEnd < value.sourceFirst)
    throw std::invalid_argument("Canonical IQ source interval is reversed");

  const uint64_t decimation = value.inputSamplesPerOutput;
  const uint64_t offset = (uint64_t(value.decimationPhase) + decimation -
    value.sourceFirst % decimation) % decimation;
  const uint64_t count = value.sourceEnd - value.sourceFirst;
  // Division avoids forming an overflowing outputSamples * decimation span.
  const uint64_t expected = offset < count ? 1 + (count - 1 - offset) / decimation : 0;
  if (value.outputSamples != expected ||
      (expected ? value.firstInputSample != value.sourceFirst + offset :
        value.firstInputSample != 0))
    throw std::invalid_argument("Canonical IQ output schedule does not match its source interval");
}

// Streaming channelizer continuity, not CPI-overlap eligibility. A caller
// must reset before bridging a gap or changing any provenance/physical
// transform, and advance resetGeneration when a state reset occurs. This
// predicate deliberately cannot manufacture missing provenance or resets.
inline bool canonicalIqContinues(const CanonicalLineage& previous,
  const CanonicalLineage& next)
{
  validateCanonicalLineage(previous);
  validateCanonicalLineage(next);
  if (previous.captureId != next.captureId ||
      previous.captureEpoch != next.captureEpoch ||
      previous.configurationEpoch != next.configurationEpoch ||
      previous.laneId != next.laneId ||
      previous.resetGeneration != next.resetGeneration ||
      previous.referenceId != next.referenceId ||
      previous.calibrationId != next.calibrationId ||
      previous.sourceEnd != next.sourceFirst ||
      previous.captureFrequencyHz != next.captureFrequencyHz ||
      previous.frequencyHz != next.frequencyHz ||
      previous.inputRateHz != next.inputRateHz ||
      previous.sampleRateHz != next.sampleRateHz ||
      previous.inputSamplesPerOutput != next.inputSamplesPerOutput ||
      previous.decimationPhase != next.decimationPhase ||
      previous.groupDelayInputSamples != next.groupDelayInputSamples ||
      previous.channelCount != next.channelCount) return false;
  for (uint32_t i = 0; i < previous.channelCount; ++i)
    if (previous.channelIds[i] != next.channelIds[i]) return false;
  return true;
}

} // namespace blah2::iq
