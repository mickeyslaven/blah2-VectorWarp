#pragma once
#include "GpuResidentBackend.h"

namespace blah2::resident {
// Shared by Coordinator and CPU boundary tests. Validate the complete capture
// before copying; preserve canonical capture order, including the reference.
template<class Capture,class Samples>
void copyCanonicalInput(const Config& config,Span<std::complex<double>> output,
 const Capture& capture,Samples samples){
 const size_t n=config.geometry.clutterSamples;
 if(capture.size()!=config.inputChannels||!output.data||
    output.size!=n*config.inputChannels)
  throw std::invalid_argument("Resident capture/canonical input dimensions differ");
 for(const auto& channel:capture)if(samples(channel).size()!=n)
  throw std::invalid_argument("Resident captured CPI size differs");
 size_t offset=0;
 for(const auto& channel:capture){const auto& values=samples(channel);
  std::copy(values.begin(),values.end(),output.data+offset);offset+=n;}
}
}
