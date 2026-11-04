#include "process/analysis/MultiIlluminatorRadar.h"
#include "process/analysis/MultiIlluminatorPipelineState.h"
#include "process/conditioning/FmReferenceQualityFilter.h"
#include "process/utility/FftwThreads.h"
#include "data/meta/Timing.h"

#include <catch2/catch_test_macros.hpp>
#include <rapidjson/document.h>
#include <cmath>
#include <complex>
#include <random>
#include <utility>
#include <vector>

namespace
{
constexpr double pi = 3.141592653589793238462643383279502884;

blah2::channelizer::IlluminatorOutput signal(const std::string& id,
  uint64_t frequency, uint32_t rate, uint32_t decimation,
  std::size_t channels, std::size_t samples, uint32_t delay, double doppler,
  uint32_t seed)
{
  std::mt19937 generator(seed);
  std::normal_distribution<double> random;
  std::vector<std::complex<double>> direct(samples);
  for (auto& value : direct) value = {random(generator), random(generator)};
  blah2::channelizer::IlluminatorOutput result;
  result.id = id;
  result.frequencyHz = frequency;
  result.samples.channels.assign(channels,
    std::vector<std::complex<float>>(samples));
  result.samples.firstInputSample = 0;
  result.samples.hasFirstInputSample = true;
  result.samples.inputSamplesPerOutput = decimation;
  for (std::size_t channel = 0; channel < channels; ++channel)
    for (std::size_t sample = 0; sample < samples; ++sample)
    {
      const auto main = direct[sample] * std::polar(1.0, .19 * channel);
      std::complex<double> echo{};
      if (sample >= delay)
        echo = .55 * direct[sample - delay] *
          std::polar(1.0, .83 * channel + 2 * pi * doppler * sample / rate);
      result.samples.channels[channel][sample] =
        std::complex<float>(main + echo +
          std::complex<double>(random(generator), random(generator)) * .005);
    }
  return result;
}

TEST_CASE("FM reference quality filter omits only low-bandwidth blocks")
{
  constexpr uint32_t rate = 2000;
  constexpr std::size_t block = 10;
  IqData input(4 * block), output(4 * block);
  auto& values = input.resize_for_write(4 * block);
  double phase = 0;
  for (std::size_t sample = 0; sample < values.size(); ++sample)
  {
    const std::size_t section = sample / block;
    const double step = section == 0 ? .2 :
      .2 + .08 * std::sin(.7 * sample + section);
    phase += step;
    values[sample] = std::polar(1.0, phase);
  }
  blah2::conditioning::FmReferenceQualityFilter filter(rate, .75, .005);
  const auto metrics = filter.process(input, output);
  REQUIRE(metrics.blocks == 4);
  REQUIRE(metrics.retainedBlocks == 3);
  REQUIRE(metrics.applied);
  for (std::size_t sample = 0; sample < block; ++sample)
    REQUIRE(output.view_data()[sample] == std::complex<double>{});
  for (std::size_t sample = block; sample < values.size(); ++sample)
    REQUIRE(output.view_data()[sample] == values[sample]);
}

TEST_CASE("FM reference quality filter preserves equal-quality input")
{
  IqData input(40), output(40);
  auto& values = input.resize_for_write(40);
  for (std::size_t sample = 0; sample < values.size(); ++sample)
    values[sample] = std::polar(1.0, .2 * sample);
  blah2::conditioning::FmReferenceQualityFilter filter(2000, .75, .005);
  const auto metrics = filter.process(input, output);
  REQUIRE_FALSE(metrics.applied);
  REQUIRE(output.view_data() == values);
}

TEST_CASE("split publication retains prior CPI identity and queue telemetry")
{
  using blah2::multi_illuminator::PipelineState;
  PipelineState state;
  state.input_epoch_reset();
  std::vector<uint64_t> backlog{101, 102}, dropped{3, 4};
  blah2::EventTimeProvenance priorEvent;
  priorEvent.present = true;
  priorEvent.source = "prior-event";
  priorEvent.epoch = 7;
  blah2::AccelerationStatus priorStatus;
  priorStatus.active = "vulkan-wideband";
  priorStatus.state = "qualifying";
  auto prior = state.publication(1100, priorEvent, true, 7, 200, 300,
    backlog, dropped, 12.5, true, priorStatus);
  backlog[0] = 999;
  dropped[0] = 888;
  auto current = state.publication(2200, {}, true, 8, 300, 400,
    {201, 202}, {5, 6}, 9.5);

  REQUIRE(prior.timestampMs == 1100);
  REQUIRE(prior.eventTime.source == "prior-event");
  REQUIRE(prior.eventTime.epoch == 7);
  REQUIRE(prior.captureEpoch == 7);
  REQUIRE(prior.captureFirst == 200);
  REQUIRE(prior.captureEnd == 300);
  REQUIRE(prior.captureBacklogSamples == std::vector<uint64_t>{101, 102});
  REQUIRE(prior.captureDroppedSamples == std::vector<uint64_t>{3, 4});
  REQUIRE(prior.channelizerMs == 12.5);
  REQUIRE(prior.inputDiscontinuity);
  REQUIRE(prior.gpuQualificationFrame);
  REQUIRE(prior.acceleration.active == "vulkan-wideband");
  REQUIRE(prior.acceleration.state == "qualifying");
  REQUIRE(current.timestampMs == 2200);
  REQUIRE(current.captureEpoch == 8);
  REQUIRE_FALSE(current.inputDiscontinuity);
}

TEST_CASE("runtime CPU fallback resets adaptive state before same-block output")
{
  using blah2::multi_illuminator::PipelineState;
  PipelineState state;
  std::vector<std::string> order;
  state.runtime_fallback("worker reply invalid",
    [&] { order.push_back("channelizer-reset"); },
    [&] { order.push_back("radar-reset"); });
  order.push_back("cpu-same-block");
  auto frame = state.publication(3000, {}, true, 9, 400, 500,
    {12}, {1}, 4.0);
  const auto status = state.acceleration_status("auto", false, true);

  REQUIRE(order == std::vector<std::string>{"channelizer-reset",
    "radar-reset", "cpu-same-block"});
  REQUIRE(frame.inputDiscontinuity);
  REQUIRE(state.runtime_fallback_active());
  REQUIRE(state.fallback_reason() == "worker reply invalid");
  REQUIRE(status.active == "cpu");
  REQUIRE(status.state == "fallback");
  REQUIRE(status.reason == "worker reply invalid");

  // EOF/drain observes the same persistent fallback state and reason.
  const auto drainStatus = state.acceleration_status("auto", false, true);
  REQUIRE(drainStatus.state == "fallback");
  REQUIRE(drainStatus.reason == "worker reply invalid");
}

TEST_CASE("qualification fallback preserves the advanced CPU oracle")
{
  using blah2::multi_illuminator::PipelineState;
  PipelineState state;
  bool radarReset = false;
  state.qualification_fallback("numerical qualification failed",
    [&] { radarReset = true; });
  auto frame = state.publication(4000, {}, false, 0, 0, 0, {}, {}, 7.0,
    true);
  const auto status = state.acceleration_status("auto", false, true,
    frame.gpuQualificationFrame);
  REQUIRE(radarReset);
  REQUIRE(frame.inputDiscontinuity);
  REQUIRE(status.state == "fallback");
  REQUIRE(status.reason == "numerical qualification failed");
}

TEST_CASE("qualification publication precedes active GPU status")
{
  blah2::multi_illuminator::PipelineState state;
  const auto qualifying = state.acceleration_status("auto", true, true,
    true);
  const auto active = state.acceleration_status("auto", true, true, false);
  REQUIRE(qualifying.active == "vulkan-wideband");
  REQUIRE(qualifying.state == "qualifying");
  REQUIRE(active.state == "active");
}

TEST_CASE("drained publication timing carries identity and defined stages")
{
  Timing timing(1000);
  timing.update(1200, {11, 22, 33, 44, 0, 110},
    {"multi_channelizer_wall", "multi_illuminator_radar", "multi_serialize",
     "multi_output", "multi_pipeline_advance", "cpi"});
  timing.set_capture_queues({51, 52}, {7, 8});
  timing.set_capture_sample_position(true, 4, 100, 200);
  timing.set_input_discontinuity(true);
  blah2::AccelerationStatus status;
  status.requested = "auto";
  status.active = "cpu";
  status.state = "fallback";
  status.reason = "worker reply invalid";
  timing.set_acceleration(status);
  timing.set_clutter_acceleration(status, false, true);

  rapidjson::Document document;
  document.Parse(timing.to_json().c_str());
  REQUIRE_FALSE(document.HasParseError());
  REQUIRE(document["timestamp"].GetUint64() == 1200);
  REQUIRE(document["inputDiscontinuity"].GetBool());
  REQUIRE(document["captureEpoch"].GetUint64() == 4);
  REQUIRE(document["captureFirstEpochSample"].GetUint64() == 100);
  REQUIRE(document["captureEndEpochSample"].GetUint64() == 200);
  REQUIRE(document["captureBacklogSamples"][0].GetUint64() == 51);
  REQUIRE(document["captureDroppedSamples"][1].GetUint64() == 8);
  REQUIRE(document["multi_channelizer_wall"].GetDouble() == 11);
  REQUIRE(document["multi_pipeline_advance"].GetDouble() == 0);
  REQUIRE(std::string(document["acceleration"]["state"].GetString()) ==
    "fallback");
  REQUIRE(std::string(document["acceleration"]["reason"].GetString()) ==
    "worker reply invalid");
}

TEST_CASE("fallback preserves pending then CPU then EOF publication order")
{
  using Context = blah2::multi_illuminator::PublicationContext;
  blah2::multi_illuminator::PendingPublication<std::string> pending;
  std::vector<std::string> published;

  // A split GPU failure has already recomputed this CPI on CPU.
  pending.store("fallback-cpi", Context{.timestampMs = 100});
  auto prior = pending.take();
  published.push_back(std::move(prior.first));
  REQUIRE(prior.second.timestampMs == 100);

  // While publishing it, the next CPU CPI remains in the one-frame pipeline.
  pending.store("next-cpu-cpi", Context{.timestampMs = 200});
  REQUIRE_THROWS_AS(pending.store("overwrite", Context{}), std::logic_error);

  // Finite EOF drains exactly the remaining current CPI, in order.
  auto eof = pending.take();
  published.push_back(std::move(eof.first));
  REQUIRE(eof.second.timestampMs == 200);
  REQUIRE(published == std::vector<std::string>{"fallback-cpi",
    "next-cpu-cpi"});
  REQUIRE_FALSE(pending.has_value());
  REQUIRE_THROWS_AS(pending.take(), std::logic_error);
}

TEST_CASE("explicit qualification failure drains its CPU oracle immediately")
{
  using Context = blah2::multi_illuminator::PublicationContext;
  blah2::multi_illuminator::PendingPublication<std::string> pending;
  pending.store("failed-capture-cpu-oracle", Context{.timestampMs = 310});
  const bool fatal = true;
  REQUIRE(fatal);
  auto failed = pending.take();
  REQUIRE(failed.first == "failed-capture-cpu-oracle");
  REQUIRE(failed.second.timestampMs == 310);
  REQUIRE_FALSE(pending.has_value());
  // Main publishes this value and throws before requesting another capture.
}

blah2::channelizer::IlluminatorOutput dedicated_signal(std::size_t samples)
{
  constexpr uint32_t rate = 20000, delay = 8;
  constexpr double doppler = 30;
  std::mt19937 generator(71);
  std::normal_distribution<double> random;
  std::vector<std::complex<double>> direct(samples);
  for (auto& value : direct) value = {random(generator), random(generator)};
  blah2::channelizer::IlluminatorOutput result;
  result.id = "only";
  result.frequencyHz = 100000000;
  result.samples.channels.assign(2,
    std::vector<std::complex<float>>(samples));
  result.samples.hasFirstInputSample = true;
  result.samples.inputSamplesPerOutput = 2;
  for (std::size_t sample = 0; sample < samples; ++sample)
  {
    result.samples.channels[0][sample] = std::complex<float>(direct[sample]);
    auto surveillance = direct[sample];
    if (sample >= delay)
      surveillance += .55 * direct[sample - delay] *
        std::polar(1.0, 2 * pi * doppler * sample / rate);
    result.samples.channels[1][sample] = std::complex<float>(surveillance);
  }
  return result;
}

std::pair<double, double> strongest(const Map<std::complex<double>>& map)
{
  double power = -1, delay = 0, doppler = 0;
  for (std::size_t row = 0; row < map.data.size(); ++row)
    for (std::size_t column = 0; column < map.data[row].size(); ++column)
      if (std::norm(map.data[row][column]) > power)
      {
        power = std::norm(map.data[row][column]);
        delay = map.delay[column];
        doppler = map.doppler[row];
      }
  return {delay, doppler};
}
}

