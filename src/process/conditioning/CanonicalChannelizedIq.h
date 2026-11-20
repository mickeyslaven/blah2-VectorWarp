#pragma once

#include "data/CanonicalIqLineage.h"
#include "process/conditioning/IlluminatorChannelizerBank.h"

namespace blah2::channelizer
{

// Explicit, opt-in adapter. The producer supplies capture/configuration,
// reference/calibration, reset and physical-channel identities. Do not derive
// them from a wall clock, block ordinal, receiver name, or default settings.
// sourceFirst/sourceEnd describe the exact raw block passed to process().
// No IQ is copied and no existing output layout or worker ABI changes.
inline iq::CanonicalLineage canonicalLineage(const IlluminatorOutput& output,
  iq::CanonicalLineage provenance)
{
  if (output.id != provenance.laneId || output.frequencyHz != provenance.frequencyHz)
    throw std::invalid_argument("Channelizer lane does not match canonical provenance");
  output.samples.validateStorage();
  if (output.samples.channelCount() != provenance.channelCount ||
      provenance.channelCount == 0 || provenance.channelCount > iq::maxCanonicalChannels)
    throw std::invalid_argument("Channelizer channels do not match canonical provenance");
  const auto channelSize = [](const auto& view) { return view.size(); };
  const auto count = output.samples.visitChannel(0, channelSize);
  for (std::size_t channel = 0; channel < output.samples.channelCount(); ++channel)
    if (output.samples.visitChannel(channel, channelSize) != count)
      throw std::invalid_argument("Channelizer coherent channels have unequal lengths");
  if (output.samples.hasFirstInputSample != (count != 0))
    throw std::invalid_argument("Channelizer output sample anchor is missing or spurious");
  if (output.samples.inputSamplesPerOutput != provenance.inputSamplesPerOutput ||
      output.samples.groupDelayInputSamples != provenance.groupDelayInputSamples ||
      provenance.decimationPhase != 0)
    throw std::invalid_argument("Channelizer transform does not match canonical provenance");
  provenance.firstInputSample = output.samples.firstInputSample;
  provenance.outputSamples = count;
  iq::validateCanonicalLineage(provenance);
  return provenance;
}

} // namespace blah2::channelizer
