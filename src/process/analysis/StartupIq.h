#pragma once

#include "data/IqData.h"
#include <cmath>
#include <complex>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace blah2::startup {

// Private deterministic broadband IQ for device/geometry qualification only.
// Coherent direct energy plus independent channel noise keeps clutter solves
// nondegenerate. Power-of-two scaling exercises three different input levels.
// No capture identity, timestamp, tracker, recording or publication is involved.
inline void fillIq(const std::vector<IqData*>& channels, uint32_t samples,
    unsigned frame) {
  if (channels.empty() || !samples)
    throw std::invalid_argument("Startup IQ geometry is empty");
  std::vector<std::deque<std::complex<double>>*> output;
  output.reserve(channels.size());
  for (auto* channel : channels) {
    if (!channel || channel->get_n() < samples)
      throw std::invalid_argument("Startup IQ storage is too small");
    output.push_back(&channel->resize_for_write(samples));
  }
  uint32_t random = 0x6d2b79f5u + frame * 0x9e3779b9u;
  const auto scalar = [&] {
    random = random * 1664525u + 1013904223u;
    return (double(random >> 16) - 32768.0) / 32768.0;
  };
  const double scale = std::ldexp(1.0, int(frame % 3) * 4 - 4);
  for (uint32_t sample = 0; sample < samples; ++sample) {
    const double real = scalar(), imaginary = scalar();
    const std::complex<double> common{real, imaginary};
    for (std::size_t channel = 0; channel < channels.size(); ++channel) {
      const double noiseReal = scalar(), noiseImaginary = scalar();
      (*output[channel])[sample] = scale *
        (common * std::complex<double>{1.0, double(channel) / 16.0} +
         std::complex<double>{noiseReal, noiseImaginary} * .25);
    }
  }
}

} // namespace blah2::startup
