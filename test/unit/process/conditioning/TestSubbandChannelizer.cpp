#include "process/conditioning/SubbandChannelizer.h"

#include <catch2/catch_test_macros.hpp>
#include <algorithm>
#include <cmath>
#include <complex>
#include <vector>

namespace
{
constexpr double pi = 3.141592653589793238462643383279502884;

std::complex<double> tone(const std::vector<std::complex<float>>& values,
  double frequency, double sampleRate, std::size_t discard = 0)
{
  std::complex<double> result{};
  for (std::size_t sample = discard; sample < values.size(); ++sample)
    result += std::complex<double>(values[sample]) *
      std::polar(1.0, -2 * pi * frequency * sample / sampleRate);
  return result / double(values.size() - discard);
}

std::vector<std::vector<std::complex<float>>> fixture(std::size_t samples)
{
  constexpr double inputRate = 2400000;
  constexpr double selectedCenter = 400000;
  constexpr double selectedTone = 30000;
  constexpr double rejectedTone = 760000;
  std::vector<std::vector<std::complex<float>>> result(2,
    std::vector<std::complex<float>>(samples));
  for (std::size_t sample = 0; sample < samples; ++sample)
  {
    const auto selected = std::polar(1.0,
      2 * pi * (selectedCenter + selectedTone) * sample / inputRate);
    const auto rejected = std::polar(0.9,
      2 * pi * rejectedTone * sample / inputRate);
    result[0][sample] = std::complex<float>(selected + rejected);
    result[1][sample] = std::complex<float>(
      selected * std::polar(1.0, 0.63) + rejected * std::polar(1.0, -0.4));
  }
  return result;
}

std::vector<double> scalar_taps(uint32_t inputRate, uint32_t outputRate,
  double occupiedBandwidth, uint32_t tapsPerPhase)
{
  const uint32_t decimation = inputRate / outputRate;
  std::size_t count = std::size_t(tapsPerPhase) * decimation + 1;
  if (!(count & 1)) ++count;
  const double normalized =
    (occupiedBandwidth / 2 + outputRate / 2.0) / 2 / inputRate;
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
  for (auto& tap : taps) tap /= sum;
  return taps;
}

std::vector<std::vector<std::complex<float>>> scalar_channelize(
  const std::vector<std::vector<std::complex<float>>>& input,
  uint64_t firstInputSample)
{
  constexpr uint32_t inputRate = 2400000, outputRate = 300000;
  constexpr double centerOffset = 437500, occupiedBandwidth = 200000;
  constexpr uint32_t tapsPerPhase = 24;
  const uint32_t decimation = inputRate / outputRate;
  const auto taps = scalar_taps(inputRate, outputRate, occupiedBandwidth,
    tapsPerPhase);
  std::vector<std::vector<std::complex<float>>> output(input.size());
  std::vector<std::vector<std::complex<double>>> history(input.size());
  for (auto& channel : history) channel.reserve(input.front().size());
  auto oscillator = std::polar(1.0, std::remainder(-2 * pi * centerOffset *
    double(firstInputSample) / inputRate, 2 * pi));
  const auto step = std::polar(1.0, -2 * pi * centerOffset / inputRate);
  for (std::size_t sample = 0; sample < input.front().size(); ++sample)
  {
    for (std::size_t channel = 0; channel < input.size(); ++channel)
      history[channel].push_back(
        std::complex<double>(input[channel][sample]) * oscillator);
    const uint64_t absolute = firstInputSample + sample;
    if (absolute % decimation == 0)
      for (std::size_t channel = 0; channel < input.size(); ++channel)
      {
        std::complex<double> value{};
        const std::size_t available = std::min(taps.size(), sample + 1);
        for (std::size_t tap = 0; tap < available; ++tap)
          value += taps[tap] * history[channel][sample - tap];
        output[channel].emplace_back(value);
      }
    oscillator *= step;
  }
  return output;
}
}

