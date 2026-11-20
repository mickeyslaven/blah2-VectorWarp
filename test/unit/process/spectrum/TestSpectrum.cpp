#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "process/spectrum/SpectrumAnalyser.h"
#include "rapidjson/document.h"
#include <cmath>
#include <complex>
#include <deque>
#include <random>
#include <vector>
#include <algorithm>
#include <chrono>
#include <iostream>
#include <limits>

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

TEST_CASE("Native float spectrum exactly matches widened input without owning samples", "[spectrum][regression]")
{
  for (const auto& geometry : std::vector<std::pair<uint32_t, double>>{
      {1, 128}, {8, 64}, {64, 512}, {72, 512}, {81, 576},
      {65, 512}, {63, 504}, {128, 65536}, {257, 64}}) {
    const auto count = geometry.first;
    SpectrumAnalyser legacy(count, geometry.second, 101234500, count * 64.0);
    SpectrumAnalyser native(count, geometry.second, 101234500, count * 64.0);
    IqData widened(count), output(count);
    output.push_back({7, -9}); // Spectrum publication must not replace IQ.
    const auto retained = output.get_data();
    std::vector<std::complex<float>> input(count + 3);
    const auto* address = input.data();
    const auto capacity = input.capacity();
    for (unsigned pattern = 0; pattern < 5; ++pattern) {
      for (uint32_t i = 0; i < input.size(); ++i) {
        if (pattern == 0) input[i] = {0.0f, -0.0f};
        else if (pattern == 1) input[i] = i == 0 ? std::complex<float>{1, -1} : std::complex<float>{};
        else if (pattern == 2) input[i] = {float(int(i % 17) - 8) / 8, float(int(i % 23) - 11) / 16};
        else if (pattern == 3) input[i] = {std::numeric_limits<float>::denorm_min(), -std::numeric_limits<float>::min()};
        else input[i] = {(i % 2 ? -1 : 1) * std::numeric_limits<float>::max(), std::numeric_limits<float>::epsilon()};
      }
      const auto original = input;
      auto& data = widened.resize_for_write(count);
      std::copy_n(input.begin(), count, data.begin());
      legacy.process(&widened);
      native.process(input, &output);
      REQUIRE(output.to_json(1234567) == widened.to_json(1234567));
      REQUIRE(input == original);
      REQUIRE(input.data() == address);
      REQUIRE(input.capacity() == capacity);
      REQUIRE(output.get_data() == retained);
      const auto storage = output.take_storage_metrics();
      REQUIRE(storage.allocations == 0);
      REQUIRE(storage.materializations == 0);
    }
    const auto previous = output.to_json(1234567);
    input.resize(count - 1);
    CHECK_THROWS_AS(native.process(input, &output), std::invalid_argument);
    REQUIRE(output.to_json(1234567) == previous);
    CHECK_THROWS_AS(native.process(input, nullptr), std::invalid_argument);
  }
}

// Explicitly selected, bounded benchmark; excluded from ordinary unit runs.
// Alternate order to avoid assigning cache/thermal bias to one implementation.
TEST_CASE("Native float full CPI spectrum benchmark", "[.benchmark][spectrum]")
{
  constexpr uint32_t count = 12288000;
  std::vector<std::complex<float>> input(count);
  for (uint32_t i = 0; i < count; ++i)
    input[i] = {float(int(i % 251) - 125) / 128, float(int(i % 257) - 128) / 128};
  SpectrumAnalyser legacy(count, 2000, 527000000, 24576000);
  SpectrumAnalyser native(count, 2000, 527000000, 24576000);
  IqData widened(count), output(count);
  std::vector<double> legacyTimes, nativeTimes;
  const auto run = [&](bool direct) {
    const auto started = std::chrono::steady_clock::now();
    if (direct) native.process(input, &output);
    else {
      auto& data = widened.resize_for_write(count);
      std::copy(input.begin(), input.end(), data.begin());
      legacy.process(&widened);
    }
    auto json = (direct ? output : widened).to_json(1234567);
    const double elapsed = std::chrono::duration<double, std::milli>(
      std::chrono::steady_clock::now() - started).count();
    return std::make_pair(elapsed, std::move(json));
  };
  const auto coldLegacy = run(false);
  const auto coldNative = run(true);
  REQUIRE(coldLegacy.second == coldNative.second);
  for (unsigned iteration = 0; iteration < 9; ++iteration) {
    auto first = run(iteration % 2 != 0);
    auto second = run(iteration % 2 == 0);
    REQUIRE(first.second == second.second);
    legacyTimes.push_back(iteration % 2 ? second.first : first.first);
    nativeTimes.push_back(iteration % 2 ? first.first : second.first);
    REQUIRE(output.get_length() == 0);
    std::cout << "SPECTRUM_BENCH iteration=" << iteration
      << " samples=" << count << " legacy_ms=" << legacyTimes.back()
      << " native_ms=" << nativeTimes.back() << '\n';
  }
  std::sort(legacyTimes.begin(), legacyTimes.end());
  std::sort(nativeTimes.begin(), nativeTimes.end());
  std::cout << "SPECTRUM_BENCH_SUMMARY pairs=9 samples=" << count
    << " cold_legacy_ms=" << coldLegacy.first << " cold_native_ms=" << coldNative.first
    << " legacy_p50_ms=" << legacyTimes[4] << " native_p50_ms=" << nativeTimes[4]
    << " legacy_max_ms=" << legacyTimes.back() << " native_max_ms=" << nativeTimes.back()
    << " eliminated_fp64_sample_bytes=" << uint64_t(count) * sizeof(std::complex<double>)
    << " avoided_sample_read_write_bytes=" << uint64_t(count) * sizeof(std::complex<double>) * 2
    << '\n';
}
