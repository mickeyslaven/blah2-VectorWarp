#pragma once
#include <cmath>
#include <cstdint>
#include <stdexcept>

namespace track_before_detect {
inline bool validMaximum(uint32_t value) { return value == 4 || value == 8; }
inline uint32_t bankCount(uint32_t maximum) {
  if (!validMaximum(maximum)) throw std::invalid_argument("Track-before-detect maximum must be 4 or 8 CPIs");
  return maximum == 4 ? 2u : 3u;
}
inline uint32_t bankLength(uint32_t bank) {
  if (bank >= 3) throw std::invalid_argument("Track-before-detect bank index invalid");
  return 2u << bank;
}
inline double trialCount(uint32_t maximum, uint32_t motionBins) {
  const double width = double(2u * motionBins + 1u);
  return double(bankCount(maximum)) * width * width;
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