TEST_CASE("subband channelizer isolates an FM-sized illuminator coherently")
{
  blah2::channelizer::SubbandChannelizer channelizer(
    {2400000, 300000, 400000, 200000, 24}, 2);
  const auto output = channelizer.process(fixture(48000), 0);
  REQUIRE(output.channels.size() == 2);
  REQUIRE(output.channels[0].size() == output.channels[1].size());
  REQUIRE(output.channels[0].size() > 5000);
  REQUIRE(output.hasFirstInputSample);
  REQUIRE(output.inputSamplesPerOutput == 8);

  const std::size_t discard = 100;
  const auto selected0 = tone(output.channels[0], 30000, 300000, discard);
  const auto selected1 = tone(output.channels[1], 30000, 300000, discard);
  // The rejected 760 kHz input aliases to 60 kHz after mixing/decimation if
  // the anti-alias filter does not suppress it.
  const auto rejected = tone(output.channels[0], 60000, 300000, discard);
  REQUIRE(std::abs(selected0) > 0.9);
  REQUIRE(20 * std::log10(std::abs(selected0) / std::abs(rejected)) > 45);
  REQUIRE(std::abs(std::arg(selected1 / selected0) - 0.63) < 1e-3);
}

TEST_CASE("subband channelizer is block-boundary independent")
{
  const auto input = fixture(24017);
  blah2::channelizer::SubbandChannelizer whole(
    {2400000, 300000, -300000, 180000, 16}, 2);
  blah2::channelizer::SubbandChannelizer split(
    {2400000, 300000, -300000, 180000, 16}, 2);
  const auto expected = whole.process(input, 1000);

  std::vector<std::vector<std::complex<float>>> first(2), second(2), third(2);
  for (std::size_t channel = 0; channel < 2; ++channel)
  {
    first[channel].assign(input[channel].begin(), input[channel].begin() + 113);
    second[channel].assign(input[channel].begin() + 113,
      input[channel].begin() + 7901);
    third[channel].assign(input[channel].begin() + 7901, input[channel].end());
  }
  const auto a = split.process(first, 1000);
  const auto b = split.process(second, 1113);
  const auto c = split.process(third, 8901);
  std::vector<std::vector<std::complex<float>>> actual(2);
  for (std::size_t channel = 0; channel < 2; ++channel)
  {
    actual[channel].insert(actual[channel].end(), a.channels[channel].begin(), a.channels[channel].end());
    actual[channel].insert(actual[channel].end(), b.channels[channel].begin(), b.channels[channel].end());
    actual[channel].insert(actual[channel].end(), c.channels[channel].begin(), c.channels[channel].end());
    REQUIRE(actual[channel].size() == expected.channels[channel].size());
    for (std::size_t sample = 0; sample < actual[channel].size(); ++sample)
      REQUIRE(std::abs(actual[channel][sample] - expected.channels[channel][sample]) < 1e-6);
  }
  const uint64_t firstOutput = a.hasFirstInputSample ? a.firstInputSample :
    b.hasFirstInputSample ? b.firstInputSample : c.firstInputSample;
  REQUIRE(firstOutput == expected.firstInputSample);
}

TEST_CASE("FFT overlap-save matches the unchanged 24-tap-per-phase scalar FIR")
{
  constexpr uint64_t firstInputSample = 37;
  constexpr std::size_t sampleCount = 10007;
  std::vector<std::vector<std::complex<float>>> input(2,
    std::vector<std::complex<float>>(sampleCount));
  uint32_t state = 0x51f15e5dU;
  for (std::size_t sample = 0; sample < sampleCount; ++sample)
    for (std::size_t channel = 0; channel < input.size(); ++channel)
    {
      state = state * 1664525U + 1013904223U;
      const float real = float(int32_t(state >> 8)) / float(1U << 23);
      state = state * 1664525U + 1013904223U;
      const float imaginary = float(int32_t(state >> 8)) / float(1U << 23);
      input[channel][sample] = {real, imaginary};
    }

  blah2::channelizer::SubbandChannelizer channelizer(
    {2400000, 300000, 437500, 200000, 24}, input.size());
  const auto actual = channelizer.process(input, firstInputSample);
  const auto expected = scalar_channelize(input, firstInputSample);
  const uint64_t expectedFirst = ((firstInputSample + 7) / 8) * 8;
  const std::size_t expectedCount =
    (firstInputSample + sampleCount - 1 - expectedFirst) / 8 + 1;
  REQUIRE(actual.hasFirstInputSample);
  REQUIRE(actual.firstInputSample == expectedFirst);
  REQUIRE(actual.inputSamplesPerOutput == 8);
  REQUIRE(actual.channels.size() == expected.size());
  for (std::size_t channel = 0; channel < expected.size(); ++channel)
  {
    REQUIRE(actual.channels[channel].size() == expectedCount);
    REQUIRE(actual.channels[channel].size() == expected[channel].size());
    for (std::size_t sample = 0; sample < expected[channel].size(); ++sample)
    {
      // FFT convolution changes the FP64 addition order before conversion to
      // FP32. This permits about 16 float ULP at unit scale while remaining
      // far below the channelizer's input precision and receiver noise floor.
      const float tolerance = 2e-6f *
        std::max(1.0f, std::abs(expected[channel][sample]));
      REQUIRE(std::abs(actual.channels[channel][sample] -
        expected[channel][sample]) <= tolerance);
    }
  }
}

