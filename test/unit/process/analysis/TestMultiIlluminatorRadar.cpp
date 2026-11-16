#include "process/analysis/MultiIlluminatorRadar.h"
#include "process/analysis/RadarAlgorithmContract.h"
#include "process/analysis/MultiIlluminatorPipelineState.h"
#include "process/analysis/MultiIlluminatorStatus.h"
#include "process/analysis/StartupIq.h"
#include "process/conditioning/FmReferenceQualityFilter.h"
#include "process/detection/Centroid.h"
#include "process/utility/FftwThreads.h"
#include "data/meta/Timing.h"
#include "capture/ProcessingTopology.h"
#include "process/analysis/NativeProductIdentity.h"
#include "process/ambiguity/ResidentLaneBudget.h"
#include "process/ambiguity/GpuLaneLease.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_message.hpp>
#include <catch2/catch_approx.hpp>
#include <rapidjson/document.h>
#include <algorithm>
#include <cmath>
#include <complex>
#include <filesystem>
#include <limits>
#include <random>
#include <utility>
#include <vector>
#include <thread>

namespace
{
constexpr double pi = 3.141592653589793238462643383279502884;

TEST_CASE("one algorithm contract shares behavior without resolving lane geometry twice")
{
  blah2::radar_contract::Algorithm contract{
    .5, 2, 6, 5, 15, 1e-5, 6, 7, 1, 0, UINT32_MAX, false,
    3, 6, 10, 10, 50, .6, 5, true, false, false, false, false,
    true, 0, 0, 0};
  contract.integrationBeamTolerance = 1;
  contract.trackerGuidedSustain = true;
  contract.trackerGuidedShadow = true;
  contract.trackerGuidedMaximumTracks = 16;
  contract.trackerNormalizedInnovationGate = false;
  blah2::multi_illuminator::RadarConfig channelized;
  channelized.cpiSeconds = .625;
  channelized.delayReferenceRateHz = 2400000;
  channelized.guardCells = 4;
  channelized.trainingCells = 12;
  channelized.minimumDelay = 8;
  channelized.centroidRadius = 10;
  blah2::radar_contract::applyReceiverIndependent(contract, channelized);
  const auto detector = blah2::radar_contract::residentDetector(contract, true);
  const auto tracker = blah2::radar_contract::residentTracker(contract, true, 128);
  REQUIRE(channelized.pfa == detector.pfa);
  REQUIRE(channelized.minimumDopplerHz == detector.minDoppler);
  REQUIRE(*channelized.centroidDopplerRadius == detector.centroidDoppler);
  REQUIRE(channelized.trackerM == tracker.m);
  REQUIRE(channelized.trackerN == tracker.n);
  REQUIRE(channelized.trackerDelete == tracker.nDelete);
  REQUIRE(channelized.trackerMinimumSnrDb == tracker.minInitiationSnrDb);
  REQUIRE(channelized.advanced.trackerAccelerationModel == "adaptive_gate");
  REQUIRE(channelized.advanced.trackerBeamToleranceConfigured);
  REQUIRE(channelized.advanced.trackGuidedSustain);
  REQUIRE(channelized.trackerNormalizedInnovationGate ==
    tracker.normalizedInnovationGate);
  REQUIRE(channelized.cpiSeconds == .625);
  REQUIRE(channelized.guardCells == 4);
  REQUIRE(channelized.trainingCells == 12);
  REQUIRE(channelized.minimumDelay == 8);
  REQUIRE(channelized.centroidRadius == 10);
  const auto lane = blah2::multi_illuminator::scaleRadarConfigForLane(
    channelized, 300000);
  REQUIRE(lane.guardCells == 1);
  REQUIRE(lane.trainingCells == 2);
  REQUIRE(lane.minimumDelay == 1);
  REQUIRE(lane.centroidRadius == 2);
}

TEST_CASE("unified algorithm contract preserves a configured zero centroid radius")
{
  blah2::radar_contract::Algorithm contract{
    .5, 2, 6, 5, 15, 1e-5, 0, 0, 1, 0, UINT32_MAX, false,
    3, 6, 10, 10, 50, .6, 5, true, false, false, false, false,
    true, 0, 0, 0};
  const auto detector = blah2::radar_contract::residentDetector(contract, true);
  REQUIRE(detector.centroidDelay == 0);
  REQUIRE(detector.centroidDoppler == 0);
  blah2::multi_illuminator::RadarConfig channelized;
  blah2::radar_contract::applyReceiverIndependent(contract, channelized);
  REQUIRE(*channelized.centroidDopplerRadius == 0);
}

TEST_CASE("lane delay geometry preserves physical units across sample rates")
{
  blah2::multi_illuminator::RadarConfig config;
  config.delayReferenceRateHz = 2400000;
  config.delayMin = -10;
  config.delayMax = 400;
  config.clutterDelayMin = -10;
  config.clutterDelayMax = 401;
  config.clutterFollowsAmbiguity = true;
  config.minimumDelay = 5;
  config.guardCells = 2;
  config.trainingCells = 6;
  config.centroidRadius = 6;

  const auto fm = blah2::multi_illuminator::scaleRadarConfigForLane(
    config, 300000);
  REQUIRE(fm.delayMin == -2);
  REQUIRE(fm.delayMax == 50);
  REQUIRE(fm.clutterDelayMin == -2);
  REQUIRE(fm.clutterDelayMax == 51);
  REQUIRE(fm.minimumDelay == 1);
  REQUIRE(fm.guardCells == 1);
  REQUIRE(fm.trainingCells == 1);
  REQUIRE(fm.centroidRadius == 1);
  REQUIRE(fm.centroidDopplerRadius == 6);

  const auto atsc = blah2::multi_illuminator::scaleRadarConfigForLane(
    config, 6144000);
  REQUIRE(atsc.delayMin == -26);
  REQUIRE(atsc.delayMax == 1024);
  REQUIRE(atsc.clutterDelayMin == -26);
  REQUIRE(atsc.clutterDelayMax == 1025);
  REQUIRE(atsc.minimumDelay == 13);
  REQUIRE(atsc.guardCells == 6);
  REQUIRE(atsc.trainingCells == 16);
  REQUIRE(atsc.centroidRadius == 16);
  REQUIRE(atsc.centroidDopplerRadius == 6);

  auto longRange = config;
  longRange.delayReferenceRateHz = 2000000;
  longRange.minimumDelay = 80;
  const auto projectedThreshold =
    blah2::multi_illuminator::scaleRadarConfigForLane(longRange, 6144000);
  REQUIRE(projectedThreshold.minimumDelay == 246);
}

TEST_CASE("centroid Doppler support is invariant to delay-rate projection")
{
  blah2::multi_illuminator::RadarConfig config;
  config.delayReferenceRateHz = 2400000;
  config.cpiSeconds = .5;
  for (const uint32_t rate : {300000u, 2400000u, 6144000u}) {
    const auto lane = blah2::multi_illuminator::scaleRadarConfigForLane(config, rate);
    REQUIRE(lane.centroidDopplerRadius == 6);
    REQUIRE(double(lane.centroidRadius) / rate >= 6.0 / 2400000);
    REQUIRE(double(lane.centroidRadius) / rate - 6.0 / 2400000 < 1.0 / rate);
    Centroid centroid(lane.centroidRadius, *lane.centroidDopplerRadius,
      1.0 / lane.cpiSeconds);
    Detection detections({100, 100, 100}, {0, 10, 13}, {20, 10, 10});
    const auto result = centroid.process(&detections);
    REQUIRE(result->get_doppler() == std::vector<double>{0, 13});
  }
  config.centroidDopplerRadius = 3;
  REQUIRE(blah2::multi_illuminator::scaleRadarConfigForLane(config, 300000)
    .centroidDopplerRadius == 3);
}

TEST_CASE("nondivisible CAF uses actual Doppler spacing without changing input cadence")
{
  constexpr uint32_t samples = 2003, rate = 20000;
  Ambiguity ambiguity(-2, 20, -100, 100, rate, samples, true);
  const double effective = double(ambiguity.get_n_corr()) *
    ambiguity.get_n_doppler_bins() / rate;
  REQUIRE(ambiguity.get_cpi() == effective);
  REQUIRE(effective < double(samples) / rate);
  const double actualSpacing = ambiguity.result()->doppler[1] - ambiguity.result()->doppler[0];
  REQUIRE(std::abs(actualSpacing - 1.0 / effective) < 1e-10);
  // Just inside the actual one-bin interval, but outside the nominal interval.
  const double candidate = (actualSpacing + double(rate) / samples) / 2;
  Detection detections({10, 10}, {0, candidate}, {20, 10});
  Centroid correct(1, 1, 1.0 / effective);
  Centroid nominal(1, 1, double(rate) / samples);
  REQUIRE(correct.process(&detections)->get_nDetections() == 1);
  REQUIRE(nominal.process(&detections)->get_nDetections() == 2);
}

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

TEST_CASE("ATSC pilot-aided lanes remain admitted through startup preparation")
{
  blah2::channelizer::IlluminatorDefinition definition;
  definition.id = "atsc";
  definition.frequencyHz = 518000000;
  definition.occupiedBandwidthHz = 6000000;
  definition.outputRateHz = 6144000;
  definition.standard = "ATSC-1.0";
  definition.atscReferenceMode = "pilot_aided";
  blah2::multi_illuminator::RadarConfig config;
  config.cpiSeconds = .005;
  config.delayMin = -2;
  config.delayMax = 8;
  config.clutterDelayMin = -2;
  config.clutterDelayMax = 9;
  config.dopplerMin = -100;
  config.dopplerMax = 100;
  config.accelerationMode = "cpu";
  blah2::multi_illuminator::MultiIlluminatorRadar radar(
    6144000, 2, {definition}, config);
  REQUIRE_NOTHROW(radar.prepareStartup());
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
    backlog, dropped, 12.5, true, true, true, priorStatus, true);
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
  REQUIRE(prior.widebandGpuExecuted);
  REQUIRE(prior.widebandCpuExecuted);
  REQUIRE(prior.acceleration.active == "vulkan-wideband");
  REQUIRE(prior.acceleration.state == "qualifying");
  REQUIRE(prior.publishFullMap);
  REQUIRE(current.timestampMs == 2200);
  REQUIRE(current.captureEpoch == 8);
  REQUIRE_FALSE(current.inputDiscontinuity);
  REQUIRE_FALSE(current.publishFullMap);
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
  timing.observe_capture_peak({60, 70});
  timing.set_capture_storage_contract(8, 80);
  timing.set_capture_discard_accounting({1, 2}, {3, 4});
  timing.set_capture_terminal(true);
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
  REQUIRE(document["capturePeakBacklogSamples"][0].GetUint64() == 60);
  REQUIRE(document["capturePeakBytes"].GetUint64() == 1040);
  REQUIRE(document["captureBoundaryDiscardedSamples"][1].GetUint64() == 2);
  REQUIRE(document["captureStopDiscardedSamples"][0].GetUint64() == 3);
  REQUIRE(document["captureStorageBytesPerSample"].GetUint() == 8);
  REQUIRE(document["captureConfiguredSamplesPerChannel"].GetUint64() == 80);
  REQUIRE(document["captureTerminal"].GetBool());
  REQUIRE(document["multi_channelizer_wall"].GetDouble() == 11);
  REQUIRE(document["multi_pipeline_advance"].GetDouble() == 0);
  REQUIRE(std::string(document["acceleration"]["state"].GetString()) ==
    "fallback");
  REQUIRE(std::string(document["acceleration"]["reason"].GetString()) ==
    "worker reply invalid");
}

