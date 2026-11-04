#include "process/conditioning/SubbandChannelizer.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fftw3.h>
#include <limits>
#include <numeric>
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

struct SubbandChannelizer::FftState
{
  std::size_t size = 2048;
  std::size_t channels = 0;
  fftw_complex* work = nullptr;
  fftw_plan forward = nullptr;
  fftw_plan inverse = nullptr;
  std::vector<std::complex<double>> kernel;

  explicit FftState(const std::vector<double>& taps, std::size_t channelCount)
    : channels(channelCount)
  {
    while (size < taps.size() * 2) size *= 2;
    work = fftw_alloc_complex(size * channelCount);
    kernel.resize(size);
    if (!work) throw std::bad_alloc();
    std::memset(work, 0, sizeof(fftw_complex) * size * channels);
    const int length = int(size);
    forward = fftw_plan_many_dft(1, &length, int(channels), work, nullptr,
      1, length, work, nullptr, 1, length, FFTW_FORWARD, FFTW_MEASURE);
    inverse = fftw_plan_many_dft(1, &length, int(channels), work, nullptr,
      1, length, work, nullptr, 1, length, FFTW_BACKWARD, FFTW_MEASURE);
    if (!forward || !inverse)
      throw std::runtime_error("Subband channelizer FFT plan failed");
    for (std::size_t channel = 0; channel < channels; ++channel)
      for (std::size_t index = 0; index < size; ++index)
      {
        const std::size_t at = channel * size + index;
        work[at][0] = index < taps.size() ? taps[index] : 0;
        work[at][1] = 0;
      }
    fftw_execute(forward);
    for (std::size_t index = 0; index < size; ++index)
      kernel[index] = {work[index][0], work[index][1]};
  }

  ~FftState()
  {
    if (forward) fftw_destroy_plan(forward);
    if (inverse) fftw_destroy_plan(inverse);
    if (work) fftw_free(work);
  }
};

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
    std::vector<std::complex<double>>(taps_.size() - 1));
  fft_ = std::make_unique<FftState>(taps_, channelCount_);
  oscillatorScratch_.resize(fft_->size - (taps_.size() - 1));
  const double step = -2 * pi * config_.centerOffsetHz /
    config_.inputRateHz;
  oscillatorStep_ = std::polar(1.0, step);
  const int64_t integralOffset = std::llround(config_.centerOffsetHz);
  if (std::abs(config_.centerOffsetHz - double(integralOffset)) < 1e-9)
  {
    const uint64_t magnitude = integralOffset < 0 ?
      uint64_t(-(integralOffset + 1)) + 1 : uint64_t(integralOffset);
    const uint64_t divisor = std::gcd<uint64_t>(config_.inputRateHz,
      magnitude % config_.inputRateHz);
    const uint64_t period = divisor ? config_.inputRateHz / divisor : 1;
    if (period <= 65536)
    {
      oscillatorTable_.resize(period);
      for (uint64_t index = 0; index < period; ++index)
        oscillatorTable_[index] = std::polar(1.0, step * double(index));
    }
  }
}

SubbandChannelizer::~SubbandChannelizer() = default;

std::vector<double> SubbandChannelizer::design_filter(const Config& config,
  uint32_t decimation)
{
  const uint64_t requested = uint64_t(config.tapsPerPhase) * decimation + 1;
  if (requested > 131071)
    throw std::invalid_argument("Subband channelizer filter is too large");
  std::size_t count = std::size_t(requested);
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
  return process_impl(samples, firstInputSample,
    [&input](std::size_t channel, std::size_t sample) {
      return std::complex<double>(input[channel][sample]);
    });
}

SubbandChannelizer::Output SubbandChannelizer::process(
  const std::vector<const std::deque<std::complex<double>>*>& input,
  uint64_t firstInputSample)
{
  if (input.size() != channelCount_ ||
      std::any_of(input.begin(), input.end(), [](const auto* channel) {
        return channel == nullptr;
      }))
    throw std::invalid_argument("Subband channelizer input channel count changed");
  const std::size_t samples = input.empty() ? 0 : input.front()->size();
  for (const auto* channel : input)
    if (channel->size() != samples)
      throw std::invalid_argument("Subband channelizer input channels are unpaired");
  return process_impl(samples, firstInputSample,
    [&input](std::size_t channel, std::size_t sample) {
      return (*input[channel])[sample];
    });
}

