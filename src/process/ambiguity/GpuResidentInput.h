#pragma once
#include "GpuResidentBackend.h"
#include "data/CanonicalSignal.h"
#include <cmath>

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
inline void copyCanonicalSignalInput(const Config& config,
 Span<std::complex<double>> output,const iq::CoherentSamples& capture,
 size_t first){
 const size_t n=config.geometry.clutterSamples;
 if(capture.channelCount()!=config.inputChannels||!output.data||
    output.size!=n*config.inputChannels)
  throw std::invalid_argument("Resident capture/canonical input dimensions differ");
 // Validate the entire immutable CPI before changing the worker mapping. A
 // malformed later channel must never leave a partially replaced frame.
 for(size_t channel=0;channel<capture.channelCount();++channel)
  capture.visitChannel(channel,[&](const auto& values){
   if(first>values.size()||n>values.size()-first)
    throw std::invalid_argument("Resident captured CPI size differs");
  });
 for(size_t channel=0;channel<capture.channelCount();++channel)
  capture.copyChannelTo(channel,output.data+channel*n,n,first);
}
// Qualification stage boundary, never a detector/tracker result. Fused.imag
// is a zero-based beam annotation, not a complex amplitude component.
template<class Map>
void copyProductOracleInput(const Config& config,Span<const std::complex<double>> fused,
 Map& output,std::vector<uint32_t>& beams){
 const auto& g=config.geometry;const uint64_t cells=uint64_t(g.doppler)*g.delays;
 const bool coherent=config.fusion.mode!=FusionMode::noncoherent;
 if(!cells||!fused.data||fused.size!=cells||output.get_nRows()!=g.doppler||
    output.get_nCols()!=g.delays||config.delayAxis.size()!=g.delays||config.dopplerAxis.size()!=g.doppler)
  throw std::invalid_argument("Resident product oracle input shape differs");
 for(size_t i=0;i<fused.size;++i){const auto value=fused.data[i];
  if(!std::isfinite(value.real())||value.real()<0||!std::isfinite(value.imag())||
     (coherent?(value.imag()<0||value.imag()>=config.fusion.beams||std::floor(value.imag())!=value.imag()):value.imag()!=0))
   throw std::invalid_argument("Resident product oracle amplitude/beam invalid");
 }
 output.delay.assign(config.delayAxis.begin(),config.delayAxis.end());
 output.doppler.assign(config.dopplerAxis.begin(),config.dopplerAxis.end());
 beams.clear();if(coherent)beams.resize(cells);
 for(size_t i=0;i<fused.size;++i){const auto value=fused.data[i];
  output.data[i/g.delays][i%g.delays]={value.real(),0};
  if(coherent)beams[i]=uint32_t(value.imag())+1u;
 }
}
}
