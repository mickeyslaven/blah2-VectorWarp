#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace blah2::resident_fusion {
// Shared bounded diagonal whitening calibration. Both resident fusion and its
// canonical CPU qualification oracle consume their independently learned
// reference covariance; neither may substitute the other backend's weights.
inline std::array<double,8> whiteningScales(const std::complex<double>* covariance,
 const std::complex<double>* weights,uint32_t channels,double diagonalLoading,
 bool covarianceValid){
 std::array<double,8> scale{};scale.fill(1.0);
 if(!covarianceValid)return scale;
 if(!covariance||!weights||!channels||channels>8||
    !std::isfinite(diagonalLoading)||diagonalLoading<0)
  throw std::invalid_argument("Surveillance whitening covariance invalid");
 for(uint32_t i=0;i<channels*channels;++i)
  if(!std::isfinite(covariance[i].real())||!std::isfinite(covariance[i].imag()))
   throw std::invalid_argument("Surveillance whitening covariance is nonfinite");
 for(uint32_t i=0;i<channels;++i)
  if(!std::isfinite(weights[i].real())||!std::isfinite(weights[i].imag()))
   throw std::invalid_argument("Surveillance whitening reference weight is nonfinite");
 double loadedTrace=0;
 for(uint32_t c=0;c<channels;++c)loadedTrace+=covariance[c*channels+c].real();
 const double rawTrace=loadedTrace/(1.0+diagonalLoading);
 const double referenceLoading=diagonalLoading*rawTrace/channels;
 std::complex<double> rayleigh{};
 for(uint32_t left=0;left<channels;++left)
  for(uint32_t right=0;right<channels;++right)
   rayleigh+=std::conj(weights[left])*covariance[left*channels+right]*weights[right];
 const double principal=std::max(0.0,rayleigh.real()-referenceLoading);
 std::array<double,8> noise{},ordered{};
 double residualTrace=0;
 for(uint32_t channel=0;channel<channels;++channel){
  noise[channel]=std::max(0.0,covariance[channel*channels+channel].real()-
   principal*std::norm(weights[channel]));
  residualTrace+=noise[channel];
 }
 const double floor=std::max(referenceLoading,.05*residualTrace/channels);
 for(uint32_t channel=0;channel<channels;++channel)
  ordered[channel]=noise[channel]=std::max(noise[channel],floor);
 for(uint32_t channel=1;channel<channels;++channel){
  const double value=ordered[channel];uint32_t position=channel;
  while(position&&ordered[position-1]>value){
   ordered[position]=ordered[position-1];--position;
  }
  ordered[position]=value;
 }
 const double median=ordered[channels/2];
 if(std::isfinite(median)&&median>0)
  for(uint32_t channel=0;channel<channels;++channel)
   scale[channel]=std::clamp(median/noise[channel],.5,2.0);
 return scale;
}

inline std::array<std::complex<double>,8> calibratedWeights(
 std::array<std::complex<double>,8> steering,uint32_t channels,
 const std::complex<double>* directWeights,const double* channelScale,
 double nullFraction,bool whiten){
 if(!channels||channels>8||!directWeights||!channelScale||
    !std::isfinite(nullFraction)||nullFraction<0||nullFraction>1)
  throw std::invalid_argument("Surveillance fusion calibration arguments invalid");
 std::array<std::complex<double>,8> combined{},anchor{};
 double directNorm=0,combinedNorm=0;
 for(uint32_t channel=0;channel<channels;++channel){
  if(!std::isfinite(directWeights[channel].real())||
     !std::isfinite(directWeights[channel].imag())||
     !std::isfinite(steering[channel].real())||
     !std::isfinite(steering[channel].imag())||
     !std::isfinite(channelScale[channel])||!(channelScale[channel]>0))
   throw std::invalid_argument("Surveillance fusion calibration weights invalid");
  directNorm+=std::norm(directWeights[channel]);
  combined[channel]=(whiten?channelScale[channel]:1.0)*steering[channel];
  combinedNorm+=std::norm(combined[channel]);
 }
 if(!(directNorm>0)||!std::isfinite(directNorm)||
    !(combinedNorm>0)||!std::isfinite(combinedNorm))
  throw std::invalid_argument("Surveillance fusion calibration has zero norm");
 const double combinedScale=1.0/std::sqrt(combinedNorm);
 const double anchorScale=1.0/std::sqrt(directNorm);
 std::complex<double> alpha{};
 for(uint32_t channel=0;channel<channels;++channel){
  combined[channel]*=combinedScale;
  anchor[channel]=directWeights[channel]*anchorScale;
  alpha+=std::conj(anchor[channel])*combined[channel];
 }
 // Apply the requested amplitude null after optional whitening and baseline
 // normalization.  Do not renormalize afterward: doing so erases an aligned
 // null, and inventing an arbitrary orthogonal component makes the result
 // depend on an eigenvector's otherwise meaningless global phase.
 if(nullFraction>0)for(uint32_t channel=0;channel<channels;++channel)
  combined[channel]-=nullFraction*alpha*anchor[channel];
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
