#include <catch2/catch_test_macros.hpp>
#include "data/Map.h"
#include "rapidjson/document.h"
#include <cmath>

TEST_CASE("Zero ambiguity samples still serialize a complete finite map", "[map]")
{
  Map<std::complex<double>> map(3, 3);
  map.delay = {-1, 0, 1};
  map.doppler = {-10, 0, 10};
  for (auto& row : map.data) for (auto& value : row) value = {0, 0};
  map.set_metrics();
  REQUIRE(std::isfinite(map.noisePower));
  REQUIRE(std::isfinite(map.maxPower));
  rapidjson::Document doc;
  doc.Parse(map.to_json(0).c_str());
  REQUIRE_FALSE(doc.HasParseError());
  for (const auto& row : doc["data"].GetArray())
    for (const auto& cell : row.GetArray()) REQUIRE(std::isfinite(cell.GetDouble()));
  doc.Parse(map.delay_bin_to_km(map.to_json(0), 2000000).c_str());
  REQUIRE_FALSE(doc.HasParseError());
  REQUIRE(doc["delay"].Size() == 3);
}

TEST_CASE("Invalid map JSON cannot enter unchecked RapidJSON mutation", "[map]")
{
  Map<std::complex<double>> map(1, 1);
  map.delay = {0}; map.doppler = {0};
  CHECK_THROWS(map.delay_bin_to_km("{\"data\":[", 2000000));
  CHECK_THROWS(map.delay_bin_to_km("{\"delay\":false}", 2000000));
  CHECK_THROWS(map.delay_bin_to_km("{\"delay\":[]}", 0));
}