TEST_CASE("deque capture view matches the float channelizer input")
{
  using Channelizer = blah2::channelizer::SubbandChannelizer;
  std::vector<std::vector<std::complex<float>>> packed(2,
    std::vector<std::complex<float>>(9001));
  for (std::size_t channel = 0; channel < packed.size(); ++channel)
    for (std::size_t sample = 0; sample < packed[channel].size(); ++sample)
      packed[channel][sample] = {float(std::sin(.013 * sample + channel)),
        float(std::cos(.017 * sample - channel))};
  std::vector<std::deque<std::complex<double>>> storage(packed.size());
  std::vector<const std::deque<std::complex<double>>*> views;
  for (std::size_t channel = 0; channel < packed.size(); ++channel)
  {
    storage[channel].insert(storage[channel].end(), packed[channel].begin(),
      packed[channel].end());
    views.push_back(&storage[channel]);
  }
  Channelizer fromPacked({2400000, 300000, -400000, 200000, 24}, 2);
  Channelizer fromCapture({2400000, 300000, -400000, 200000, 24}, 2);
  const auto expected = fromPacked.process(packed, 11);
  const auto actual = fromCapture.process(views, 11);
  REQUIRE(actual.firstInputSample == expected.firstInputSample);
  REQUIRE(actual.channels == expected.channels);
}

TEST_CASE("full-quality wideband B210 decimation uses a larger FFT block")
{
  using Channelizer = blah2::channelizer::SubbandChannelizer;
  Channelizer channelizer({60000000, 300000, 400000, 200000, 24}, 2);
  REQUIRE(channelizer.group_delay_input_samples() == 2400);
  std::vector<std::vector<std::complex<float>>> input(2,
    std::vector<std::complex<float>>(20000, {1, 0}));
  const auto output = channelizer.process(input, 0);
  REQUIRE(output.channels.size() == 2);
  REQUIRE(output.channels[0].size() == 100);
  REQUIRE(output.channels[1].size() == 100);
  REQUIRE_THROWS(Channelizer({1000000000, 1, 0, .5, 128}, 2));
}

TEST_CASE("subband channelizer rejects unsafe inputs and resynchronizes gaps")
{
  using Channelizer = blah2::channelizer::SubbandChannelizer;
  REQUIRE_THROWS(Channelizer({2400000, 280000, 0, 200000, 16}, 2));
  REQUIRE_THROWS(Channelizer({2400000, 300000, 1150000, 200000, 16}, 2));
  REQUIRE_THROWS(Channelizer({2400000, 300000, 0, 300000, 16}, 2));
  Channelizer channelizer({2400000, 300000, 0, 200000, 16}, 2);
  channelizer.process(std::vector<std::vector<std::complex<float>>>(2,
    std::vector<std::complex<float>>(100)), 0);
  const auto afterGap = channelizer.process(
    std::vector<std::vector<std::complex<float>>>(2,
      std::vector<std::complex<float>>(10)), 101);
  REQUIRE(afterGap.hasFirstInputSample);
  REQUIRE(afterGap.firstInputSample == 104);
  REQUIRE_THROWS(channelizer.process(
    std::vector<std::vector<std::complex<float>>>{
      std::vector<std::complex<float>>(10),
      std::vector<std::complex<float>>(9)}, 100));
}
