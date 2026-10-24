#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace blah2::resident_fusion {
inline std::array<std::complex<double>,8> calibratedWeights(
 std::array<std::complex<double>,8> steering,uint32_t channels,
 const std::complex<double>* directWeights,const double* channelScale,
 double nullFraction,bool whiten){
 if(!channels||channels>8||!directWeights||!channelScale||
    !std::isfinite(nullFraction)||nullFraction<0||nullFraction>1)
  throw std::invalid_argument("Surveillance fusion calibration arguments invalid");
 double directNorm=0,steeringNorm=0;
 for(uint32_t channel=0;channel<channels;++channel){
  if(!std::isfinite(directWeights[channel].real())||
     !std::isfinite(directWeights[channel].imag())||
     !std::isfinite(steering[channel].real())||
     !std::isfinite(steering[channel].imag()))
   throw std::invalid_argument("Surveillance fusion calibration weights invalid");
  directNorm+=std::norm(directWeights[channel]);
  steeringNorm+=std::norm(steering[channel]);
 }
 if(!(directNorm>0)||!std::isfinite(directNorm)||
    !(steeringNorm>0)||!std::isfinite(steeringNorm))
  throw std::invalid_argument("Surveillance fusion calibration has zero norm");
 const double steeringScale=std::sqrt(directNorm/steeringNorm);
 for(uint32_t channel=0;channel<channels;++channel)steering[channel]*=steeringScale;
 std::array<std::complex<double>,8> combined{};
 std::complex<double> projection{};
 for(uint32_t channel=0;channel<channels;++channel)
  projection+=std::conj(directWeights[channel])*steering[channel];
 projection*=nullFraction/directNorm;
 for(uint32_t channel=0;channel<channels;++channel)
  steering[channel]-=projection*directWeights[channel];
 if(whiten){
  double combinedNorm=0;
  for(uint32_t channel=0;channel<channels;++channel){
   if(!std::isfinite(channelScale[channel])||!(channelScale[channel]>0))
    throw std::invalid_argument("Noise-whitened fusion scale invalid");
   combined[channel]=channelScale[channel]*steering[channel];
   combinedNorm+=std::norm(combined[channel]);
  }
  if(!(combinedNorm>0)||!std::isfinite(combinedNorm))
   throw std::invalid_argument("Noise-whitened fusion produced invalid weights");
  const double combinedScale=1.0/std::sqrt(combinedNorm);
  for(uint32_t channel=0;channel<channels;++channel)combined[channel]*=combinedScale;
 }else{
  for(uint32_t channel=0;channel<channels;++channel)
   combined[channel]=steering[channel]/std::sqrt(directNorm);
 }
 return combined;
}

inline std::array<std::complex<double>,8> measuredWeights(
 const std::vector<std::complex<float>>& manifold,
 const std::vector<std::complex<float>>& directResponse,uint32_t beams,uint32_t channels,
 uint32_t beam,const std::complex<double>* directWeights,
 const double* channelScale,double nullFraction,bool whiten){
 if(!channels||channels>8||beam>=beams||!directWeights||
    !channelScale||manifold.size()!=uint64_t(beams)*channels||
    directResponse.size()!=channels||
    !std::isfinite(nullFraction)||nullFraction<0||nullFraction>1)
  throw std::invalid_argument("Measured surveillance manifold dimensions invalid");
 std::array<std::complex<double>,8> steering{};
 for(uint32_t channel=0;channel<channels;++channel){
  const auto direct=directWeights[channel];
  const auto targetResponse=manifold[uint64_t(beam)*channels+channel];
  const auto anchorResponse=directResponse[channel];
  if(!std::isfinite(direct.real())||!std::isfinite(direct.imag())||
     !std::isfinite(targetResponse.real())||!std::isfinite(targetResponse.imag())||
     !std::isfinite(anchorResponse.real())||!std::isfinite(anchorResponse.imag())||
     !(std::norm(targetResponse)>1e-12f)||!(std::norm(anchorResponse)>1e-12f))
   throw std::invalid_argument("Measured surveillance manifold response invalid");
  steering[channel]=direct*(std::complex<double>(targetResponse)/
    std::complex<double>(anchorResponse));
 }
 return calibratedWeights(steering,channels,directWeights,channelScale,nullFraction,whiten);
}
}