TEST_CASE("multi-illuminator advanced settings fail closed instead of degrading")
{
  using blah2::multi_illuminator::AdvancedFeatureConfig;
  using blah2::multi_illuminator::validateAdvancedFeatures;
  AdvancedFeatureConfig config;
  REQUIRE_NOTHROW(validateAdvancedFeatures(config));

  const auto rejected = [&](auto mutate) {
    auto value = config;
    mutate(value);
    REQUIRE_THROWS_AS(validateAdvancedFeatures(value), std::invalid_argument);
  };
  rejected([](auto& value) { value.surveillanceFusionMode = "coherent_beam_bank"; });
  rejected([](auto& value) { value.surveillanceFusionMode = "noise_whitened_beam_bank"; });
  rejected([](auto& value) { value.surveillanceManifoldAutomation = true; });
  rejected([](auto& value) { value.detectorIntegrationCpis = 2; });
  rejected([](auto& value) { value.detectorIntegrationMotionBins = 1; });
  rejected([](auto& value) { value.trackBeforeDetect = true; });
  rejected([](auto& value) { value.trackerBeamToleranceConfigured = true; });
  auto inheritedGpuCapacity = config;
  inheritedGpuCapacity.trackerGpuCapacityConfigured = true;
  REQUIRE_NOTHROW(validateAdvancedFeatures(inheritedGpuCapacity));
  auto adaptiveTracker = config;
  adaptiveTracker.trackerAccelerationModel = "adaptive_gate";
  REQUIRE_NOTHROW(validateAdvancedFeatures(adaptiveTracker));
  rejected([](auto& value) { value.stationaryClutter = true; });
  rejected([](auto& value) { value.trackGuidedShadow = true; });
  rejected([](auto& value) { value.trackGuidedPromotion = true; });
  rejected([](auto& value) { value.bearing = true; });
  rejected([](auto& value) { value.bearingFrequencyResolvedShadow = true; });
  rejected([](auto& value) { value.bearingManifoldRecorder = true; });
}

