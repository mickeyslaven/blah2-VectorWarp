#pragma once

#include "data/IqData.h"
#include <cmath>
#include <complex>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace blah2::startup {

// Bounded private qualification stimulus, never a capture/replay adapter. The
// caller selects a valid CFAR cell and derives amplitude from its actual gates.
struct Target {
 uint32_t delay=0;
 double dopplerHz=0,sampleRate=0,amplitude=0;
 std::vector<std::complex<double>> response;
};
inline void fillTargetIq(const std::vector<IqData*>& channels,uint32_t samples,
 unsigned frame,const Target& target,bool positive){
 if(channels.empty()||target.response.size()!=channels.size()||!samples||
    !target.delay||target.delay>=samples/4||!std::isfinite(target.sampleRate)||
    target.sampleRate<=0||!std::isfinite(target.dopplerHz)||
    !std::isfinite(target.amplitude)||target.amplitude<=0||target.amplitude>4096)
  throw std::invalid_argument("Private qualification target is outside bounded IQ geometry");
 std::vector<std::deque<std::complex<double>>*> output;
 for(auto* channel:channels){
  if(!channel||channel->get_n()<samples)
   throw std::invalid_argument("Private qualification IQ storage is too small");
  output.push_back(&channel->resize_for_write(samples));
 }
 for(auto value:target.response)if(!std::isfinite(value.real())||
   !std::isfinite(value.imag())||std::norm(value)>1.000001)
  throw std::invalid_argument("Private qualification spatial response invalid");
 uint32_t random=0x6d2b79f5u+frame*0x9e3779b9u;
 const auto scalar=[&]{random=random*1664525u+1013904223u;
  return (double(random>>16)-32768.0)/32768.0;};
 std::vector<std::complex<double>> common(samples+target.delay);
 for(auto& value:common){const double real=scalar(),imaginary=scalar();value={real,imaginary};}
 const double phaseStep=2*std::acos(-1.0)*target.dopplerHz/target.sampleRate;
 for(uint32_t sample=0;sample<samples;++sample){
  const auto echo=positive?target.amplitude*common[sample]*
   std::polar(1.0,phaseStep*(double(frame)*samples+sample)):std::complex<double>{};
  for(size_t channel=0;channel<channels.size();++channel){
   const double real=scalar(),imaginary=scalar();
   const auto value=64*target.amplitude*common[sample+target.delay]+
    .25*std::complex<double>{real,imaginary}+target.response[channel]*echo;
   // Both backends receive the exact same canonical FP32-representable values.
   (*output[channel])[sample]={double(float(value.real())),double(float(value.imag()))};
  }
 }
}

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
