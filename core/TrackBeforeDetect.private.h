#pragma once
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <vector>
#include "process/detection/IntegratedCfar.h"

namespace track_before_detect {
constexpr uint32_t splitMotionFlag = 0x80000000u;
constexpr uint32_t fractionalDelayFlag = 0x40000000u;
struct MotionBins { uint32_t delay=0,doppler=0; bool fractionalDelay=false; };
inline uint32_t encodeMotionBins(uint32_t delay,uint32_t doppler,bool fractionalDelay=false){
  if(delay>2||doppler>16)throw std::invalid_argument("Track-before-detect motion bins invalid");
  return splitMotionFlag|(fractionalDelay?fractionalDelayFlag:0u)|(doppler<<8)|delay;
}
inline MotionBins decodeMotionBins(uint32_t value){
  if(!(value&splitMotionFlag)){
    if(value>2)throw std::invalid_argument("Track-before-detect legacy motion encoding invalid");
    return {value,value,false};
  }
  if(value&~(splitMotionFlag|fractionalDelayFlag|0x00001f03u))throw std::invalid_argument("Track-before-detect motion encoding invalid");
  MotionBins result{value&0xffu,(value>>8)&0xffu,bool(value&fractionalDelayFlag)};
  if(result.delay>2||result.doppler>16)throw std::invalid_argument("Track-before-detect motion bins invalid");
  return result;
}
inline uint32_t motionEcho(const MotionBins& motion){
  return motion.delay|(motion.doppler<<16u)|(motion.fractionalDelay?fractionalDelayFlag:0u);
}

// Radar convention: delay rate = -Doppler * wavelength / range-bin width.
// Inter-CPI stride uses all consumed capture samples, including the tail not
// retained by the block correlation. Doppler-bin spacing is a separate issue.
// No truth, learned target state, device identity or configured nominal CPI.
inline double delayDriftPerHz(uint32_t cpiStepSamples,double carrierHz){
  if(!cpiStepSamples||!std::isfinite(carrierHz)||carrierHz<=0)
    throw std::invalid_argument("Fractional delay geometry invalid");
  return -double(cpiStepSamples)/carrierHz;
}
constexpr double maximumDelayRate=256.0;
// One immutable offset per Doppler row/path/age; delay columns and residual
// delay paths share it. Zero drift retains the allocation-free integer path.
inline uint32_t fractionalDelayTableWords(uint32_t rows,uint32_t depth,
    uint32_t dopplerBins,double scale){
  if(!rows||rows>65535||(depth!=4&&depth!=8)||dopplerBins>16||
      !std::isfinite(scale)||scale>0)
    throw std::invalid_argument("Fractional delay table geometry invalid");
  return scale==0?0:rows*(2u*dopplerBins+1u)*depth;
}
inline void validateDelayDrift(double scale,const std::vector<double>& doppler){
  if(!std::isfinite(scale)||scale>0)
    throw std::invalid_argument("Fractional delay scale invalid");
  for(double hz:doppler)if(!std::isfinite(hz)||!std::isfinite(scale*hz)||
      std::abs(scale*hz)>maximumDelayRate)
    throw std::invalid_argument("Fractional delay exceeds bounded coordinate range");
}
// Q16 nearest-coordinate interpolation, ties away from zero displacement.
// One cell from each distinct CPI, never a blend or multiple votes. Endpoint
// Dopplers integrate the existing constant-Doppler-motion path trapezoidally.
inline int delayPathColumn(int column,int residual,uint32_t age,double scale,
    double newestDoppler,double olderDoppler){
  if(age>7||residual < -2||residual>2||!std::isfinite(scale)||
      !std::isfinite(newestDoppler)||!std::isfinite(olderDoppler))
    throw std::invalid_argument("Fractional delay path invalid");
  if(scale==0)return column-residual*int(age);
  const double meanHz=.5*newestDoppler+.5*olderDoppler;
  const double rate=scale*meanHz;
  if(!std::isfinite(rate)||std::abs(rate)>maximumDelayRate)
    throw std::invalid_argument("Fractional delay path exceeds bound");
  const double displacement=rate*double(age);
  const int fixed=int(std::floor(std::abs(displacement)*65536.0+.5));
  const int nearest=(fixed+32768)/65536;
  return column-residual*int(age)-(displacement<0?-nearest:nearest);
}
// Independent CPU scalar oracle for a single path. Histories are newest-first.
// -1 invalidates the whole depth at an edge/nonfinite value, never clamps or
// renormalizes a partial path into a shorter, incorrectly calibrated bank.
inline double pathMean(const std::vector<std::vector<double>>& history,
    uint32_t columns,const std::vector<double>& doppler,uint32_t row,
    uint32_t column,uint32_t depth,int dr,int dc,double scale=0){
  validateDelayDrift(scale,doppler);
  if(!columns||row>=doppler.size()||column>=columns||
      (depth!=2&&depth!=4&&depth!=8)||history.size()<depth||dr < -16||dr>16)
    throw std::invalid_argument("Temporal path oracle geometry invalid");
  double sum=0;
  for(uint32_t age=0;age<depth;++age){
    const int rr=int(row)-dr*int(age);
    if(rr<0||rr>=int(doppler.size()))return -1;
    const int cc=delayPathColumn(int(column),dc,age,scale,doppler[row],doppler[rr]);
    if(cc<0||cc>=int(columns))return -1;
    if(history[age].size()!=uint64_t(columns)*doppler.size())
      throw std::invalid_argument("Temporal path oracle history shape invalid");
    const double value=history[age][uint64_t(rr)*columns+uint32_t(cc)];
    if(!std::isfinite(value)||value<0)return -1;
    sum+=value;
  }
  return std::isfinite(sum)?sum/depth:-1;
}
inline bool validMaximum(uint32_t value) { return value == 4 || value == 8; }
inline uint32_t bankCount(uint32_t maximum) {
  if (!validMaximum(maximum)) throw std::invalid_argument("Track-before-detect maximum must be 4 or 8 CPIs");
  return maximum == 4 ? 2u : 3u;
}
inline uint32_t detectorPassCount(uint32_t maximum) { return 1u + bankCount(maximum); }
inline uint32_t bankLength(uint32_t bank) {
  if (bank >= 3) throw std::invalid_argument("Track-before-detect bank index invalid");
  return 2u << bank;
}
inline double trialCount(uint32_t maximum, uint32_t delayBins,uint32_t dopplerBins) {
  return blah2::detection::integrated_cfar::motionFamilyTrials(
    maximum, delayBins, dopplerBins);
}
inline double trialCount(uint32_t maximum,uint32_t encoded){const auto m=decodeMotionBins(encoded);
 return trialCount(maximum,m.delay,m.doppler);}
inline uint64_t angleHistoryWords(uint64_t cells, uint32_t maximum,
    uint32_t beamTolerance) {
  if (beamTolerance == UINT32_MAX) return 1;
  const uint32_t depth = validMaximum(maximum) ? maximum : 1u;
  if (!cells || cells > std::numeric_limits<uint64_t>::max() / depth)
    throw std::invalid_argument("Track-before-detect angle history dimensions invalid");
  return cells * depth;
}
inline double passPfa(double baselinePfa, uint32_t maximum,
    uint32_t delayBins,uint32_t dopplerBins,uint32_t pass) {
  if (!pass || pass >= detectorPassCount(maximum)) {
    // Pre-register half of the configured family budget for baseline2 and
    // half for ALL enabled motion paths/depths. Unready banks do not donate
    // budget, so warm-up cannot change a surviving family's threshold.
    if (!pass) return baselinePfa * .5;
    throw std::invalid_argument("Track-before-detect pass index invalid");
  }
  return baselinePfa / (2 * trialCount(maximum,delayBins,dopplerBins));
}

// Cell averaging CFAR with an L-CPI noncoherent sum has a Gamma(L) CUT and a
// Gamma(n*L) training sum.  The false-alarm survival is I_x(nL,L), where
// x=n/(n+alpha).  L is deliberately small (2/4/8), so the exact integer beta
// tail is stable and avoids a dependency on a special-functions library.
inline double survival(double alpha, uint32_t trainingCells, uint32_t cpis) {
  return blah2::detection::integrated_cfar::survival(
    alpha, trainingCells, cpis);
}

inline double cfarAlpha(double pfa, uint32_t trainingCells, uint32_t cpis) {
  return blah2::detection::integrated_cfar::alpha(
    pfa, trainingCells, cpis);
}
}  // namespace track_before_detect
