#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <mutex>

// Bounded receiver-side telemetry for normalized UHD fc32 samples. Sampling
// one complex value in 32 keeps this inexpensive at wideband B210 rates while
// still publishing well over the admission minimum each second.
class UsrpInputQuality {
public:
  struct Channel {
    double gainDb = 0, railFraction = 0, nearRailFraction = 0;
    double p999Fraction = 0, rmsFraction = 0;
    uint64_t sampledComponents = 0;
  };
  struct Snapshot {
    bool available = false;
    std::array<Channel, 2> channels{};
  };

  void reset(const std::array<double, 2>& gainDb) {
    std::lock_guard<std::mutex> lock(mutex);
    gains = gainDb;
    accumulated = {};
    published = {};
  }

  void observe(const std::complex<float>* first,
      const std::complex<float>* second, std::size_t count) {
    if (!first || !second || !count) return;
    std::lock_guard<std::mutex> lock(mutex);
    for (std::size_t index = 0; index < count; index += SAMPLE_STRIDE) {
      observe_component(accumulated[0], first[index].real());
      observe_component(accumulated[0], first[index].imag());
      observe_component(accumulated[1], second[index].real());
      observe_component(accumulated[1], second[index].imag());
    }
    if (accumulated[0].sampled >= TARGET_COMPONENTS &&
        accumulated[1].sampled >= TARGET_COMPONENTS) publish_locked();
  }

  Snapshot snapshot() const {
    std::lock_guard<std::mutex> lock(mutex);
    return published;
  }

private:
  static constexpr std::size_t HISTOGRAM_BINS = 1024;
  static constexpr std::size_t SAMPLE_STRIDE = 32;
  static constexpr uint64_t TARGET_COMPONENTS = 131072;
  static constexpr double RAIL = 1.0 - 1.0 / 32768.0;
  static constexpr double NEAR_RAIL = .95;

  struct Accumulator {
    uint64_t sampled = 0, rails = 0, nearRails = 0;
    long double sumSquares = 0;
    std::array<uint64_t, HISTOGRAM_BINS> histogram{};
  };

  static void observe_component(Accumulator& value, float component) {
    const double magnitude = std::min(1.0, std::abs(static_cast<double>(component)));
    ++value.sampled;
    value.sumSquares += magnitude * magnitude;
    if (magnitude >= RAIL) ++value.rails;
    if (magnitude >= NEAR_RAIL) ++value.nearRails;
    const auto bin = std::min<std::size_t>(HISTOGRAM_BINS - 1,
      static_cast<std::size_t>(magnitude * HISTOGRAM_BINS));
    ++value.histogram[bin];
  }

  static double quantile_999(const Accumulator& value) {
    const uint64_t target = (value.sampled * 999 + 999) / 1000;
    uint64_t cumulative = 0;
    for (std::size_t bin = 0; bin < HISTOGRAM_BINS; ++bin) {
      cumulative += value.histogram[bin];
      if (cumulative >= target)
        return std::min(1.0, static_cast<double>(bin + 1) / HISTOGRAM_BINS);
    }
    return 1.0;
  }

  void publish_locked() {
    Snapshot next;
    next.available = true;
    for (std::size_t index = 0; index < next.channels.size(); ++index) {
      const auto& source = accumulated[index];
      auto& target = next.channels[index];
      target.gainDb = gains[index];
      target.sampledComponents = source.sampled;
      target.railFraction = static_cast<double>(source.rails) / source.sampled;
      target.nearRailFraction = static_cast<double>(source.nearRails) / source.sampled;
      target.p999Fraction = quantile_999(source);
      target.rmsFraction = std::sqrt(static_cast<double>(source.sumSquares / source.sampled));
    }
    published = next;
    accumulated = {};
  }

  mutable std::mutex mutex;
  std::array<double, 2> gains{};
  std::array<Accumulator, 2> accumulated{};
  Snapshot published;
};