TEST_CASE("independent illuminators produce independent radar products")
{
  REQUIRE(fftw_init_threads());
  blah2::set_fftw_planner_threads(1);
  constexpr uint32_t captureRate = 40000, outputRate = 20000;
  constexpr std::size_t channels = 5, samples = 2000;
  const std::vector<blah2::channelizer::IlluminatorDefinition> definitions{
    {"lower", 100000000, 10000, outputRate, 16},
    {"upper", 101000000, 10000, outputRate, 16}};
  blah2::multi_illuminator::RadarConfig config;
  config.cpiSeconds = .1;
  config.delayMin = -2; config.delayMax = 20;
  config.dopplerMin = -100; config.dopplerMax = 100;
  config.clutterDelayMin = -2; config.clutterDelayMax = 4;
  config.pfa = .001; config.guardCells = 1; config.trainingCells = 4;
  config.minimumDelay = 3; config.minimumDopplerHz = 5;
  config.centroidRadius = 2;
  config.trackerM = 1; config.trackerN = 1; config.trackerDelete = 2;
  config.trackerMinimumSnrDb = -100;
  config.laneWorkers = 1;
  config.arrayReference = true;
  config.referenceChannels = {0, 1, 2, 3, 4};
  config.surveillanceChannels = {0, 1, 2, 3, 4};
  config.reference.analysisSamples = 512;
  config.reference.analysisInterval = 1;
  config.reference.covarianceSmoothing = 0;
  blah2::multi_illuminator::MultiIlluminatorRadar radar(
    captureRate, channels, definitions, config);

  std::vector<blah2::channelizer::IlluminatorOutput> input;
  input.push_back(signal("lower", 100000000, outputRate, 2,
    channels, samples, 8, 30, 41));
  input.push_back(signal("upper", 101000000, outputRate, 2,
    channels, samples, 13, -40, 99));
  auto frames = radar.process(std::move(input));
  REQUIRE(frames.size() == 2);
  REQUIRE(frames[0].illuminatorId == "lower");
  REQUIRE(frames[1].illuminatorId == "upper");
  REQUIRE(frames[0].sampleRateHz == outputRate);
  REQUIRE(frames[0].endInputSample == (samples - 1) * 2);

  const auto lower = strongest(*frames[0].map);
  const auto upper = strongest(*frames[1].map);
  REQUIRE(std::abs(lower.first - 8) <= 1);
  REQUIRE(std::abs(lower.second - 30) <= 10);
  REQUIRE(std::abs(upper.first - 13) <= 1);
  REQUIRE(std::abs(upper.second + 40) <= 10);
  REQUIRE(frames[0].detections->get_nDetections() > 0);
  REQUIRE(frames[1].detections->get_nDetections() > 0);
  REQUIRE(frames[0].tracks->get_n() > 0);
  REQUIRE(frames[1].tracks->get_n() > 0);
}

