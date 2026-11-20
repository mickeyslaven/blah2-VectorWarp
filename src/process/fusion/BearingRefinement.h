#pragma once

#include "data/Map.h"
#include <array>
#include <complex>
#include <cstdint>
#include <vector>

namespace blah2::fusion {

// Independent bounded CPU qualification of the resident finite-beam spatial
// statistic. Inputs are canonical CPU CAFs and independently learned reference
// weights, never resident outputs. This owns no temporal/detection/tracker path.
class BearingRefinement {
public:
  struct Config {
    uint32_t channels = 0, beams = 0, exclusionBins = 0;
    double frequencyHz = 0, radiusM = 0, element0Rad = 0, elementStepRad = 0;
    double directPathAzimuthRad = 0, directPathNullDb = 0;
    uint32_t method = 0, referenceAnchor = 0, guardRadius = 1, trainingRadius = 3;
    float diagonalLoading = .01f;
    std::vector<uint32_t> channelMapping;
    std::vector<std::complex<float>> measuredResponses;
  };
  enum class Outcome { valid, noSpatialPower, invalidInput };
  struct Result {
    uint32_t spatial = 0;
    std::array<uint32_t, 3> beams{}; // One-based; zero means absent.
    std::array<float, 3> powers{};
    uint32_t covarianceSamples = 0, pivotSwaps = 0;
    bool adaptiveUsed = false, singularFallback = false;
    Outcome outcome = Outcome::noSpatialPower;
  };

  BearingRefinement(Config config,
    const std::vector<std::complex<double>>& referenceWeights,
    const std::array<double, 8>& channelScale, bool whiten);

  // The source cell is the pre-centroid/interpolation CFAR cell. Current spatial
  // evidence does not change the originating 2/4/8-CPI bank's detection SNR.
  Result refineAt(const std::vector<Map<std::complex<double>>*>& maps,
    uint32_t row, uint32_t column) const;
  const std::vector<std::complex<float>>& calibratedWeights() const { return weights_; }

private:
  Config config_;
  std::vector<std::complex<float>> weights_;
};
}
