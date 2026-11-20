#include "process/analysis/MultiIlluminatorRadar.h"
#include "process/pipeline/RawIqStages.h"
#include "process/analysis/MultiIlluminatorPipelineState.h"
#include "process/analysis/MultiIlluminatorStatus.h"
#include "process/analysis/StartupIq.h"
#include "process/conditioning/FmReferenceQualityFilter.h"
#include "process/detection/Centroid.h"
#include "process/detection/DopplerRidgeAdmission.h"
#include "process/utility/FftwThreads.h"
#include "data/meta/Timing.h"
#include "capture/ProcessingTopology.h"
#include "process/analysis/NativeProductIdentity.h"
#include "process/ambiguity/ResidentLaneBudget.h"
#include "process/ambiguity/GpuLaneLease.h"

#include <catch2/catch_test_macros.hpp>
#include "process/ambiguity/GpuWorkerLifetime.h"
#include <catch2/catch_message.hpp>
#include <catch2/catch_approx.hpp>
#include <rapidjson/document.h>
#include <algorithm>
#include <cmath>
#include <complex>
#include <filesystem>
#include <fstream>
#include <limits>
#include <random>
#include <utility>
#include <vector>
#include <thread>
#include <type_traits>

namespace
{
constexpr double pi = 3.141592653589793238462643383279502884;

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

TEST_CASE("Ridge birth and temporal beam admission reject unavailable products")
{
  using blah2::detection::validateDopplerRidgeAdmission;
  using blah2::detection::validateTemporalRouteAdmission;
  for (const bool resident : {false, true}) {
    REQUIRE_NOTHROW(validateDopplerRidgeAdmission(resident, false));
    if (resident)
      REQUIRE_THROWS_AS(validateDopplerRidgeAdmission(resident, true), std::invalid_argument);
    else REQUIRE_NOTHROW(validateDopplerRidgeAdmission(resident, true));
    for (const bool active : {false, true}) for (const bool tracking : {false, true}) {
      if (resident && active && tracking)
        REQUIRE_NOTHROW(validateTemporalRouteAdmission(resident, true, active, tracking));
      else
        REQUIRE_THROWS_AS(validateTemporalRouteAdmission(resident, true, active, tracking), std::invalid_argument);
    }
  }
}

TEST_CASE("temporal preflight budgets use complete native lane geometry")
{
  using blah2::detection::temporalIntegrationBudget;
  for (const bool fractional : {false, true}) {
    const auto motion = [&](uint32_t delay, uint32_t doppler) {
      return track_before_detect::encodeMotionBins(delay, doppler, fractional);
    };
    const auto narrow = temporalIntegrationBudget(601, 1261, 8, motion(1, 1));
    REQUIRE(narrow.workingBytes == 218263968);
    REQUIRE(narrow.pathSamples == 95490486);
    REQUIRE(temporalIntegrationBudget(601, 1261, 8, motion(2, 2)).pathSamples == 265251350);
    REQUIRE_THROWS_AS(temporalIntegrationBudget(601, 1261, 8, motion(2, 4)), std::invalid_argument);
    REQUIRE(temporalIntegrationBudget(601, 1261, 4, motion(2, 4)).pathSamples == 204622470);
    REQUIRE(temporalIntegrationBudget(601, 511, 8, motion(2, 4)).pathSamples == 193479930);
    REQUIRE_THROWS_AS(temporalIntegrationBudget(601, 511, 8, motion(2, 16)), std::invalid_argument);
    REQUIRE_NOTHROW(temporalIntegrationBudget(1, 426088, 8, motion(2, 4)));
    REQUIRE_THROWS_AS(temporalIntegrationBudget(1, 426089, 8, motion(2, 4)), std::invalid_argument);
  }
  REQUIRE_THROWS_AS(temporalIntegrationBudget(0, 1, 8, 0), std::invalid_argument);
  REQUIRE_THROWS_AS(temporalIntegrationBudget(1024, 1025, 2, 0), std::invalid_argument);
  REQUIRE_NOTHROW(temporalIntegrationBudget(1, 932067, 8, 0));
  REQUIRE_THROWS_AS(temporalIntegrationBudget(1, 932068, 8, 0), std::invalid_argument);
}

TEST_CASE("immutable radar plans preserve physical intent across backends and temporal depths")
{
  using namespace blah2::pipeline;
  static_assert(!std::is_copy_assignable_v<RadarPlan>);
  static_assert(std::is_same_v<decltype(std::declval<RadarPlan>().products()), const ProductConfig&>);
  constexpr uint32_t rate = 20000, samples = 2000;
  auto shape = makeAmbiguity(-1, 10, -200, 200, rate, samples, true);
  std::map<uint32_t, std::string> identities;
  for (uint32_t depth : {1u, 2u, 4u, 8u}) for (const char* backend : {"cpu", "gpu", "gpu-resident", "auto"}) {
    RadarConfig intent;
    intent.accelerationMode = backend;
    intent.delayReferenceRateHz = 160000;
    intent.guardCells = 8; intent.trainingCells = 16;
    intent.minimumDelay = 8; intent.centroidRadius = 16;
    intent.centroidDopplerRadius = 7;
    intent.trackerN = 17; intent.trackerDelete = 10;
    intent.trackerNormalizedInnovationGate = false;
    intent.trackerRecovery = {true, 60, 12};
    intent.advanced.detectorIntegrationCpis = depth;
    intent.advanced.trackBeforeDetect = depth > 2;
    intent.advanced.detectorIntegrationMotionBins = depth > 2 ?
      track_before_detect::encodeMotionBins(1, 2, true) : depth == 2 ? 1 : 0;
    const auto lane = scaleRadarConfigForLane(intent, rate);
    const auto plan = resolveRadarPlan(lane, *shape, rate, 100000000, samples);
    auto replayLane = lane; replayLane.offlineReplay = true;
    REQUIRE(resolveRadarPlan(replayLane, *shape, rate, 100000000, samples).identity() == plan.identity());
    if (identities.count(depth)) REQUIRE(plan.identity() == identities.at(depth));
    else identities.emplace(depth, plan.identity());
    REQUIRE(ProductStages(plan).planIdentity() == plan.identity());
    const auto detector = plan.residentDetector();
    const auto tracker = plan.residentTracker(128);
    const auto& host = plan.products();
    REQUIRE(detector.guard == host.guard); REQUIRE(host.guard == 1);
    REQUIRE(detector.train == host.train); REQUIRE(host.train == 2);
    REQUIRE(detector.minDelay == host.minimumDelay); REQUIRE(host.minimumDelay == 1);
    REQUIRE(detector.centroidDelay == host.centroidDelay); REQUIRE(host.centroidDelay == 2);
    REQUIRE(detector.centroidDoppler == 7);
    REQUIRE(detector.pfa == intent.pfa);
    REQUIRE(detector.integrationCpis == depth);
    REQUIRE(detector.integrationMotionBins == intent.advanced.detectorIntegrationMotionBins);
    REQUIRE(tracker.m == host.trackerM); REQUIRE(tracker.n == host.trackerN);
    REQUIRE(tracker.nDelete == host.trackerDelete);
    REQUIRE(tracker.minInitiationSnrDb == host.minimumSnr);
    REQUIRE(tracker.rangeRes == 299792458.0 / rate);
    REQUIRE(tracker.lambda == 299792458.0 / 100000000);
    REQUIRE(tracker.cpi == shape->get_cpi());
    REQUIRE_FALSE(tracker.normalizedInnovationGate);
    REQUIRE(tracker.stationaryPersistenceCpis == 60);
    REQUIRE(tracker.stationaryMaxDopplerHz == 12);
    intent.pfa = .1;
    REQUIRE(plan.products().pfa == 1e-5);
  }
}

TEST_CASE("resolved radar identity changes for every owned policy field")
{
  using namespace blah2::pipeline;
  ProductConfig original;
  // Disable tracking so independent stage/depth mutations remain admissible.
  original.stages.tracking = false;
  const auto identity = RadarPlan(original).identity();
#define MUTATE_FIELD(field, value) { auto changed = original; changed.field = value; \
  INFO(#field); REQUIRE(RadarPlan(changed).identity() != identity); }
  MUTATE_FIELD(stages.clutter, false); MUTATE_FIELD(stages.detection, false);
  MUTATE_FIELD(stages.tracking, true); MUTATE_FIELD(pfa, .01);
  MUTATE_FIELD(guard, 3); MUTATE_FIELD(train, 7); MUTATE_FIELD(minimumDelay, 2);
  MUTATE_FIELD(minimumDoppler, 16); MUTATE_FIELD(cpiSeconds, .25);
  MUTATE_FIELD(centroidDelay, 7); MUTATE_FIELD(centroidDoppler, 7);
  MUTATE_FIELD(integrationCpis, 2); MUTATE_FIELD(integrationMotion, 1);
  MUTATE_FIELD(beamCount, 12); MUTATE_FIELD(beamTolerance, 1); MUTATE_FIELD(fineBeamFactor, 4);
  MUTATE_FIELD(delayDriftPerHz, .01); MUTATE_FIELD(trackerM, 2); MUTATE_FIELD(trackerN, 7);
  MUTATE_FIELD(trackerDelete, 11); MUTATE_FIELD(maximumAcceleration, 11);
  MUTATE_FIELD(rangeResolution, 2); MUTATE_FIELD(wavelength, 2); MUTATE_FIELD(minimumSnr, 6);
  MUTATE_FIELD(adaptiveGate, false); MUTATE_FIELD(normalizedInnovation, false);
  MUTATE_FIELD(ridgeBirth.enabled, true); MUTATE_FIELD(ridgeBirth.persistenceCpis, 13);
  MUTATE_FIELD(ridgeBirth.toleranceHz, 4); MUTATE_FIELD(ridgeBirth.minimumDelaySeparationKm, .5);
  MUTATE_FIELD(recovery.stationaryClutter, true); MUTATE_FIELD(recovery.persistenceCpis, 61);
  MUTATE_FIELD(recovery.maximumDopplerHz, 1); MUTATE_FIELD(recovery.maximumStationaryCells, 65537);
#undef MUTATE_FIELD
  rapidjson::Document parsed; parsed.Parse(identity.c_str());
  REQUIRE_FALSE(parsed.HasParseError());
  REQUIRE(parsed.MemberCount() == 36); // schema, fixed tracker policy and every configured field.
  REQUIRE(std::string(parsed["tracker_policy"].GetString())==tracker_policy::identity);
}

TEST_CASE("T06 resident stationary capacity refuses unsupported limits without retuning host intent")
{
  using namespace blah2::pipeline;
  for(uint32_t capacity:{1u,65535u,65536u,65537u}) {
    ProductConfig config;config.recovery.stationaryClutter=true;config.recovery.maximumStationaryCells=capacity;
    const RadarPlan plan(config);
    REQUIRE(plan.products().recovery.maximumStationaryCells==capacity);
    REQUIRE_NOTHROW(ProductStages(plan));
    if(capacity==tracker_policy::residentStationaryCapacity)REQUIRE_NOTHROW(plan.residentTracker(128));
    else REQUIRE_THROWS_AS(plan.residentTracker(128),std::invalid_argument);
    config.stages.tracking=false;
    REQUIRE_NOTHROW(RadarPlan(config).residentTracker(128));
  }
}

TEST_CASE("radar plan preserves zero centroid radius and refuses invalid stage intent")
{
  blah2::pipeline::ProductConfig intent;
  intent.centroidDelay = intent.centroidDoppler = 0;
  const auto detector = blah2::pipeline::RadarPlan(intent).residentDetector();
  REQUIRE(detector.centroidDelay == 0);
  REQUIRE(detector.centroidDoppler == 0);
  intent.stages.detection = false;
  REQUIRE_THROWS_AS(blah2::pipeline::RadarPlan(intent), std::invalid_argument);
  intent.stages.tracking = false;
  const auto disabled = blah2::pipeline::RadarPlan(intent);
  REQUIRE_FALSE(disabled.residentDetector().enabled);
  REQUIRE_FALSE(disabled.residentTracker(128).enabled);
  REQUIRE(blah2::pipeline::RadarPlan::detectorPfa(1e-5, 1, 1, 0) == 1e-5);
  REQUIRE(blah2::pipeline::RadarPlan::detectorPfa(1e-5, 12, 2, 1) == 1e-5 / 108);
  for (uint32_t depth : {4u, 8u})
    REQUIRE(blah2::pipeline::RadarPlan::detectorPfa(1e-5, 12, depth, 1) == 1e-5 / 12);
  auto shape = blah2::pipeline::makeAmbiguity(-1, 10, -200, 200, 20000, 2000, true);
  blah2::pipeline::RadarConfig spatial;
  spatial.trackerBeamTolerance = 1;
  spatial.coherentFusion = CoherentBeamBank::Config{};
  REQUIRE_THROWS_AS(blah2::pipeline::resolveRadarPlan(
    spatial, *shape, 20000, 100000000, 2000), std::invalid_argument);
  const auto qualified = blah2::pipeline::resolveRadarPlan(
    spatial, *shape, 20000, 100000000, 2000, true);
  REQUIRE(qualified.products().beamTolerance == 1);
  REQUIRE(qualified.residentDetector().integrationBeamTolerance == 1);
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

TEST_CASE("ATSC automatic selection shares the causal pilot path live and in replay")
{
  using blah2::conditioning::resolveAtscReferenceMode;
  for (const std::string standard : {"ATSC", "ATSC-1.0", "ATSC-3.0"}) {
    REQUIRE(resolveAtscReferenceMode(standard, "automatic", false) ==
      (standard == "ATSC-3.0" ? "raw" : "pilot_aided"));
    REQUIRE(resolveAtscReferenceMode(standard, "raw", true) == "raw");
    if (standard == "ATSC-3.0")
      REQUIRE_THROWS_AS(resolveAtscReferenceMode(standard, "pilot_aided", false), std::invalid_argument);
    else
      REQUIRE(resolveAtscReferenceMode(standard, "pilot_aided", false) == "pilot_aided");
  }
  REQUIRE(resolveAtscReferenceMode("ATSC-3.0", "automatic", true) == "raw");
  REQUIRE_THROWS_AS(resolveAtscReferenceMode("ATSC-3.0", "pilot_aided", true), std::invalid_argument);
  REQUIRE(resolveAtscReferenceMode("ATSC-1.0", "automatic", true) == "pilot_aided");
  REQUIRE(resolveAtscReferenceMode("ATSC-1.0", "automatic", true, true) == "raw");
  REQUIRE_THROWS_AS(resolveAtscReferenceMode("ATSC-1.0", "pilot_aided", true, true), std::invalid_argument);
  REQUIRE_THROWS_AS(resolveAtscReferenceMode("FM", "automatic", true), std::invalid_argument);
}

TEST_CASE("host-conditioned AUTO lanes retain the portable GPU route")
{
  using blah2::pipeline::shouldAttemptResidentFrontend;
  REQUIRE(shouldAttemptResidentFrontend("auto", true, true));
  REQUIRE_FALSE(shouldAttemptResidentFrontend("auto", true, false));
  REQUIRE_FALSE(shouldAttemptResidentFrontend("gpu", true, true));
  REQUIRE(shouldAttemptResidentFrontend("gpu-resident", true, true));
  REQUIRE_FALSE(shouldAttemptResidentFrontend("gpu-resident", true, false));
}

TEST_CASE("AUTO retains portable eligibility after optional resident absence or failure")
{
  using blah2::pipeline::shouldAttemptResidentFrontend;
  using blah2::pipeline::shouldAttemptPortableFrontend;
  // FP64 enumeration is insufficient when the optional resident module was
  // disabled. Portable FP32 admission remains independent of that build choice.
  REQUIRE_FALSE(shouldAttemptResidentFrontend("auto", true, true, false));
  for (const char* reason : {"disabled module", "module unavailable", "initialization failed",
      "qualification failed", "runtime failed"}) {
    CAPTURE(reason);
    REQUIRE(shouldAttemptPortableFrontend("auto", false, false));
  }
  REQUIRE_FALSE(shouldAttemptPortableFrontend("auto", true, false));
  REQUIRE_FALSE(shouldAttemptPortableFrontend("auto", false, true));
  REQUIRE_FALSE(shouldAttemptPortableFrontend("gpu-resident", false, false));
  REQUIRE_FALSE(shouldAttemptPortableFrontend("cpu", false, false));
  REQUIRE(shouldAttemptPortableFrontend("gpu", false, false));
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

  state.gpu_requalified();
  const auto recovered = state.acceleration_status("auto", true, true);
  REQUIRE_FALSE(state.runtime_fallback_active());
  REQUIRE(state.fallback_reason().empty());
  REQUIRE(recovered.active == "vulkan-wideband");
  REQUIRE(recovered.state == "active");
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

TEST_CASE("optional wideband qualification failure continues explicit GPU lanes")
{
  blah2::multi_illuminator::PendingPublication<std::string> pending;
  blah2::multi_illuminator::PipelineState state;
  unsigned firResets=0,radarResets=0;
  state.runtime_fallback("wideband deadline",[&]{++firResets;},
    [&]{++radarResets;});
  const auto status=state.acceleration_status("gpu",false,true);
  REQUIRE(status.requested=="gpu"); // Downstream lane policy is unchanged.
  REQUIRE(status.active=="cpu");REQUIRE(status.state=="fallback");
  REQUIRE(firResets==1);REQUIRE(radarResets==1);
  pending.store("failed-capture-cpu",state.publication(310,{},true,7,
    100,200,{}, {},1.0,true,false,true,status));
  auto failed = pending.take();
  REQUIRE(failed.first == "failed-capture-cpu");
  REQUIRE(failed.second.timestampMs == 310);
  REQUIRE(failed.second.captureFirst==100);
  REQUIRE(failed.second.inputDiscontinuity);
  REQUIRE(failed.second.widebandCpuExecuted);
  REQUIRE_FALSE(failed.second.widebandGpuExecuted);
  pending.store("next-capture-cpu",state.publication(320,{},true,7,
    200,300,{}, {},1.0,false,false,true,status));
  auto next=pending.take();
  REQUIRE(next.first=="next-capture-cpu");
  REQUIRE(next.second.captureFirst==200);
  REQUIRE_FALSE(next.second.inputDiscontinuity);
  REQUIRE_FALSE(pending.has_value());
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
  auto stationary = config;
  stationary.stationaryClutter = true;
  REQUIRE_NOTHROW(validateAdvancedFeatures(stationary));
  auto bearing = config; bearing.bearing = true;
  REQUIRE_NOTHROW(validateAdvancedFeatures(bearing));
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
    }
    REQUIRE_THROWS_AS(warmed.prepareStartup(), std::logic_error);
  }
}

TEST_CASE("canonical temporal products refuse an unintegrated alternate route")
{
  for(const uint32_t depth:{2u,4u,8u}){
    blah2::pipeline::ProductConfig config;
    config.stages.tracking=false;config.integrationCpis=depth;
    blah2::pipeline::ProductStages products{blah2::pipeline::RadarPlan(config)};
    auto map=std::make_unique<Map<std::complex<double>>>(3,16);
    REQUIRE_THROWS_AS(products.process(map,{1,0,2000,100,true},false),std::logic_error);
  }
}

TEST_CASE("direct and channelized ingress share canonical products and disabled stages")
{
  REQUIRE(fftw_init_threads());
  blah2::set_fftw_planner_threads(1);
  using namespace blah2;
  constexpr uint32_t captureRate = 40000, rate = 20000, samples = 2000;
  constexpr uint64_t frequency = 100000000;
  const std::vector<iq::SignalDefinition> definitions{
    {"canonical", frequency, 10000, rate, 16}};
  const std::vector<pipeline::Stages> selections{
    {true, true, true}, {false, true, true},
    {true, true, false}, {false, false, false}};
  for (const auto selected : selections) for (const auto depth : {1u, 2u, 4u, 8u})
      for (const bool doubleInput : {false, true}) for (const bool privateControls : {false,true}) {
    if(privateControls&&(!selected.clutter||!selected.detection||!selected.tracking||doubleInput))continue;
    CAPTURE(selected.clutter, selected.detection, selected.tracking, depth, doubleInput, privateControls);
    auto config = smallLaneConfig();
    config.stages = selected;
    config.offlineReplay = true;
    config.borrowedClutter = false;
    config.accelerationMode = "cpu";
    config.trackerMinimumSnrDb = -100;
    if(privateControls)config.pfa=1e-6;
    // Preserve the same initiation threshold while admitting the requested
    // disjoint temporal support's minimum deletion horizon.
    config.trackerDelete = std::max(config.trackerDelete, depth - 1);
    config.advanced.detectorIntegrationCpis = depth;
    config.advanced.trackBeforeDetect = depth > 2;
    config.advanced.detectorIntegrationMotionBins = depth > 2 ?
      track_before_detect::encodeMotionBins(1, 1, false) : 0;
    pipeline::RadarBank bank(captureRate, 2, definitions, config);
    auto liveConfig = config; liveConfig.offlineReplay = false;
    pipeline::RadarBank liveBank(captureRate, 2, definitions, liveConfig);
    channelizer::IlluminatorChannelizerBank ingress(frequency, captureRate, 2, definitions);
    IqData reference(samples), surveillance(samples);
    std::vector<IqData*> channels{&surveillance};
    auto ambiguity = pipeline::makeAmbiguity(config.delayMin, config.delayMax,
      config.dopplerMin, config.dopplerMax, rate, samples, true);
    std::vector<Ambiguity*> processors{ambiguity.get()};
    pipeline::RawIqStages frontend({config.clutterDelayMin, config.clutterDelayMax,
      samples, 1, 1, false, config.reference});
    pipeline::ProductStages products(pipeline::resolveRadarPlan(
      config, *ambiguity, rate, frequency, samples));
    char observerDirectory[] = "/tmp/vectorwarp-plan-parity-XXXXXX";
    REQUIRE(mkdtemp(observerDirectory));
    qualification_observer::Config observed;
    observed.directory = observerDirectory; observed.summaryOnly = true;
    observed.maxMapCells = 4194304;
    bank.setQualificationObserver(std::make_shared<qualification_observer::Observer>(observed, true, true));
    Noncoherent fusion;
    uint64_t epoch = 1;
    const uint64_t cycle = privateControls?2*depth:std::max(2u, depth);
    for (uint64_t frame = 0; frame < 2 * cycle; ++frame) {
      // Reach every requested bank, then skip three CPIs and repeat warm-up.
      const uint64_t phase = frame % cycle;
      const uint64_t first = (frame + (frame >= cycle ? 3 : 0)) * samples * 2;
      if (frame == cycle) {
        frontend.resetReference(); products.reset("input_discontinuity"); ++epoch;
      }
      auto wideband = signal("canonical", frequency, captureRate, 1, 2,
        samples * 2, 16, 30, 170 + uint32_t(frame));
      if(privateControls){
        IqData a(samples*2),b(samples*2);
        const double trials=depth>2?2*track_before_detect::trialCount(depth,
          config.advanced.detectorIntegrationMotionBins):(depth==2?2:1);
        const double alpha=detection::integrated_cfar::alpha(config.pfa/trials,
          2*config.trainingCells,depth);
        const startup::Target target{32,30,double(captureRate),16*std::sqrt(alpha),{{0,0},{1,0}}};
        startup::fillTargetIq({&a,&b},samples*2,uint32_t(phase),target,phase>=depth);
        for(size_t channel=0;channel<2;++channel){
          const auto& values=(channel?b:a).view_data();
          for(size_t sample=0;sample<values.size();++sample)
            wideband.samples.channels[channel][sample]=std::complex<float>(values[sample]);
        }
      }
      auto blocks = ingress.process(wideband.samples.channels, first);
      REQUIRE(blocks.size() == 1);
      auto& block = blocks.front().samples;
      REQUIRE(block.channels.front().size() == samples);
      if (doubleInput) {
        using Storage = std::vector<std::vector<std::complex<double>>>;
        Storage precise(block.channels.size());
        for (size_t channel = 0; channel < precise.size(); ++channel)
          for (const auto value : block.channels[channel])
            precise[channel].emplace_back(std::nextafter(double(value.real()), INFINITY),
              std::nextafter(double(value.imag()), -INFINITY));
        auto storage = std::make_shared<const Storage>(std::move(precise));
        iq::BorrowedSamples::DoubleChannels views;
        for (const auto& channel : *storage) views.emplace_back(channel.data(), channel.size());
        block.borrowed = iq::BorrowedSamples{storage, std::move(views)};
        block.channels.clear();
      }
      // The one-lane path consumes the identical normalized samples, without
      // inheriting any channelizer/receiver identity or a different DSP graph.
      auto assignNormalized = [samples](IqData& target, const auto& input) {
        auto& output = target.resize_for_write(samples);
        for (uint32_t index = 0; index < samples; ++index) output[index] = input[index];
      };
      block.visitChannel(0, [&](const auto& view) { assignNormalized(reference, view); });
      block.visitChannel(1, [&](const auto& view) { assignNormalized(surveillance, view); });
      REQUIRE(pipeline::processFrontend(frontend, nullptr, reference, channels,
        processors, selected.clutter, false));
      std::unique_ptr<Map<std::complex<double>>> directMap;
      pipeline::fuseMaps({ambiguity->result()}, fusion, nullptr, directMap);
      const uint64_t end = block.firstInputSample + uint64_t(samples) * 2;
      const auto timestamp = detection::signalTimeMilliseconds(end, captureRate);
      auto direct = products.process(directMap,
        {epoch, block.firstInputSample, end, timestamp, true});
      const auto actual = bank.process(blocks, pipeline::LiveTemporalIdentity{timestamp, epoch, true, {}});
      const auto live = liveBank.process(blocks, pipeline::LiveTemporalIdentity{timestamp, epoch, true, {}});
      REQUIRE(actual.size() == 1);
      REQUIRE(live.size() == 1);
      const auto& lane = actual.front();
      REQUIRE(lane.map->data == live.front().map->data);
      REQUIRE(lane.firstInputSample == live.front().firstInputSample);
      REQUIRE(lane.endInputSample == live.front().endInputSample);
      REQUIRE(bool(lane.detectorIntegration) == bool(live.front().detectorIntegration));
      if (lane.detectorIntegration) {
        REQUIRE(lane.detectorIntegration->ready == live.front().detectorIntegration->ready);
        REQUIRE(lane.detectorIntegration->window.supportSequences() ==
          live.front().detectorIntegration->window.supportSequences());
      }
      if (selected.detection)
        REQUIRE(lane.detections->to_json(timestamp) == live.front().detections->to_json(timestamp));
      if (selected.tracking)
        REQUIRE(lane.tracks->to_json(timestamp) == live.front().tracks->to_json(timestamp));
      REQUIRE(lane.inputProcessing.widenedSamples == (doubleInput ? 0 : 2 * samples));
      REQUIRE(lane.inputProcessing.copiedSamples == (doubleInput ? 2 * samples : 0));
      REQUIRE(lane.map->data == directMap->data);
      REQUIRE(lane.map->delay == directMap->delay);
      REQUIRE(lane.map->doppler == directMap->doppler);
      REQUIRE(lane.detectionEnabled == selected.detection);
      REQUIRE(lane.trackingEnabled == selected.tracking);
      REQUIRE(lane.clutterCpuExecuted == selected.clutter);
      REQUIRE(lane.ambiguityCpuExecuted);
      REQUIRE_FALSE(lane.clutterGpuExecuted);
      REQUIRE_FALSE(lane.ambiguityGpuExecuted);
      REQUIRE_FALSE(lane.residentProducts);
      REQUIRE(bool(lane.detectorIntegration) == (selected.detection && depth > 1));
      REQUIRE(bool(lane.detectorIntegration) == bool(direct.integration));
      if (direct.integration) {
        REQUIRE(lane.detectorIntegration->cpis == depth);
        REQUIRE(std::string(lane.detectorIntegration->backend) == "cpu");
        REQUIRE(lane.detectorIntegration->availableCpis == std::min<uint64_t>(phase + 1,depth));
        REQUIRE(lane.detectorIntegration->ready == direct.integration->ready);
        REQUIRE(lane.detectorIntegration->availableCpis == direct.integration->availableCpis);
        REQUIRE(lane.detectorIntegration->window.epoch == direct.integration->window.epoch);
        REQUIRE(lane.detectorIntegration->window.newestSequence == phase + 1);
        REQUIRE(lane.detectorIntegration->window.supportSequences() ==
          direct.integration->window.supportSequences());
        REQUIRE(lane.detectorIntegration->ready == (phase >= 1));
        REQUIRE(lane.detectorIntegration->firstInputSample == direct.integration->firstInputSample);
        REQUIRE(lane.detectorIntegration->endInputSample == end);
        if (depth > 2) {
          REQUIRE(lane.detectorIntegration->evidence);
          REQUIRE(direct.integration->evidence);
          for (size_t index = 0; index < 4; ++index) {
            const auto& actualBank = lane.detectorIntegration->evidence->banks[index];
            const auto& directBank = direct.integration->evidence->banks[index];
            const uint32_t bankDepth = index < 2 ? 2 : 1u << index;
            REQUIRE(actualBank.depth == bankDepth);
            REQUIRE(actualBank.ready == (bankDepth <= depth && bankDepth <= phase + 1));
            REQUIRE(actualBank.ready == directBank.ready);
            if (!actualBank.ready) continue;
            REQUIRE(actualBank.window.epoch == directBank.window.epoch);
            REQUIRE(actualBank.window.newestSequence == phase + 1);
            REQUIRE(actualBank.window.supportSequences() == directBank.window.supportSequences());
            REQUIRE(actualBank.map->data == directBank.map->data);
            REQUIRE(actualBank.validCells == directBank.validCells);
            for (uint32_t age = 0; age < bankDepth; ++age)
              REQUIRE(actualBank.window.sequences[age] == phase + 1 - age);
          }
        }
      }
      if (selected.detection)
        REQUIRE(lane.detections->to_json(1) == direct.detections->to_json(1));
      else REQUIRE(lane.detections->get_nDetections() == 0);
      if (selected.tracking)
        REQUIRE(lane.tracks->to_json(1) == direct.tracks->to_json(1));
      else REQUIRE(lane.tracks->get_n() == 0);
      if(privateControls){
        if(phase<depth){REQUIRE(lane.detections->get_nDetections()==0);REQUIRE(lane.tracks->get_n()==0);}
        if(phase+1==cycle){REQUIRE(lane.detections->get_nDetections()>0);REQUIRE(lane.tracks->get_n()>0);}
      }
    }
    std::ifstream observations(std::filesystem::path(observerDirectory) / "coherent-observer.jsonl");
    std::string row; uint64_t observedFrames = 0;
    while (std::getline(observations, row)) {
      rapidjson::Document value; value.Parse(row.c_str());
      REQUIRE_FALSE(value.HasParseError());
      REQUIRE(std::string(value["resolved_product_plan"].GetString()) == products.planIdentity());
      if (std::string(value["stage"].GetString()) == "products") ++observedFrames;
    }
    REQUIRE(observedFrames == 2 * cycle);
    std::filesystem::remove_all(observerDirectory);
  }
}

TEST_CASE("private qualification stimulus is deterministic bounded and separate from noise control")
{
  using namespace blah2;
  IqData reference(256),surveillance(256),controlReference(256),controlSurveillance(256);
  startup::Target target{16,30,20000,32,{{0,0},{1,0}}};
  startup::fillTargetIq({&reference,&surveillance},256,0,target,true);
  startup::fillTargetIq({&controlReference,&controlSurveillance},256,0,target,false);
  REQUIRE(reference.view_data()==controlReference.view_data());
  REQUIRE(surveillance.view_data()!=controlSurveillance.view_data());
  const auto saved=surveillance.get_data();
  startup::fillTargetIq({&reference,&surveillance},256,0,target,true);
  REQUIRE(surveillance.view_data()==saved);
  for(const auto value:saved){REQUIRE(std::isfinite(value.real()));REQUIRE(value.real()==double(float(value.real())));}
  target.amplitude=4097;
  REQUIRE_THROWS_AS(startup::fillTargetIq({&reference,&surveillance},256,0,target,true),std::invalid_argument);
  target.amplitude=32;target.delay=64;
  REQUIRE_THROWS_AS(startup::fillTargetIq({&reference,&surveillance},256,0,target,true),std::invalid_argument);
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
    value->delay = {0, 1, 2, 3, 4};
    value->doppler = {-2, -1, 0, 1, 2};
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
    Detection empty(std::vector<double>{}, std::vector<double>{}, std::vector<double>{});
    REQUIRE(empty.get_nDetections() == 0);
    REQUIRE(trackProducts(empty, tracker, 1100, gap)->get_n() == 0);
    integration.reset();
    map = makeMap();
    REQUIRE_FALSE(integration.process(map, 3, 100, 200, true).ready);
  }
}

TEST_CASE("tracker sample progress survives missing metadata and source epoch resets")
{
  using blah2::detection::SignalProgressClock;
  SignalProgressClock progress;
  Tracker tracker(4, 6, 3, .5, 10, 1, 1, -100, true);
  Detection candidate({10}, {0}, {30});
  for (uint64_t frame = 1; frame <= 9; ++frame) {
    const bool positioned = frame < 3 || frame > 5;
    const uint64_t epoch = frame < 8 ? 41 : 42;
    const uint64_t end = frame < 8 ? 9600000000ULL + frame * 1200000 :
      (frame - 7) * 1200000;
    const auto stamp = progress.advance(1200000, 2400000, positioned, epoch, end);
    REQUIRE(stamp == frame * 500);
    auto tracks = tracker.process(&candidate, stamp);
    REQUIRE(tracks->get_n() == 1);
    REQUIRE(tracks->get_nAssociated(0) == frame);
    if (frame >= 4) REQUIRE(tracks->get_state(0) == "ACTIVE");
  }
  const auto duplicate = progress.advance(1200000, 2400000, true, 42, 2400000);
  REQUIRE(duplicate == 4500);
  REQUIRE(tracker.process(&candidate, duplicate)->get_nAssociated(0) == 9);
  // Real skipped source samples retain elapsed motion; the missing metadata
  // frames already accounted above must not be charged a second time.
  REQUIRE(progress.advance(1200000, 2400000, true, 42, 6000000) == 6000);
  REQUIRE(progress.advance(1200000, 2400000, false) == 6500);
  REQUIRE(progress.advance(1200000, 2400000, true, 42, 10800000) == 8000);
  REQUIRE_THROWS_AS(progress.advance(0, 2400000), std::invalid_argument);
  REQUIRE_THROWS_AS(progress.advance(1, 0), std::invalid_argument);
  SignalProgressClock overflow;
  REQUIRE_THROWS_AS(overflow.advance(UINT64_MAX, 1), std::invalid_argument);
  REQUIRE(overflow.advance(1200000, 2400000) == 500);
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

TEST_CASE("consumer cycle timing includes omitted phases and retains explicit completed identity")
{
  using Clock = std::chrono::steady_clock;
  const auto at = [](int milliseconds) {
    return Clock::time_point{} + std::chrono::milliseconds(milliseconds);
  };
  Timing timing(0);
  rapidjson::Document json;
  timing.complete_processing_cycle(at(0)); // An empty polling iteration.
  timing.update(100, {250}, {"cpi"});
  timing.record_processing_cycle(at(0), at(10), at(260), at(300));
  json.Parse(timing.to_json().c_str());
  REQUIRE_FALSE(json.HasMember("processingCycle"));
  timing.complete_processing_cycle(at(400));
  timing.complete_processing_cycle(at(900)); // Never count the same CPI twice.
  timing.update(200, {350}, {"cpi"});
  json.Parse(timing.to_json().c_str());
  const auto& first = json["processingCycle"];
  REQUIRE(json["nCpi"].GetUint64() == 2);
  REQUIRE(json["cpi"].GetDouble() == 350);
  REQUIRE(first["lastCompletedNCpi"].GetUint64() == 1);
  REQUIRE(first["lastCompletedTimestamp"].GetUint64() == 100);
  REQUIRE(first["processing"]["samples"].GetUint64() == 1);
  REQUIRE(first["processing"]["latestMs"].GetDouble() == 400);
  REQUIRE(first["capture_prepare"]["latestMs"].GetDouble() == 10);
  REQUIRE(first["pipeline"]["latestMs"].GetDouble() == 250);
  REQUIRE(first["publication"]["latestMs"].GetDouble() == 40);
  REQUIRE(first["post_publication"]["latestMs"].GetDouble() == 100);
  REQUIRE(first["completion_interval"]["samples"].GetUint64() == 0);

  timing.record_processing_cycle(at(1000), at(1020), at(1370), at(1400));
  timing.complete_processing_cycle(at(1500));
  json.Parse(timing.to_json().c_str());
  const auto& second = json["processingCycle"];
  REQUIRE(second["lastCompletedNCpi"].GetUint64() == 2);
  REQUIRE(second["processing"]["samples"].GetUint64() == 2);
  REQUIRE(second["processing"]["totalMs"].GetDouble() == 900);
  REQUIRE(second["processing"]["meanMs"].GetDouble() == 450);
  REQUIRE(second["processing"]["maximumMs"].GetDouble() == 500);
  REQUIRE(second["pipeline"]["totalMs"].GetDouble() == 600);
  REQUIRE(second["completion_interval"]["samples"].GetUint64() == 1);
  REQUIRE(second["completion_interval"]["latestMs"].GetDouble() == 1100);
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
      REQUIRE(acquired); ++calls;
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
  // Independent portable workers own separate queues/fences and must not
  // serialize their host FP64 refinement through the aggregate budget lock.
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
  bank.detectorPower.assign(9 * 21, 1);
  for (int column = 0; column < 21; ++column) bank.map->delay.push_back(column);
  for (int row = 0; row < 9; ++row) {
    bank.map->doppler.push_back(row - 4);
    for (auto& cell : bank.map->data[row]) cell = {1, 0};
  }
  bank.map->data[6][10] = {30, 0};
  bank.map->data[6][9] = {3, 0}; bank.map->data[6][11] = {5, 0};
  bank.detectorPower[6 * 21 + 10] = 900;
  bank.detectorPower[6 * 21 + 9] = 9;
  bank.detectorPower[6 * 21 + 11] = 25;
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
      REQUIRE(frame.detections->get_delay() == interpolated->get_delay());
      REQUIRE(frame.detections->get_doppler() == interpolated->get_doppler());
      REQUIRE(frame.detections->get_snr() == interpolated->get_snr());
      REQUIRE(frame.detections->get_evidence().size() == frame.detections->get_nDetections());
      for (const auto& lineage : frame.detections->get_evidence()) {
        REQUIRE(lineage.epoch == metrics.window.epoch);
        REQUIRE(lineage.newestSequence == cpi + 1);
        REQUIRE(lineage.depth == 2);
        REQUIRE(lineage.bank == (motion ? 2 : 1));
        REQUIRE(lineage.delayMotion == metrics.window.delayMotion);
        REQUIRE(lineage.dopplerMotion == metrics.window.dopplerMotion);
        REQUIRE(lineage.sequences == metrics.window.sequences);
      }
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
using SharedSignalStorage=std::vector<std::vector<std::complex<float>>>;
std::shared_ptr<SharedSignalStorage> borrowSignal(
    blah2::channelizer::IlluminatorOutput& signal) {
  auto storage=std::make_shared<SharedSignalStorage>(std::move(signal.samples.channels));
  signal.samples.channels.clear();
  blah2::iq::BorrowedSamples borrowed;borrowed.owner=storage;
  for(const auto& channel:*storage)
    std::get<blah2::iq::BorrowedSamples::FloatChannels>(borrowed.channels).push_back({channel.data(),channel.size()});
  signal.samples.borrowed=std::move(borrowed);
  return storage;
}
}

TEST_CASE("canonical shared signal views reject ambiguous invalid or unpaired storage")
{
  using namespace blah2;
  const std::vector<channelizer::IlluminatorDefinition> definitions{
    {"only",100000000,10000,20000,16}};
  auto config=laneGpuConfig();config.accelerationMode="cpu";
  multi_illuminator::MultiIlluminatorRadar radar(40000,2,definitions,config);
  for(const std::string fault:{"owner","mixed","null","unpaired"}){
    CAPTURE(fault);
    auto input=dedicated_signal(2000);
    auto storage=borrowSignal(input);
    if(fault=="owner")input.samples.borrowed->owner.reset();
    if(fault=="mixed")input.samples.channels.resize(2);
    auto& views=std::get<iq::BorrowedSamples::FloatChannels>(input.samples.borrowed->channels);
    if(fault=="null")views[0].values=nullptr;
    if(fault=="unpaired")--views[0].count;
    REQUIRE_THROWS_AS(radar.process({input}),std::invalid_argument);
  }
  auto valid=dedicated_signal(2000);borrowSignal(valid);
  REQUIRE(valid.samples.channels.empty());
  REQUIRE(valid.samples.channelCount()==2);
  REQUIRE_THROWS_AS(valid.samples.channel(0).at(2000),std::out_of_range);
  REQUIRE(radar.process({valid}).size()==1);
}

TEST_CASE("canonical leased ingestion preserves products across partial gap and multi CPI blocks")
{
  using namespace blah2;
  const std::vector<channelizer::IlluminatorDefinition> definitions{
    {"only",100000000,10000,20000,16}};
  for(const bool directFloat:{false,true}){
    CAPTURE(directFloat);
    auto config=laneGpuConfig();config.accelerationMode="cpu";
    config.borrowedClutter=directFloat;
    config.pfa=.001;config.trackerM=1;config.trackerN=1;
    config.trackerDelete=2;config.trackerMinimumSnrDb=-100;
    multi_illuminator::MultiIlluminatorRadar owned(40000,2,definitions,config),
      leased(40000,2,definitions,config);
    const auto compare=[&](channelizer::IlluminatorOutput input,size_t frames){
      const auto expected=owned.process({input});
      auto storage=borrowSignal(input);
      std::weak_ptr<const void> lifetime=storage;
      std::vector<channelizer::IlluminatorOutput> block;
      block.push_back(std::move(input));
      const auto actual=leased.process(std::move(block));
      // Partial-CPI retention must have copied, and complete-CPI observers must
      // have finished. Deliberately overwrite/release the former lease now.
      for(auto& channel:*storage)
        std::fill(channel.begin(),channel.end(),std::complex<float>{123,-456});
      storage.reset();REQUIRE(lifetime.expired());
      REQUIRE(actual.size()==frames);REQUIRE(expected.size()==frames);
      for(size_t frame=0;frame<frames;++frame){
        compareLaneMaps(actual[frame],expected[frame]);
        REQUIRE(actual[frame].map->data==expected[frame].map->data);
        REQUIRE(actual[frame].inputProcessing.dequeMaterializations == 0);
        REQUIRE(actual[frame].inputProcessing.materializedSamples == 0);
        REQUIRE(actual[frame].inputProcessing.copiedSamples == 0);
        REQUIRE(actual[frame].inputProcessing.contiguousAllocations == 0);
        REQUIRE(actual[frame].inputProcessing.contiguousSamples ==
          actual[frame].inputProcessing.widenedSamples);
        REQUIRE(std::isfinite(actual[frame].inputProcessing.ingestionMs));
        REQUIRE(actual[frame].detections->to_json_km(1234,20000)==
          expected[frame].detections->to_json_km(1234,20000));
        REQUIRE(actual[frame].tracks->to_json(1234)==expected[frame].tracks->to_json(1234));
      }
    };
    compare(dedicated_signal(2000),1);
    auto half=dedicated_signal(1000);half.samples.firstInputSample=4000;
    compare(half,0);
    half.samples.firstInputSample=6000;compare(half,1);
    half.samples.firstInputSample=8000;compare(half,0);
    auto gap=dedicated_signal(2000);gap.samples.firstInputSample=16000;
    compare(gap,1); // Discards the old half, with identical adaptive reset.
    auto multiple=dedicated_signal(4000);multiple.samples.firstInputSample=20000;
    compare(multiple,2);
    owned.reset();leased.reset();compare(dedicated_signal(2000),1);
  }
}

TEST_CASE("FP64 bank ingestion preserves products through partial CPIs rejection gaps and reset")
{
  using namespace blah2;
  const std::vector<iq::SignalDefinition> definitions{{"only",100000000,10000,20000,16}};
  using Storage = std::vector<std::vector<std::complex<double>>>;
  const auto raw = dedicated_signal(4000);
  Storage values(2);
  for (size_t channel = 0; channel < 2; ++channel)
    for (const auto sample : raw.samples.channels[channel])
      values[channel].emplace_back(std::nextafter(double(sample.real()), INFINITY),
        std::nextafter(double(sample.imag()), -INFINITY));
  auto storage = std::make_shared<const Storage>(std::move(values));
  const auto input = [&](size_t offset, size_t count, uint64_t first, bool zeros = false) {
    auto owner = zeros ? std::make_shared<const Storage>(2,
      std::vector<std::complex<double>>(4000)) : storage;
    iq::SignalBlock block;
    block.id = "only"; block.frequencyHz = 100000000;
    block.samples.firstInputSample = first; block.samples.hasFirstInputSample = true;
    block.samples.inputSamplesPerOutput = 2;
    iq::BorrowedSamples::DoubleChannels views;
    for (const auto& channel : *owner) views.emplace_back(channel.data() + offset, count);
    block.samples.borrowed = iq::BorrowedSamples{owner, std::move(views)};
    return block;
  };
  for (const bool borrowed : {false, true}) {
    CAPTURE(borrowed);
    auto config = smallLaneConfig(); config.borrowedClutter = borrowed;
    config.trackerMinimumSnrDb = -100; config.advanced.detectorIntegrationCpis = 2;
    pipeline::RadarBank whole(40000, 2, definitions, config),
      split(40000, 2, definitions, config);
    const auto compare = [&](uint64_t first) {
      auto expected = whole.process({input(0, 4000, first)});
      REQUIRE(split.process({input(0, 700, first)}).empty());
      auto actual = split.process({input(700, 3300, first + 1400)});
      REQUIRE(expected.size() == 2); REQUIRE(actual.size() == expected.size());
      for (size_t frame = 0; frame < actual.size(); ++frame) {
        compareLaneMaps(actual[frame], expected[frame]);
        REQUIRE(actual[frame].map->data == expected[frame].map->data);
        REQUIRE(actual[frame].inputProcessing.widenedSamples == 0);
        REQUIRE(actual[frame].inputProcessing.copiedSamples == 4000);
        REQUIRE(actual[frame].detections->to_json(1) == expected[frame].detections->to_json(1));
        REQUIRE(actual[frame].tracks->to_json(1) == expected[frame].tracks->to_json(1));
        REQUIRE(actual[frame].detectorIntegration->ready == expected[frame].detectorIntegration->ready);
        REQUIRE(actual[frame].detectorIntegration->window.epoch == expected[frame].detectorIntegration->window.epoch);
        REQUIRE(actual[frame].detectorIntegration->window.supportSequences() ==
          expected[frame].detectorIntegration->window.supportSequences());
      }
      REQUIRE(actual.back().detections->get_nDetections() > 0);
      REQUIRE(actual.back().tracks->get_n() > 0);
    };
    compare(0);
    REQUIRE(whole.process({input(0, 700, 8000)}).empty());
    REQUIRE(split.process({input(0, 700, 8000)}).empty());
    compare(16000); // Both discard the same partial CPI at a sample gap.
    REQUIRE(whole.process({input(0, 2000, 24000, true)}).empty());
    REQUIRE(split.process({input(0, 2000, 24000, true)}).empty());
    compare(28000); // Rejected reference cannot retain temporal/tracker history.
    whole.reset(); split.reset(); compare(0);
    REQUIRE_THROWS_AS(whole.process({input(0, 2000, UINT64_MAX - 10)}), std::overflow_error);
  }
  // A mock FP32-only transport proves refusal occurs before any GPU dispatch.
  for (const std::string mode : {"gpu", "auto"}) {
    auto config = laneGpuConfig(); config.accelerationMode = mode;
    auto state = std::make_shared<LaneGpuState>();
    pipeline::RadarBank radar(40000, 2, definitions, config,
      [state](const GpuGeometry& geometry, const std::string&) {
        return std::make_unique<LaneGpuDouble>(geometry, state);
      });
    REQUIRE_THROWS_AS(radar.process({input(0, 2000, 0)}), std::invalid_argument);
    REQUIRE(state->clutterCalls == 0); REQUIRE(state->ambiguityCalls == 0);
  }
}

TEST_CASE("canonical pending publication pins shared data through EOF drain and reset")
{
  using namespace blah2;
  multi_illuminator::PendingPublication<std::vector<channelizer::IlluminatorOutput>> pending;
  auto input=dedicated_signal(2000);
  auto owner=borrowSignal(input);std::weak_ptr<const void> lifetime=owner;
  const auto expected=(*owner)[0][7];owner.reset();
  multi_illuminator::PublicationContext context;
  context.captureEpoch=23;context.captureFirst=8000;context.captureEnd=12000;
  input.samples.firstInputSample=context.captureFirst;
  std::vector<channelizer::IlluminatorOutput> block;block.push_back(std::move(input));
  pending.store(std::move(block),context);
  REQUIRE_FALSE(lifetime.expired());
  {
    auto drained=pending.take(); // The same take path is used by EOF draining.
    REQUIRE_FALSE(pending.has_value());REQUIRE_FALSE(lifetime.expired());
    REQUIRE(drained.second.captureEpoch==23);
    REQUIRE(drained.second.captureFirst==drained.first[0].samples.firstInputSample);
    REQUIRE(drained.first[0].samples.channel(0)[7]==expected);
    pending.store(std::move(drained.first),std::move(drained.second));
  }
  REQUIRE_FALSE(lifetime.expired());pending.reset();REQUIRE(lifetime.expired());
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

TEST_CASE("portable replacement cannot double a retired resident GPU reservation")
{
  using namespace blah2;
  auto config = laneGpuConfig(); config.accelerationMode = "auto";
  config.accelerationDevice = "auto";
  const std::vector<channelizer::IlluminatorDefinition> definitions{
    {"first", 100000000, 10000, 20000, 16}};
  unsigned attempts = 0;
  const auto factory = [&](const GpuGeometry& geometry, const std::string& device) {
    REQUIRE(device == "auto"); // The resident candidate must not pin portable selection.
    ++attempts;
    return std::make_unique<LaneGpuDouble>(geometry, std::make_shared<LaneGpuState>());
  };
  {
    struct Pending {
      Pending(){++gpu_worker_lifetime::pendingReaps();}
      ~Pending(){--gpu_worker_lifetime::pendingReaps();}
    } pending;
    multi_illuminator::MultiIlluminatorRadar blocked(40000, 2, definitions, config, factory);
    REQUIRE(attempts == 0);
  }
  multi_illuminator::MultiIlluminatorRadar available(40000, 2, definitions, config, factory);
  REQUIRE(attempts == 1);
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
      REQUIRE(frames[lane].inputProcessing.dequeMaterializations == 0);
      REQUIRE(frames[lane].inputProcessing.materializedSamples == 0);
      REQUIRE(frames[lane].inputProcessing.contiguousSamples == 4000);
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
        REQUIRE(actual.front().inputProcessing.dequeMaterializations == 0);
        REQUIRE(actual.front().clutterAcceleration.active == "cpu");
        REQUIRE(actual.front().clutterAcceleration.state == "fallback");
        REQUIRE_FALSE(actual.front().clutterAcceleration.reason.empty());
        REQUIRE(actual.front().clutterCpuExecuted);
        REQUIRE(actual.front().clutterGpuExecuted ==
          (runtime && !frame && fault == "reject"));
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
  REQUIRE_THROWS_AS(multi_illuminator::MultiIlluminatorRadar(40000, 2, rawDefinitions, config), std::invalid_argument);
  multi_illuminator::MultiIlluminatorRadar liveExperimental(40000, 2, fmDefinitions, config);
  config.offlineReplay = true;
  REQUIRE_THROWS_AS(multi_illuminator::MultiIlluminatorRadar(40000, 2, rawDefinitions, config), std::invalid_argument);
  multi_illuminator::MultiIlluminatorRadar experimental(40000, 2, fmDefinitions, config);
  for (uint64_t cpi = 0; cpi < 3; ++cpi) {
    auto next = input; next.samples.firstInputSample = cpi * 4000;
    const auto masked = experimental.process({next}), live = liveExperimental.process({next});
    REQUIRE(masked.size() == 1); REQUIRE(live.size() == 1);
    compareLaneMaps(masked.front(), live.front());
    REQUIRE(masked.front().map->data == live.front().map->data);
    REQUIRE(masked.front().detections->to_json(1) == live.front().detections->to_json(1));
    REQUIRE(masked.front().tracks->to_json(1) == live.front().tracks->to_json(1));
    REQUIRE(masked.front().inputProcessing.dequeMaterializations == 0);
    if (!cpi) REQUIRE(masked.front().map->data != baseline.front().map->data);
  }
}

TEST_CASE("CPU clutter workspace switches preserve learned reference state")
{
  using namespace blah2;
  pipeline::RawIqStages::Config config;
  config.clutterDelayMin = -2; config.clutterDelayMax = 4;
  config.samples = 2000; config.paths = 1; config.eagerPlanning = true;
  config.reference.analysisSamples = 128; config.reference.analysisInterval = 1;
  pipeline::RawIqStages stages(config);
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
  config.offlineReplay = true;
  config.cpiSeconds = .01; config.borrowedClutter = true;
  multi_illuminator::MultiIlluminatorRadar direct(rate, 2, {definition}, config),
    split(rate, 2, {definition}, config), leased(rate, 2, {definition}, config);
  direct.prepareStartup(); split.prepareStartup(); leased.prepareStartup();
  auto input = signal("atsc", definition.frequencyHz, rate, 1, 2,
    samples, 8, 30, 71);
  const double pilotHz = -4.5e6 * 684.0 / 286.0 / 4 + 150;
  for (uint32_t i = 0; i < samples; ++i) {
    const auto pilot = std::complex<float>(std::polar(.35, 2 * pi * pilotHz * i / rate));
    input.samples.channels[0][i] += pilot;
    input.samples.channels[1][i] += pilot;
  }
  conditioning::AtscPilotProjector::Config projectionConfig;
  projectionConfig.sampleRateHz = rate;
  conditioning::AtscPilotProjector legacyProjection(projectionConfig), contiguousProjection(projectionConfig);
  const std::deque<std::complex<double>> legacyInput(input.samples.channels[0].begin(),
    input.samples.channels[0].end());
  const std::vector<std::complex<double>> contiguousInput(legacyInput.begin(), legacyInput.end());
  std::deque<std::complex<double>> legacyProjected;
  std::vector<std::complex<double>> contiguousProjected;
  bool exactProjectionApplied = false;
  for (unsigned frame = 0; frame < 4; ++frame) {
    const auto legacy = legacyProjection.process(legacyInput, legacyProjected, 150, true);
    const auto contiguous = contiguousProjection.process(
      IqSampleView(contiguousInput.data(), contiguousInput.size()), contiguousProjected, 150, true);
    REQUIRE(contiguous.applied == legacy.applied);
    REQUIRE(contiguous.reason == legacy.reason);
    REQUIRE(contiguous.cfoHz == legacy.cfoHz);
    REQUIRE(contiguous.heldoutSuppressionDb == legacy.heldoutSuppressionDb);
    REQUIRE(contiguous.removedPowerFraction == legacy.removedPowerFraction);
    REQUIRE(contiguousProjected.size() == legacyProjected.size());
    REQUIRE(std::equal(contiguousProjected.begin(), contiguousProjected.end(), legacyProjected.begin()));
    exactProjectionApplied |= contiguous.applied;
  }
  REQUIRE(exactProjectionApplied);
  bool applied = false;
  for (uint64_t frame = 0; frame < 4; ++frame) {
    input.samples.firstInputSample = frame * samples;
    const auto actual = direct.process({input});
    auto borrowedInput=input;borrowSignal(borrowedInput);
    const auto borrowedFrames=leased.process({std::move(borrowedInput)});
    REQUIRE(borrowedFrames.size()==1);
    compareLaneMaps(borrowedFrames.front(),actual.front());
    REQUIRE(borrowedFrames.front().map->data==actual.front().map->data);
    REQUIRE(borrowedFrames.front().atscPilot->observedSamples==
      actual.front().atscPilot->observedSamples);
    REQUIRE(borrowedFrames.front().atscPilot->firstInputSample==
      actual.front().atscPilot->firstInputSample);
    REQUIRE(borrowedFrames.front().atscPilot->endInputSample==
      actual.front().atscPilot->endInputSample);
    REQUIRE(borrowedFrames.front().atscPilotProjection->applied==
      actual.front().atscPilotProjection->applied);
    REQUIRE(borrowedFrames.front().detections->to_json_km(1,rate)==
      actual.front().detections->to_json_km(1,rate));
    REQUIRE(borrowedFrames.front().tracks->to_json(1)==actual.front().tracks->to_json(1));
    auto first = input, second = input;
    for (auto& channel : first.samples.channels) channel.resize(samples / 2);
    for (auto& channel : second.samples.channels)
      channel.erase(channel.begin(), channel.begin() + samples / 2);
    second.samples.firstInputSample += samples / 2;
    REQUIRE(split.process({first}).empty());
    const auto expected = split.process({second});
    REQUIRE(actual.size() == 1); REQUIRE(expected.size() == 1);
    compareLaneMaps(actual.front(), expected.front());
    REQUIRE(actual.front().map->data == expected.front().map->data);
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
    REQUIRE(b.inputProcessing.path == "partial_contiguous_double");
    REQUIRE(b.inputProcessing.widenedSamples == 2 * samples);
    REQUIRE(b.inputProcessing.copiedSamples == 0);
    REQUIRE(b.inputProcessing.contiguousSamples == 2 * samples);
    REQUIRE(b.inputProcessing.dequeMaterializations == 0);
    REQUIRE(b.inputProcessing.materializedSamples == 0);
    REQUIRE(std::isfinite(a.inputProcessing.pilotQualityMs));
    REQUIRE(std::isfinite(a.inputProcessing.pilotProjectionMs));
    REQUIRE(a.detections->to_json_km(1, rate) == b.detections->to_json_km(1, rate));
    REQUIRE(a.tracks->to_json(1) == b.tracks->to_json(1));
  }
  REQUIRE(applied);
}
