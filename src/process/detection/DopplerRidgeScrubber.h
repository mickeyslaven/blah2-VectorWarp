#ifndef DOPPLERRIDGESCRUBBER_H
#define DOPPLERRIDGESCRUBBER_H

#include "data/Map.h"

#include <complex>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

struct DopplerRidgeScrubberConfig
{
  // off: no extra work; shadow: compare raw and scrubbed paths; active: use
  // scrubbed detections while retaining the raw comparison path.
  std::string mode = "off";
  uint32_t persistenceCpis = 12;
  double minimumExcessDb = 3.0;
  double maximumSubtractionDb = 12.0;
  double dopplerToleranceHz = 4.0;
  uint32_t neighborhoodRows = 8;
  uint32_t delayStride = 4;
  // Multi-lane shadow only: deterministic contiguous comparison window per
  // input epoch. Active mode remains continuous and ignores this limit.
  uint32_t shadowMaximumCpis = 256;
};

class DopplerRidgeScrubber
{
public:
  struct Metrics
  {
    uint32_t observedRows = 0;
    uint32_t qualifiedRows = 0;
    double maximumAppliedDb = 0;
    std::vector<double> qualifiedDopplerHz;
  };

  explicit DopplerRidgeScrubber(DopplerRidgeScrubberConfig config = {});

  // Returns one linear-power background estimate per Doppler row. The source
  // map is never changed; callers may use the result only in their detector.
  const std::vector<std::vector<double>>& process(Map<std::complex<double>>* map,
    int32_t minimumDelay);
  void reset();
  const Metrics& metrics() const noexcept { return metrics_; }
  const DopplerRidgeScrubberConfig& config() const noexcept { return config_; }

private:
  DopplerRidgeScrubberConfig config_;
  std::vector<double> rowMedianPower_, localBaselinePower_, learnedExcessPower_;
  std::vector<std::vector<double>> learnedCellExcessPower_, backgroundPower_;
  std::vector<double> scratch_;
  std::vector<uint32_t> persistence_;
  std::vector<uint32_t> misses_;
  std::vector<bool> ridgeCandidate_;
  std::vector<double> doppler_;
  Metrics metrics_;

  static double median(std::vector<double>& values);
  void ensureTopology(Map<std::complex<double>>* map);
};

#endif
