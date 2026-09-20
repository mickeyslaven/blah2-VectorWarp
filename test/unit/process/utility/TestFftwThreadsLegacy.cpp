#include "process/utility/FftwThreads.h"

#include <stdexcept>

namespace {
int forwardedThreads = 0;

void require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}
}

extern "C" void fftw_plan_with_nthreads(int threads) {
  forwardedThreads = threads;
}

int main() {
  try {
    require(blah2::fftw_planner_threads() == 1,
      "Legacy FFTW wrapper must start with the documented default");
    blah2::set_fftw_planner_threads(3);
    require(forwardedThreads == 3, "Legacy FFTW wrapper did not forward the setter");
    require(blah2::fftw_planner_threads() == 3,
      "Legacy FFTW wrapper did not retain the configured thread count");
    bool rejected = false;
    try { blah2::set_fftw_planner_threads(0); }
    catch (const std::invalid_argument&) { rejected = true; }
    require(rejected && forwardedThreads == 3,
      "Legacy FFTW wrapper accepted an invalid thread count");
    blah2::set_fftw_planner_threads(1);
    require(forwardedThreads == 1 && blah2::fftw_planner_threads() == 1,
      "Legacy FFTW wrapper did not restore the configured thread count");
  } catch (const std::exception&) {
    return 1;
  }
  return 0;
}
