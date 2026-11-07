#pragma once
#include "process/ambiguity/GpuWidebandChannelizer.h"
#include <algorithm>
#include <cmath>
#include <exception>
#include <limits>
#include <string>
#include <utility>
#include <vector>
namespace blah2::wideband_gpu {
inline bool needsCpuFallback(const std::string& mode,bool gpuActive){return !gpuActive||mode=="auto";}
template<class Gpu,class Failure> bool attemptWithAutoFallback(bool automatic,Gpu&& gpu,Failure&& failure){try{std::forward<Gpu>(gpu)();return true;}catch(const std::exception& error){if(!automatic)throw;std::forward<Failure>(failure)(error);return false;}}

enum class QualificationStatus {passed,inconclusive,failed};
struct QualificationResult
{
  QualificationStatus status=QualificationStatus::failed;
  bool accepted=false;
  double worstRms=0,worstPeak=0;
  std::string reason;
};

// Compare the first real capture after both stateful channelizers process it.
// The caller publishes the CPU output for that capture regardless of outcome;
// on success both histories are already positioned for the next GPU capture,
// while on failure the CPU history is ready for fail-closed continuation.
inline QualificationResult qualifyFirstBlock(
  const std::vector<LaneOutput>& cpu,const std::vector<LaneOutput>& gpu,
  double gpuMilliseconds,double deadlineMilliseconds,
  double rmsLimit=2e-3,double peakLimit=5e-3)
{
  QualificationResult result;
  if(!std::isfinite(gpuMilliseconds)||gpuMilliseconds<0||
      !std::isfinite(deadlineMilliseconds)||deadlineMilliseconds<=0||
      gpuMilliseconds>deadlineMilliseconds||!std::isfinite(rmsLimit)||
      !std::isfinite(peakLimit)||rmsLimit<0||peakLimit<0)
  {result.reason="wideband GPU first-block deadline qualification failed";return result;}
  if(cpu.size()!=gpu.size())
  {result.reason="wideband GPU qualification lane count differs";return result;}
  double representativeEnergy=0,representativeSamples=0;
  for(size_t lane=0;lane<cpu.size();++lane)
  {
    const auto& expected=cpu[lane];const auto& actual=gpu[lane];
    if(expected.frequencyHz!=actual.frequencyHz||
       expected.firstInputSample!=actual.firstInputSample||
       expected.hasFirstInputSample!=actual.hasFirstInputSample||
       expected.inputSamplesPerOutput!=actual.inputSamplesPerOutput||
       expected.channels.size()!=actual.channels.size())
    {result.reason="wideband GPU qualification metadata differs";return result;}
    for(size_t channel=0;channel<expected.channels.size();++channel)
    {
      if(expected.channels[channel].size()!=actual.channels[channel].size())
      {result.reason="wideband GPU qualification sample count differs";return result;}
      double error=0,signal=0,maxError=0,peak=0;
      for(size_t sample=0;sample<expected.channels[channel].size();++sample)
      {
        const std::complex<double> reference(expected.channels[channel][sample]);
        const std::complex<double> difference=
          std::complex<double>(actual.channels[channel][sample])-reference;
        const double e=std::norm(difference),s=std::norm(reference);
        if(!std::isfinite(e)||!std::isfinite(s))
        {result.reason="wideband GPU qualification output is non-finite";return result;}
        error+=e;signal+=s;representativeEnergy+=s;
        representativeSamples+=1;maxError=std::max(maxError,e);
        peak=std::max(peak,s);
      }
      if(!std::isfinite(error)||!std::isfinite(signal)||
          !std::isfinite(representativeEnergy))
      {result.reason="wideband GPU qualification energy overflowed";
       return result;}
      const double rms=std::sqrt(error/std::max(signal,1e-30));
      const double relativePeak=std::sqrt(maxError/std::max(peak,1e-30));
      result.worstRms=std::max(result.worstRms,rms);
      result.worstPeak=std::max(result.worstPeak,relativePeak);
      if(rms>rmsLimit||relativePeak>peakLimit)
      {result.reason="wideband GPU first-block numerical qualification failed";return result;}
    }
  }
  if(!representativeSamples||
      representativeEnergy<=representativeSamples*1e-20)
  {result.status=QualificationStatus::inconclusive;
   result.reason="wideband GPU qualification input is not representative";
   return result;}
  result.status=QualificationStatus::passed;result.accepted=true;
  result.reason="wideband GPU first-block qualification passed";
  return result;
}
}