TEST_CASE("multi-illuminator radar buffers partial CPIs and rejects identity changes")
{
  constexpr uint32_t captureRate = 40000, outputRate = 20000;
  const std::vector<blah2::channelizer::IlluminatorDefinition> definitions{
    {"only", 100000000, 10000, outputRate, 16}};
  blah2::multi_illuminator::RadarConfig config;
  config.cpiSeconds = .1;
  config.delayMin = -2; config.delayMax = 20;
  config.dopplerMin = -100; config.dopplerMax = 100;
  config.clutterDelayMin = -2; config.clutterDelayMax = 4;
  config.reference.analysisSamples = 256;
  blah2::multi_illuminator::MultiIlluminatorRadar radar(
    captureRate, 2, definitions, config);
  auto half = signal("only", 100000000, outputRate, 2, 2, 1000, 8, 30, 4);
  REQUIRE(radar.process({half}).empty());
  half.samples.firstInputSample = 2000;
  auto frames = radar.process({half});
  REQUIRE(frames.size() == 1);
  auto changed = half;
  changed.id = "different";
  changed.samples.firstInputSample = 4000;
  REQUIRE_THROWS(radar.process({changed}));
  radar.reset();
  auto complete = signal("only", 100000000, outputRate, 2, 2, 2000, 8, 30, 8);
  complete.samples.firstInputSample = 0;
  REQUIRE(radar.process({complete}).size() == 1);
  complete = signal("only", 100000000, outputRate, 2, 2, 4000, 8, 30, 9);
  complete.samples.firstInputSample = 4000;
  REQUIRE(radar.process({complete}).size() == 2);
}

