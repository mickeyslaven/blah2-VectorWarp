#pragma once

#include <cmath>
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
    int64_t firstWholeSeconds = 0;
    double firstFractionalSeconds = 0;
  };
  struct Snapshot {
    bool available = false;
    uint64_t epoch = 0;
    int64_t epochTick = 0;
    int64_t nextTick = 0;
    uint64_t samples = 0;
    uint64_t blocks = 0;
  };

  explicit UsrpDeviceTime(double sampleRate) : sampleRate_(sampleRate) {
    if (!std::isfinite(sampleRate_) || sampleRate_ <= 0)
      throw std::invalid_argument("[USRP] Device-time sample rate must be finite and positive.");
  }

  void configure_sample_rate(double sampleRate) {
    if (!std::isfinite(sampleRate) || sampleRate <= 0)
      throw std::invalid_argument("[USRP] Applied device-time sample rate must be finite and positive.");
    std::lock_guard<std::mutex> lock(mutex_);
    if (available_ || samples_ || blocks_)
      throw std::logic_error("[USRP] Device-time sample rate cannot change while a stream is active.");
    sampleRate_ = sampleRate;
  }

  template<class Metadata>
  Observation validate(const Metadata& metadata, uint64_t received) const {
    if (!received)
      throw std::runtime_error("[USRP] UHD returned an empty successful receive block.");
    if (!metadata.has_time_spec)
      throw std::runtime_error("[USRP] UHD receive metadata omitted device time. Block rejected before coherent publication.");
    std::lock_guard<std::mutex> lock(mutex_);
    const int64_t wholeSeconds = metadata.time_spec.get_full_secs();
    const double fractionalSeconds = metadata.time_spec.get_frac_secs();
    validate_split_time(wholeSeconds, fractionalSeconds);
    int64_t tick = 0;
    if (available_) {
      const long double seconds =
        (static_cast<long double>(wholeSeconds) - lastWholeSeconds_) +
        (static_cast<long double>(fractionalSeconds) - lastFractionalSeconds_);
      const long double scaled = seconds * sampleRate_;
      const auto delta = checked_ticks(scaled,
        "[USRP] UHD device-time delta is outside the supported sample-tick range.");
      if (delta != lastBlockSamples_) {
        throw std::runtime_error("[USRP] UHD device-time discontinuity: expected tick " +
          std::to_string(nextTick_) + ", relative delta " + std::to_string(delta) +
          " samples" +
          ". Block rejected before coherent publication.");
      }
      tick = nextTick_;
    } else
      tick = absolute_ticks(wholeSeconds, fractionalSeconds, sampleRate_);
    if (tick < 0 || received > static_cast<uint64_t>(std::numeric_limits<int64_t>::max() - tick))
      throw std::runtime_error("[USRP] UHD device time is outside the supported sample-tick range.");
    return {tick, tick + static_cast<int64_t>(received), samples_, received,
      wholeSeconds, fractionalSeconds};
  }

  template<class Metadata>
  Observation validate_after(const Metadata& metadata, uint64_t received,
      const Observation& prior) const {
    if (!received)
      throw std::runtime_error("[USRP] UHD returned an empty successful receive block.");
    if (!metadata.has_time_spec)
      throw std::runtime_error("[USRP] UHD receive metadata omitted device time. Block rejected before coherent publication.");
    std::lock_guard<std::mutex> lock(mutex_);
    const int64_t wholeSeconds = metadata.time_spec.get_full_secs();
    const double fractionalSeconds = metadata.time_spec.get_frac_secs();
    validate_split_time(wholeSeconds, fractionalSeconds);
    const long double seconds =
      (static_cast<long double>(wholeSeconds) - prior.firstWholeSeconds) +
      (static_cast<long double>(fractionalSeconds) -
        prior.firstFractionalSeconds);
    const long double scaled = seconds * sampleRate_;
    const auto delta = checked_ticks(scaled,
      "[USRP] UHD device-time delta is outside the supported sample-tick range.");
    if (delta != prior.samples)
      throw std::runtime_error("[USRP] UHD device-time discontinuity during requalification: expected tick " +
        std::to_string(prior.nextTick) + ", relative delta " +
        std::to_string(delta) + " samples. Candidate blocks discarded before coherent publication.");
    const int64_t tick = prior.nextTick;
    if (tick < 0 || received > static_cast<uint64_t>(std::numeric_limits<int64_t>::max() - tick))
      throw std::runtime_error("[USRP] UHD device time is outside the supported sample-tick range.");
    return {tick, tick + static_cast<int64_t>(received),
      prior.priorSamples + prior.samples, received, wholeSeconds,
      fractionalSeconds};
  }

  void commit(const Observation& observation) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (observation.priorSamples != samples_)
      throw std::logic_error("[USRP] Device-time observation committed out of order.");
    if (!available_) epochTick_ = observation.firstTick;
    available_ = true;
    nextTick_ = observation.nextTick;
    lastWholeSeconds_ = observation.firstWholeSeconds;
    lastFractionalSeconds_ = observation.firstFractionalSeconds;
    lastBlockSamples_ = observation.samples;
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
    lastWholeSeconds_ = 0;
    lastFractionalSeconds_ = 0;
    lastBlockSamples_ = 0;
    samples_ = blocks_ = 0;
  }

  void discontinuity() {
    std::lock_guard<std::mutex> lock(mutex_);
    ++epoch_;
    available_ = false;
    epochTick_ = nextTick_ = 0;
    lastWholeSeconds_ = 0;
    lastFractionalSeconds_ = 0;
    lastBlockSamples_ = 0;
    samples_ = blocks_ = 0;
  }

