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
    bool calibrated = false;
    double directPathAzimuthRad = 0;
  };

  explicit CoherentBeamBank(const Config& config);

  void calibrate(const std::vector<std::complex<double>>& directPathWeights);

  std::unique_ptr<Map<std::complex<double>>> process(
    const std::vector<Map<std::complex<double>> *>& maps) const;

private:
  bool calibrated_ = false;
  double waveNumber_ = 0;
  double directPathAzimuthRad_ = 0;
  std::vector<std::array<double, 3>> positionsM_;
  std::vector<std::vector<std::complex<double>>> weights_;
};

#endif
