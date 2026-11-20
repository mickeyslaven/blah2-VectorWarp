#ifndef REPLAY_ECHO_INJECTOR_H
#define REPLAY_ECHO_INJECTOR_H

#include "data/IqData.h"
#include <array>
#include <cstdint>
#include <deque>
#include <vector>

namespace blah2::analysis {

class ReplayEchoInjector {
public:
  static constexpr uint64_t maximumHistoryBytes = 16ULL << 20;
  struct Frame {
    uint64_t epoch = 0, first = 0, end = 0;
    bool positioned = false;
  };
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
  // A muted qualification prefix still advances the unmodified delay history.
  void apply(const Frame& frame, const std::vector<IqData*>& channels, bool inject = true);
  bool enabled() const { return config_.enabled; }

private:
  Config config_;
  uint32_t sampleRate_ = 0;
  double frequencyHz_ = 0;
  double directPathAzimuthRad_ = 0;
  std::vector<std::array<double, 3>> positions_;
  uint64_t delaySamples_ = 0, epoch_ = 0, nextSample_ = 0;
  bool positioned_ = false;
  std::vector<std::deque<std::complex<double>>> history_;
};

}

#endif