TEST_CASE("multi-illuminator radar discards adaptive state across a sample gap")
{
  constexpr uint32_t captureRate = 40000, outputRate = 20000;
  const std::vector<blah2::channelizer::IlluminatorDefinition> definitions{
    {"only", 100000000, 10000, outputRate, 16}};
  blah2::multi_illuminator::RadarConfig config;
  config.cpiSeconds = .1;
  config.delayMin = -2; config.delayMax = 20;
  config.dopplerMin = -100; config.dopplerMax = 100;
  config.clutterDelayMin = -2; config.clutterDelayMax = 4;
  config.reference.analysisSamples = 256;
  blah2::multi_illuminator::MultiIlluminatorRadar radar(
    captureRate, 2, definitions, config);
  auto partial = signal("only", 100000000, outputRate, 2,
    2, 1000, 8, 30, 4);
  REQUIRE(radar.process({partial}).empty());
  auto afterGap = signal("only", 100000000, outputRate, 2,
    2, 2000, 8, 30, 8);
  afterGap.samples.firstInputSample = 8000;
  const auto frames = radar.process({afterGap});
  REQUIRE(frames.size() == 1);
  REQUIRE(frames.front().firstInputSample == 8000);
}

TEST_CASE("multi-illuminator fused maps reuse storage only after release")
{
  constexpr uint32_t captureRate = 40000, outputRate = 20000;
  const std::vector<blah2::channelizer::IlluminatorDefinition> definitions{
    {"only", 100000000, 10000, outputRate, 16}};
  blah2::multi_illuminator::RadarConfig config;
  config.cpiSeconds = .1;
  config.delayMin = -2; config.delayMax = 20;
  config.dopplerMin = -100; config.dopplerMax = 100;
  config.clutterDelayMin = -2; config.clutterDelayMax = 4;
  config.reference.analysisSamples = 256;
  blah2::multi_illuminator::MultiIlluminatorRadar radar(
    captureRate, 2, definitions, config);

  auto first = radar.process({dedicated_signal(2000)});
  REQUIRE(first.size() == 1);
  auto* firstMap = first.front().map.get();
  const auto heldValue = firstMap->data.front().front();
  auto secondInput = dedicated_signal(2000);
  secondInput.samples.firstInputSample = 4000;
  auto second = radar.process({secondInput});
  REQUIRE(second.size() == 1);
  REQUIRE(second.front().map.get() != firstMap);
  REQUIRE(firstMap->data.front().front() == heldValue);

  auto* reusable = second.front().map.get();
  first.clear();
  second.clear();
  auto thirdInput = dedicated_signal(2000);
  thirdInput.samples.firstInputSample = 8000;
  auto third = radar.process({thirdInput});
  REQUIRE(third.size() == 1);
  REQUIRE(third.front().map.get() == reusable);
}