TEST_CASE("terminal capture accounting is defined before the first CPI")
{
  Timing timing(5000);
  timing.set_capture_terminal(true);
  timing.set_capture_queues({3, 3}, {1, 1});
  rapidjson::Document document;
  document.Parse(timing.to_json().c_str());
  REQUIRE_FALSE(document.HasParseError());
  REQUIRE(document["timestamp"].GetUint64() == 5000);
  REQUIRE(document["nCpi"].GetUint64() == 0);
  REQUIRE(document["uptime_s"].GetDouble() == 0.0);
  REQUIRE(document["captureTerminal"].GetBool());
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
  REQUIRE(config.trackerAccelerationModel == "adaptive_gate");
  REQUIRE_NOTHROW(validateAdvancedFeatures(config));

  const auto rejected = [&](auto mutate) {
    auto value = config;
    mutate(value);
    REQUIRE_THROWS_AS(validateAdvancedFeatures(value), std::invalid_argument);
  };
  for (const auto* mode : {"coherent_beam_bank", "noise_whitened_beam_bank"}) {
    auto coherent = config; coherent.surveillanceFusionMode = mode;
    REQUIRE_NOTHROW(validateAdvancedFeatures(coherent));
  }
  rejected([](auto& value) { value.surveillanceManifoldAutomation = true; });
  for (const uint32_t motion : {0u, 1u}) {
    auto integrated = config;
    integrated.detectorIntegrationCpis = 2;
    integrated.detectorIntegrationMotionBins = motion;
    REQUIRE_NOTHROW(validateAdvancedFeatures(integrated));
  }
  rejected([](auto& value) { value.detectorIntegrationCpis = 0; });
  rejected([](auto& value) { value.detectorIntegrationCpis = 4; });
  rejected([](auto& value) { value.detectorIntegrationMotionBins = 1; });
  rejected([](auto& value) {
    value.detectorIntegrationCpis = 2; value.detectorIntegrationMotionBins = 2;
  });
  rejected([](auto& value) { value.trackBeforeDetect = true; });
  for (const uint32_t depth : {4u, 8u}) {
    auto tbd = config;
    tbd.trackBeforeDetect = true;
    tbd.detectorIntegrationCpis = depth;
    tbd.detectorIntegrationMotionBins = track_before_detect::encodeMotionBins(1, 4, true);
    tbd.stationaryClutter = true;
    tbd.trackGuidedShadow = true;
    REQUIRE_NOTHROW(validateAdvancedFeatures(tbd));
  }
  auto beam = config; beam.trackerBeamToleranceConfigured = true;
  REQUIRE_NOTHROW(validateAdvancedFeatures(beam));
  auto inheritedGpuCapacity = config;
  inheritedGpuCapacity.trackerGpuCapacityConfigured = true;
  REQUIRE_NOTHROW(validateAdvancedFeatures(inheritedGpuCapacity));
  auto adaptiveTracker = config;
  adaptiveTracker.trackerAccelerationModel = "adaptive_gate";
  REQUIRE_NOTHROW(validateAdvancedFeatures(adaptiveTracker));
  auto replayShadow = config;
  replayShadow.temporalShadow = true;
  replayShadow.temporalShadowIlluminatorId = "lane";
  replayShadow.temporalShadowArtifactPath = "temporal-shadow-test.vwtbd";
  REQUIRE_THROWS_AS(validateAdvancedFeatures(replayShadow), std::invalid_argument);
  replayShadow.temporalShadowOfflineReplay = true;
  REQUIRE_NOTHROW(validateAdvancedFeatures(replayShadow));
  replayShadow.temporalShadowBootstrapCovariance = "robust_shrinkage";
  REQUIRE_NOTHROW(validateAdvancedFeatures(replayShadow));
  replayShadow.temporalShadowBootstrapCovariance = "automatic";
  REQUIRE_THROWS_AS(validateAdvancedFeatures(replayShadow), std::invalid_argument);
  replayShadow.temporalShadowBootstrapCovariance = "diagonal";
  replayShadow.temporalShadowCoherentPathEvidence = true;
  replayShadow.temporalShadowNullMinimumSamples = 4;
  replayShadow.temporalShadowNullAlpha = .2;
  REQUIRE_THROWS_AS(validateAdvancedFeatures(replayShadow), std::invalid_argument);
  replayShadow.temporalShadowInputStatistic = "post_clutter_bootstrap";
  REQUIRE_THROWS_AS(validateAdvancedFeatures(replayShadow), std::invalid_argument);
  replayShadow.temporalShadowCoherentPathEvidence = false;
  replayShadow.temporalShadowInputStatistic = "raw_power";
  REQUIRE_THROWS_AS(validateAdvancedFeatures(replayShadow), std::invalid_argument);
  rejected([](auto& value) { value.temporalShadowCoherentPathEvidence = true; });
  auto stationary = config;
  stationary.stationaryClutter = true;
  REQUIRE_NOTHROW(validateAdvancedFeatures(stationary));
  rejected([](auto& value) { value.trackGuidedShadow = true; });
  rejected([](auto& value) { value.trackGuidedPromotion = true; });
  auto bearing = config; bearing.bearing = true;
  REQUIRE_NOTHROW(validateAdvancedFeatures(bearing));
  rejected([](auto& value) { value.bearingFrequencyResolvedShadow = true; });
  rejected([](auto& value) { value.bearingManifoldRecorder = true; });
}

TEST_CASE("coherent lanes bind explicit transmitter geometry without receiver gates")
{
  using namespace blah2::multi_illuminator;
  REQUIRE(fftw_init_threads());
  blah2::set_fftw_planner_threads(1);
  std::vector<blah2::channelizer::IlluminatorDefinition> lanes{
    {"east", 100000000, 10000, 20000, 16},
    {"north", 101000000, 10000, 20000, 16}};
  RadarConfig config;
  config.cpiSeconds = .1;
  config.delayMin = -2; config.delayMax = 20;
  config.dopplerMin = -100; config.dopplerMax = 100;
  config.clutterDelayMin = -2; config.clutterDelayMax = 4;
  config.arrayReference = true;
  config.referenceChannels = config.surveillanceChannels = {0, 1, 2};
  config.reference.analysisSamples = 512;
  config.reference.analysisInterval = 1;
  config.reference.covarianceSmoothing = 0;
  config.laneWorkers = 1;
  config.advanced.surveillanceFusionMode = "coherent_beam_bank";
  config.coherentFusion = CoherentBeamBank::Config{100000000, 12,
    {{{.2, 0, 0}}, {{-.1, .1732050808, 0}}, {{-.1, -.1732050808, 0}}}, 0, 0, {}, {}};
  config.manifoldIdentity.receiverLocation = {40, -75, 100};
  config.manifoldIdentity.xAxisBearingDeg = 15;
  REQUIRE_THROWS_AS(MultiIlluminatorRadar(40000, 3, lanes, config), std::invalid_argument);
  lanes[0].transmitterLocation = std::array<double, 3>{40, -74.9, 150};
  lanes[1].transmitterLocation = std::array<double, 3>{40.1, -75, 180};
  MultiIlluminatorRadar radar(40000, 3, lanes, config);
  auto frames = radar.process({signal("east", 100000000, 20000, 2, 3, 2000, 8, 30, 41),
    signal("north", 101000000, 20000, 2, 3, 2000, 8, 30, 41)});
  REQUIRE(frames.size() == 2);
  REQUIRE(frames[0].transmitterLocation == lanes[0].transmitterLocation);
  REQUIRE(frames[1].transmitterLocation == lanes[1].transmitterLocation);
  REQUIRE(std::abs(*frames[0].directPathAzimuthRad - *frames[1].directPathAzimuthRad) > 1);
  REQUIRE_FALSE(frames[0].residentProducts);
  config.advanced.surveillanceFusionMode = "noise_whitened_beam_bank";
  REQUIRE_THROWS_AS(MultiIlluminatorRadar(40000, 3, lanes, config), std::invalid_argument);
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
  for (const auto& frame : frames) {
    const auto& diagnostics = frame.trackerDiagnostics;
    REQUIRE_FALSE(diagnostics.noOp);
    REQUIRE(diagnostics.detections == frame.detections->get_nDetections());
    REQUIRE(diagnostics.tracksBefore == 0);
    REQUIRE(diagnostics.tracksAfter == frame.tracks->get_n());
    REQUIRE(diagnostics.initiated == frame.tracks->get_n());
    REQUIRE(diagnostics.birthCandidates == diagnostics.detections);
  }
}

TEST_CASE("startup preparation leaves first real lane products and epochs unchanged")
{
  REQUIRE(fftw_init_threads());
  blah2::set_fftw_planner_threads(1);
  const std::vector<blah2::channelizer::IlluminatorDefinition> definitions{
    {"lower", 99000000, 10000, 20000, 16},
    {"middle", 100000000, 10000, 20000, 16},
    {"upper", 101000000, 10000, 20000, 16}};
  for (bool arrayReference : {false, true}) {
    blah2::multi_illuminator::RadarConfig config;
    config.cpiSeconds = .1;
    config.delayMin = -2; config.delayMax = 20;
    config.dopplerMin = -100; config.dopplerMax = 100;
    config.clutterDelayMin = -2; config.clutterDelayMax = 4;
    config.guardCells = 1; config.trainingCells = 4;
    config.centroidRadius = 2;
    config.laneWorkers = 1;
    config.arrayReference = arrayReference;
    config.referenceChannel = 0;
    config.referenceChannels = {0, 1, 2};
    config.surveillanceChannels = arrayReference ?
      std::vector<uint32_t>{0, 1, 2} : std::vector<uint32_t>{1};
    config.reference.analysisSamples = 256;
    config.reference.covarianceSmoothing = .8;
    blah2::multi_illuminator::MultiIlluminatorRadar clean(40000, 3, definitions, config);
    blah2::multi_illuminator::MultiIlluminatorRadar warmed(40000, 3, definitions, config);
    warmed.prepareStartup();
    warmed.prepareStartup(); // No input/state is admitted by a repeated call.
    std::vector<blah2::channelizer::IlluminatorOutput> input;
    for (std::size_t lane = 0; lane < definitions.size(); ++lane)
      input.push_back(signal(definitions[lane].id, definitions[lane].frequencyHz,
        20000, 2, 3, 2000, 8, 30, 41 + lane));
    const auto expected = clean.process(input);
    const auto actual = warmed.process(std::move(input));
    REQUIRE(actual.size() == expected.size());
    for (std::size_t lane = 0; lane < actual.size(); ++lane) {
      REQUIRE(actual[lane].map->data == expected[lane].map->data);
      REQUIRE(actual[lane].firstInputSample == expected[lane].firstInputSample);
      REQUIRE(actual[lane].endInputSample == expected[lane].endInputSample);
      REQUIRE(actual[lane].detections->to_json(1234) == expected[lane].detections->to_json(1234));
      REQUIRE(actual[lane].tracks->to_json(1234) == expected[lane].tracks->to_json(1234));
      REQUIRE_FALSE(actual[lane].temporalShadowActive);
    }
    REQUIRE_THROWS_AS(warmed.prepareStartup(), std::logic_error);
  }
}

