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
    uint64_t epoch = 0;
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
      throw std::runtime_error("[USRP] UHD receive metadata omitted device time. Block rejected before coherent publication.");
    const auto tick = metadata.time_spec.to_ticks(static_cast<double>(sampleRate_));
    if (tick < 0 || received > static_cast<uint64_t>(std::numeric_limits<int64_t>::max() - tick))
      throw std::runtime_error("[USRP] UHD device time is outside the supported sample-tick range.");
    std::lock_guard<std::mutex> lock(mutex_);
    if (available_ && tick != nextTick_)
      throw std::runtime_error("[USRP] UHD device-time discontinuity: expected tick " +
        std::to_string(nextTick_) + ", received " + std::to_string(tick) +
        ". Block rejected before coherent publication.");
    return {tick, tick + static_cast<int64_t>(received), samples_, received};
  }

  template<class Metadata>
  Observation validate_after(const Metadata& metadata, uint64_t received,
      const Observation& prior) const {
    if (!received)
      throw std::runtime_error("[USRP] UHD returned an empty successful receive block.");
    if (!metadata.has_time_spec)
      throw std::runtime_error("[USRP] UHD receive metadata omitted device time. Block rejected before coherent publication.");
    const auto tick = metadata.time_spec.to_ticks(static_cast<double>(sampleRate_));
    if (tick < 0 || received > static_cast<uint64_t>(std::numeric_limits<int64_t>::max() - tick))
      throw std::runtime_error("[USRP] UHD device time is outside the supported sample-tick range.");
    if (tick != prior.nextTick)
      throw std::runtime_error("[USRP] UHD device-time discontinuity during requalification: expected tick " +
        std::to_string(prior.nextTick) + ", received " + std::to_string(tick) +
        ". Candidate blocks discarded before coherent publication.");
    return {tick, tick + static_cast<int64_t>(received),
      prior.priorSamples + prior.samples, received};
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
    return {available_, epoch_, epochTick_, nextTick_, samples_, blocks_};
  }

  void restart() {
    std::lock_guard<std::mutex> lock(mutex_);
    epoch_ = 0;
    available_ = false;
    epochTick_ = nextTick_ = 0;
    samples_ = blocks_ = 0;
  }

  void discontinuity() {
    std::lock_guard<std::mutex> lock(mutex_);
    ++epoch_;
    available_ = false;
    epochTick_ = nextTick_ = 0;
    samples_ = blocks_ = 0;
  }

private:
  const uint32_t sampleRate_;
  mutable std::mutex mutex_;
  uint64_t epoch_ = 0;
  bool available_ = false;
  int64_t epochTick_ = 0;
  int64_t nextTick_ = 0;
  uint64_t samples_ = 0;
  uint64_t blocks_ = 0;
};
