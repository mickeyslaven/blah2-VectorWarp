#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "process/spectrum/SpectrumAnalyser.h"
#include "rapidjson/document.h"
#include <cmath>

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