TEST_CASE("energy temporal shadow preserves radar products without phase or null claims")
{
  REQUIRE(fftw_init_threads());
  blah2::set_fftw_planner_threads(1);
  constexpr uint32_t captureRate = 40000, outputRate = 20000;
  const std::vector<blah2::channelizer::IlluminatorDefinition> definitions{
    {"lane", 100000000, 10000, outputRate, 16}};
  blah2::multi_illuminator::RadarConfig baseline;
  baseline.cpiSeconds = .1;
  baseline.delayMin = -2; baseline.delayMax = 20;
  baseline.dopplerMin = -100; baseline.dopplerMax = 100;
  baseline.clutterDelayMin = -2; baseline.clutterDelayMax = 4;
  baseline.pfa = .001; baseline.guardCells = 1; baseline.trainingCells = 4;
  baseline.minimumDelay = 3; baseline.minimumDopplerHz = 5;
  baseline.centroidRadius = 2; baseline.laneWorkers = 1;
  baseline.reference.analysisSamples = 256;
  baseline.trackerM = 1; baseline.trackerN = 1; baseline.trackerDelete = 2;
  baseline.trackerMinimumSnrDb = -100;
  auto shadowConfig = baseline;
  const auto artifact = std::filesystem::temp_directory_path() /
    ("vectorwarp-coherent-shadow-" + std::to_string(std::random_device{}()) + ".vwtbd");
  std::filesystem::remove(artifact);
  shadowConfig.advanced.temporalShadow = true;
  shadowConfig.advanced.temporalShadowOfflineReplay = true;
  shadowConfig.advanced.temporalShadowIlluminatorId = "lane";
  shadowConfig.advanced.temporalShadowArtifactPath = artifact.string();
  {
    blah2::multi_illuminator::MultiIlluminatorRadar control(
      captureRate, 2, definitions, baseline);
    blah2::multi_illuminator::MultiIlluminatorRadar shadow(
      captureRate, 2, definitions, shadowConfig);
    for (uint64_t cpi = 0; cpi < 6; ++cpi)
    {
      auto input = signal("lane", 100000000, outputRate, 2, 2, 2000,
        8, 30, 71);
      input.samples.firstInputSample = cpi * 4000;
      const auto expected = control.process({input});
      const auto actual = shadow.process({input});
      REQUIRE(expected.size() == 1);
      REQUIRE(actual.size() == 1);
      REQUIRE(actual.front().map->data == expected.front().map->data);
      REQUIRE(actual.front().detections->to_json(1234) ==
        expected.front().detections->to_json(1234));
      REQUIRE(actual.front().tracks->to_json(1234) ==
        expected.front().tracks->to_json(1234));
      REQUIRE(actual.front().temporalShadowActive);
      REQUIRE_FALSE(actual.front().temporalShadowCoherentActive);
      REQUIRE_FALSE(actual.front().temporalShadowNullCalibrationFrozen);
      if (cpi == 5)
      {
        REQUIRE(actual.front().temporalShadowNullTestedCells == 0);
        REQUIRE(actual.front().temporalShadowNullSignificantCells == 0);
        REQUIRE(actual.front().temporalShadowNullUnresolvedCells == 0);
        REQUIRE(actual.front().temporalShadowIntegratedCells > 0);
      }
    }
  }
  std::filesystem::remove(artifact);
}

namespace {
blah2::multi_illuminator::RadarConfig smallLaneConfig() {
  blah2::multi_illuminator::RadarConfig config;
  config.cpiSeconds = .1;
  config.delayMin = -2; config.delayMax = 20;
  config.dopplerMin = -100; config.dopplerMax = 100;
  config.clutterDelayMin = -2; config.clutterDelayMax = 4;
  config.pfa = .001; config.guardCells = 1; config.trainingCells = 4;
  config.minimumDelay = 3; config.minimumDopplerHz = 5;
  config.centroidRadius = 2; config.laneWorkers = 1;
  config.reference.analysisSamples = 256;
  config.trackerM = 1; config.trackerN = 1; config.trackerDelete = 2;
  return config;
}
}

TEST_CASE("canonical capture topology resolves recorded roles independently of receiver")
{
  using namespace blah2::capture_topology;
  for (const auto* type : {"Kraken", "Usrp", "RspDuo", "HackRF"}) {
    REQUIRE(adapter(type).coherent == (std::string(type) != "HackRF"));
    Request request;
    request.channelCount = 5;
    request.referenceChannels = std::vector<uint32_t>{3};
    auto dedicated = resolve(request);
    REQUIRE(dedicated.referenceChannel == 3);
    REQUIRE(dedicated.surveillanceChannels == std::vector<uint32_t>{0, 1, 2, 4});
    request.referenceChannel = 0;
    REQUIRE_THROWS_AS(resolve(request), std::invalid_argument);
    request.referenceMode = "array_eigenbeam";
    request.referenceChannels = std::vector<uint32_t>{4, 1, 3, 0, 2};
    request.coherent = true; // recording's declared topology, not live adapter
    const auto array = resolve(request);
    REQUIRE(array.referenceChannels == std::vector<uint32_t>{0, 1, 2, 3, 4});
    REQUIRE(array.surveillanceChannels == array.referenceChannels);
    request.coherent = adapter(type).coherent;
    if (std::string(type) == "HackRF") REQUIRE_THROWS_AS(resolve(request), std::invalid_argument);
    else REQUIRE_NOTHROW(resolve(request));
    request.coherent = true;
    request.referenceChannels = std::vector<uint32_t>{0, 0};
    REQUIRE_THROWS_AS(resolve(request), std::invalid_argument);
  }
}

TEST_CASE("shared production integration binds contiguous capture windows and resets tracker")
{
  using namespace blah2::detection;
  REQUIRE(signalTimeMilliseconds(1001, 1000) == 1001);
  REQUIRE(signalTimeMilliseconds((uint64_t(1) << 53) + 1, 1000) == (uint64_t(1) << 53) + 1);
  REQUIRE_THROWS_AS(signalTimeMilliseconds(1, 0), std::invalid_argument);
  REQUIRE_THROWS_AS(signalTimeMilliseconds(UINT64_MAX, 1), std::invalid_argument);
  DetectorIntegration integration(0);
  const auto makeMap = [] {
    auto value = std::make_unique<Map<std::complex<double>>>(5, 5);
    value->delay = {0, 4}; value->doppler = {-2, 2};
    for (auto& row : value->data) for (auto& cell : row) cell = {2, 0};
    return value;
  };
  auto map = makeMap();
  auto first = integration.process(map, 3, 100, 200, true);
  REQUIRE_FALSE(first.ready);
  map = makeMap();
  auto second = integration.process(map, 3, 200, 300, true);
  REQUIRE(second.ready);
  REQUIRE(second.firstInputSample == 100);
  REQUIRE(second.endInputSample == 300);
  REQUIRE(std::norm(map->data[2][2]) == Catch::Approx(4));
  for (const bool positioned : {true, false}) {
    map = makeMap();
    auto gap = integration.process(map, 3, 400, 500, positioned);
    REQUIRE_FALSE(gap.ready);
    REQUIRE(gap.historyReset);
    Tracker tracker(1, 1, 2, .1, 0, 1, 1, -100, true);
    Detection candidate({10}, {20}, {30});
    REQUIRE(tracker.process(&candidate, 1000)->get_n() > 0);
    Detection empty({}, {}, {});
    REQUIRE(trackProducts(empty, tracker, 1100, gap)->get_n() == 0);
    integration.reset();
    map = makeMap();
    REQUIRE_FALSE(integration.process(map, 3, 100, 200, true).ready);
  }
}

TEST_CASE("stage execution receipts are exact frame bound and never carried into another frame")
{
  Timing timing(0);
  timing.update(100, {}, {});
  const uint64_t first = (uint64_t(1) << 53) + 1;
  timing.set_capture_sample_position(true, 8, first, first + 100);
  blah2::AccelerationStatus status;
  status.active = "vulkan"; status.device = "test-device";
  timing.set_acceleration(status, true, false);
  timing.set_clutter_acceleration(status, true, true);
  timing.set_wideband_acceleration(status, false, true);
  rapidjson::Document json;
  json.Parse(timing.to_json().c_str());
  for (const auto* key : {"acceleration", "clutterAcceleration", "widebandAcceleration"}) {
    const auto& receipt = json[key]["execution"];
    REQUIRE(receipt["known"].GetBool());
    REQUIRE(receipt["frameBound"].GetBool());
    REQUIRE(std::string(receipt["firstInputSampleExact"].GetString()) == std::to_string(first));
  }
  timing.update(200, {}, {});
  json.Parse(timing.to_json().c_str());
  REQUIRE_FALSE(json["acceleration"]["execution"]["known"].GetBool());
  REQUIRE_FALSE(json["clutterAcceleration"]["execution"]["gpuExecuted"].GetBool());
  timing.set_wideband_acceleration(status, true, false);
  timing.set_capture_terminal(true);
  json.Parse(timing.to_json().c_str());
  REQUIRE_FALSE(json["widebandAcceleration"]["execution"]["known"].GetBool());
  blah2::analysis::NativeProductIdentity identity;
  identity.captureSamplePositionAvailable = true; identity.captureEpoch = 8;
  identity.firstInputSample = first; identity.endInputSample = first + 98;
  identity.inputSamplesPerOutput = 2;
  json.Parse(blah2::analysis::annotate_native_product("{}", identity).c_str());
  REQUIRE(json["end_input_sample"].GetUint64() == first + 98);
  REQUIRE(std::string(json["input_lineage"]["endInputSampleExact"].GetString()) == std::to_string(first + 100));
}

TEST_CASE("resident lane reservations cap all sessions before worker allocation")
{
  blah2::resident::LaneBudget budget(3, 2, 128ULL << 20);
  REQUIRE(budget.sessions == 2);
  REQUIRE(budget.bytesPerSession == (64ULL << 20));
  REQUIRE(budget.allocationLimit(0, 8ULL << 20) == (56ULL << 20));
  REQUIRE(budget.allocationLimit(1, 8ULL << 20) * 2 + (16ULL << 20) == budget.totalBytes);
  REQUIRE_THROWS_AS(budget.allocationLimit(2, 1), std::runtime_error);
  REQUIRE_THROWS_AS(budget.allocationLimit(0, 64ULL << 20), std::runtime_error);
  REQUIRE_THROWS_AS(blah2::resident::LaneBudget(0), std::invalid_argument);
}