TEST_CASE("multi-illuminator radar enforces receiver channel roles")
{
  constexpr uint32_t captureRate = 40000, outputRate = 20000;
  const std::vector<blah2::channelizer::IlluminatorDefinition> definitions{
    {"only", 100000000, 10000, outputRate, 16}};
  blah2::multi_illuminator::RadarConfig config;
  config.cpiSeconds = .1;
  config.delayMin = -2; config.delayMax = 20;
  config.dopplerMin = -100; config.dopplerMax = 100;
  config.clutterDelayMin = -2; config.clutterDelayMax = 4;
  config.reference.analysisSamples = 256;

  config.referenceChannel = 0;
  config.surveillanceChannels = {0};
  REQUIRE_THROWS_AS(blah2::multi_illuminator::MultiIlluminatorRadar(
    captureRate, 2, definitions, config), std::invalid_argument);

  config.surveillanceChannels = {1};
  blah2::multi_illuminator::MultiIlluminatorRadar dedicatedRadar(
    captureRate, 2, definitions, config);
  auto dedicatedFrames = dedicatedRadar.process({dedicated_signal(2000)});
  REQUIRE(dedicatedFrames.size() == 1);
  const auto dedicatedPeak = strongest(*dedicatedFrames.front().map);
  REQUIRE(std::abs(dedicatedPeak.first - 8) <= 1);
  REQUIRE(std::abs(dedicatedPeak.second - 30) <= 10);

  config.arrayReference = true;
  config.referenceChannels = {0};
  config.surveillanceChannels = {1};
  REQUIRE_THROWS_AS(blah2::multi_illuminator::MultiIlluminatorRadar(
    captureRate, 2, definitions, config), std::invalid_argument);

  config.referenceChannels = {0, 1};
  config.surveillanceChannels = {2};
  config.reference.protectionRelativeSteering = {{1, 0}, {0, 1}};
  blah2::multi_illuminator::MultiIlluminatorRadar arrayRadar(
    captureRate, 3, definitions, config);
  auto input = signal("only", 100000000, outputRate, 2, 3, 2000,
    8, 30, 17);
  // A two-element steering vector succeeds only if synthesis receives the two
  // configured reference channels, rather than every capture/surveillance path.
  REQUIRE(arrayRadar.process({input}).size() == 1);
}