private:
  static void validate_split_time(int64_t wholeSeconds,
      double fractionalSeconds) {
    if (wholeSeconds < 0 || !std::isfinite(fractionalSeconds) ||
        fractionalSeconds < 0 || fractionalSeconds >= 1)
      throw std::runtime_error(
        "[USRP] UHD device time has an invalid split timestamp.");
  }

  static uint64_t checked_ticks(long double scaled, const char* error) {
    if (!std::isfinite(scaled) || scaled < 0)
      throw std::runtime_error(error);
    const long double rounded = std::round(scaled);
    // 2^63 is exactly representable even where long double has only double
    // precision. Comparing against INT64_MAX would round that maximum up to
    // 2^63 on such platforms and admit an overflowing llround/cast.
    const long double limit = std::ldexp(1.0L, 63);
    if (!std::isfinite(rounded) || rounded >= limit)
      throw std::runtime_error(error);
    return static_cast<uint64_t>(rounded);
  }

  static int64_t absolute_ticks(int64_t wholeSeconds,
      double fractionalSeconds, double sampleRate) {
    constexpr const char* error =
      "[USRP] UHD device time is outside the supported sample-tick range.";
    const long double integralRateValue = std::floor(sampleRate);
    const long double limit = std::ldexp(1.0L, 63);
    if (!std::isfinite(integralRateValue) || integralRateValue < 0 ||
        integralRateValue >= limit)
      throw std::runtime_error(error);
    const auto integralRate = static_cast<uint64_t>(integralRateValue);
    const auto whole = static_cast<uint64_t>(wholeSeconds);
    const uint64_t maximum = std::numeric_limits<int64_t>::max();
    if (integralRate && whole > maximum / integralRate)
      throw std::runtime_error(error);
    const uint64_t integralTicks = whole * integralRate;
    // Keep the epoch-sized integral product out of floating point. Only the
    // sub-Hz rate residue and split fractional second require rounding.
    const long double residual =
      static_cast<long double>(wholeSeconds) *
        (static_cast<long double>(sampleRate) - integralRateValue) +
      static_cast<long double>(fractionalSeconds) * sampleRate;
    const uint64_t residualTicks = checked_ticks(residual, error);
    if (residualTicks > maximum - integralTicks)
      throw std::runtime_error(error);
    return static_cast<int64_t>(integralTicks + residualTicks);
  }

  double sampleRate_;
  mutable std::mutex mutex_;
  uint64_t epoch_ = 0;
  bool available_ = false;
  int64_t epochTick_ = 0;
  int64_t nextTick_ = 0;
  int64_t lastWholeSeconds_ = 0;
  double lastFractionalSeconds_ = 0;
  uint64_t lastBlockSamples_ = 0;
  uint64_t samples_ = 0;
  uint64_t blocks_ = 0;
};