TEST_CASE("portable lane lease holds only device calls and preserves optional interfaces")
{
  using namespace blah2;
  auto budget = std::make_shared<resident::LaneBudget>(1);
  struct Backend final : GpuBackend, GpuFrameBackend, GpuClutterFrameBackend {
    std::shared_ptr<resident::LaneBudget> budget;
    unsigned calls = 0;
    explicit Backend(std::shared_ptr<resident::LaneBudget> value) : budget(std::move(value)) {}
    void deviceCall() {
      bool acquired = false;
      std::thread observer([&] {
        acquired = budget->execution.try_lock();
        if (acquired) budget->execution.unlock();
      });
      observer.join();
      REQUIRE_FALSE(acquired); ++calls;
    }
    GpuDevice device() const override { return {"fake:0", "fake", 0}; }
    void process(const std::vector<std::complex<float>>&, const std::vector<std::complex<float>>&,
        std::vector<std::complex<float>>&) override { deviceCall(); }
    GpuFrameBuffers frameBuffers() override { return {}; }
    void processFrame() override { deviceCall(); }
    bool clutterAvailable() const override { return true; }
    GpuClutterBuffers clutterBuffers() override { return {}; }
    bool processClutterFrame() override { deviceCall(); return true; }
  };
  auto owned = std::make_unique<Backend>(budget); auto* observed = owned.get();
  auto wrapped = leaseGpuLane(std::move(owned), budget);
  std::vector<std::complex<float>> values;
  wrapped->process(values, values, values);
  dynamic_cast<GpuFrameBackend&>(*wrapped).processFrame();
  REQUIRE(dynamic_cast<GpuClutterFrameBackend&>(*wrapped).processClutterFrame());
  REQUIRE(observed->calls == 3);
  // Host work and CPU fallback can acquire the lease immediately afterwards.
  REQUIRE(budget->execution.try_lock()); budget->execution.unlock();
}

TEST_CASE("host temporal banks keep FP64 fractional paths exact and accept source epoch zero")
{
  constexpr uint32_t rows = 9, columns = 17;
  const auto makeMap = [=](uint32_t sequence) {
    auto map = std::make_unique<Map<std::complex<double>>>(rows, columns);
    for (uint32_t column = 0; column < columns; ++column) map->delay.push_back(column);
    for (uint32_t row = 0; row < rows; ++row) {
      map->doppler.push_back(double(row) - 4);
      for (uint32_t column = 0; column < columns; ++column)
        map->data[row][column] = {std::sqrt(1.0 + sequence * .125 + row * .01 + column * .001), 0};
    }
    return map;
  };
  const auto motion = track_before_detect::encodeMotionBins(1, 1, true);
  blah2::detection::DetectorIntegration integrated(4, motion, -.25);
  std::vector<std::vector<double>> history;
  blah2::detection::IntegrationMetrics metrics;
  for (uint32_t sequence = 1; sequence <= 4; ++sequence) {
    auto map = makeMap(sequence);
    std::vector<double> power;
    for (const auto& row : map->data) for (const auto cell : row) power.push_back(std::norm(cell));
    history.insert(history.begin(), std::move(power));
    metrics = integrated.process(map, 0, (sequence - 1) * 100, sequence * 100, true);
    REQUIRE(metrics.captureEpoch == 0);
    REQUIRE(metrics.ready == (sequence >= 2));
  }
  REQUIRE(metrics.evidence->banks[2].ready);
  REQUIRE_FALSE(metrics.evidence->banks[3].ready);
  const auto& bank = *metrics.evidence->banks[2].map;
  const std::vector<double> doppler(bank.doppler.begin(), bank.doppler.end());
  for (uint32_t row = 0; row < rows; ++row)
    for (uint32_t column = 0; column < columns; ++column) {
      double expected = 0;
      for (int dr = -1; dr <= 1; ++dr) for (int dc = -1; dc <= 1; ++dc)
        expected = std::max(expected, track_before_detect::pathMean(history,
          columns, doppler, row, column, 4, dr, dc, -.25));
      REQUIRE(std::norm(bank.data[row][column]) == Catch::Approx(expected).epsilon(1e-14));
    }
  auto map = makeMap(5);
  const auto gap = integrated.process(map, 0, 500, 600, true);
  REQUIRE(gap.historyReset);
  REQUIRE_FALSE(gap.ready);
}

TEST_CASE("temporal family PFA is preregistered and interpolation retains its originating statistic")
{
  for (const uint32_t maximum : {4u, 8u}) {
    const double pfa = 1e-4;
    double allocated = track_before_detect::passPfa(pfa, maximum, 1, 4, 0);
    for (uint32_t pass = 1; pass < track_before_detect::detectorPassCount(maximum); ++pass)
      allocated += 27 * track_before_detect::passPfa(pfa, maximum, 1, 4, pass);
    REQUIRE(allocated == Catch::Approx(pfa).epsilon(1e-14));
  }
  blah2::detection::IntegrationMetrics metrics;
  metrics.cpis = 4; metrics.ready = true;
  metrics.evidence = std::make_shared<blah2::integration::TemporalEvidence::Result>();
  auto& bank = metrics.evidence->banks[0];
  bank.ready = true; bank.depth = 2;
  bank.window.epoch = 1; bank.window.newestSequence = 2;
  bank.window.depth = 2; bank.window.sequences = {2, 1};
  bank.map = std::make_unique<Map<std::complex<double>>>(9, 21);
  bank.motion.resize(9 * 21);
  for (int column = 0; column < 21; ++column) bank.map->delay.push_back(column);
  for (int row = 0; row < 9; ++row) {
    bank.map->doppler.push_back(row - 4);
    for (auto& cell : bank.map->data[row]) cell = {1, 0};
  }
  bank.map->data[6][10] = {30, 0};
  bank.map->data[6][9] = {3, 0}; bank.map->data[6][11] = {5, 0};
  bank.map->set_metrics();
  auto display = *bank.map;
  CfarDetector1D detector(.01, 1, 3, 0, 0);
  Centroid centroid(2, 2, 1); Interpolate interpolate(true, true);
  auto first = blah2::detection::detectProducts(display, detector, centroid, interpolate, metrics);
  REQUIRE(first->get_nDetections() == 1);
  // A different family dominates neighboring display cells. Neither position,
  // SNR nor the original two-CPI evidence may migrate to that statistic.
  display.data[6][9] = {1000, 0}; display.data[6][11] = {2000, 0};
  display.set_metrics();
  auto second = blah2::detection::detectProducts(display, detector, centroid, interpolate, metrics);
  REQUIRE(second->get_delay() == first->get_delay());
  REQUIRE(second->get_doppler() == first->get_doppler());
  REQUIRE(second->get_snr() == first->get_snr());
  REQUIRE(second->get_evidence().front().depth == 2);
  REQUIRE(second->get_evidence().front().sequences[1] == 1);
}

TEST_CASE("lane two-CPI integration drives production detection and resets exact lineage")
{
  REQUIRE(fftw_init_threads()); blah2::set_fftw_planner_threads(1);
  using namespace blah2::multi_illuminator;
  const std::vector<blah2::channelizer::IlluminatorDefinition> definitions{
    {"lane", 100000000, 10000, 20000, 16}};
  for (const uint32_t motion : {0u, 1u}) {
    auto config = smallLaneConfig();
    config.trackerMinimumSnrDb = -100;
    // RadarConfig carries the common contract's effective pass PFA.
    config.pfa /= motion ? 9.0 : 1.0;
    auto integratedConfig = config;
    integratedConfig.advanced.detectorIntegrationCpis = 2;
    integratedConfig.advanced.detectorIntegrationMotionBins = motion;
    MultiIlluminatorRadar raw(40000, 2, definitions, config);
    MultiIlluminatorRadar integrated(40000, 2, definitions, integratedConfig);
    blah2::integration::TemporalEvidence expected({2, motion, motion});
    const auto input = [](uint64_t first, uint32_t seed = 71) {
      auto value = signal("lane", 100000000, 20000, 2, 2, 2000, 8, 30, seed);
      value.samples.firstInputSample = first;
      return value;
    };
    uint64_t priorEpoch = 0;
    for (uint64_t cpi = 0; cpi < 3; ++cpi) {
      auto source = input(cpi * 4000, uint32_t(71 + cpi));
      auto single = raw.process({source});
      auto frames = integrated.process({source}, LiveTemporalIdentity{0, 7, true});
      REQUIRE(frames.size() == 1);
      auto& frame = frames.front();
      REQUIRE(frame.detectorIntegration);
      const auto& metrics = *frame.detectorIntegration;
      REQUIRE(metrics.captureEpoch == 7);
      REQUIRE(metrics.availableCpis == std::min<uint64_t>(cpi + 1, 2));
      REQUIRE(metrics.window.newestSequence == cpi + 1);
      auto evidence = expected.process(*single.front().map, metrics.window.epoch, cpi + 1);
      priorEpoch = metrics.window.epoch;
      if (!cpi) {
        REQUIRE_FALSE(metrics.ready);
        REQUIRE(single.front().detections->get_nDetections() > 0);
        REQUIRE(frame.detections->get_nDetections() == 0);
        REQUIRE(frame.tracks->get_n() == 0);
        REQUIRE(frame.map->data == single.front().map->data);
        continue;
      }
      REQUIRE(metrics.ready);
      REQUIRE(metrics.window.supportSequences() == std::vector<uint64_t>{cpi + 1, cpi});
      auto& bank = evidence.banks[motion ? 1 : 0];
      REQUIRE(frame.map->data == bank.map->data);
      const double spacing = frame.map->doppler[1] - frame.map->doppler[0];
      CfarDetector1D detector(config.pfa, config.guardCells, config.trainingCells,
        config.minimumDelay, config.minimumDopplerHz, 2);
      Centroid centroid(config.centroidRadius, config.centroidRadius, spacing);
      Interpolate interpolate(true, true);
      auto detected = detector.process(bank.map.get());
      auto centered = centroid.process(detected.get());
      auto interpolated = interpolate.process(centered.get(), bank.map.get());
      REQUIRE(frame.detections->to_json(0) == interpolated->to_json(0));
      REQUIRE(frame.detections->get_nDetections() > 0);
      REQUIRE(frame.tracks->get_n() > 0);
    }
    auto frames = integrated.process({input(16000)}, LiveTemporalIdentity{0, 7, true});
    REQUIRE_FALSE(frames.front().detectorIntegration->ready);
    REQUIRE(frames.front().detectorIntegration->window.epoch > priorEpoch);
    REQUIRE(frames.front().detectorIntegration->window.newestSequence == 1);
    REQUIRE(frames.front().tracks->get_n() == 0);
    frames = integrated.process({input(20000)}, LiveTemporalIdentity{0, 7, true});
    REQUIRE(frames.front().detectorIntegration->ready);
    priorEpoch = frames.front().detectorIntegration->window.epoch;
    // Even adjacent sample numbers cannot bridge a capture epoch change.
    frames = integrated.process({input(24000)}, LiveTemporalIdentity{0, 8, true});
    REQUIRE_FALSE(frames.front().detectorIntegration->ready);
    REQUIRE(frames.front().detectorIntegration->window.epoch > priorEpoch);
    REQUIRE(frames.front().tracks->get_n() == 0);
    for (const uint64_t first : {28000u, 32000u}) {
      frames = integrated.process({input(first)}, LiveTemporalIdentity{0, 8, false});
      REQUIRE_FALSE(frames.front().detectorIntegration->ready);
      REQUIRE(frames.front().detectorIntegration->availableCpis == 0);
      REQUIRE(frames.front().detections->get_nDetections() == 0);
      REQUIRE(frames.front().tracks->get_n() == 0);
    }
    frames = integrated.process({input(36000)}, LiveTemporalIdentity{0, 8, true});
    REQUIRE_FALSE(frames.front().detectorIntegration->ready);
    frames = integrated.process({input(40000)}, LiveTemporalIdentity{0, 8, true});
    REQUIRE(frames.front().detectorIntegration->ready);
    // A replayed CPI and an explicit reset both require fresh history.
    frames = integrated.process({input(40000)}, LiveTemporalIdentity{0, 8, true});
    REQUIRE_FALSE(frames.front().detectorIntegration->ready);
    integrated.reset();
    frames = integrated.process({input(44000)}, LiveTemporalIdentity{0, 8, true});
    REQUIRE_FALSE(frames.front().detectorIntegration->ready);
    REQUIRE(frames.front().detectorIntegration->window.newestSequence == 1);
  }
}

