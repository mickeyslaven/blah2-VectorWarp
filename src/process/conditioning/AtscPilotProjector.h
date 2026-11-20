#pragma once
#include "data/IqSampleView.h"

#include <complex>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

namespace blah2::conditioning {

class AtscPilotProjector {
public:
  struct Config {
    uint32_t sampleRateHz = 0;
    double minimumHeldoutSuppressionDb = 6;
    double maximumRemovedPowerFraction = .25;
    double maximumAbsoluteCfoHz = 10000;
    double minimumDurationSeconds = .01;
    double cfoBlockSeconds = .001;
    double maximumCfoStepHz = 5;
    uint32_t qualificationCpis = 3;
    uint32_t unlockFailures = 3;
  };
  struct Metrics {
    bool available = false, qualified = false, applied = false;
    double cfoHz = 0, heldoutSuppressionDb = 0;
    double removedPowerFraction = 0;
    double coarseCfoHz = 0, residualCfoHz = 0;
    bool cfoLocked = false;
    uint32_t qualifiedCpis = 0, failedCpis = 0;
    std::size_t samples = 0;
    std::string reason = "not_observed";
  };

  explicit AtscPilotProjector(Config config);
  void reset();
  Metrics process(const std::deque<std::complex<double>>& input,
    std::deque<std::complex<double>>& output, double cfoHz,
    bool pilotCandidate);
  Metrics process(const std::complex<float>* input, std::size_t samples,
    std::vector<std::complex<double>>& output, double cfoHz,
    bool pilotCandidate);
  Metrics process(IqSampleView input, std::vector<std::complex<double>>& output,
    double cfoHz, bool pilotCandidate);

private:
  template<class Sample, class Output>
  Metrics process_samples(std::size_t samples, Sample sample, Output& output,
    double cfoHz, bool pilotCandidate);
  Config config_;
  bool haveCfo_ = false, locked_ = false;
  double cfoHz_ = 0;
  uint32_t qualifiedCpis_ = 0, failedCpis_ = 0;
  bool acceptCfo(double cfoHz, Metrics& metrics);
  void fail();
};

} // namespace blah2::conditioning
