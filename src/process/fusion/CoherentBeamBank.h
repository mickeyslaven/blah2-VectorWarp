#ifndef COHERENT_BEAM_BANK_H
#define COHERENT_BEAM_BANK_H

#include "data/Map.h"

#include <array>
#include <complex>
#include <cstdint>
#include <memory>
#include <vector>

class CoherentBeamBank
{
public:
  struct Config
  {
    double frequencyHz = 0;
    uint32_t beams = 12;
    std::vector<std::array<double, 3>> positionsM;
    double directPathAzimuthRad = 0;
    double directPathNullDb = 0;
    // Optional row-major measured responses: beams x positionsM.size().
    std::vector<std::complex<double>> measuredResponses;
  };

  explicit CoherentBeamBank(const Config& config);

  void calibrate(const std::vector<std::complex<double>>& directPathWeights);

  std::unique_ptr<Map<std::complex<double>>> process(
    const std::vector<Map<std::complex<double>> *>& maps) const;

  // Offline diagnostics may retain the complete bank, while the production
  // map path below continues to expose only the maximum. Both paths use the
  // exact calibrated weights held by this object.
  std::vector<double> powerBankAt(
    const std::vector<Map<std::complex<double>> *>& maps,
    uint32_t row, uint32_t column) const;
  std::vector<double> powerBank(
    const std::vector<std::complex<double>>& channels) const;

private:
  double waveNumber_ = 0;
  double directPathAzimuthRad_ = 0;
  double directPathNullDb_ = 0;
  std::vector<std::array<double, 3>> positionsM_;
  std::vector<std::complex<double>> measuredResponses_;
  std::size_t measuredDirectBeam_ = 0;
  std::vector<std::vector<std::complex<double>>> weights_;
};

#endif
