#ifndef BEARING_STEERING_H
#define BEARING_STEERING_H

#include <complex>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace blah2::bearing_steering
{
inline uint32_t physicalElement(uint32_t channel,
  const std::vector<uint32_t>& channelMapping)
{
  if (channelMapping.empty()) return channel;
  if (channel >= channelMapping.size())
    throw std::invalid_argument("Bearing channel mapping is incomplete");
  return channelMapping[channel];
}

// Per-channel ambiguity-map responses use the phase convention emitted by the
// correlation stage.  It is the opposite of the raw-IQ steering convention
// used by CoherentBeamBank.  Return the coefficient applied directly to that
// map response; do not reuse the raw-IQ sign here.
inline std::complex<double> mapCoefficient(
  std::complex<double> directPathResponse, double relativePhase)
{
  return std::conj(directPathResponse * std::polar(1.0, relativePhase));
}
}

#endif
