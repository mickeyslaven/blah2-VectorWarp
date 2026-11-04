#include "process/conditioning/IlluminatorChannelizerBank.h"

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <complex>
#include <cstdint>
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

TEST_CASE("shared-forward raster matches independent full FIR channelizers")
{
  using Bank = blah2::channelizer::IlluminatorChannelizerBank;
  using Definition = blah2::channelizer::IlluminatorDefinition;
  using Channelizer = blah2::channelizer::SubbandChannelizer;
  constexpr uint32_t inputRate = 2400000;
  constexpr uint32_t outputRate = 300000;
  constexpr uint64_t center = 90900000;
  constexpr uint64_t first = 37;
  constexpr std::size_t channels = 2;
  constexpr std::size_t count = 7319;
  const std::vector<Definition> definitions{
    {"lower", 90500000, 200000, outputRate, 24},
    {"upper", 91300000, 200000, outputRate, 24}};
  std::vector<std::vector<std::complex<float>>> input(channels,
    std::vector<std::complex<float>>(count));
  uint32_t state = 0x971e137bU;
  for (std::size_t sample = 0; sample < count; ++sample)
    for (std::size_t channel = 0; channel < channels; ++channel)
    {
      state = state * 1664525U + 1013904223U;
      const float i = float(int32_t(state)) / 2147483648.0f;
      state = state * 1664525U + 1013904223U;
      const float q = float(int32_t(state)) / 2147483648.0f;
      input[channel][sample] = {i, q};
    }

  Bank bank(center, inputRate, channels, definitions);
  REQUIRE(bank.uses_shared_forward());
  const auto combined = bank.process(input, first);
  const uint64_t afterGapFirst = first + count + 13;
  const auto afterGap = bank.process(input, afterGapFirst);
  REQUIRE(combined.size() == definitions.size());
  for (std::size_t lane = 0; lane < definitions.size(); ++lane)
  {
    const auto& definition = definitions[lane];
    Channelizer scalar({inputRate, outputRate,
      double(int64_t(definition.frequencyHz) - int64_t(center)),
      double(definition.occupiedBandwidthHz), definition.tapsPerPhase},
      channels);
    const auto expected = scalar.process(input, first);
    const auto expectedAfterGap = scalar.process(input, afterGapFirst);
    const auto& actual = combined[lane].samples;
    REQUIRE(actual.hasFirstInputSample == expected.hasFirstInputSample);
    REQUIRE(actual.firstInputSample == expected.firstInputSample);
    REQUIRE(actual.inputSamplesPerOutput == expected.inputSamplesPerOutput);
    REQUIRE(actual.groupDelayInputSamples == expected.groupDelayInputSamples);
    REQUIRE(actual.channels.size() == expected.channels.size());
    for (std::size_t channel = 0; channel < channels; ++channel)
    {
      REQUIRE(actual.channels[channel].size() == expected.channels[channel].size());
      for (std::size_t sample = 0; sample < expected.channels[channel].size();
           ++sample)
      {
        // Both paths evaluate the same 24-tap/phase FIR in double precision;
        // only FFT association and the final float rounding differ.
        REQUIRE(std::abs(actual.channels[channel][sample] -
          expected.channels[channel][sample]) < 2e-5f);
      }
    }
    REQUIRE(afterGap[lane].samples.firstInputSample ==
      expectedAfterGap.firstInputSample);
    for (std::size_t channel = 0; channel < channels; ++channel)
    {
      REQUIRE(afterGap[lane].samples.channels[channel].size() ==
        expectedAfterGap.channels[channel].size());
      for (std::size_t sample = 0;
           sample < expectedAfterGap.channels[channel].size(); ++sample)
        REQUIRE(std::abs(afterGap[lane].samples.channels[channel][sample] -
          expectedAfterGap.channels[channel][sample]) < 2e-5f);
    }
  }
}

TEST_CASE("heterogeneous illuminators retain independent channelizer fallback")
{
  using Bank = blah2::channelizer::IlluminatorChannelizerBank;
  Bank bank(90900000, 2400000, 2,
    {{"wide", 90500000, 200000, 300000, 24},
     {"narrow", 91300000, 100000, 200000, 24}});
  REQUIRE_FALSE(bank.uses_shared_forward());
}