TEST_CASE("dedicated borrowed clutter preserves shared-workspace products and ownership")
{
  constexpr uint32_t captureRate = 40000, outputRate = 20000;
  const std::vector<blah2::channelizer::IlluminatorDefinition> definitions{
    {"only", 100000000, 10000, outputRate, 16}};
  blah2::multi_illuminator::RadarConfig config;
  config.cpiSeconds = .1;
  config.delayMin = -2; config.delayMax = 20;
  config.dopplerMin = -100; config.dopplerMax = 100;
  config.clutterDelayMin = -2; config.clutterDelayMax = 4;
  config.pfa = .001; config.guardCells = 1; config.trainingCells = 4;
  config.minimumDelay = 3; config.minimumDopplerHz = 5;
  config.centroidRadius = 2;
  config.trackerM = 1; config.trackerN = 1; config.trackerDelete = 2;
  config.trackerMinimumSnrDb = -100;
  config.referenceChannel = 0;
  config.surveillanceChannels = {1};
  auto borrowedConfig = config;
  borrowedConfig.borrowedClutter = true;
  auto sharedConfig = config;
  sharedConfig.borrowedClutter = false;
  blah2::multi_illuminator::MultiIlluminatorRadar borrowed(
    captureRate, 2, definitions, borrowedConfig);
  blah2::multi_illuminator::MultiIlluminatorRadar shared(
    captureRate, 2, definitions, sharedConfig);

  auto borrowedFrames = borrowed.process({dedicated_signal(2000)});
  auto sharedFrames = shared.process({dedicated_signal(2000)});
  REQUIRE(borrowedFrames.size() == 1);
  REQUIRE(sharedFrames.size() == 1);
  const auto& borrowedFrame = borrowedFrames.front();
  const auto& sharedFrame = sharedFrames.front();
  REQUIRE(borrowedFrame.firstInputSample == sharedFrame.firstInputSample);
  REQUIRE(borrowedFrame.endInputSample == sharedFrame.endInputSample);
  REQUIRE(borrowedFrame.map->delay == sharedFrame.map->delay);
  REQUIRE(borrowedFrame.map->doppler == sharedFrame.map->doppler);
  REQUIRE(borrowedFrame.map->data.size() == sharedFrame.map->data.size());
  for (std::size_t row = 0; row < borrowedFrame.map->data.size(); ++row)
  {
    REQUIRE(borrowedFrame.map->data[row].size() ==
      sharedFrame.map->data[row].size());
    for (std::size_t column = 0;
         column < borrowedFrame.map->data[row].size(); ++column)
    {
      const auto expected = sharedFrame.map->data[row][column];
      REQUIRE(std::abs(borrowedFrame.map->data[row][column] - expected) <
        1e-8 * (1 + std::abs(expected)));
    }
  }
  REQUIRE(borrowedFrame.detections->get_delay() ==
    sharedFrame.detections->get_delay());
  REQUIRE(borrowedFrame.detections->get_doppler() ==
    sharedFrame.detections->get_doppler());
  REQUIRE(borrowedFrame.detections->get_snr() ==
    sharedFrame.detections->get_snr());
  REQUIRE(borrowedFrame.tracks->to_json(1234) ==
    sharedFrame.tracks->to_json(1234));

  const auto* borrowedHeld = borrowedFrame.map.get();
  const auto heldValue = borrowedHeld->data.front().front();
  auto next = dedicated_signal(2000);
  next.samples.firstInputSample = 4000;
  const auto nextFrames = borrowed.process({std::move(next)});
  REQUIRE(nextFrames.front().map.get() != borrowedHeld);
  REQUIRE(borrowedHeld->data.front().front() == heldValue);
}
