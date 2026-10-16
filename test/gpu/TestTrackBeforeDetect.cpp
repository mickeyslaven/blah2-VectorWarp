#include "core/TrackBeforeDetect.private.h"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>

namespace {
unsigned checks = 0;
void check(bool value, const char* message) {
  ++checks;
  if (!value) throw std::runtime_error(message);
}
bool close(double left, double right, double tolerance = 1e-10) {
  return std::abs(left - right) <= tolerance * std::max({1.0, std::abs(left), std::abs(right)});
}
}

int main() try {
  using namespace track_before_detect;
  check(bankCount(4) == 2 && bankCount(8) == 3, "Unexpected multi-timescale bank count");
  check(bankLength(0) == 2 && bankLength(1) == 4 && bankLength(2) == 8,
    "Unexpected multi-timescale bank lengths");
  check(trialCount(8, 1) == 27, "Motion-path trial correction changed");
  const uint32_t training = 12;
  const double alphaOne = 7.0;
  check(close(survival(alphaOne, training, 1),
    std::pow(double(training) / (training + alphaOne), training)),
    "Single-CPI CFAR survival differs");
  const double pfa = 0.005 / 72.0;
  double previousSumThreshold = 0;
  for (uint32_t cpis : {2u, 4u, 8u}) {
    const double alpha = cfarAlpha(pfa, training, cpis);
    check(double(cpis) * alpha > previousSumThreshold,
      "Longer integration did not increase sum-domain threshold");
    check(close(survival(alpha, training, cpis), pfa, 1e-8),
      "Exact Gamma CFAR inversion differs");
    previousSumThreshold = double(cpis) * alpha;
  }
  bool rejected = false;
  try { (void)bankCount(6); } catch (const std::invalid_argument&) { rejected = true; }
  check(rejected, "Invalid bank maximum was accepted");
  std::cout << "TRACK_BEFORE_DETECT_MATH_PASS checks=" << checks
    << " banks=2,4,8 exact_gamma_cfar=1 motion_trials=27\n";
  return 0;
} catch (const std::exception& error) {
  std::cerr << "FAIL track-before-detect math: " << error.what() << '\n';
  return 1;
}
