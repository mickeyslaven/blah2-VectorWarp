#ifndef BLAH2_RSPDUO_OVERLOAD_STATUS_H
#define BLAH2_RSPDUO_OVERLOAD_STATUS_H

#include <array>
#include <cstdint>
#include <mutex>

// SDK-free transition tracking. A snapshot is consistent across event and
// telemetry threads; callbacks perform no allocation, formatting or I/O here.
class RspDuoOverloadStatus {
public:
  struct Channel {
    bool active = false;
    uint64_t detectedCount = 0, correctedCount = 0;
    uint64_t lastDetectedAt = 0, lastCorrectedAt = 0;
  };
  struct Snapshot {
    bool monitoring = false;
    std::array<Channel, 2> channels{};
    uint64_t ackFailures = 0;
  };
  void reset() {
    std::lock_guard<std::mutex> lock(mutex);
    state = {};
  }
  void monitoring(bool enabled) {
    std::lock_guard<std::mutex> lock(mutex);
    state.monitoring = enabled;
  }
  void event(unsigned channel, bool detected, uint64_t at) {
    if (channel >= 2 || at == 0) return;
    std::lock_guard<std::mutex> lock(mutex);
    auto& c = state.channels[channel];
    if (c.active == detected) return; // Duplicate or startup-only correction.
    c.active = detected;
    if (detected) { ++c.detectedCount; c.lastDetectedAt = at; }
    else { ++c.correctedCount; c.lastCorrectedAt = at; }
  }
  void ackFailed() {
    std::lock_guard<std::mutex> lock(mutex);
    ++state.ackFailures;
  }
  Snapshot snapshot() const {
    std::lock_guard<std::mutex> lock(mutex);
    return state;
  }
private:
  mutable std::mutex mutex;
  Snapshot state;
};

#endif