TEST_CASE("per-illuminator birth thresholds preserve detector products and other lanes")
{
  REQUIRE(fftw_init_threads()); blah2::set_fftw_planner_threads(1);
  const std::vector<blah2::channelizer::IlluminatorDefinition> definitions{
    {"lower", 100000000, 10000, 20000, 16},
    {"upper", 101000000, 10000, 20000, 16}};
  auto baseline = smallLaneConfig(); baseline.trackerMinimumSnrDb = 100;
  auto calibrated = baseline;
  calibrated.trackerMinimumSnrDbByIlluminator["upper"] = -100;
  blah2::multi_illuminator::MultiIlluminatorRadar control(40000, 2, definitions, baseline);
  blah2::multi_illuminator::MultiIlluminatorRadar candidate(40000, 2, definitions, calibrated);
  for (uint64_t sequence = 0; sequence < 2; ++sequence) {
    if (sequence) { control.reset(); candidate.reset(); }
    std::vector<blah2::channelizer::IlluminatorOutput> input;
    for (const auto& lane : definitions) {
      auto value = signal(lane.id, lane.frequencyHz, 20000, 2, 2, 2000, 8, 30, 71);
      value.samples.firstInputSample = sequence * 4000;
      input.push_back(std::move(value));
    }
    const auto expected = control.process(input), actual = candidate.process(input);
    REQUIRE(actual.size() == 2); REQUIRE(expected.size() == 2);
    for (std::size_t lane = 0; lane < actual.size(); ++lane) {
      REQUIRE(actual[lane].map->data == expected[lane].map->data);
      REQUIRE(actual[lane].detections->to_json(1234) == expected[lane].detections->to_json(1234));
      REQUIRE(actual[lane].detections->get_nDetections() > 0);
      REQUIRE(expected[lane].tracks->get_n() == 0);
    }
    REQUIRE(actual[0].tracks->get_n() == 0);
    REQUIRE(actual[0].trackerMinimumSnrDb == 100);
    REQUIRE_FALSE(actual[0].trackerMinimumSnrOverridden);
    REQUIRE(actual[1].tracks->get_n() > 0);
    REQUIRE(actual[1].trackerMinimumSnrDb == -100);
    REQUIRE(actual[1].trackerMinimumSnrOverridden);
  }
  for (const double invalid : {std::numeric_limits<double>::quiet_NaN(),
      std::numeric_limits<double>::infinity(), -101.0, 101.0}) {
    calibrated.trackerMinimumSnrDbByIlluminator["upper"] = invalid;
    REQUIRE_THROWS_AS(blah2::multi_illuminator::MultiIlluminatorRadar(
      40000, 2, definitions, calibrated), std::invalid_argument);
  }
  calibrated.trackerMinimumSnrDbByIlluminator = {{"missing", 5}};
  REQUIRE_THROWS_AS(blah2::multi_illuminator::MultiIlluminatorRadar(
    40000, 2, definitions, calibrated), std::invalid_argument);
}

