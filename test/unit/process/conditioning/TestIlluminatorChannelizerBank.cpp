#include "process/conditioning/IlluminatorChannelizerBank.h"

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <complex>
#include <vector>

namespace
{
constexpr double pi = 3.141592653589793238462643383279502884;

std::complex<double> amplitude(
  const std::vector<std::complex<float>>& values, double frequency,
  double sampleRate, std::size_t discard = 100)
{
  std::complex<double> result{};
  for (std::size_t sample = discard; sample < values.size(); ++sample)
    result += std::complex<double>(values[sample]) *
      std::polar(1.0, -2 * pi * frequency * sample / sampleRate);
  return result / double(values.size() - discard);
}
}

TEST_CASE("one Kraken passband yields two independent FM illuminators")
{
  constexpr uint32_t inputRate = 2400000;
  constexpr uint64_t center = 90900000;
  constexpr std::size_t count = 48000;
  constexpr std::size_t channels = 5;
  const std::vector<double> phase{0, .2, -.35, .71, -1.1};
  std::vector<std::vector<std::complex<float>>> input(channels,
    std::vector<std::complex<float>>(count));
  for (std::size_t sample = 0; sample < count; ++sample)
    for (std::size_t channel = 0; channel < channels; ++channel)
    {
      const auto lower = std::polar(1.0, 2 * pi *
        (-400000.0 + 22000.0) * sample / inputRate + phase[channel]);
      const auto upper = std::polar(.8, 2 * pi *
        (400000.0 - 31000.0) * sample / inputRate - .4 * phase[channel]);
      input[channel][sample] = std::complex<float>(lower + upper);
    }

  blah2::channelizer::IlluminatorChannelizerBank bank(center, inputRate,
    channels, {{"fm-90.5", 90500000, 200000, 300000, 24},
               {"fm-91.3", 91300000, 200000, 300000, 24}});
  const auto output = bank.process(input, 0);
  REQUIRE(output.size() == 2);
  REQUIRE(output[0].id == "fm-90.5");
  REQUIRE(output[1].id == "fm-91.3");
  REQUIRE(output[0].samples.channels.size() == channels);
  REQUIRE(output[1].samples.channels.size() == channels);

  const auto lower = amplitude(output[0].samples.channels[0], 22000, 300000);
  const auto lowerLeak = amplitude(output[1].samples.channels[0], 22000, 300000);
  const auto upper = amplitude(output[1].samples.channels[0], -31000, 300000);
  const auto upperLeak = amplitude(output[0].samples.channels[0], -31000, 300000);
  REQUIRE(std::abs(lower) > .9);
  REQUIRE(std::abs(upper) > .7);
  REQUIRE(20 * std::log10(std::abs(lower) / std::abs(lowerLeak)) > 45);
  REQUIRE(20 * std::log10(std::abs(upper) / std::abs(upperLeak)) > 45);

  for (std::size_t channel = 1; channel < channels; ++channel)
  {
    const auto lowerChannel = amplitude(
      output[0].samples.channels[channel], 22000, 300000);
    const auto upperChannel = amplitude(
      output[1].samples.channels[channel], -31000, 300000);
    REQUIRE(std::abs(std::arg(lowerChannel / lower) - phase[channel]) < 1e-3);
    REQUIRE(std::abs(std::arg(upperChannel / upper) + .4 * phase[channel]) < 1e-3);
  }
}

TEST_CASE("illuminator bank rejects duplicate and out-of-band stations")
{
  using Bank = blah2::channelizer::IlluminatorChannelizerBank;
  using Definition = blah2::channelizer::IlluminatorDefinition;
  REQUIRE_THROWS(Bank(90900000, 2400000, 5,
    std::vector<Definition>{{"same", 90500000, 200000, 300000, 16},
      {"same", 91300000, 200000, 300000, 16}}));
  REQUIRE_THROWS(Bank(90900000, 2400000, 5,
    std::vector<Definition>{{"outside", 93000000, 200000, 300000, 16}}));
}
