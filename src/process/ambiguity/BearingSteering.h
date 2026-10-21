#ifndef BEARING_STEERING_H
#define BEARING_STEERING_H

#include <complex>
#include <cmath>
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

// CAF computes surveillance * conj(reference), preserving a constant complex
// surveillance-channel multiplier. Match CoherentBeamBank's negative raw-IQ
// spatial phase; conjugate the model only when forming the combining weight.
inline std::complex<double> mapCoefficient(
  std::complex<double> directPathResponse, double relativePhase)
{
  return std::conj(directPathResponse * std::polar(1.0, -relativePhase));
}

// Bounded rank-one cancellation model for explicit replay experiments only.
// General multi-tap cancellation is not necessarily this orthogonal projection.
// Measured post-clutter manifolds must not be projected a second time.
inline bool normalizeModel(std::complex<double>* response, uint32_t channels,
  const std::complex<double>* referenceWeights, bool projectReference)
{
  if (!response || !channels) return false;
  const auto reject = [&]() {
    for (uint32_t c = 0; c < channels; ++c) response[c] = {};
    return false;
  };
  double originalPower = 0;
  for (uint32_t c = 0; c < channels; ++c) {
    if (!std::isfinite(response[c].real()) || !std::isfinite(response[c].imag()))
      return reject();
    originalPower += std::norm(response[c]);
  }
  if (!(originalPower > 0) || !std::isfinite(originalPower)) return reject();
  if (projectReference) {
    if (!referenceWeights) return reject();
    double referencePower = 0;
    std::complex<double> projection{};
    for (uint32_t c = 0; c < channels; ++c) {
      if (!std::isfinite(referenceWeights[c].real()) ||
          !std::isfinite(referenceWeights[c].imag())) return reject();
      referencePower += std::norm(referenceWeights[c]);
      projection += std::conj(referenceWeights[c]) * response[c];
    }
    if (!(referencePower > 0) || !std::isfinite(referencePower)) return reject();
    projection /= referencePower;
    for (uint32_t c = 0; c < channels; ++c)
      response[c] -= projection * referenceWeights[c];
  }
  double power = 0;
  for (uint32_t c = 0; c < channels; ++c) power += std::norm(response[c]);
  // Do not amplify a candidate that cancellation makes unobservable.
  if (!(power > originalPower * 1e-12) || !std::isfinite(power)) return reject();
  const double scale = 1 / std::sqrt(power);
  for (uint32_t c = 0; c < channels; ++c) response[c] *= scale;
  return true;
}
}

#endif
