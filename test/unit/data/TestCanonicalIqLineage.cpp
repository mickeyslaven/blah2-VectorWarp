#include "data/CanonicalIqLineage.h"
#include "process/conditioning/CanonicalChannelizedIq.h"

#include <iostream>
#include <limits>
#include <stdexcept>

namespace
{
using blah2::iq::CanonicalLineage;
using blah2::iq::canonicalIqContinues;
using blah2::iq::validateCanonicalLineage;

void require(bool ok, const char* message)
{
  if (!ok) throw std::runtime_error(message);
}

template <class Function> void rejects(Function&& function)
{
  try { function(); }
  catch (const std::invalid_argument&) { return; }
  throw std::runtime_error("Invalid canonical lineage was accepted");
}

CanonicalLineage block(uint64_t first = 101, uint64_t end = 133)
{
  CanonicalLineage value;
  value.captureId = "capture-session-A";
  value.captureEpoch = 17;
  value.configurationEpoch = 21;
  value.laneId = "rf-lane-1";
  value.resetGeneration = 3;
  value.referenceId = "reference-definition-sha256-A";
  value.calibrationId = "calibration-snapshot-sha256-A";
  value.sourceFirst = first;
  value.sourceEnd = end;
  value.captureFrequencyHz = 100000000;
  value.frequencyHz = 99500000;
  value.inputRateHz = 2400000;
  value.sampleRateHz = 300000;
  value.inputSamplesPerOutput = 8;
  value.groupDelayInputSamples = 96.5;
  value.channelCount = 2;
  value.channelIds[0] = 4;
  value.channelIds[1] = 1;
  // The fixture's integer schedule is independent of the validator's formula.
  for (uint64_t sample = first; sample < end; ++sample)
    if (sample % 8 == 0)
    {
      if (!value.outputSamples) value.firstInputSample = sample;
      ++value.outputSamples;
    }
  return value;
}

void geometry()
{
  for (uint64_t phase = 0; phase < 8; ++phase)
    for (uint64_t length = 0; length < 32; ++length)
      validateCanonicalLineage(block(phase, phase + length));
  auto value = block((uint64_t(1) << 54) + 3, (uint64_t(1) << 54) + 35);
  validateCanonicalLineage(value);
  require(value.firstInputSample == (uint64_t(1) << 54) + 8,
    "Sample lineage lost integer precision above 2^53");
  value = block(std::numeric_limits<uint64_t>::max() - 31,
    std::numeric_limits<uint64_t>::max());
  validateCanonicalLineage(value);
  value = block(0, 8);
  value.firstInputSample = value.decimationPhase = 3;
  validateCanonicalLineage(value);
  value = block();
  value.inputSamplesPerOutput = 1;
  value.sampleRateHz = value.inputRateHz;
  value.groupDelayInputSamples = 0;
  value.firstInputSample = value.sourceFirst;
  value.outputSamples = value.sourceEnd - value.sourceFirst;
  value.captureEpoch = value.configurationEpoch = value.resetGeneration = 0;
  validateCanonicalLineage(value); // Explicit initial counters are valid.
  value.sourceFirst = value.firstInputSample = 0;
  value.sourceEnd = value.outputSamples = std::numeric_limits<uint64_t>::max();
  validateCanonicalLineage(value); // No intermediate span multiplication.
  value.captureId.assign(blah2::iq::maxLineageIdentityBytes, 'x');
  value.channelCount = blah2::iq::maxCanonicalChannels;
  for (uint32_t channel = 0; channel < value.channelCount; ++channel)
    value.channelIds[channel] = channel;
  validateCanonicalLineage(value); // Bounded identities and channel limit are inclusive.
}

void invalidGeometry()
{
  const auto invalid = [](auto mutate) {
    auto value = block(); mutate(value);
    rejects([&] { validateCanonicalLineage(value); });
  };
  invalid([](auto& v) { v.captureId.clear(); });
  invalid([](auto& v) { v.laneId.assign(129, 'x'); });
  invalid([](auto& v) { v.referenceId.push_back('\0'); });
  invalid([](auto& v) { v.calibrationId.clear(); });
  invalid([](auto& v) { v.captureEpoch.reset(); });
  invalid([](auto& v) { v.configurationEpoch.reset(); });
  invalid([](auto& v) { v.resetGeneration.reset(); });
  invalid([](auto& v) { v.channelCount = 0; });
  invalid([](auto& v) { v.channelCount = 9; });
  invalid([](auto& v) { v.channelIds[1] = v.channelIds[0]; });
  invalid([](auto& v) { v.captureFrequencyHz = 0; });
  invalid([](auto& v) { v.frequencyHz = 0; });
  invalid([](auto& v) { v.inputRateHz = 0; });
  invalid([](auto& v) { ++v.sampleRateHz; });
  invalid([](auto& v) { v.sampleRateHz = 0x80000000u + 1200000; v.inputSamplesPerOutput = 2; });
  invalid([](auto& v) { v.inputSamplesPerOutput = 0; });
  invalid([](auto& v) { v.decimationPhase = 8; });
  invalid([](auto& v) { v.groupDelayInputSamples = -0.5; });
  invalid([](auto& v) { v.groupDelayInputSamples = std::numeric_limits<double>::quiet_NaN(); });
  invalid([](auto& v) { v.groupDelayInputSamples = std::numeric_limits<double>::infinity(); });
  invalid([](auto& v) { v.sourceEnd = v.sourceFirst - 1; });
  invalid([](auto& v) { ++v.firstInputSample; });
  invalid([](auto& v) { ++v.outputSamples; });
  invalid([](auto& v) { v.outputSamples = std::numeric_limits<uint64_t>::max(); });
  auto empty = block(1, 2);
  empty.firstInputSample = 1;
  rejects([&] { validateCanonicalLineage(empty); });
}

void continuity()
{
  const auto previous = block();
  const auto next = block(133, 165);
  require(canonicalIqContinues(previous, next), "Adjacent blocks lost continuity");
  const auto breaks = [&](auto mutate) {
    auto changed = next; mutate(changed);
    require(!canonicalIqContinues(previous, changed), "Changed lineage retained history");
  };
  breaks([](auto& v) { v.captureId += "-other"; });
  breaks([](auto& v) { ++*v.captureEpoch; });
  breaks([](auto& v) { ++*v.configurationEpoch; });
  breaks([](auto& v) { v.laneId += "-other"; });
  breaks([](auto& v) { ++*v.resetGeneration; });
  breaks([](auto& v) { v.referenceId += "-other"; });
  breaks([](auto& v) { v.calibrationId += "-other"; });
  breaks([](auto& v) { ++v.captureFrequencyHz; });
  breaks([](auto& v) { ++v.frequencyHz; });
  breaks([](auto& v) { v.inputRateHz *= 2; v.sampleRateHz *= 2; });
  breaks([](auto& v) { v.inputSamplesPerOutput = 4; v.sampleRateHz *= 2; v.outputSamples *= 2; });
  breaks([](auto& v) { v.decimationPhase = 1; ++v.firstInputSample; });
  breaks([](auto& v) { v.groupDelayInputSamples += .5; });
  breaks([](auto& v) { v.channelCount = 1; });
  breaks([](auto& v) { std::swap(v.channelIds[0], v.channelIds[1]); });
  require(!canonicalIqContinues(previous, block(141, 173)), "Sample gap retained history");
  require(!canonicalIqContinues(previous, block(125, 157)), "Sample overlap retained history");
  const auto empty = block(133, 134);
  require(canonicalIqContinues(previous, empty) &&
    canonicalIqContinues(empty, block(134, 165)), "Empty output broke raw-input continuity");
}

blah2::channelizer::IlluminatorOutput outputFor(const CanonicalLineage& lineage)
{
  blah2::channelizer::IlluminatorOutput output;
  output.id = lineage.laneId;
  output.frequencyHz = lineage.frequencyHz;
  output.samples.firstInputSample = lineage.firstInputSample;
  output.samples.hasFirstInputSample = lineage.outputSamples != 0;
  output.samples.inputSamplesPerOutput = lineage.inputSamplesPerOutput;
  output.samples.groupDelayInputSamples = lineage.groupDelayInputSamples;
  output.samples.channels.resize(lineage.channelCount);
  for (auto& channel : output.samples.channels) channel.resize(lineage.outputSamples);
  return output;
}

void adapter()
{
  using blah2::channelizer::canonicalLineage;
  auto provenance = block();
  const auto output = outputFor(provenance);
  provenance.outputSamples = provenance.firstInputSample = 0;
  const auto actual = canonicalLineage(output, provenance);
  require(actual.outputSamples == 4 && actual.firstInputSample == 104 &&
    actual.sourceFirst == 101 && actual.sourceEnd == 133 &&
    actual.captureId == provenance.captureId && actual.captureEpoch == 17 &&
    actual.configurationEpoch == 21 && actual.resetGeneration == 3 &&
    actual.referenceId == provenance.referenceId && actual.calibrationId == provenance.calibrationId &&
    actual.channelIds == provenance.channelIds, "Adapter lost capture provenance");
  const auto invalid = [&](auto mutate) {
    auto changed = output; mutate(changed);
    rejects([&] { canonicalLineage(changed, provenance); });
  };
  invalid([](auto& v) { v.id += "-other"; });
  invalid([](auto& v) { ++v.frequencyHz; });
  invalid([](auto& v) { v.samples.channels.clear(); });
  invalid([](auto& v) { v.samples.channels[1].pop_back(); });
  invalid([](auto& v) { v.samples.hasFirstInputSample = false; });
  invalid([](auto& v) { ++v.samples.firstInputSample; });
  invalid([](auto& v) { v.samples.inputSamplesPerOutput = 4; });
  invalid([](auto& v) { ++v.samples.groupDelayInputSamples; });
  auto missingProvenance = provenance;
  missingProvenance.calibrationId.clear();
  rejects([&] { canonicalLineage(output, missingProvenance); });
  provenance = block(1, 2);
  const auto empty = outputFor(provenance);
  require(canonicalLineage(empty, provenance).outputSamples == 0,
    "Adapter rejected a legitimate empty decimation output");
  auto spurious = empty;
  spurious.samples.hasFirstInputSample = true;
  rejects([&] { canonicalLineage(spurious, provenance); });
}
}

int main()
{
  try
  {
    geometry(); invalidGeometry(); continuity(); adapter();
    std::cout << "Canonical IQ lineage and channelizer adapter tests passed\n";
    return 0;
  }
  catch (const std::exception& error)
  {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
