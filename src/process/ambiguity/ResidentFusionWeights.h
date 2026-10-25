#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace blah2::resident_fusion {
inline std::array<std::complex<double>,8> normalizedWeights(
 const std::array<std::complex<double>,8>& steering,
 const std::complex<double>* directWeights,const double* channelScale,
 uint32_t channels,double nullFraction,bool whiten){
 if(!channels||channels>8||!directWeights||!channelScale||
    !std::isfinite(nullFraction)||nullFraction<0||nullFraction>1)
  throw std::invalid_argument("Surveillance fusion null dimensions invalid");
 std::array<std::complex<double>,8> combined{},anchor{},orthogonal{};
 double directNorm=0,combinedNorm=0;
 for(uint32_t channel=0;channel<channels;++channel){
  if(!std::isfinite(directWeights[channel].real())||
     !std::isfinite(directWeights[channel].imag())||
     !std::isfinite(channelScale[channel])||!(channelScale[channel]>0))
   throw std::invalid_argument("Surveillance fusion weights invalid");
  directNorm+=std::norm(directWeights[channel]);
  combined[channel]=(whiten?channelScale[channel]:1.0)*steering[channel];
  combinedNorm+=std::norm(combined[channel]);
 }
 if(!(directNorm>0)||!std::isfinite(directNorm)||
    !(combinedNorm>0)||!std::isfinite(combinedNorm))
  throw std::invalid_argument("Surveillance fusion produced invalid weights");
 const double combinedScale=1.0/std::sqrt(combinedNorm);
 const double anchorScale=1.0/std::sqrt(directNorm);
 std::complex<double> alpha{};
 for(uint32_t channel=0;channel<channels;++channel){
  combined[channel]*=combinedScale;
  anchor[channel]=directWeights[channel]*anchorScale;
  alpha+=std::conj(anchor[channel])*combined[channel];
 }
 if(nullFraction<=0)return combined;
 double orthogonalNorm=0;
 for(uint32_t channel=0;channel<channels;++channel){
  orthogonal[channel]=combined[channel]-alpha*anchor[channel];
  orthogonalNorm+=std::norm(orthogonal[channel]);
 }
 const auto targetAlpha=(1.0-nullFraction)*alpha;
 const double remaining=std::max(0.0,1.0-std::norm(targetAlpha));
 if(remaining>1e-15&&orthogonalNorm<=1e-15){
  uint32_t basis=0;
  for(uint32_t channel=1;channel<channels;++channel)
   if(std::norm(anchor[channel])<std::norm(anchor[basis]))basis=channel;
  const auto basisProjection=std::conj(anchor[basis]);
  orthogonalNorm=0;
  for(uint32_t channel=0;channel<channels;++channel){
   orthogonal[channel]=(channel==basis?std::complex<double>(1,0):std::complex<double>{})-
    basisProjection*anchor[channel];
   orthogonalNorm+=std::norm(orthogonal[channel]);
  }
 }
 if(remaining>1e-15&&!(orthogonalNorm>0))
  throw std::invalid_argument("Surveillance fusion cannot preserve direct-path null");
 const double orthogonalScale=remaining>1e-15?std::sqrt(remaining/orthogonalNorm):0;
 for(uint32_t channel=0;channel<channels;++channel)
  combined[channel]=targetAlpha*anchor[channel]+orthogonalScale*orthogonal[channel];
 return combined;
}

inline std::array<std::complex<double>,8> measuredWeights(
 const std::vector<std::complex<float>>& manifold,uint32_t beams,uint32_t channels,
 uint32_t beam,uint32_t directBeam,const std::complex<double>* directWeights,
 const double* channelScale,double nullFraction,bool whiten){
 if(!channels||channels>8||beam>=beams||directBeam>=beams||!directWeights||
    !channelScale||manifold.size()!=uint64_t(beams)*channels||
    !std::isfinite(nullFraction)||nullFraction<0||nullFraction>1)
  throw std::invalid_argument("Measured surveillance manifold dimensions invalid");
 std::array<std::complex<double>,8> steering{};
 double directNorm=0,steeringNorm=0;
 for(uint32_t channel=0;channel<channels;++channel){
  const auto direct=directWeights[channel];
  const auto targetResponse=manifold[uint64_t(beam)*channels+channel];
  const auto directResponse=manifold[uint64_t(directBeam)*channels+channel];
  if(!std::isfinite(direct.real())||!std::isfinite(direct.imag())||
     !std::isfinite(targetResponse.real())||!std::isfinite(targetResponse.imag())||
     !std::isfinite(directResponse.real())||!std::isfinite(directResponse.imag())||
     !(std::norm(targetResponse)>1e-12f)||!(std::norm(directResponse)>1e-12f))
   throw std::invalid_argument("Measured surveillance manifold response invalid");
  directNorm+=std::norm(direct);
  steering[channel]=direct*(std::complex<double>(targetResponse)/
    std::complex<double>(directResponse));
  steeringNorm+=std::norm(steering[channel]);
 }
 if(!(directNorm>0)||!std::isfinite(directNorm)||
    !(steeringNorm>0)||!std::isfinite(steeringNorm))
  throw std::invalid_argument("Measured surveillance manifold produced invalid weights");
 const double steeringScale=std::sqrt(directNorm/steeringNorm);
 for(uint32_t channel=0;channel<channels;++channel){
  steering[channel]*=steeringScale;
 }
 return normalizedWeights(steering,directWeights,channelScale,channels,
  nullFraction,whiten);
}
}
