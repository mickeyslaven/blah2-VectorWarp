#include "process/analysis/ComplexCutoutRecorder.h"
#include "process/analysis/ExperimentalRouteAdmission.h"
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
    REQUIRE(recorder.complete());
    REQUIRE(recorder.bytes() > 0);
    REQUIRE_THROWS_AS(blah2::analysis::ComplexCutoutRecorder(config, 2400000, true),
      std::runtime_error);
    rapidjson::Document status; status.Parse(recorder.statusJson().c_str());
    REQUIRE(status["complete"].GetBool());
    REQUIRE(status["bytes"].GetUint64() == recorder.bytes());
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

TEST_CASE("complex cutout stops before a record exceeds its storage budget")
{
  using namespace blah2::analysis;
  const std::string path = "/tmp/vectorwarp-bounded-cutout-" + std::to_string(getpid()) + ".jsonl";
  ComplexCutoutRecorder::Config config;
  config.enabled = true; config.path = path; config.maximumBytes = 1ULL << 20;
  config.delayRadiusBins = 64; config.dopplerRadiusBins = 64;
  Map<std::complex<double>> map(129, 129);
  map.delay.resize(129); map.doppler.resize(129);
  for (int index = 0; index < 129; ++index) {
    map.delay[index] = index - 64; map.doppler[index] = index - 64;
    for (auto& cell : map.data[index]) cell = {1.23456789012345, -6.7890123456789};
  }
  {
    ComplexCutoutRecorder recorder(config, 2400000, true);
    recorder.append(1, {&map, &map, &map, &map, &map, &map, &map, &map});
    REQUIRE(recorder.complete());
    REQUIRE(recorder.frames() == 0);
    REQUIRE(recorder.bytes() == 0);
  }
  std::ifstream input(path, std::ios::binary | std::ios::ate);
  REQUIRE(input.tellg() == 0);
  input.close(); std::remove(path.c_str());
  config.maximumBytes = (256ULL << 20) + 1;
  REQUIRE_THROWS_AS(ComplexCutoutRecorder(config, 2400000, true), std::invalid_argument);
}

TEST_CASE("experimental native routes reject accepted but inert combinations")
{
  using namespace blah2::analysis;
  for (const std::string mode : {"cpu", "gpu"}) {
    for (bool replay : {false, true})
      REQUIRE_NOTHROW(validateTargetProtection(true, replay, false, mode, true, true, 12, 0));
    REQUIRE_NOTHROW(validateComplexCutoutRoute(true, true, mode));
  }
  for (const std::string mode : {"auto", "gpu-resident"}) {
    REQUIRE_THROWS_AS(validateTargetProtection(true, true, false, mode, true, true, 12, 0), std::invalid_argument);
    REQUIRE_THROWS_AS(validateComplexCutoutRoute(true, true, mode), std::invalid_argument);
  }
  REQUIRE_THROWS_AS(validateTargetProtection(true, false, false, "cpu", false, true, 12, 0), std::invalid_argument);
  REQUIRE_THROWS_AS(validateTargetProtection(true, true, true, "cpu", true, true, 12, 0), std::invalid_argument);
  REQUIRE_THROWS_AS(validateTargetProtection(true, true, false, "cpu", true, false, 12, 0), std::invalid_argument);
  REQUIRE_THROWS_AS(validateTargetProtection(true, true, false, "cpu", true, true, 12, 12), std::invalid_argument);
  REQUIRE_THROWS_AS(validateComplexCutoutRoute(true, false, "cpu"), std::invalid_argument);
  REQUIRE_NOTHROW(validateTargetProtection(false, false, false, "auto", false, false, 12, 0));
  REQUIRE_NOTHROW(validateTemporalExperimentCells(601, 1261));
  REQUIRE_NOTHROW(validateTemporalExperimentCells(1024, 1024));
  REQUIRE_THROWS_AS(validateTemporalExperimentCells(1025, 1024), std::invalid_argument);
  REQUIRE_THROWS_AS(validateTemporalExperimentCells(UINT64_MAX, UINT64_MAX), std::invalid_argument);
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
    recorder.appendNative(1234, {&map}, {{.5, 0}, {.5, 0}},
      std::complex<double>{.5, 0}, &identity, 4);
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
  REQUIRE(document["pilot_reference"][0].GetDouble() == Catch::Approx(.5));
  REQUIRE(document["pilot_anchor_channel_id"].GetUint() == 4);
  const auto annotated = annotate_native_product("{\"native\":1.234567890123456e-18}", identity);
  REQUIRE(annotated.find("\"native\":1.234567890123456e-18") != std::string::npos);
}
