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
    recorder.append(1234, {&first, &second}, {{.5, .25}, {-.25, .5}},
      std::complex<double>{.125, -.75});
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
  REQUIRE(document.HasMember("pilot_reference"));
  REQUIRE(document["pilot_reference"][0].GetDouble() == Catch::Approx(.125));
  REQUIRE(document["pilot_reference"][1].GetDouble() == Catch::Approx(-.75));
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

TEST_CASE("native cutout preserves tiny complex phase and exact capture identity")
{
  using namespace blah2::analysis;
  const std::string path = "/tmp/vectorwarp-native-cutout-" +
    std::to_string(getpid()) + ".jsonl";
  ComplexCutoutRecorder::Config config;
  config.enabled = true; config.path = path; config.maxFrames = 1;
  config.delayKm = 0; config.dopplerHz = 0;
  config.delayRadiusBins = 0; config.dopplerRadiusBins = 0;
  Map<std::complex<double>> map(1, 1);
  map.delay = {0}; map.doppler = {0};
  const std::complex<double> tiny{1.234567890123456e-18, -9.876543210987654e-21};
  map.data[0][0] = tiny;
  NativeProductIdentity identity;
  identity.illuminatorId = "lane"; identity.frequencyHz = 100000000;
  identity.sampleRateHz = 20000; identity.channelIds = {4};
  identity.referenceChannelIds = {0, 2};
  identity.captureSamplePositionAvailable = true;
  identity.captureFirst = 9007199254740993ULL;
  identity.captureEnd = identity.captureFirst + 4000;
  identity.eventTime.present = true; identity.eventTime.epoch = 7;
  {
    ComplexCutoutRecorder recorder(config, 40000, true);
    recorder.appendNative(1234, {&map}, {{.5, 0}, {.5, 0}}, std::nullopt, &identity);
    recorder.appendNative(2345, {&map}, {}, std::nullopt, &identity);
    REQUIRE(recorder.frames() == 1);
  }
  std::ifstream input(path);
  const std::string text((std::istreambuf_iterator<char>(input)), {});
  std::remove(path.c_str());
  rapidjson::Document document;
  document.Parse<rapidjson::kParseFullPrecisionFlag>(text.c_str());
  REQUIRE_FALSE(document.HasParseError());
  REQUIRE(document["channels"][0][0][0][0].GetDouble() == tiny.real());
  REQUIRE(document["channels"][0][0][0][1].GetDouble() == tiny.imag());
  REQUIRE(document["channel_ids"][0].GetUint() == 4);
  REQUIRE(document["reference_channel_ids"][1].GetUint() == 2);
  REQUIRE(document["reference_weights"].Size() == 2);
  REQUIRE(document["capture_first_input_sample"].GetUint64() == identity.captureFirst);
  REQUIRE(document["sample_rate_hz"].GetUint() == 20000);
  REQUIRE(std::string(document["eventTime"]["epoch"].GetString()) == "7");
  REQUIRE_FALSE(document.HasMember("pilot_reference"));
  const auto annotated = annotate_native_product("{\"native\":1.234567890123456e-18}", identity);
  REQUIRE(annotated.find("\"native\":1.234567890123456e-18") != std::string::npos);
}
