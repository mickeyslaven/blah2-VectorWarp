#pragma once

#include <cmath>
#include <cstdint>
#include <stdexcept>

namespace blah2::detection::integrated_cfar {

// CA-CFAR survival for an L-sample noncoherent mean-power CUT against the
// mean of N independent L-sample training cells.  L is deliberately bounded
// to the production 2/4/8-CPI banks, so the integer beta tail is stable and
// does not require a special-functions dependency.
inline double survival(double alpha, uint32_t trainingCells, uint32_t cpis) {
  if (!trainingCells || !cpis || cpis > 8 || !std::isfinite(alpha) || alpha < 0)
    throw std::invalid_argument("Integrated CFAR parameters invalid");
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

inline double alpha(double pfa, uint32_t trainingCells, uint32_t cpis) {
  if (!(pfa > 0) || !(pfa < 1) || !std::isfinite(pfa))
    throw std::invalid_argument("Integrated CFAR probability invalid");
  double low = 0, high = 1;
  while (survival(high, trainingCells, cpis) > pfa && high < 1e12) high *= 2;
  for (unsigned iteration = 0; iteration < 96; ++iteration) {
    const double middle = (low + high) * .5;
    if (survival(middle, trainingCells, cpis) > pfa) low = middle;
    else high = middle;
  }
  return high;
}

inline double motionFamilyTrials(uint32_t maximumCpis,
    uint32_t delayBins, uint32_t dopplerBins) {
  if ((maximumCpis != 4 && maximumCpis != 8) || delayBins > 2 || dopplerBins > 16)
    throw std::invalid_argument("Integrated CFAR motion family invalid");
  const uint32_t banks = maximumCpis == 4 ? 2u : 3u;
  return double(banks) * double(2u * delayBins + 1u) *
    double(2u * dopplerBins + 1u);
}

} // namespace blah2::detection::integrated_cfar
