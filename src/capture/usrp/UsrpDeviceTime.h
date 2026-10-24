#pragma once

#include <cstdint>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string>

class UsrpDeviceTime {
public:
  struct Observation {
    int64_t firstTick = 0;
    int64_t nextTick = 0;
    uint64_t priorSamples = 0;
    uint64_t samples = 0;
  };
  struct Snapshot {
    bool available = false;
    int64_t epochTick = 0;
    int64_t nextTick = 0;
    uint64_t samples = 0;
    uint64_t blocks = 0;
  };

  explicit UsrpDeviceTime(uint32_t sampleRate) : sampleRate_(sampleRate) {
    if (!sampleRate_) throw std::invalid_argument("[USRP] Device-time sample rate is zero.");
  }

  template<class Metadata>
  Observation validate(const Metadata& metadata, uint64_t received) const {
    if (!received)
      throw std::runtime_error("[USRP] UHD returned an empty successful receive block.");
    if (!metadata.has_time_spec)
      throw std::runtime_error("[USRP] UHD receive metadata omitted device time. Input stopped before accepting this block.");
    const auto tick = metadata.time_spec.to_ticks(static_cast<double>(sampleRate_));
    if (tick < 0 || received > static_cast<uint64_t>(std::numeric_limits<int64_t>::max() - tick))
      throw std::runtime_error("[USRP] UHD device time is outside the supported sample-tick range.");
    std::lock_guard<std::mutex> lock(mutex_);
    if (available_ && tick != nextTick_)
      throw std::runtime_error("[USRP] UHD device-time discontinuity: expected tick " +
        std::to_string(nextTick_) + ", received " + std::to_string(tick) +
        ". Input stopped before accepting this block.");
    return {tick, tick + static_cast<int64_t>(received), samples_, received};
  }

  void commit(const Observation& observation) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (observation.priorSamples != samples_)
      throw std::logic_error("[USRP] Device-time observation committed out of order.");
    if (!available_) epochTick_ = observation.firstTick;
    available_ = true;
    nextTick_ = observation.nextTick;
    samples_ += observation.samples;
    ++blocks_;
  }

  Snapshot snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return {available_, epochTick_, nextTick_, samples_, blocks_};
  }

private:
  const uint32_t sampleRate_;
  mutable std::mutex mutex_;
  bool available_ = false;
  int64_t epochTick_ = 0;
  int64_t nextTick_ = 0;
  uint64_t samples_ = 0;
  uint64_t blocks_ = 0;
};
