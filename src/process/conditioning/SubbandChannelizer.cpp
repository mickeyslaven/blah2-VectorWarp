#include "process/conditioning/SubbandChannelizer.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace blah2::channelizer
{
namespace
{
constexpr double pi = 3.141592653589793238462643383279502884;

bool finite_positive(double value)
{
  return std::isfinite(value) && value > 0;
}
}

SubbandChannelizer::SubbandChannelizer(Config config,
  std::size_t channelCount)
  : config_(config), channelCount_(channelCount)
{
  if (!channelCount_ || !config_.inputRateHz || !config_.outputRateHz ||
      config_.inputRateHz % config_.outputRateHz != 0)
    throw std::invalid_argument(
      "Subband channelizer requires a positive integer sample-rate ratio");
  decimation_ = config_.inputRateHz / config_.outputRateHz;
  if (!decimation_ || !finite_positive(config_.occupiedBandwidthHz) ||
      config_.occupiedBandwidthHz >= config_.outputRateHz ||
      !std::isfinite(config_.centerOffsetHz) ||
      std::abs(config_.centerOffsetHz) + config_.occupiedBandwidthHz / 2 >=
        config_.inputRateHz / 2.0 ||
      config_.tapsPerPhase < 4 || config_.tapsPerPhase > 128)
    throw std::invalid_argument("Invalid subband channelizer configuration");

  taps_ = design_filter(config_, decimation_);
  groupDelayInputSamples_ = (taps_.size() - 1) / 2.0;
  history_.assign(channelCount_,
    std::vector<std::complex<double>>(taps_.size()));
  const double step = -2 * pi * config_.centerOffsetHz /
    config_.inputRateHz;
  oscillatorStep_ = std::polar(1.0, step);
}

std::vector<double> SubbandChannelizer::design_filter(const Config& config,
  uint32_t decimation)
{
  std::size_t count = std::size_t(config.tapsPerPhase) * decimation + 1;
  if (!(count & 1)) ++count;
  const double passEdge = config.occupiedBandwidthHz / 2;
  const double stopEdge = config.outputRateHz / 2.0;
  const double cutoff = (passEdge + stopEdge) / 2;
  const double normalized = cutoff / config.inputRateHz;
  const double middle = (count - 1) / 2.0;
  std::vector<double> taps(count);
  double sum = 0;
  for (std::size_t index = 0; index < count; ++index)
  {
    const double offset = double(index) - middle;
    const double sinc = offset == 0 ? 2 * normalized :
      std::sin(2 * pi * normalized * offset) / (pi * offset);
    const double window = 0.54 - 0.46 *
      std::cos(2 * pi * index / double(count - 1));
    taps[index] = sinc * window;
    sum += taps[index];
  }
  if (!std::isfinite(sum) || std::abs(sum) <
      std::numeric_limits<double>::epsilon())
    throw std::invalid_argument("Subband channelizer filter is invalid");
  for (double& tap : taps) tap /= sum;
  return taps;
}

void SubbandChannelizer::reset()
{
  for (auto& channel : history_)
    std::fill(channel.begin(), channel.end(), std::complex<double>{});
  writeIndex_ = historyFill_ = 0;
  started_ = false;
  nextInputSample_ = 0;
  oscillator_ = {1, 0};
}

SubbandChannelizer::Output SubbandChannelizer::process(
  const std::vector<std::vector<std::complex<float>>>& input,
  uint64_t firstInputSample)
{
  if (input.size() != channelCount_)
    throw std::invalid_argument("Subband channelizer input channel count changed");
  const std::size_t samples = input.empty() ? 0 : input.front().size();
  for (const auto& channel : input)
    if (channel.size() != samples)
      throw std::invalid_argument("Subband channelizer input channels are unpaired");
  if (started_ && firstInputSample != nextInputSample_)
    throw std::invalid_argument("Subband channelizer input has a sample gap");
  if (samples > std::numeric_limits<uint64_t>::max() - firstInputSample)
    throw std::overflow_error("Subband channelizer sample position overflow");

  if (!started_)
  {
    const double phase = std::remainder(-2 * pi * config_.centerOffsetHz *
      double(firstInputSample) / config_.inputRateHz, 2 * pi);
    oscillator_ = std::polar(1.0, phase);
    started_ = true;
  }

  Output output;
  output.channels.resize(channelCount_);
  output.inputSamplesPerOutput = decimation_;
  output.groupDelayInputSamples = groupDelayInputSamples_;
  const std::size_t expected = samples / decimation_ + 1;
  for (auto& channel : output.channels) channel.reserve(expected);

  for (std::size_t sample = 0; sample < samples; ++sample)
  {
    const uint64_t absolute = firstInputSample + sample;
    for (std::size_t channel = 0; channel < channelCount_; ++channel)
      history_[channel][writeIndex_] =
        std::complex<double>(input[channel][sample]) * oscillator_;
    writeIndex_ = (writeIndex_ + 1) % taps_.size();
    historyFill_ = std::min(historyFill_ + 1, taps_.size());

    // The zero-filled initial state is a valid causal FIR state. Emitting its
    // short startup transient keeps the first channelized CPI aligned with the
    // first capture CPI instead of adding a permanent one-CPI publication lag.
    if (absolute % decimation_ == 0)
    {
      if (!output.hasFirstInputSample)
      {
        output.firstInputSample = absolute;
        output.hasFirstInputSample = true;
      }
      for (std::size_t channel = 0; channel < channelCount_; ++channel)
      {
        std::complex<double> value{};
        for (std::size_t tap = 0; tap < taps_.size(); ++tap)
        {
          const std::size_t position =
            (writeIndex_ + taps_.size() - 1 - tap) % taps_.size();
          value += taps_[tap] * history_[channel][position];
        }
        output.channels[channel].emplace_back(value);
      }
    }
    oscillator_ *= oscillatorStep_;
  }
  if (samples)
  {
    const double magnitude = std::abs(oscillator_);
    if (std::isfinite(magnitude) && magnitude > 0) oscillator_ /= magnitude;
  }
  nextInputSample_ = firstInputSample + samples;
  return output;
}

} // namespace blah2::channelizer
