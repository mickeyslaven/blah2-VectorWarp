#ifndef REPLAY_ECHO_INJECTOR_H
#define REPLAY_ECHO_INJECTOR_H

#include "data/IqData.h"
#include <array>
#include <cstdint>
#include <vector>

namespace blah2::analysis {

class ReplayEchoInjector {
public:
  struct Config {
    bool enabled = false;
    double amplitudeDb = -40;
    double delayKm = 0;
    double dopplerHz = 0;
    double targetAzimuthRad = 0;
  };

  ReplayEchoInjector(const Config& config, uint32_t sampleRate,
    double frequencyHz, double directPathAzimuthRad,
    std::vector<std::array<double, 3>> positions, bool replayInput);
  void apply(uint64_t firstSample, const std::vector<IqData*>& channels) const;
  bool enabled() const { return config_.enabled; }

private:
  Config config_;
  uint32_t sampleRate_ = 0;
  double frequencyHz_ = 0;
  double directPathAzimuthRad_ = 0;
  std::vector<std::array<double, 3>> positions_;
};

}

#endif