TEST_CASE("selected live temporal lane is isolated and keeps exact capture lineage")
{
  REQUIRE(fftw_init_threads()); blah2::set_fftw_planner_threads(1);
  const std::vector<blah2::channelizer::IlluminatorDefinition> definitions{
    {"lower", 100000000, 10000, 20000, 16},
    {"upper", 101000000, 10000, 20000, 16}};
  auto baseline = smallLaneConfig(); baseline.trackerMinimumSnrDb = -100;
  auto observed = baseline;
  observed.liveTemporalShadow.enabled = true;
  observed.liveTemporalShadowIlluminatorId = "upper";
  observed.liveTemporalShadow.temporal.maximumCpis = 4;
  observed.liveTemporalShadow.temporal.delayMotionBins = 0;
  observed.liveTemporalShadow.temporal.dopplerMotionBins = 0;
  observed.liveTemporalShadow.scoring.enabled = true;
  observed.liveTemporalShadow.scoring.maximumCpis = 4;
  observed.liveTemporalShadow.scoring.delayMotionBins = 0;
  observed.liveTemporalShadow.scoring.dopplerMotionBins = 0;
  observed.liveTemporalShadow.maximumWorkMs = 25;
  observed.liveTemporalShadow.minimumIntervalMs = 100;
  blah2::multi_illuminator::MultiIlluminatorRadar control(40000, 2, definitions, baseline);
  blah2::multi_illuminator::MultiIlluminatorRadar candidate(40000, 2, definitions, observed);
  REQUIRE_FALSE(control.liveTemporalShadowStatus());
  REQUIRE_FALSE(control.takeLiveTemporalEvidence());
  uint64_t previousGeneration = 0;
  for (uint64_t cpi = 0; cpi < 6; ++cpi) {
    if (cpi) std::this_thread::sleep_for(std::chrono::milliseconds(105));
    if (cpi == 4) { control.reset(); candidate.reset(); }
    // Final CPI deliberately skips a capture interval.
    const uint64_t first = (cpi + (cpi == 5 ? 1 : 0)) * 4000;
    std::vector<blah2::channelizer::IlluminatorOutput> input;
    for (const auto& lane : definitions) {
      auto value = signal(lane.id, lane.frequencyHz, 20000, 2, 2, 2000, 8, 30, 71);
      value.samples.firstInputSample = first; input.push_back(std::move(value));
    }
    const auto expected = control.process(input);
    const auto actual = candidate.process(input,
      blah2::multi_illuminator::LiveTemporalIdentity{1234000 + cpi * 100, 19, true});
    REQUIRE(actual.size() == 2); REQUIRE(expected.size() == 2);
    REQUIRE_FALSE(actual[0].liveTemporalShadow);
    REQUIRE(actual[1].liveTemporalShadow);
    for (std::size_t lane = 0; lane < actual.size(); ++lane) {
      REQUIRE(actual[lane].map->data == expected[lane].map->data);
      REQUIRE(actual[lane].detections->to_json(1234) == expected[lane].detections->to_json(1234));
      REQUIRE(actual[lane].tracks->to_json(1234) == expected[lane].tracks->to_json(1234));
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (candidate.liveTemporalShadowStatus()->busy && std::chrono::steady_clock::now() < deadline)
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    REQUIRE_FALSE(candidate.liveTemporalShadowStatus()->busy);
    REQUIRE_FALSE(candidate.liveTemporalShadowStatus()->disabled);
    auto evidence = candidate.takeLiveTemporalEvidence(); REQUIRE(evidence);
    REQUIRE(evidence->frame.firstSample == first);
    REQUIRE(evidence->frame.endSample == first + 4000);
    REQUIRE(evidence->frame.sampleRateHz == 40000);
    REQUIRE(evidence->frame.mapSampleRateHz == 20000);
    REQUIRE(evidence->frame.tuningHz == 101000000);
    REQUIRE(evidence->frame.captureEpoch == 19);
    REQUIRE(evidence->frame.timestampMs == 1234000 + cpi * 100);
    REQUIRE(evidence->temporal.availableCpis == (cpi < 4 ? cpi + 1 : 1));
    if (cpi == 3) REQUIRE(evidence->temporal.banks[2].ready);
    if (cpi >= 4) REQUIRE(evidence->frame.configurationEpoch > previousGeneration);
    previousGeneration = evidence->frame.configurationEpoch;
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(105));
  std::vector<blah2::channelizer::IlluminatorOutput> unpositioned;
  for (const auto& lane : definitions) {
    auto value = signal(lane.id, lane.frequencyHz, 20000, 2, 2, 2000, 8, 30, 71);
    value.samples.firstInputSample = 28000; unpositioned.push_back(std::move(value));
  }
  candidate.process(std::move(unpositioned),
    blah2::multi_illuminator::LiveTemporalIdentity{1234600, 19, false});
  REQUIRE_FALSE(candidate.takeLiveTemporalEvidence());
  REQUIRE(candidate.liveTemporalShadowStatus()->dropped > 0);
  for (const auto selection : {"", "missing"}) {
    observed.liveTemporalShadowIlluminatorId = selection;
    REQUIRE_THROWS_AS(blah2::multi_illuminator::MultiIlluminatorRadar(
      40000, 2, definitions, observed), std::invalid_argument);
  }
  observed.liveTemporalShadowIlluminatorId = "upper";
  observed.offlineReplay = true;
  REQUIRE_THROWS_AS(blah2::multi_illuminator::MultiIlluminatorRadar(
    40000, 2, definitions, observed), std::invalid_argument);
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
  auto unmatchedShadow = config;
  unmatchedShadow.advanced.temporalShadow = true;
  unmatchedShadow.advanced.temporalShadowOfflineReplay = true;
  unmatchedShadow.advanced.temporalShadowIlluminatorId = "missing";
  REQUIRE_THROWS_AS(blah2::multi_illuminator::MultiIlluminatorRadar(
    captureRate, 2, definitions, unmatchedShadow), std::invalid_argument);
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

namespace {
// A CPU numerical double for the existing worker interfaces: no device/module
// discovery. It deliberately rounds at the same FP32 IPC boundaries as GPU.
struct LaneGpuState {
  blah2::GpuGeometry geometry{};
  std::string fault;
  unsigned clutterCalls = 0, ambiguityCalls = 0;
};
class LaneGpuDouble final : public blah2::GpuBackend,
    public blah2::GpuClutterFrameBackend {
  std::shared_ptr<LaneGpuState> state_;
  std::vector<std::complex<float>> reference_, surveillance_, estimate_;
  WienerHopf clutter_;
  Ambiguity ambiguity_;
public:
  LaneGpuDouble(blah2::GpuGeometry geometry, std::shared_ptr<LaneGpuState> state)
    : state_(std::move(state)), reference_(geometry.clutterSamples),
      surveillance_(size_t(geometry.clutterSamples) * geometry.channels),
      estimate_(surveillance_.size()),
      clutter_(0, int32_t(geometry.clutterBins), geometry.clutterSamples),
      ambiguity_(geometry.delayMin, geometry.delayMin + geometry.delays - 1,
        -100, 100, 20000, geometry.clutterSamples, true) {
    state_->geometry = geometry;
    if (geometry.range != ambiguity_.get_nfft() ||
        geometry.doppler != ambiguity_.get_n_doppler_bins())
      throw std::invalid_argument("Lane GPU fixture geometry differs");
  }
  blah2::GpuDevice device() const override { return {"test", "CPU numerical double", 0}; }
  bool clutterAvailable() const override { return state_->fault != "unavailable"; }
  blah2::GpuClutterBuffers clutterBuffers() override {
    return {reference_.data(), reference_.size(), surveillance_.data(),
      surveillance_.size(), estimate_.data(), estimate_.size()};
  }
  bool processClutterFrame() override {
    ++state_->clutterCalls;
    if (state_->fault == "throw") throw std::runtime_error("Injected lane device loss");
    if (state_->fault == "reject") return false;
    const auto& g = state_->geometry;
    IqData reference(g.clutterSamples), input(g.clutterSamples);
    auto& r = reference.resize_for_write(g.clutterSamples);
    std::copy(reference_.begin(), reference_.end(), r.begin());
    for (uint32_t channel = 0; channel < g.channels; ++channel) {
      auto& y = input.resize_for_write(g.clutterSamples);
      const auto first = surveillance_.begin() + size_t(channel) * g.clutterSamples;
      std::copy_n(first, g.clutterSamples, y.begin());
      if (!clutter_.process(&reference, &input)) return false;
      const auto& filtered = input.view_data();
      for (uint32_t i = 0; i < g.clutterSamples; ++i)
        estimate_[size_t(channel) * g.clutterSamples + i] =
          std::complex<float>(std::complex<double>(first[i]) - filtered[i]);
    }
    if (state_->fault == "wrong") estimate_[0] += std::complex<float>(100, 0);
    return true;
  }
  void process(const std::vector<std::complex<float>>& reference,
      const std::vector<std::complex<float>>& surveillance,
      std::vector<std::complex<float>>& output) override {
    ++state_->ambiguityCalls;
    const auto& g = state_->geometry;
    const uint32_t correlation = ambiguity_.get_n_corr();
    std::deque<std::complex<double>> unpacked(g.clutterSamples);
    for (uint32_t row = 0; row < g.doppler; ++row)
      std::copy_n(reference.begin() + size_t(row) * g.range, correlation,
        unpacked.begin() + size_t(row) * correlation);
    output.resize(size_t(g.channels) * g.delays * g.doppler);
    for (uint32_t channel = 0; channel < g.channels; ++channel) {
      IqData input(g.clutterSamples);
      auto& y = input.resize_for_write(g.clutterSamples);
      for (uint32_t row = 0; row < g.doppler; ++row)
        std::copy_n(surveillance.begin() +
            (size_t(channel) * g.doppler + row) * g.range, correlation,
          y.begin() + size_t(row) * correlation);
      ambiguity_.process(unpacked, &input);
      for (uint32_t row = 0; row < g.doppler; ++row)
        for (uint32_t col = 0; col < g.delays; ++col)
          output[(size_t(channel) * g.delays + col) * g.doppler +
            (row + g.doppler / 2 + 1) % g.doppler] =
            ambiguity_.result()->data[row][col];
    }
  }
};
blah2::multi_illuminator::RadarConfig laneGpuConfig() {
  blah2::multi_illuminator::RadarConfig config;
  config.cpiSeconds = .1;
  config.delayMin = -2; config.delayMax = 20;
  config.dopplerMin = -100; config.dopplerMax = 100;
  config.clutterDelayMin = -2; config.clutterDelayMax = 4;
  config.guardCells = 1; config.trainingCells = 4;
  config.minimumDelay = 3; config.minimumDopplerHz = 5;
  config.centroidRadius = 2;
  config.workers = 1; config.laneWorkers = 1;
  config.accelerationMode = "gpu";
  return config;
}
void compareLaneMaps(const blah2::multi_illuminator::RadarFrame& actual,
    const blah2::multi_illuminator::RadarFrame& expected) {
  REQUIRE(actual.map->delay == expected.map->delay);
  REQUIRE(actual.map->doppler == expected.map->doppler);
  REQUIRE(actual.firstInputSample == expected.firstInputSample);
  REQUIRE(actual.endInputSample == expected.endInputSample);
  double error = 0, power = 0;
  for (size_t r = 0; r < expected.map->data.size(); ++r)
    for (size_t c = 0; c < expected.map->data[r].size(); ++c) {
      error += std::norm(actual.map->data[r][c] - expected.map->data[r][c]);
      power += std::norm(expected.map->data[r][c]);
    }
  REQUIRE(error <= std::max(1e-20, power * 1e-8));
}
}

TEST_CASE("portable cohorts cap workers before construction and retain capacity after a failed slot")
{
  using namespace blah2;
  for (const std::string mode : {"auto", "gpu"}) {
    auto config = laneGpuConfig(); config.accelerationMode = mode;
    std::vector<channelizer::IlluminatorDefinition> definitions;
    for (unsigned lane = 0; lane < 9; ++lane)
      definitions.push_back({"lane-" + std::to_string(lane), 100000000 + 100000 * lane,
        10000, 20000, 16});
    unsigned attempts = 0;
    const auto factory = [&](const GpuGeometry& geometry, const std::string&) -> std::unique_ptr<GpuBackend> {
      if (++attempts == 2) throw std::runtime_error("injected allocation failure");
      return std::make_unique<LaneGpuDouble>(geometry, std::make_shared<LaneGpuState>());
    };
    multi_illuminator::MultiIlluminatorRadar radar(40000, 2, definitions, config, factory);
    REQUIRE(attempts == 8); // slot nine never reaches even an injected factory
    std::vector<channelizer::IlluminatorOutput> input;
    for (const auto& lane : definitions)
      input.push_back(signal(lane.id, lane.frequencyHz, 20000, 2, 2, 2000, 8, 30, 17));
    auto frames = radar.process(std::move(input));
    REQUIRE(frames.size() == 9);
    REQUIRE(frames[1].acceleration.state == "fallback");
    REQUIRE(frames[1].acceleration.reason.find("allocation failure") != std::string::npos);
    REQUIRE(frames[8].acceleration.active == "cpu");
    REQUIRE(frames[8].acceleration.reason.find("aggregate-gpu-capacity-exhausted") != std::string::npos);
    REQUIRE_FALSE(frames[8].ambiguityGpuExecuted);
    REQUIRE(frames[8].ambiguityCpuExecuted);
  }
}

TEST_CASE("every accelerated lane offers full signed clutter geometry and qualifies before input")
{
  using namespace blah2;
  for (const std::string mode : {"gpu", "auto"}) for (const int first : {-2, 2}) {
    auto config = laneGpuConfig();
    config.accelerationMode = mode;
    config.clutterDelayMin = first; config.clutterDelayMax = first + 6;
    const std::vector<channelizer::IlluminatorDefinition> definitions{
      {"first", 100000000, 10000, 20000, 16},
      {"second", 100100000, 10000, 20000, 16}};
    std::vector<std::shared_ptr<LaneGpuState>> states;
    const auto factory = [&](const GpuGeometry& geometry, const std::string&) {
      auto state = std::make_shared<LaneGpuState>(); states.push_back(state);
      return std::make_unique<LaneGpuDouble>(geometry, state);
    };
    multi_illuminator::MultiIlluminatorRadar accelerated(40000, 2, definitions, config, factory);
    auto cpuConfig = config; cpuConfig.accelerationMode = "cpu";
    multi_illuminator::MultiIlluminatorRadar cpu(40000, 2, definitions, cpuConfig);
    accelerated.prepareStartup(); accelerated.prepareStartup();
    REQUIRE(states.size() == definitions.size());
    for (const auto& state : states) {
      REQUIRE(state->geometry.clutterSamples == 2000);
      REQUIRE(state->geometry.clutterBins == 6);
      REQUIRE(state->geometry.clutterDelayMin == first);
      REQUIRE(state->clutterCalls >= Acceleration::ClutterQualificationFrames);
      if (mode == "gpu") REQUIRE(state->clutterCalls == Acceleration::TotalQualificationFrames);
    }
    std::vector<channelizer::IlluminatorOutput> input;
    for (const auto& definition : definitions)
      input.push_back(signal(definition.id, definition.frequencyHz, 20000, 2, 2, 2000, 8, 30, 17));
    const auto baseline = cpu.process(input), frames = accelerated.process(input);
    REQUIRE(frames.size() == 2);
    for (size_t lane = 0; lane < frames.size(); ++lane) {
      compareLaneMaps(frames[lane], baseline[lane]);
      REQUIRE(frames[lane].clutterAcceleration.requested == mode);
      if (mode == "gpu") {
        REQUIRE(frames[lane].clutterAcceleration.active == "vulkan");
        REQUIRE(frames[lane].clutterAcceleration.state == "ready");
        REQUIRE(frames[lane].clutterGpuExecuted);
        REQUIRE_FALSE(frames[lane].clutterCpuExecuted);
      } else if (frames[lane].clutterAcceleration.state == "fallback") {
        REQUIRE_FALSE(frames[lane].clutterAcceleration.reason.empty());
        REQUIRE(frames[lane].clutterCpuExecuted);
      }
    }
  }
}

TEST_CASE("lane clutter rejection retains exact CPU fallback and independently qualified ambiguity")
{
  using namespace blah2;
  const std::vector<channelizer::IlluminatorDefinition> definitions{
    {"only", 100000000, 10000, 20000, 16}};
  for (const std::string fault : {"unavailable", "reject", "wrong", "throw"})
    for (const bool runtime : {false, true}) {
      if (runtime && fault == "wrong") continue;
      auto config = laneGpuConfig();
      auto state = std::make_shared<LaneGpuState>();
      if (!runtime) state->fault = fault;
      const auto factory = [state](const GpuGeometry& geometry, const std::string&) {
        return std::make_unique<LaneGpuDouble>(geometry, state);
      };
      multi_illuminator::MultiIlluminatorRadar accelerated(40000, 2, definitions, config, factory);
      config.accelerationMode = "cpu";
      multi_illuminator::MultiIlluminatorRadar cpu(40000, 2, definitions, config);
      accelerated.prepareStartup();
      if (runtime) state->fault = fault;
      for (uint64_t frame = 0; frame < 3; ++frame) {
        auto input = dedicated_signal(2000);
        input.samples.firstInputSample = frame * 4000;
        const auto baseline = cpu.process({input});
        const unsigned before = state->clutterCalls;
        const auto actual = accelerated.process({input});
        REQUIRE(actual.size() == 1);
        compareLaneMaps(actual.front(), baseline.front());
        REQUIRE(actual.front().clutterAcceleration.active == "cpu");
        REQUIRE(actual.front().clutterAcceleration.state == "fallback");
        REQUIRE_FALSE(actual.front().clutterAcceleration.reason.empty());
        REQUIRE(actual.front().clutterCpuExecuted);
        REQUIRE_FALSE(actual.front().clutterGpuExecuted);
        if (frame || !runtime) REQUIRE(state->clutterCalls == before);
        if (fault != "throw") {
          REQUIRE(actual.front().acceleration.active == "vulkan");
          REQUIRE(actual.front().ambiguityGpuExecuted);
        }
      }
    }
}

TEST_CASE("zero-reference CPI is dropped and later valid input recovers without observation")
{
  using namespace blah2;
  const std::vector<channelizer::IlluminatorDefinition> definitions{
    {"only", 100000000, 10000, 20000, 16}};
  for (const std::string mode : {"cpu", "gpu", "auto"})
    for (const bool split : {false, true}) {
      CAPTURE(mode, split);
      auto config = laneGpuConfig(); config.accelerationMode = mode;
      auto state = std::make_shared<LaneGpuState>();
      const auto factory = [state](const GpuGeometry& geometry, const std::string&) {
        return std::make_unique<LaneGpuDouble>(geometry, state);
      };
      multi_illuminator::MultiIlluminatorRadar radar(40000, 2, definitions, config, factory);
      std::vector<uint64_t> observed;
      radar.setNativeCafObserver([&](const multi_illuminator::NativeCafView& view) {
        observed.push_back(view.firstInputSample);
      });
      radar.prepareStartup();
      REQUIRE(observed.empty());
      REQUIRE(radar.process({dedicated_signal(2000)}).size() == 1);
      REQUIRE(radar.laneHealth().front().acceptedCpis == 1);
      auto rejected = dedicated_signal(2000);
      std::fill(rejected.samples.channels[0].begin(), rejected.samples.channels[0].end(),
        std::complex<float>{});
      rejected.samples.firstInputSample = 4000;
      if (split) {
        auto tail = rejected;
        for (auto& channel : rejected.samples.channels) channel.resize(1000);
        for (auto& channel : tail.samples.channels) channel.erase(channel.begin(), channel.begin() + 1000);
        tail.samples.firstInputSample = 6000;
        REQUIRE(radar.process({rejected}).empty());
        REQUIRE(radar.process({tail}).empty());
      } else {
        REQUIRE(radar.process({rejected}).empty());
      }
      REQUIRE(observed == std::vector<uint64_t>{0});
      const auto rejectedHealth = radar.laneHealth().front();
      REQUIRE(rejectedHealth.rejectedCpis == 1);
      REQUIRE_FALSE(rejectedHealth.lastAccepted);
      REQUIRE(rejectedHealth.lastFirstInputSample == 4000);
      REQUIRE(rejectedHealth.lastReason == "clutter_reference_rejected");
      auto recovered = dedicated_signal(2000);
      recovered.samples.firstInputSample = 8000;
      const auto actual = radar.process({recovered});
      REQUIRE(actual.size() == 1);
      REQUIRE(observed == std::vector<uint64_t>{0, 8000});
      const auto recoveredHealth = radar.laneHealth().front();
      REQUIRE(recoveredHealth.acceptedCpis == 2);
      REQUIRE(recoveredHealth.rejectedCpis == 1);
      REQUIRE(recoveredHealth.lastAccepted);
      REQUIRE(recoveredHealth.lastReason.empty());
      REQUIRE(actual.front().trackerDiagnostics.tracksBefore == 0);
      config.accelerationMode = "cpu";
      multi_illuminator::MultiIlluminatorRadar fresh(40000, 2, definitions, config);
      const auto expected = fresh.process({recovered});
      REQUIRE(expected.size() == 1);
      compareLaneMaps(actual.front(), expected.front());
    }
}

TEST_CASE("a rejected CPI inside a larger input does not skip the following CPI")
{
  using namespace blah2;
  auto config = laneGpuConfig(); config.accelerationMode = "cpu";
  const std::vector<channelizer::IlluminatorDefinition> definitions{
    {"only", 100000000, 10000, 20000, 16}};
  multi_illuminator::MultiIlluminatorRadar radar(40000, 2, definitions, config);
  auto input = dedicated_signal(2000), valid = input;
  std::fill(input.samples.channels[0].begin(), input.samples.channels[0].end(),
    std::complex<float>{});
  for (size_t channel = 0; channel < input.samples.channels.size(); ++channel)
    input.samples.channels[channel].insert(input.samples.channels[channel].end(),
      valid.samples.channels[channel].begin(), valid.samples.channels[channel].end());
  const auto result = radar.process({input});
  REQUIRE(result.size() == 1);
  REQUIRE(result.front().firstInputSample == 4000);
  REQUIRE(result.front().endInputSample == 7998);
}

TEST_CASE("nondivisible lane CPI retains sample cadence and actual CAF axes")
{
  using namespace blah2;
  auto config = laneGpuConfig(); config.accelerationMode = "cpu";
  config.cpiSeconds = 2003.0 / 20000;
  const std::vector<channelizer::IlluminatorDefinition> definitions{
    {"only", 100000000, 10000, 20000, 16}};
  multi_illuminator::MultiIlluminatorRadar radar(40000, 2, definitions, config);
  Ambiguity oracle(config.delayMin, config.delayMax, config.dopplerMin,
    config.dopplerMax, 20000, 2003, true);
  for (uint64_t cpi = 0; cpi < 2; ++cpi) {
    auto input = dedicated_signal(2003);
    input.samples.firstInputSample = cpi * 4006;
    const auto frames = radar.process({input});
    REQUIRE(frames.size() == 1);
    REQUIRE(frames.front().firstInputSample == cpi * 4006);
    REQUIRE(frames.front().endInputSample == cpi * 4006 + 4004);
    REQUIRE(frames.front().map->doppler == oracle.result()->doppler);
  }
}

TEST_CASE("FM waveform label never silently changes the default reference")
{
  using namespace blah2;
  auto config = laneGpuConfig(); config.accelerationMode = "cpu";
  std::vector<channelizer::IlluminatorDefinition> rawDefinitions{
    {"only", 100000000, 10000, 20000, 16}};
  auto fmDefinitions = rawDefinitions; fmDefinitions.front().standard = "FM";
  multi_illuminator::MultiIlluminatorRadar raw(40000, 2, rawDefinitions, config);
  multi_illuminator::MultiIlluminatorRadar fm(40000, 2, fmDefinitions, config);
  const auto input = dedicated_signal(2000);
  const auto baseline = raw.process({input}), actual = fm.process({input});
  REQUIRE(baseline.size() == 1); REQUIRE(actual.size() == 1);
  compareLaneMaps(actual.front(), baseline.front());
  REQUIRE(actual.front().map->data == baseline.front().map->data);
  config.experimentalFmReferenceQualityFilter = true;
  REQUIRE_THROWS_AS(multi_illuminator::MultiIlluminatorRadar(40000, 2, fmDefinitions, config), std::invalid_argument);
  config.offlineReplay = true;
  REQUIRE_THROWS_AS(multi_illuminator::MultiIlluminatorRadar(40000, 2, rawDefinitions, config), std::invalid_argument);
  multi_illuminator::MultiIlluminatorRadar experimental(40000, 2, fmDefinitions, config);
  const auto masked = experimental.process({input});
  REQUIRE(masked.size() == 1);
  REQUIRE(masked.front().map->data != baseline.front().map->data);
}

TEST_CASE("CPU clutter workspace switches preserve learned reference state")
{
  using namespace blah2;
  production_replay::RawIqStages::Config config;
  config.clutterDelayMin = -2; config.clutterDelayMax = 4;
  config.samples = 2000; config.paths = 1; config.eagerPlanning = true;
  config.reference.analysisSamples = 128; config.reference.analysisInterval = 1;
  production_replay::RawIqStages stages(config);
  IqData first(2000), second(2000), reference(2000);
  startup::fillIq({&first, &second}, 2000, 0);
  stages.reference().process_into({&first, &second}, reference);
  auto metrics = stages.reference().get_metrics();
  REQUIRE(metrics.updates > 0);
  stages.setBorrowed(true);
  REQUIRE(stages.reference().get_metrics().updates == metrics.updates);
  REQUIRE(stages.reference().get_metrics().weights == metrics.weights);
  stages.reference().process_into({&first, &second}, reference);
  metrics = stages.reference().get_metrics();
  stages.setBorrowed(false);
  REQUIRE(stages.reference().get_metrics().updates == metrics.updates);
  REQUIRE(stages.reference().get_metrics().weights == metrics.weights);
}

TEST_CASE("native CAF observer preserves channels and does not change radar products")
{
  using namespace blah2;
  const std::vector<channelizer::IlluminatorDefinition> definitions{
    {"only", 100000000, 10000, 20000, 16}};
  auto config = laneGpuConfig(); config.accelerationMode = "cpu";
  config.surveillanceChannels = {2, 1};
  multi_illuminator::MultiIlluminatorRadar observed(40000, 3, definitions, config),
    baseline(40000, 3, definitions, config);
  unsigned calls = 0;
  bool sawComplexPhase = false;
  observed.setNativeCafObserver([&](const multi_illuminator::NativeCafView& view) {
    ++calls;
    REQUIRE(view.illuminatorId == "only");
    REQUIRE(view.channelIds == std::vector<uint32_t>{2, 1});
    REQUIRE(view.referenceChannelIds == std::vector<uint32_t>{0});
    REQUIRE(view.maps.size() == 2);
    REQUIRE(view.firstInputSample == 0);
    REQUIRE(view.endInputSample == 3998);
    REQUIRE(view.sampleRateHz == 20000);
    for (const auto* map : view.maps)
      for (const auto& row : map->data)
        for (const auto value : row) sawComplexPhase |= std::abs(value.imag()) > 1e-12;
  });
  observed.prepareStartup(); baseline.prepareStartup();
  REQUIRE(calls == 0);
  const auto input = signal("only", 100000000, 20000, 2, 3, 2000, 8, 30, 71);
  const auto expected = baseline.process({input}), actual = observed.process({input});
  REQUIRE(actual.size() == 1); REQUIRE(expected.size() == 1);
  REQUIRE(calls == 1); REQUIRE(sawComplexPhase);
  compareLaneMaps(actual.front(), expected.front());
  REQUIRE(actual.front().detections->to_json_km(1, 20000) ==
    expected.front().detections->to_json_km(1, 20000));
  REQUIRE(actual.front().tracks->to_json(1) == expected.front().tracks->to_json(1));
  REQUIRE_THROWS_AS(observed.setNativeCafObserver({}), std::logic_error);
}

TEST_CASE("native CAF observer is absent on rejected input and fails closed on exception")
{
  using namespace blah2;
  const std::vector<channelizer::IlluminatorDefinition> definitions{
    {"only", 100000000, 10000, 20000, 16}};
  auto config = laneGpuConfig(); config.accelerationMode = "cpu";
  multi_illuminator::MultiIlluminatorRadar radar(40000, 2, definitions, config);
  unsigned calls = 0;
  radar.setNativeCafObserver([&](const multi_illuminator::NativeCafView&) {
    ++calls; throw std::runtime_error("observer write failed");
  });
  auto rejected = dedicated_signal(2000);
  std::fill(rejected.samples.channels[0].begin(), rejected.samples.channels[0].end(),
    std::complex<float>{});
  REQUIRE(radar.process({rejected}).empty()); REQUIRE(calls == 0);
  radar.reset();
  REQUIRE_THROWS_AS(radar.process({dedicated_signal(2000)}), std::runtime_error);
  REQUIRE(calls == 1);
  radar.reset();
  REQUIRE_THROWS_AS(radar.process({dedicated_signal(2000)}), std::logic_error);
  REQUIRE(calls == 1);
}

TEST_CASE("global lane status cannot hide heterogeneous fallback or executed stages")
{
  using namespace blah2::multi_illuminator;
  std::vector<RadarFrame> frames(2);
  for (size_t i = 0; i < frames.size(); ++i) {
    frames[i].illuminatorId = i ? "fallback-lane" : "gpu-lane";
    frames[i].acceleration.requested = "auto";
    frames[i].acceleration.active = "vulkan";
    frames[i].acceleration.state = "ready";
    frames[i].clutterAcceleration = frames[i].acceleration;
    frames[i].firstInputSample = 1000;
    frames[i].endInputSample = 1998;
    frames[i].inputSamplesPerOutput = 2;
  }
  REQUIRE(aggregate_status(frames).ambiguity.state == "ready");
  frames[0].ambiguityGpuExecuted = frames[0].clutterGpuExecuted = true;
  frames[0].ambiguityCpuExecuted = frames[0].clutterCpuExecuted = false;
  frames[1].acceleration.active = "cpu"; frames[1].acceleration.state = "fallback";
  frames[1].acceleration.reason = "qualification unavailable";
  frames[1].clutterAcceleration = frames[1].acceleration;
  frames[1].ambiguityCpuExecuted = frames[1].clutterCpuExecuted = true;
  const auto summary = aggregate_status(frames);
  REQUIRE(summary.ambiguity.active == "mixed");
  REQUIRE(summary.ambiguity.state == "fallback");
  REQUIRE(summary.ambiguity.reason.find("fallback-lane: qualification unavailable") != std::string::npos);
  REQUIRE(summary.clutter.state == "fallback");
  REQUIRE(summary.ambiguityGpuExecuted); REQUIRE(summary.ambiguityCpuExecuted);
  REQUIRE(summary.clutterGpuExecuted); REQUIRE(summary.clutterCpuExecuted);
  REQUIRE(summary.ambiguity.members.size() == 2);
  REQUIRE(summary.ambiguity.members[0].backend == "vulkan");
  REQUIRE(summary.ambiguity.members[0].gpuExecuted);
  REQUIRE_FALSE(summary.ambiguity.members[0].cpuExecuted);
  REQUIRE(summary.ambiguity.members[0].firstInputSample == 1000);
  REQUIRE(summary.ambiguity.members[0].endInputSample == 2000);
  REQUIRE(summary.ambiguity.members[1].backend == "cpu");
  REQUIRE(summary.ambiguity.members[1].cpuExecuted);
  REQUIRE_FALSE(summary.ambiguity.members[1].gpuExecuted);
  Timing timing(0); timing.set_acceleration(summary.ambiguity,
    summary.ambiguityGpuExecuted, summary.ambiguityCpuExecuted);
  timing.set_wideband_acceleration(summary.ambiguity, true, false);
  timing.set_multi_lane_health({{"gpu-lane", 100000000, 4, 1, true, false,
    1000, 1999, "clutter_reference_rejected"}});
  rapidjson::Document document; document.Parse(timing.to_json().c_str());
  REQUIRE(document["acceleration"]["gpuExecuted"].GetBool());
  REQUIRE(document["acceleration"]["cpuExecuted"].GetBool());
  REQUIRE(document["widebandAcceleration"]["gpuExecuted"].GetBool());
  REQUIRE_FALSE(document["widebandAcceleration"]["cpuExecuted"].GetBool());
  REQUIRE(document["multiIlluminatorLaneHealth"].Size() == 1);
  REQUIRE(document["multiIlluminatorLaneHealth"][0]["rejectedCpis"].GetUint64() == 1);
  PipelineState pipeline;
  pipeline.startup_fallback("Optional GPU channelizer unavailable");
  const auto wideband = pipeline.acceleration_status("gpu", false, true);
  REQUIRE(wideband.requested == "gpu"); REQUIRE(wideband.active == "cpu");
  REQUIRE(wideband.state == "fallback"); REQUIRE_FALSE(wideband.reason.empty());
}

TEST_CASE("ATSC pilot float ingestion matches split double ingestion without staging copies")
{
  using namespace blah2;
  constexpr uint32_t rate = 6144000, samples = 61440;
  channelizer::IlluminatorDefinition definition;
  definition.id = "atsc"; definition.frequencyHz = 518000000;
  definition.occupiedBandwidthHz = 6000000; definition.outputRateHz = rate;
  definition.standard = "ATSC-1.0"; definition.atscReferenceMode = "pilot_aided";
  auto config = laneGpuConfig(); config.accelerationMode = "cpu";
  config.cpiSeconds = .01; config.borrowedClutter = true;
  multi_illuminator::MultiIlluminatorRadar direct(rate, 2, {definition}, config),
    split(rate, 2, {definition}, config);
  direct.prepareStartup(); split.prepareStartup();
  auto input = signal("atsc", definition.frequencyHz, rate, 1, 2,
    samples, 8, 30, 71);
  const double pilotHz = -4.5e6 * 684.0 / 286.0 / 4 + 150;
  for (uint32_t i = 0; i < samples; ++i) {
    const auto pilot = std::complex<float>(std::polar(.35, 2 * pi * pilotHz * i / rate));
    input.samples.channels[0][i] += pilot;
    input.samples.channels[1][i] += pilot;
  }
  bool applied = false;
  for (uint64_t frame = 0; frame < 4; ++frame) {
    input.samples.firstInputSample = frame * samples;
    const auto actual = direct.process({input});
    auto first = input, second = input;
    for (auto& channel : first.samples.channels) channel.resize(samples / 2);
    for (auto& channel : second.samples.channels)
      channel.erase(channel.begin(), channel.begin() + samples / 2);
    second.samples.firstInputSample += samples / 2;
    REQUIRE(split.process({first}).empty());
    const auto expected = split.process({second});
    REQUIRE(actual.size() == 1); REQUIRE(expected.size() == 1);
    compareLaneMaps(actual.front(), expected.front());
    const auto& a = actual.front(); const auto& b = expected.front();
    REQUIRE(a.firstInputSample == b.firstInputSample);
    REQUIRE(a.endInputSample == b.endInputSample);
    REQUIRE(a.atscReferenceEffective == b.atscReferenceEffective);
    REQUIRE(a.atscReferenceReason == b.atscReferenceReason);
    REQUIRE(a.atscPilot->observedSamples == b.atscPilot->observedSamples);
    REQUIRE(a.atscPilot->firstInputSample == b.atscPilot->firstInputSample);
    REQUIRE(a.atscPilot->endInputSample == b.atscPilot->endInputSample);
    REQUIRE(a.atscPilotProjection->applied == b.atscPilotProjection->applied);
    applied |= a.atscPilotProjection->applied;
    REQUIRE(a.inputProcessing.path == "native_float_borrowed");
    REQUIRE(a.inputProcessing.widenedSamples == 0);
    REQUIRE(a.inputProcessing.copiedSamples == 0);
    REQUIRE(b.inputProcessing.path == "partial_double");
    REQUIRE(b.inputProcessing.widenedSamples == 2 * samples);
    REQUIRE(b.inputProcessing.copiedSamples == 2 * samples);
    REQUIRE(std::isfinite(a.inputProcessing.pilotQualityMs));
    REQUIRE(std::isfinite(a.inputProcessing.pilotProjectionMs));
    REQUIRE(a.detections->to_json_km(1, rate) == b.detections->to_json_km(1, rate));
    REQUIRE(a.tracks->to_json(1) == b.tracks->to_json(1));
  }
  REQUIRE(applied);
}

TEST_CASE("ridge shadow has a contiguous finite lifecycle without changing native outputs")
{
  using namespace blah2;
  const std::vector<channelizer::IlluminatorDefinition> definitions{
    {"only", 100000000, 10000, 20000, 16}};
  auto config = laneGpuConfig(); config.accelerationMode = "cpu";
  multi_illuminator::MultiIlluminatorRadar baseline(40000, 2, definitions, config);
  config.dopplerRidgeScrubber.mode = "shadow";
  config.dopplerRidgeScrubber.shadowMaximumCpis = 2;
  config.dopplerRidgeScrubber.persistenceCpis = 2;
  multi_illuminator::MultiIlluminatorRadar shadow(40000, 2, definitions, config);
  double completedCost = 0;
  for (uint64_t frame = 0; frame < 4; ++frame) {
    auto input = dedicated_signal(2000); input.samples.firstInputSample = frame * 4000;
    const auto expected = baseline.process({input}), actual = shadow.process({input});
    REQUIRE(actual.size() == 1); REQUIRE(expected.size() == 1);
    compareLaneMaps(actual.front(), expected.front());
    REQUIRE(actual.front().detections->to_json_km(1, 20000) ==
      expected.front().detections->to_json_km(1, 20000));
    REQUIRE(actual.front().tracks->to_json(1) == expected.front().tracks->to_json(1));
    const auto& metrics = *actual.front().ridgeScrubber;
    REQUIRE(metrics.mode == "shadow"); REQUIRE(metrics.epoch == 1);
    REQUIRE(metrics.maximumCpis == 2);
    REQUIRE(metrics.observedCpis == std::min<uint64_t>(frame + 1, 2));
    REQUIRE(metrics.comparisonAvailable == (frame < 2));
    REQUIRE(metrics.state == (frame ? "completed" : "running"));
    REQUIRE(std::isfinite(metrics.comparisonMs));
    if (frame == 1) completedCost = metrics.totalComparisonMs;
    if (frame >= 2) {
      REQUIRE(metrics.comparisonMs == 0);
      REQUIRE(metrics.totalComparisonMs == completedCost);
      REQUIRE(metrics.reason == "shadow_window_complete");
    }
  }
  auto gap = dedicated_signal(2000); gap.samples.firstInputSample = 20000;
  auto frames = shadow.process({gap});
  REQUIRE(frames.front().ridgeScrubber->epoch == 2);
  REQUIRE(frames.front().ridgeScrubber->observedCpis == 1);
  REQUIRE(frames.front().ridgeScrubber->resetReason == "input_discontinuity");
  shadow.reset(); frames = shadow.process({dedicated_signal(2000)});
  REQUIRE(frames.front().ridgeScrubber->epoch == 3);
  REQUIRE(frames.front().ridgeScrubber->resetReason == "explicit_reset");
  auto rejected = dedicated_signal(2000); rejected.samples.firstInputSample = 4000;
  std::fill(rejected.samples.channels[0].begin(), rejected.samples.channels[0].end(),
    std::complex<float>{});
  REQUIRE(shadow.process({rejected}).empty());
  auto recovered = dedicated_signal(2000); recovered.samples.firstInputSample = 8000;
  frames = shadow.process({recovered});
  REQUIRE(frames.front().ridgeScrubber->epoch == 4);
  REQUIRE(frames.front().ridgeScrubber->observedCpis == 1);
  REQUIRE(frames.front().ridgeScrubber->resetReason == "clutter_reference_rejected");
}

TEST_CASE("active ridge mode ignores the shadow window and helper bounds fail closed")
{
  using namespace blah2;
  REQUIRE_THROWS_AS(DopplerRidgeShadowWindow(0), std::invalid_argument);
  REQUIRE_THROWS_AS(DopplerRidgeShadowWindow(65536), std::invalid_argument);
  DopplerRidgeShadowWindow window(1);
  window.observe(2.5); REQUIRE_FALSE(window.available());
  REQUIRE_THROWS_AS(window.observe(1), std::logic_error);
  window.reset("test_reset"); REQUIRE(window.available());
  REQUIRE(window.observed() == 0); REQUIRE(window.totalMs() == 0);
  const std::vector<channelizer::IlluminatorDefinition> definitions{
    {"only", 100000000, 10000, 20000, 16}};
  auto config = laneGpuConfig(); config.accelerationMode = "cpu";
  config.dopplerRidgeScrubber.mode = "active";
  config.dopplerRidgeScrubber.shadowMaximumCpis = 1;
  multi_illuminator::MultiIlluminatorRadar bounded(40000, 2, definitions, config);
  config.dopplerRidgeScrubber.shadowMaximumCpis = 256;
  multi_illuminator::MultiIlluminatorRadar original(40000, 2, definitions, config);
  for (uint64_t frame = 0; frame < 3; ++frame) {
    auto input = dedicated_signal(2000); input.samples.firstInputSample = frame * 4000;
    const auto actual = bounded.process({input}), expected = original.process({input});
    REQUIRE(actual.front().ridgeScrubber->comparisonAvailable);
    REQUIRE(actual.front().ridgeScrubber->state == "continuous");
    REQUIRE(actual.front().detections->to_json_km(1, 20000) ==
      expected.front().detections->to_json_km(1, 20000));
    REQUIRE(actual.front().tracks->to_json(1) == expected.front().tracks->to_json(1));
  }
}
