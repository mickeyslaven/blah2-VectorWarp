#ifndef COMPLEX_CUTOUT_RECORDER_H
#define COMPLEX_CUTOUT_RECORDER_H

#include "data/Map.h"
#include <complex>
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

namespace blah2::analysis {

class ComplexCutoutRecorder {
public:
  struct Config {
    bool enabled = false;
    std::string path;
    double delayKm = 0;
    double dopplerHz = 0;
    uint32_t delayRadiusBins = 3;
    uint32_t dopplerRadiusBins = 3;
    uint32_t maxFrames = 32;
  };

  ComplexCutoutRecorder(const Config& config, uint32_t sampleRate,
    bool replayInput);
  bool enabled() const { return config_.enabled; }
  uint32_t frames() const { return frames_; }
  void append(uint64_t timestamp,
    const std::vector<Map<std::complex<double>>*>& maps,
    const std::vector<std::complex<double>>& referenceWeights = {});

private:
  Config config_;
  uint32_t sampleRate_ = 0;
  uint32_t frames_ = 0;
  std::ofstream output_;
};

}

#endif
