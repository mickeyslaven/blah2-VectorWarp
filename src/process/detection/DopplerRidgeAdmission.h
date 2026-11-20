#pragma once

#include <stdexcept>
#include <string>

namespace blah2::detection {
inline void validateTemporalRouteAdmission(bool residentProducts,
    bool beamTolerance,bool temporalActive,bool trackerEnabled) {
  if(beamTolerance&&(!residentProducts||!temporalActive||!trackerEnabled))
    throw std::invalid_argument("Tracker beam_tolerance requires active resident coherent temporal tracking");
}

// Admission follows the product implementation, independent of receiver and
// frontend acceleration. Reject a requested statistic if that implementation
// cannot execute it; an enabled setting must never become a silent no-op.
inline void validateDopplerRidgeAdmission(bool residentProducts,
  bool ridgeBirthFilter) {
  if (ridgeBirthFilter && residentProducts)
    throw std::invalid_argument(
      "Doppler ridge birth filter is unavailable in resident products");
}

} // namespace blah2::detection
