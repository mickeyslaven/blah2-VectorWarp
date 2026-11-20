/// @file CfarDetector1D.h
/// @class CfarDetector1D
/// @brief A class to implement a 1D CFAR detector.
/// @details Converts an AmbiguityMap to DetectionData. 1D CFAR operates across delay, to minimise detections from the zero-Doppler line.
/// @author 30hours

#ifndef CFARDETECTOR1D_H
#define CFARDETECTOR1D_H

#include "data/Map.h"
#include "data/Detection.h"
#include <stdint.h>
#include <complex>
#include <memory>
#include <vector>
#include <limits>
#include "process/detection/IntegratedCfar.h"

namespace blah2::integration { struct EvidenceWorkBudget; }

class CfarDetector1D
{
private:
  /// @brief Probability of false alarm, numeric in [0,1]
  double pfa;

  /// @brief Number of single-sided guard cells.
  int8_t nGuard;

  /// @brief Number of single-sided training cells.
  int8_t nTrain;

  /// @brief Minimum delay to process detections (bins).
  int32_t minDelay;

  /// @brief Minimum absolute Doppler to process detections (Hz).
  double minDoppler;

  /// @brief Number of independent power samples averaged into each map cell.
  uint32_t integrationCpis;

public:
  // Optional replay observation at the actual pre-centroid comparison. Counts
  // include each ready temporal-bank CUT once, never the display-grid size.
  struct Trials {
    uint64_t evaluated = 0, accepted = 0, nonfinite = 0;
  };
  /// @brief Constructor.
  /// @param pfa Probability of false alarm, numeric in [0,1].
  /// @param nGuard Number of single-sided guard cells.
  /// @param nTrain Number of single-sided training cells.
  /// @param minDelay Minimum delay to process detections (bins).
  /// @param minDoppler Minimum absolute Doppler to process detections (Hz).
  /// @return The object.
  CfarDetector1D(double pfa, int8_t nGuard, int8_t nTrain, int32_t minDelay,
    double minDoppler, uint32_t integrationCpis = 1);

  /// @brief Destructor.
  /// @return Void.
  ~CfarDetector1D();
  std::unique_ptr<CfarDetector1D> forIntegration(uint32_t cpis,
      double trials = 1) const {
    return std::make_unique<CfarDetector1D>(pfa / trials, nGuard, nTrain,
      minDelay, minDoppler, cpis);
  }
  /// @brief Implement the 1D CFAR detector.
  /// @param x Ambiguity map data of IQ samples.
  /// @param detectorPower Optional exact row-major power statistic. Temporal
  /// banks must provide this instead of re-squaring their display amplitudes.
  /// @return Detections from the 1D CFAR detector.
  std::unique_ptr<Detection> process(Map<std::complex<double>> *x,
    const std::vector<std::vector<double>>* cellBackgroundPower = nullptr,
    const blah2::integration::EvidenceWorkBudget* budget = nullptr,
    std::size_t maximumDetections = std::numeric_limits<std::size_t>::max(),
    uint64_t* truncatedDetections = nullptr, Trials* trials = nullptr,
    const std::vector<double>* detectorPower = nullptr);

  /// @brief Exclude unavailable temporal CUTs and training cells. An empty
  /// mask preserves the original detector contract and arithmetic order.
  std::unique_ptr<Detection> process(Map<std::complex<double>> *x,
    const std::vector<uint8_t>& validCells,
    const std::vector<std::vector<double>>* cellBackgroundPower = nullptr,
    const blah2::integration::EvidenceWorkBudget* budget = nullptr,
    std::size_t maximumDetections = std::numeric_limits<std::size_t>::max(),
    uint64_t* truncatedDetections = nullptr, Trials* trials = nullptr,
    const std::vector<double>* detectorPower = nullptr);
};

#endif
