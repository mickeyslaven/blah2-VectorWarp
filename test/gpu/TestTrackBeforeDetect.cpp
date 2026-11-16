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
  check(fractionalDelayTableWords(17,8,16,-.566)==17*33*8&&
    fractionalDelayTableWords(17,4,4,0)==0&&
    fractionalDelayTableWords(65535,8,16,-1)==65535*33*8,
    "Fractional table bounds or disabled allocation changed");
  for(uint32_t depth:{0u,1u,2u,9u}){bool rejected=false;
    try{fractionalDelayTableWords(17,depth,4,-.566);}catch(const std::invalid_argument&){rejected=true;}
    check(rejected,"Invalid table depth admitted");}
  check(bankCount(4) == 2 && bankCount(8) == 3, "Unexpected multi-timescale bank count");
  check(detectorPassCount(4) == 3 && detectorPassCount(8) == 4,
    "Conventional detector pass is not retained");
  check(bankLength(0) == 2 && bankLength(1) == 4 && bankLength(2) == 8,
    "Unexpected multi-timescale bank lengths");
  check(trialCount(8, 1, 1) == 27, "Legacy motion-path trial correction changed");
  check(trialCount(8, 1, 4) == 81, "Split Doppler-path trial correction changed");
  check(trialCount(8, 2, 16) == 495, "Expanded motion-path trial correction changed");
  const auto encoded = encodeMotionBins(1, 4);
  const auto decoded = decodeMotionBins(encoded);
  check(decoded.delay == 1 && decoded.doppler == 4,
    "Split delay/Doppler motion encoding changed");
  const auto expanded = decodeMotionBins(encodeMotionBins(2, 16));
  check(expanded.delay == 2 && expanded.doppler == 16,
    "Expanded delay/Doppler motion encoding changed");
  check(angleHistoryWords(307111, 8, UINT32_MAX) == 1,
    "Disabled angle consistency retained a production-size GPU allocation");
  check(angleHistoryWords(307111, 8, 1) == 2456888,
    "Enabled angle consistency history dimensions changed");
  const auto fractional=encodeMotionBins(2,4,true);
  check(decodeMotionBins(fractional).fractionalDelay&&
    trialCount(8,fractional)==trialCount(8,encodeMotionBins(2,4)),
    "Fractional paths added trials or lost mode identity");
  check(decodeMotionBins(0).delay==0&&decodeMotionBins(1).doppler==1&&
    decodeMotionBins(2).delay==2,"Legacy integer encoding changed");
  const int expected[]={0,1,1,2,2,3,3,4};
  for(uint32_t age=0;age<8;++age)for(int sign:{-1,1}){
    check(delayPathColumn(20,0,age,-.566,-sign,-sign)==20-sign*expected[age],
      "Fractional 0.566 trajectory or signed rounding differs");
    for(int residual=-2;residual<=2;++residual)
      check(delayPathColumn(20,residual,age,0,100,100)==20-residual*int(age),
        "Disabled fractional motion changed integer coordinates");
  }
  check(delayPathColumn(20,0,1,-.5,-1,-1)==19&&
    delayPathColumn(20,0,1,-.5,1,1)==21,"Nearest-cell tie policy differs");
  // 1001 consumed samples, versus 3*333 retained samples: only the former is
  // the actual inter-CPI stride, and this case crosses a nearest-cell boundary.
  check(delayPathColumn(20,0,1,delayDriftPerHz(1001,2000),-1,-1)==19&&
    delayPathColumn(20,0,1,delayDriftPerHz(999,2000),-1,-1)==20,
    "Correlation tail was incorrectly removed from inter-CPI motion");
  // The two Doppler endpoints account for the existing acceleration path.
  check(delayPathColumn(20,0,4,-.25,4,0)==22,
    "Doppler-changing path did not integrate its endpoint rates");
  std::vector<std::vector<double>> histories(8,std::vector<double>(9,1));
  for(uint32_t depth:{2u,4u,8u}){
    check(pathMean(histories,9,{0},0,4,depth,0,0)==1,
      "Constant power gain changed with depth");
    check(pathMean(histories,9,{-1},0,0,depth,0,0,-.566)==-1,
      "Incomplete edge path was clipped or renormalized");
  }
  histories[3][4]=std::numeric_limits<double>::quiet_NaN();
  check(pathMean(histories,9,{0},0,4,8,0,0)==-1,"Nonfinite path admitted");
  check(close(passPfa(.001, 8, 1, 4, 0), .0005) &&
      close(passPfa(.001, 8, 1, 4, 1), .001 / 162),
    "Conventional and motion-bank PFA separation changed");
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
    << " banks=2,4,8 exact_gamma_cfar=1 delay_bins=1 doppler_bins=4 motion_trials=81\n";
  return 0;
} catch (const std::exception& error) {
  std::cerr << "FAIL track-before-detect math: " << error.what() << '\n';
  return 1;
}
