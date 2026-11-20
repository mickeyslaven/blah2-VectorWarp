#pragma once

#include "process/integration/EvidenceWorkBudget.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <vector>

namespace blah2::detection::integrated_cfar {

// Interleave independent CUTs, not the additions within a CUT. Contiguous
// row updates allow native vectorization while preserving the exact legacy
// left-then-right sum, including edges. No prefix subtraction or reassociation.
inline void orderedTrainingSums(const std::vector<double>& power,
    uint32_t guard, uint32_t training, std::vector<double>& sums,
    const integration::EvidenceWorkBudget* budget = nullptr) {
  sums.assign(power.size(), 0);
  const int columns = int(power.size());
  const auto accumulate = [&](int first, int last) {
    first = std::max(first, 1 - columns);
    last = std::min(last, columns);
    for (int offset = first; offset < last; ++offset) {
      if (budget) budget->check();
      const int begin = std::max(0, -offset);
      const int end = std::min(columns, columns - offset);
      for (int column = begin; column < end; ++column)
        sums[column] += power[column + offset];
    }
  };
  accumulate(-int(guard + training), -int(guard));
  accumulate(int(guard) + 1, int(guard + training) + 1);
}

// CA-CFAR survival for an L-sample noncoherent mean-power CUT against the
// mean of N independent L-sample training cells.  L is deliberately bounded
// to the production 2/4/8-CPI banks, so the integer beta tail is stable and
// does not require a special-functions dependency.
inline double survival(double alpha, uint32_t trainingCells, uint32_t cpis) {
  if (!trainingCells || !cpis || cpis > 8 || !std::isfinite(alpha) || alpha < 0)
    throw std::invalid_argument("Integrated CFAR parameters invalid");
  const double x = double(trainingCells) / (double(trainingCells) + alpha);
  if (!(x > 0) || x > 1) return 0;
  const uint64_t total = uint64_t(trainingCells) * cpis + cpis - 1u;
  double term = std::pow(x, double(total));
  double result = term;
  const double ratio = (1.0 - x) / x;
  // The first beta-tail term can underflow while a later term is representable.
  // Retain the ordinary operation order, using logs only in that extreme tail.
  if (term < std::numeric_limits<double>::min() || !std::isfinite(ratio)) {
    double logTerm = double(total) * std::log(x);
    const double logRatio = std::log1p(-x) - std::log(x);
    result = std::exp(logTerm);
    for (uint32_t k = 1; k < cpis; ++k) {
      logTerm += std::log(double(total - k + 1u) / double(k)) + logRatio;
      result += std::exp(logTerm);
    }
    return result;
  }
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
  while (survival(high, trainingCells, cpis) > pfa) {
    if (high == std::numeric_limits<double>::max())
      throw std::overflow_error("Integrated CFAR threshold is not representable");
    high = high > std::numeric_limits<double>::max() / 2 ?
      std::numeric_limits<double>::max() : high * 2;
  }
  for (unsigned iteration = 0; iteration < 96; ++iteration) {
    const double middle = low + (high - low) * .5;
    if (middle == low || middle == high) break;
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
