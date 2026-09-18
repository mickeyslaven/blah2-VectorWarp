#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "process/spectrum/SpectrumAnalyser.h"
#include "rapidjson/document.h"
#include <cmath>
#include <complex>
#include <deque>
#include <random>
#include <vector>

namespace {
using Complex = std::complex<double>;
constexpr double Pi = 3.14159265358979323846;

std::vector<Complex> selected_dft(const std::deque<Complex>& input,
  uint32_t count, uint32_t decimation)
{
  std::vector<Complex> result;
  for (uint32_t first = 0; first < count; first += decimation) {
    const uint32_t k = (first + (count + 1) / 2) % count;
    Complex sum{};
    for (uint32_t sample = 0; sample < count; ++sample)
      sum += input[sample] * std::polar(1.0, -2.0 * Pi * k * sample / count);
    result.push_back(sum / static_cast<double>(count));
  }
  return result;
}

void verify_selected_bins(uint32_t count, double bandwidth)
{
  const double sampleRate = count * 64.0;
  SpectrumAnalyser spectrum(count, bandwidth, 101234500, sampleRate);
  IqData data(count);
  std::mt19937 rng(count);
  std::uniform_real_distribution<double> value(-1.0, 1.0);
  for (uint32_t i = 0; i < count; ++i) data.push_back({value(rng), value(rng)});
  const auto input = data.view_data();
  const uint32_t decimation = static_cast<uint32_t>(std::ceil(bandwidth / 64.0));
  const auto expected = selected_dft(input, count, decimation);
  spectrum.process(&data);
  REQUIRE(data.view_data() == input);
  rapidjson::Document doc;
  doc.Parse(data.to_json(0).c_str());
  REQUIRE_FALSE(doc.HasParseError());
  REQUIRE(doc["spectrum"].Size() == expected.size());
  REQUIRE(doc["frequency"].Size() == expected.size());
  for (rapidjson::SizeType i = 0; i < doc["spectrum"].Size(); ++i) {
    CHECK_THAT(doc["spectrum"][i].GetDouble(), Catch::Matchers::WithinAbs(
      20.0 * std::log10(std::max(1e-15, std::abs(expected[i]))), .011));
    const double basebandBin = static_cast<double>(i * decimation) - count / 2;
    CHECK_THAT(doc["frequency"][i].GetDouble(), Catch::Matchers::WithinAbs(
      (101234500 + basebandBin * 64.0) / 1000.0, .011));
  }
}
}

TEST_CASE("Reference spectrum has correctly shifted, sample-rate-based frequency bins", "[spectrum]")
{
  for (const uint32_t count : {8u, 9u, 16u}) {
    const double sampleRate = count * 1000;
    IqData data(count);
    for (uint32_t i = 0; i < count; ++i)
      data.push_back(std::polar(1.0, 2 * std::acos(-1.0) * i / count));
    SpectrumAnalyser spectrum(count, 1000, 100000000, sampleRate);
    spectrum.process(&data);
    rapidjson::Document doc;
    doc.Parse(data.to_json(0).c_str());
    REQUIRE_FALSE(doc.HasParseError());
    REQUIRE(doc["frequency"].Size() == count);
    REQUIRE(doc["spectrum"].Size() == count);
    size_t peak = 0;
    for (uint32_t i = 0; i < count; ++i) {
      CHECK_THAT(doc["frequency"][i].GetDouble(),
        Catch::Matchers::WithinAbs(100000.0 + static_cast<double>(i) - count / 2, .001));
      REQUIRE(std::isfinite(doc["spectrum"][i].GetDouble()));
      if (doc["spectrum"][i].GetDouble() > doc["spectrum"][peak].GetDouble()) peak = i;
    }
    CHECK_THAT(doc["frequency"][static_cast<rapidjson::SizeType>(peak)].GetDouble(),
      Catch::Matchers::WithinAbs(100001, .001));
    CHECK_THAT(doc["spectrum"][static_cast<rapidjson::SizeType>(peak)].GetDouble(),
      Catch::Matchers::WithinAbs(0, .01));
  }
}

TEST_CASE("Zero samples and short frames cannot produce malformed spectrum JSON", "[spectrum]")
{
  IqData data(8);
  SpectrumAnalyser spectrum(8, 2000, 100000000, 8000);
  CHECK_THROWS_AS(spectrum.process(&data), std::invalid_argument);
  for (int i = 0; i < 8; ++i) data.push_back({0, 0});
  spectrum.process(&data);
  rapidjson::Document doc;
  doc.Parse(data.to_json(0).c_str());
  REQUIRE_FALSE(doc.HasParseError());
  REQUIRE(doc["frequency"].Size() == 4);
  REQUIRE(doc["spectrum"].Size() == 4);
  for (const auto& value : doc["spectrum"].GetArray()) CHECK(value.GetDouble() == -300);
  CHECK_THROWS_AS(SpectrumAnalyser(0, 2000, 100000000, 8000), std::invalid_argument);
}

TEST_CASE("Folded spectrum preserves the selected full-FFT bins", "[spectrum][regression]")
{
  // 64 / 8 is divisible, so this exercises the short folded transform.
  verify_selected_bins(64, 512);
  verify_selected_bins(72, 512); // Even input with nonzero shift remainder.
  verify_selected_bins(81, 576); // Odd divisible input, decimation nine.
}

TEST_CASE("Nondivisible and odd spectrum geometry retain the full transform", "[spectrum][regression]")
{
  // Both use decimation eight but retain VectorWarp's ceil-shaped output.
  verify_selected_bins(65, 512);
  verify_selected_bins(63, 504);
}
