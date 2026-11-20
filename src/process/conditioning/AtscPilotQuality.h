#ifndef ATSC_PILOT_QUALITY_H
#define ATSC_PILOT_QUALITY_H

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>

namespace blah2::conditioning
{
// Diagnostic used to qualify the ATSC 1.0 carrier pilot. A carrier alone is
// not decoder lock; this observer never modifies IQ.
class AtscPilotQuality
{
public:
  struct Metrics
  {
    bool available = false, pilotCandidate = false;
    double cfoHz = 0, pilotToResidualDb = 0, coherence = 0;
    uint64_t firstInputSample = 0, endInputSample = 0;
    uint32_t inputSamplesPerOutput = 0;
    double groupDelayInputSamples = 0;
    std::size_t observedSamples = 0;
    bool timingAvailable = false;
  };

  explicit AtscPilotQuality(uint32_t rate)
    : rate_(rate), blockSamples_(std::max<std::size_t>(1, rate / 4000))
  {
    constexpr double pilot = -4.5e6 * 684.0 / 286.0 / 4.0;
    if (rate <= 2 * std::abs(pilot))
      throw std::invalid_argument("ATSC pilot is outside the sampled lane");
    step_ = std::polar(1.0, -2 * pi * pilot / rate);
  }

  void reset() { *this = AtscPilotQuality(rate_); }

  template<typename SampleAt>
  void observe(std::size_t count, SampleAt sampleAt, uint64_t first,
    uint32_t stride, double groupDelayInputSamples = 0)
  {
    if (!stride) throw std::invalid_argument("ATSC pilot sample stride is zero");
    if (!count || blocks_ == blockCount) return;
    if (metrics_.observedSamples == 0)
    {
      metrics_.firstInputSample = first;
      metrics_.inputSamplesPerOutput = stride;
      metrics_.groupDelayInputSamples = groupDelayInputSamples;
    }
    else if (stride != metrics_.inputSamplesPerOutput ||
        groupDelayInputSamples != metrics_.groupDelayInputSamples ||
        first != metrics_.endInputSample + stride)
      throw std::invalid_argument("ATSC pilot samples are discontinuous");
    count = std::min(count, blockSamples_ * blockCount - metrics_.observedSamples);
    if (count - 1 > (std::numeric_limits<uint64_t>::max() - first) / stride)
      throw std::overflow_error("ATSC pilot sample position overflow");
    for (std::size_t i = 0; i < count; ++i)
    {
      const std::complex<double> value(sampleAt(i));
      if (!std::isfinite(value.real()) || !std::isfinite(value.imag()))
        invalid_ = true;
      else sum_ += value * oscillator_;
      oscillator_ *= step_;
      if (++inBlock_ == blockSamples_)
      {
        const auto block = sum_ / double(blockSamples_);
        power_ += std::norm(block);
        if (blocks_) correlation_ += block * std::conj(previous_);
        previous_ = block;
        sum_ = {};
        inBlock_ = 0;
        ++blocks_;
      }
    }
    metrics_.observedSamples += count;
    metrics_.endInputSample = first + (count - 1) * stride;
    if (blocks_ == blockCount && !invalid_ && power_ > 0)
    {
      const double meanPower = power_ / blockCount;
      const double coherent = std::abs(correlation_) / (blockCount - 1);
      metrics_.coherence = std::min(1.0, coherent / meanPower);
      metrics_.cfoHz = std::arg(correlation_) * rate_ /
        (2 * pi * blockSamples_);
      metrics_.pilotToResidualDb = 10 * std::log10(
        std::max(1e-15, coherent) / std::max(1e-15, meanPower - coherent));
      metrics_.available = true;
      metrics_.pilotCandidate = metrics_.coherence >= .7;
    }
  }
  const Metrics& metrics() const { return metrics_; }

private:
  static constexpr double pi = 3.14159265358979323846;
  static constexpr std::size_t blockCount = 16;
  uint32_t rate_;
  std::size_t blockSamples_, blocks_ = 0, inBlock_ = 0;
  std::complex<double> step_, oscillator_{1, 0}, sum_{}, previous_{}, correlation_{};
  double power_ = 0;
  bool invalid_ = false;
  Metrics metrics_;
};

inline void validateAtscReferenceMode(const std::string& standard,
  const std::string& mode)
{
  if (mode != "raw" && mode != "pilot_aided" && mode != "decision_directed")
    throw std::invalid_argument("Unknown ATSC reference mode: " + mode);
  if (mode == "decision_directed")
    throw std::invalid_argument("ATSC decision_directed reference is not implemented");
  if (mode != "raw" && standard != "ATSC" && standard != "ATSC-1.0")
    throw std::invalid_argument("ATSC pilot_aided requires an ATSC 1.0 lane");
}

// Pilot projection operates causally on the current and prior CPIs, so ATSC
// 1.0 lanes use the same qualified reference path for live and replay input.
// Array references retain raw IQ because their synthesized topology is not a
// qualified input to the single-reference pilot projector.
inline std::string resolveAtscReferenceMode(const std::string& standard,
  const std::string& requested, bool /*offlineReplay*/, bool arrayReference = false)
{
  const bool atsc1 = standard == "ATSC" || standard == "ATSC-1.0";
  if (requested == "automatic") {
    if (!atsc1 && standard != "ATSC-3.0")
      throw std::invalid_argument("ATSC automatic reference requires an ATSC lane");
    return atsc1 && !arrayReference ? "pilot_aided" : "raw";
  }
  validateAtscReferenceMode(standard, requested);
  if (requested == "pilot_aided" && arrayReference)
    throw std::invalid_argument("ATSC pilot_aided does not support array reference");
  return requested;
}
} // namespace blah2::conditioning
#endif