template <typename SampleAt>
SubbandChannelizer::Output SubbandChannelizer::process_impl(
  std::size_t samples, uint64_t firstInputSample, SampleAt&& sampleAt)
{
  // A live SDR can legitimately report a discontinuity after USB pressure or
  // receiver recovery.  Old FIR samples must never bridge that gap, but the
  // next complete block is a valid new stream epoch.  Re-seed the mixer from
  // its absolute sample position and restart the zero-filled causal FIR.
  if (started_ && firstInputSample != nextInputSample_)
    reset();
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

  // Streaming overlap-save computes the identical causal FIR convolution in
  // O(N log N), rather than O(N * taps). The mixer and decimation schedule are
  // unchanged, and each physical channel retains independent pre-gap history.
  const std::size_t overlap = taps_.size() - 1;
  const std::size_t blockCapacity = fft_->size - overlap;
  // Generate only one FFT block of mixer phasors at a time.  Retaining a full
  // capture-sized oscillator per illuminator made memory scale with capture
  // bandwidth (and reached gigabytes for a wideband B210 with many lanes).
  // The block is shared by every coherent input channel before the oscillator
  // advances, so phase identity and the output schedule are unchanged.
  auto& oscillator = oscillatorScratch_;
  for (std::size_t offset = 0; offset < samples; offset += blockCapacity)
  {
    const std::size_t count = std::min(blockCapacity, samples - offset);
    if (!oscillatorTable_.empty())
    {
      std::size_t phaseIndex = (firstInputSample + offset) %
        oscillatorTable_.size();
      for (std::size_t index = 0; index < count; ++index)
      {
        oscillator[index] = oscillatorTable_[phaseIndex];
        if (++phaseIndex == oscillatorTable_.size()) phaseIndex = 0;
      }
    }
    else
    {
      for (std::size_t index = 0; index < count; ++index)
      {
        oscillator[index] = oscillator_;
        oscillator_ *= oscillatorStep_;
      }
    }
    for (std::size_t channel = 0; channel < channelCount_; ++channel)
    {
      auto& prior = history_[channel];
      const std::size_t base = channel * fft_->size;
      for (std::size_t index = 0; index < overlap; ++index)
      {
        fft_->work[base + index][0] = prior[index].real();
        fft_->work[base + index][1] = prior[index].imag();
      }
      for (std::size_t index = 0; index < count; ++index)
      {
        const auto value = sampleAt(channel, offset + index) *
          oscillator[index];
        fft_->work[base + overlap + index][0] = value.real();
        fft_->work[base + overlap + index][1] = value.imag();
      }
      for (std::size_t index = overlap + count; index < fft_->size; ++index)
        fft_->work[base + index][0] = fft_->work[base + index][1] = 0;

      if (count >= overlap)
        for (std::size_t index = 0; index < overlap; ++index)
          prior[index] = {fft_->work[base + count + index][0],
            fft_->work[base + count + index][1]};
      else
      {
        std::move(prior.begin() + count, prior.end(), prior.begin());
        for (std::size_t index = 0; index < count; ++index)
          prior[overlap - count + index] =
            {fft_->work[base + overlap + index][0],
              fft_->work[base + overlap + index][1]};
      }
    }
    fftw_execute(fft_->forward);
    for (std::size_t channel = 0; channel < channelCount_; ++channel)
      for (std::size_t index = 0; index < fft_->size; ++index)
      {
        const std::size_t at = channel * fft_->size + index;
        const auto value = std::complex<double>(fft_->work[at][0],
          fft_->work[at][1]) * fft_->kernel[index];
        fft_->work[at][0] = value.real();
        fft_->work[at][1] = value.imag();
      }
    fftw_execute(fft_->inverse);
    const uint64_t blockFirst = firstInputSample + offset;
    const uint32_t remainder = uint32_t(blockFirst % decimation_);
    const std::size_t firstOutput = remainder ? decimation_ - remainder : 0;
    if (firstOutput < count && !output.hasFirstInputSample)
    {
      output.firstInputSample = blockFirst + firstOutput;
      output.hasFirstInputSample = true;
    }
    for (std::size_t channel = 0; channel < channelCount_; ++channel)
    {
      const std::size_t base = channel * fft_->size;
      for (std::size_t index = firstOutput; index < count;
           index += decimation_)
      {
        output.channels[channel].emplace_back(
          std::complex<double>(fft_->work[base + overlap + index][0],
            fft_->work[base + overlap + index][1]) / double(fft_->size));
      }
    }
  }
  if (samples)
  {
    if (!oscillatorTable_.empty())
      oscillator_ = oscillatorTable_[(firstInputSample + samples) %
        oscillatorTable_.size()];
    else
    {
      const double magnitude = std::abs(oscillator_);
      if (std::isfinite(magnitude) && magnitude > 0) oscillator_ /= magnitude;
    }
  }
  nextInputSample_ = firstInputSample + samples;
  return output;
}

} // namespace blah2::channelizer
