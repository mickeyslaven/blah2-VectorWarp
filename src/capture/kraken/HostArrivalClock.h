#pragma once

// Host-arrival anchoring for a timestamp-less Heimdall stream.  This is a
// diagnostic/sample-index clock only: USB/TCP delivery latency is unknown, so
// it must never be promoted to a bounded RF-to-UTC clock.

#include <algorithm>
#include <cstdint>
#include <deque>
#include <limits>
#include <vector>

namespace blah2::kraken_timing {
struct Estimate {
  bool available = false;
  bool discontinuity = false;
  int64_t epochNanoseconds = 0;
  int64_t residualNanoseconds = 0;
  int64_t maximumResidualNanoseconds = 0;
  int64_t driftPpm = 0;
  uint32_t anchors = 0;
};

class HostArrivalClock {
public:
  explicit HostArrivalClock(uint32_t sampleRate) : sampleRate(sampleRate) {}

  void reset() { anchors.clear(); }

  Estimate observe(uint64_t endSample, int64_t receiptNanoseconds) {
    if (!sampleRate || receiptNanoseconds <= 0) return {};
    anchors.push_back({endSample, receiptNanoseconds});
    while (anchors.size() > 31) anchors.pop_front();
    auto value = fit();
    // A host delivery step is a discontinuity in an *estimate* of UTC.  Do
    // not bridge it, because that would silently smear one host queue state
    // over another.  The 100 ms threshold is intentionally diagnostic only.
    if (value.anchors >= 3 && value.maximumResidualNanoseconds > 100000000LL) {
      value.discontinuity = true;
      const auto current = anchors.back();
      anchors.clear(); anchors.push_back(current);
      value = fit(); value.discontinuity = true;
    }
    return value;
  }

private:
  struct Anchor { uint64_t sample; int64_t receipt; };
  uint32_t sampleRate;
  std::deque<Anchor> anchors;

  static int64_t median(std::vector<int64_t> values) {
    std::sort(values.begin(), values.end());
    const auto n = values.size();
    return n % 2 ? values[n / 2] : values[n / 2 - 1] +
      (values[n / 2] - values[n / 2 - 1]) / 2;
  }

  Estimate fit() const {
    Estimate value; value.anchors = static_cast<uint32_t>(anchors.size());
    if (anchors.empty()) return value;
    // Theil-Sen slope rejects occasional host scheduling/queue outliers.
    std::vector<int64_t> slopes;
    for (size_t i = 0; i < anchors.size(); ++i) for (size_t j = i + 1;
         j < anchors.size(); ++j) {
      const auto ds = anchors[j].sample - anchors[i].sample;
      if (!ds) continue;
      const __int128 numerator = static_cast<__int128>(anchors[j].receipt -
        anchors[i].receipt) * sampleRate;
      slopes.push_back(static_cast<int64_t>(numerator / ds));
    }
    const int64_t nominal = 1000000000LL;
    const int64_t nsPerSecond = slopes.empty() ? nominal : median(std::move(slopes));
    value.driftPpm = (nsPerSecond - nominal) * 1000000LL / nominal;
    std::vector<int64_t> offsets;
    offsets.reserve(anchors.size());
    for (const auto& anchor : anchors) {
      const __int128 elapsed = static_cast<__int128>(anchor.sample) * nsPerSecond /
        sampleRate;
      offsets.push_back(anchor.receipt - static_cast<int64_t>(elapsed));
    }
    value.epochNanoseconds = median(std::move(offsets));
    std::vector<int64_t> residuals;
    for (const auto& anchor : anchors) {
      const __int128 predicted = static_cast<__int128>(value.epochNanoseconds) +
        static_cast<__int128>(anchor.sample) * nsPerSecond / sampleRate;
      const int64_t residual = anchor.receipt - static_cast<int64_t>(predicted);
      residuals.push_back(residual);
      const int64_t magnitude = residual == std::numeric_limits<int64_t>::min() ?
        std::numeric_limits<int64_t>::max() : std::abs(residual);
      value.maximumResidualNanoseconds = std::max(value.maximumResidualNanoseconds, magnitude);
    }
    value.residualNanoseconds = median(std::move(residuals));
    value.available = true;
    return value;
  }
};
} // namespace blah2::kraken_timing
