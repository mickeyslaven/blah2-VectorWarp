#pragma once
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>

namespace track_before_detect {
constexpr uint32_t splitMotionFlag = 0x80000000u;
struct MotionBins { uint32_t delay=0,doppler=0; };
inline uint32_t encodeMotionBins(uint32_t delay,uint32_t doppler){
  if(delay>1||doppler>4)throw std::invalid_argument("Track-before-detect motion bins invalid");
  return splitMotionFlag|(doppler<<8)|delay;
}
inline MotionBins decodeMotionBins(uint32_t value){
  if(!(value&splitMotionFlag))return {value,value};
  if(value&~(splitMotionFlag|0x00000f01u))throw std::invalid_argument("Track-before-detect motion encoding invalid");
  MotionBins result{value&0xffu,(value>>8)&0xffu};
  if(result.delay>1||result.doppler>4)throw std::invalid_argument("Track-before-detect motion bins invalid");
  return result;
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
  return double(bankCount(maximum)) * double(2u*delayBins+1u)*double(2u*dopplerBins+1u);
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
    if (!pass) return baselinePfa;
    throw std::invalid_argument("Track-before-detect pass index invalid");
  }
  return baselinePfa / trialCount(maximum,delayBins,dopplerBins);
}

// Cell averaging CFAR with an L-CPI noncoherent sum has a Gamma(L) CUT and a
// Gamma(n*L) training sum.  The false-alarm survival is I_x(nL,L), where
// x=n/(n+alpha).  L is deliberately small (2/4/8), so the exact integer beta
// tail is stable and avoids a dependency on a special-functions library.
inline double survival(double alpha, uint32_t trainingCells, uint32_t cpis) {
  if (!trainingCells || !cpis || !std::isfinite(alpha) || alpha < 0)
    throw std::invalid_argument("Track-before-detect CFAR parameters invalid");
  const double x = double(trainingCells) / (double(trainingCells) + alpha);
  if (!(x > 0) || x > 1) return 0;
  const uint32_t total = trainingCells * cpis + cpis - 1u;
  double term = std::pow(x, double(total));
  double result = term;
  const double ratio = (1.0 - x) / x;
  for (uint32_t k = 1; k < cpis; ++k) {
    term *= (double(total - k + 1u) / double(k)) * ratio;
    result += term;
  }
  return result;
}

inline double cfarAlpha(double pfa, uint32_t trainingCells, uint32_t cpis) {
  if (!(pfa > 0) || !(pfa < 1) || !std::isfinite(pfa))
    throw std::invalid_argument("Track-before-detect PFA invalid");
  double low = 0, high = 1;
  while (survival(high, trainingCells, cpis) > pfa && high < 1e12) high *= 2;
  for (unsigned iteration = 0; iteration < 96; ++iteration) {
    const double middle = (low + high) * .5;
    if (survival(middle, trainingCells, cpis) > pfa) low = middle;
    else high = middle;
  }
  return high;
}
}  // namespace track_before_detect
