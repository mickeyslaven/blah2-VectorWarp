#include "process/conditioning/SubbandChannelizer.h"

#include <catch2/catch_test_macros.hpp>
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

TEST_CASE("subband channelizer rejects unsafe or discontinuous inputs")
{
  using Channelizer = blah2::channelizer::SubbandChannelizer;
  REQUIRE_THROWS(Channelizer({2400000, 280000, 0, 200000, 16}, 2));
  REQUIRE_THROWS(Channelizer({2400000, 300000, 1150000, 200000, 16}, 2));
  REQUIRE_THROWS(Channelizer({2400000, 300000, 0, 300000, 16}, 2));
  Channelizer channelizer({2400000, 300000, 0, 200000, 16}, 2);
  channelizer.process(std::vector<std::vector<std::complex<float>>>(2,
    std::vector<std::complex<float>>(100)), 0);
  REQUIRE_THROWS(channelizer.process(
    std::vector<std::vector<std::complex<float>>>(2,
      std::vector<std::complex<float>>(10)), 101));
  REQUIRE_THROWS(channelizer.process(
    std::vector<std::vector<std::complex<float>>>{
      std::vector<std::complex<float>>(10),
      std::vector<std::complex<float>>(9)}, 100));
}
