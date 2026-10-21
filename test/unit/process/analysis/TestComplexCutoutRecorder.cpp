#include "process/analysis/ComplexCutoutRecorder.h"
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <rapidjson/document.h>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <unistd.h>

TEST_CASE("complex cutout recorder preserves channel phase near the requested cell")
{
  const std::string path = "/tmp/vectorwarp-complex-cutout-" +
    std::to_string(getpid()) + ".jsonl";
  blah2::analysis::ComplexCutoutRecorder::Config config;
  config.enabled = true; config.path = path;
  config.delayKm = 0.124913524; config.dopplerHz = 0;
  config.delayRadiusBins = 1; config.dopplerRadiusBins = 1; config.maxFrames = 1;
  Map<std::complex<double>> first(3, 4), second(3, 4);
  for (auto* map : {&first, &second}) {
    map->delay = {0, 1, 2, 3}; map->doppler = {-10, 0, 10};
  }
  first.data[1][1] = {2, 3}; second.data[1][1] = {-4, 5};
  {
    blah2::analysis::ComplexCutoutRecorder recorder(config, 2400000, true);
    recorder.append(1234, {&first, &second}, {{.5, .25}, {-.25, .5}});
    recorder.append(5678, {&first, &second});
    REQUIRE(recorder.frames() == 1);
  }
  std::ifstream input(path);
  const std::string text((std::istreambuf_iterator<char>(input)), {});
  std::remove(path.c_str());
  rapidjson::Document document; document.Parse(text.c_str());
  REQUIRE_FALSE(document.HasParseError());
  REQUIRE(document["timestamp"].GetUint64() == 1234);
  REQUIRE(document["channels"].Size() == 2);
  REQUIRE(document["reference_weights"].Size() == 2);
  REQUIRE(document["reference_weights"][0][0].GetDouble() == Catch::Approx(.5));
  REQUIRE(document["channels"][0][1][1][0].GetDouble() == Catch::Approx(2));
  REQUIRE(document["channels"][0][1][1][1].GetDouble() == Catch::Approx(3));
  REQUIRE(document["channels"][1][1][1][0].GetDouble() == Catch::Approx(-4));
  REQUIRE(document["channels"][1][1][1][1].GetDouble() == Catch::Approx(5));
}

TEST_CASE("complex cutout recorder rejects live input")
{
  blah2::analysis::ComplexCutoutRecorder::Config config;
  config.enabled = true; config.path = "/tmp/unused"; config.maxFrames = 1;
  REQUIRE_THROWS_AS(blah2::analysis::ComplexCutoutRecorder(config, 2400000, false),
    std::invalid_argument);
}
