#include "process/pipeline/CanonicalRadar.h"
#include "data/meta/StreamProfile.h"
#include "process/pipeline/RadarPipeline.h"
#include "process/analysis/ExperimentalRouteAdmission.h"
#include "process/ambiguity/ResidentTemporalPublication.h"
#include "process/ambiguity/Acceleration.h"
#include "process/ambiguity/GpuResidentCoordinator.h"
#include "process/ambiguity/ReplayGpuQualification.h"
#include "process/ambiguity/GpuDeviceSelection.h"
#include "process/ambiguity/ResidentLaneBudget.h"
#include "process/ambiguity/GpuLaneLease.h"
#include "process/ambiguity/GpuWorkerLifetime.h"
#include "process/analysis/StartupIq.h"
#include "process/analysis/ReplayCoherentObserverViews.h"
#include "process/conditioning/FmReferenceQualityFilter.h"
#include "process/detection/DopplerRidgeAdmission.h"
#include "process/utility/ProcessingThreads.h"

#include <algorithm>
#include <cmath>
#include <chrono>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <utility>
#include <unistd.h>

namespace blah2::pipeline
{
namespace
{
constexpr double speedOfLight = 299792458.0;
using CostClock = std::chrono::steady_clock;
double elapsedMs(CostClock::time_point start) {
  return std::chrono::duration<double, std::milli>(CostClock::now() - start).count();
}

int64_t floorRatio(int64_t value, uint32_t numerator, uint32_t denominator)
{
  const int64_t product = value * int64_t(numerator);
  int64_t result = product / int64_t(denominator);
  if (product < 0 && product % int64_t(denominator)) --result;
  return result;
}

int64_t ceilRatio(int64_t value, uint32_t numerator, uint32_t denominator)
{
  const int64_t product = value * int64_t(numerator);
  int64_t result = product / int64_t(denominator);
  if (product > 0 && product % int64_t(denominator)) ++result;
  return result;
}

template<typename T> T checked(const int64_t value, const char* field)
{
  if (value < int64_t(std::numeric_limits<T>::min()) ||
      value > int64_t(std::numeric_limits<T>::max()))
    throw std::invalid_argument(std::string("Scaled multi-illuminator ") +
      field + " exceeds the processor limit");
  return T(value);
}
}

RadarConfig scaleRadarConfigForLane(RadarConfig config,
  const uint32_t laneSampleRateHz)
{
  if (!config.centroidDopplerRadius)
    config.centroidDopplerRadius = config.centroidRadius;
  if (!config.delayReferenceRateHz) return config;
  const uint32_t reference = *config.delayReferenceRateHz;
  if (!reference || !laneSampleRateHz)
    throw std::invalid_argument("Multi-illuminator delay reference rate is invalid");
  config.delayMin = checked<int32_t>(floorRatio(config.delayMin,
    laneSampleRateHz, reference), "minimum delay");
  config.delayMax = checked<int32_t>(ceilRatio(config.delayMax,
    laneSampleRateHz, reference), "maximum delay");
  if (config.clutterFollowsAmbiguity)
  {
    config.clutterDelayMin = config.delayMin;
    config.clutterDelayMax = checked<int32_t>(int64_t(config.delayMax) + 1,
      "clutter endpoint");
  }
  else
  {
    config.clutterDelayMin = checked<int32_t>(floorRatio(config.clutterDelayMin,
      laneSampleRateHz, reference), "minimum clutter delay");
    config.clutterDelayMax = checked<int32_t>(ceilRatio(config.clutterDelayMax,
      laneSampleRateHz, reference), "maximum clutter delay");
  }
  config.minimumDelay = checked<int32_t>(ceilRatio(config.minimumDelay,
    laneSampleRateHz, reference), "minimum detection delay");
  config.guardCells = checked<int8_t>(ceilRatio(config.guardCells,
    laneSampleRateHz, reference), "CFAR guard radius");
  config.trainingCells = checked<int8_t>(ceilRatio(config.trainingCells,
    laneSampleRateHz, reference), "CFAR training radius");
  config.centroidRadius = checked<uint16_t>(ceilRatio(config.centroidRadius,
    laneSampleRateHz, reference), "centroid radius");
  config.delayReferenceRateHz = laneSampleRateHz;
  return config;
}

RadarConfig applyLaneTopology(RadarConfig config,
  const iq::SignalDefinition& definition)
{
  const auto threshold = config.trackerMinimumSnrDbByIlluminator.find(definition.id);
  if (threshold != config.trackerMinimumSnrDbByIlluminator.end())
    config.trackerMinimumSnrDb = threshold->second;
  if (!definition.hasInputRoles) return config;
  config.arrayReference = false;
  config.referenceChannels.clear();
  config.referenceChannel = definition.referenceChannel;
  config.surveillanceChannels = definition.surveillanceChannels;
  return config;
}

void validateAdvancedFeatures(const AdvancedFeatureConfig& config)
{
  if (config.surveillanceFusionMode != "noncoherent" &&
      config.surveillanceFusionMode != "coherent_beam_bank" &&
      config.surveillanceFusionMode != "noise_whitened_beam_bank")
    throw std::invalid_argument("Unknown surveillance fusion statistic");
  if (!config.trackBeforeDetect && ((config.detectorIntegrationCpis != 1 && config.detectorIntegrationCpis != 2) ||
      config.detectorIntegrationMotionBins > 1 ||
      (config.detectorIntegrationMotionBins && config.detectorIntegrationCpis != 2)))
    throw std::invalid_argument(
      "Multi-illuminator detector integration requires one or two CPIs and at most one motion bin with two CPIs");
  if (config.trackBeforeDetect) {
    if (!track_before_detect::validMaximum(config.detectorIntegrationCpis))
      throw std::invalid_argument("Power track-before-detect requires four or eight CPIs");
    (void)track_before_detect::decodeMotionBins(config.detectorIntegrationMotionBins);
  }
  // The initiation policy is shared; resident capacity is admitted separately.
  if (config.trackerAccelerationModel != "hypothesis_bank" &&
      config.trackerAccelerationModel != "adaptive_gate")
    throw std::invalid_argument(
      "Multi-illuminator tracker initiation mode is invalid");
  if (config.bearingManifoldRecorder)
    throw std::invalid_argument(
      "Bearing calibration workers require an explicitly lane-bound artifact identity");
}

RadarPlan resolveRadarPlan(const RadarConfig& config, const Ambiguity& shape,
  uint32_t sampleRateHz, uint64_t frequencyHz, uint32_t samples, bool spatialEvidence)
{
  validateAdvancedFeatures(config.advanced);
  if (config.stages.detection && config.advanced.detectorIntegrationCpis > 1)
    (void)detection::temporalIntegrationBudget(shape.get_n_doppler_bins(),
      shape.get_n_delay_bins(), config.advanced.detectorIntegrationCpis,
      config.advanced.detectorIntegrationMotionBins);
  ProductConfig p;
  p.stages = config.stages;
  p.pfa = config.pfa;
  p.guard = config.guardCells;
  p.train = config.trainingCells;
  p.minimumDelay = config.minimumDelay;
  p.minimumDoppler = config.minimumDopplerHz;
  p.cpiSeconds = shape.get_cpi();
  p.centroidDelay = config.centroidRadius;
  p.centroidDoppler = config.centroidDopplerRadius.value_or(config.centroidRadius);
  p.integrationCpis = config.advanced.detectorIntegrationCpis;
  p.integrationMotion = config.advanced.detectorIntegrationMotionBins;
  p.delayDriftPerHz = track_before_detect::delayDriftPerHz(samples, frequencyHz);
  p.trackerM = config.trackerM;
  p.trackerN = config.trackerN;
  p.trackerDelete = config.trackerDelete;
  p.maximumAcceleration = config.trackerMaximumAcceleration;
  p.rangeResolution = speedOfLight / sampleRateHz;
  p.wavelength = speedOfLight / frequencyHz;
  p.minimumSnr = config.trackerMinimumSnrDb;
  p.adaptiveGate = config.advanced.trackerAccelerationModel == "adaptive_gate";
  p.normalizedInnovation = config.trackerNormalizedInnovationGate;
  p.ridgeBirth = config.trackerDopplerRidge;
  p.recovery = config.trackerRecovery;
  if (config.trackerBeamTolerance != UINT32_MAX && (!spatialEvidence || !config.coherentFusion))
    throw std::invalid_argument("Configured beam association requires spatial evidence");
  if (spatialEvidence && config.coherentFusion) {
    p.beamCount = config.coherentFusion->beams;
    p.beamTolerance = config.trackerBeamTolerance;
    p.fineBeamFactor = config.bearingPublication.enabled ?
      config.bearingPublication.refinementFactor : 4u;
  }
  return RadarPlan(std::move(p));
}

struct RadarBank::Context
{
  iq::SignalDefinition definition;
  uint32_t captureRate = 0, samples = 0, decimation = 0;
  RadarConfig config;
  std::size_t pendingCount = 0;
  bool positioned = false;
  uint64_t pendingFirstInputSample = 0;
  IqData reference;
  IqData conditionedReference;
  IqData projectedReference;
  std::vector<std::complex<double>> projectedReferenceContiguous;
  std::vector<std::unique_ptr<IqData>> ownedChannels;
  std::vector<IqData*> channels;
  std::vector<IqData*> referenceChannels;
  std::unique_ptr<pipeline::RawIqStages> stages;
  std::vector<std::unique_ptr<Ambiguity>> ownedAmbiguity;
  std::vector<Ambiguity*> ambiguity;
  std::unique_ptr<Acceleration> acceleration;
  Acceleration::BackendFactory portableBackendFactory;
  std::shared_ptr<resident::LaneBudget> residentBudget;
  std::unique_ptr<resident::Coordinator> residentFrontend;
  AccelerationStatus residentStatus;
  std::vector<std::complex<double>> residentReferenceWeights;
  std::shared_ptr<qualification_observer::Observer> qualificationObserver;
  std::vector<std::complex<double>> qualificationReference;
  std::optional<qualification_observer::Clutter> qualificationClutter;
  uint64_t qualificationSequence=0;
  uint64_t residentLogical = 0, residentConsumed = 0, residentAllocationLimit = 0;
  uint32_t qualificationLaneIndex = 0;
  bool residentExecuted = false;
  bool residentGpuCompleted = false;
  bool residentAttempted = false;
  bool residentProductRoute = false;
  bool residentRequired = false;
  std::optional<resident::Coordinator::Publication> residentProducts;
  std::shared_ptr<Map<std::complex<double>>> residentFused;
  bool borrowedStages = false;
  bool preferBorrowedStages = false;
  bool clutterGpuExecuted = false, clutterCpuExecuted = true;
  Noncoherent fusion;
  std::unique_ptr<CoherentBeamBank> coherentFusion;
  std::optional<bearing_manifold::Calibration> detectionCalibration, bearingCalibration;
  std::shared_ptr<Map<std::complex<double>>> fusedMap;
  std::unique_ptr<pipeline::ProductStages> products;
  RadarFrame::InputProcessingMetrics inputProcessing;
  stream_profile::Worker streamProfile{"canonical-lane"};
  std::array<double,3> profileStages{};
  bool profileActive=false;
  uint64_t profileSequence=0;
  bool directFloatBorrowed = false;
  std::unique_ptr<conditioning::FmReferenceQualityFilter> fmReferenceFilter;
  std::unique_ptr<conditioning::AtscPilotQuality> atscPilot;
  std::unique_ptr<conditioning::AtscPilotProjector> atscPilotProjector;
  std::optional<conditioning::AtscPilotProjector::Metrics>
    atscPilotProjectionMetrics;
  std::optional<LiveTemporalIdentity> liveTemporalIdentity;
  uint64_t processingEpoch = 1;
  NativeCafObserver nativeObserver;
  bool nativeObserverFailed = false;
  LaneHealthStatus health;

  Context(uint32_t captureRateHz, std::size_t channelCount,
    iq::SignalDefinition value, RadarConfig radar,
    const Acceleration::BackendFactory& backendFactory,
    std::shared_ptr<resident::LaneBudget> budget, uint32_t laneIndex,
    bool residentSelectable, const std::string& residentDevice)
    : definition(std::move(value)), captureRate(captureRateHz),
      samples(uint32_t(std::llround(radar.cpiSeconds * definition.outputRateHz))),
      decimation(captureRateHz / definition.outputRateHz),
      config(scaleRadarConfigForLane(applyLaneTopology(std::move(radar), definition),
        definition.outputRateHz)),
      reference(samples), conditionedReference(samples), projectedReference(samples)
  {
    residentBudget = std::move(budget);
    portableBackendFactory = backendFactory;
    qualificationLaneIndex = laneIndex;
    residentRequired = config.advanced.surveillanceFusionMode == "noise_whitened_beam_bank" ||
      config.advanced.bearing || config.advanced.trackerBeamToleranceConfigured;
    if (config.advanced.surveillanceFusionMode == "noncoherent" &&
        (!definition.detectionManifoldFile.empty() || !definition.bearingManifoldFile.empty()))
      throw std::invalid_argument("Lane manifold files require coherent geometry");
    if (config.residentProductTemplate && config.residentProductTemplate->bearing.referenceAnchor ==
          resident::BearingReferenceAnchor::fixedSiteProjected &&
        (!definition.detectionManifoldFile.empty() || !definition.bearingManifoldFile.empty()))
      throw std::invalid_argument("fixed_site_projected requires ideal lane-bound geometry, no measured manifold");
    if (config.advanced.surveillanceFusionMode != "noncoherent") {
      if (!config.coherentFusion || !config.arrayReference ||
          config.referenceChannels != config.surveillanceChannels ||
          config.coherentFusion->positionsM.size() != config.surveillanceChannels.size())
        throw std::invalid_argument("Coherent lane requires matching array-reference/surveillance geometry");
      if (!definition.transmitterLocation)
        throw std::invalid_argument("Coherent lane requires explicit transmitter_location: " + definition.id);
      auto& geometry = *config.coherentFusion;
      auto& identity = config.manifoldIdentity;
      const auto& tx = *definition.transmitterLocation;
      const auto& rx = identity.receiverLocation;
      if (!std::all_of(tx.begin(), tx.end(), [](double x) { return std::isfinite(x); }) ||
          std::abs(tx[0]) > 90 || std::abs(tx[1]) > 180 ||
          (tx[0] == rx[0] && tx[1] == rx[1]))
        throw std::invalid_argument("Invalid coherent lane transmitter_location: " + definition.id);
      const double radians = std::acos(-1.0) / 180.0;
      const double delta = (tx[1] - rx[1]) * radians;
      const double bearing = std::atan2(std::sin(delta) * std::cos(tx[0] * radians),
        std::cos(rx[0] * radians) * std::sin(tx[0] * radians) -
        std::sin(rx[0] * radians) * std::cos(tx[0] * radians) * std::cos(delta));
      geometry.frequencyHz = definition.frequencyHz;
      geometry.directPathAzimuthRad = std::remainder(identity.xAxisBearingDeg * radians -
        bearing, 2 * std::acos(-1.0));
      identity.frequencyHz = definition.frequencyHz;
      identity.sampleRateHz = definition.outputRateHz;
      identity.transmitterLocation = tx;
      identity.directPathAzimuthDeg = std::fmod(geometry.directPathAzimuthRad / radians + 360, 360);
      const auto detectionFile = definition.detectionManifoldFile.empty() ?
        config.detectionManifoldFile : definition.detectionManifoldFile;
      if (!detectionFile.empty()) {
        detectionCalibration = bearing_manifold::loadDetection(detectionFile,
          definition.frequencyHz, geometry.beams, uint32_t(geometry.positionsM.size()),
          identity, config.offlineReplay ? bearing_manifold::DetectionUse::offlineReplay :
            bearing_manifold::DetectionUse::liveProduction);
        const auto& calibrated = *detectionCalibration;
        geometry.measuredResponses.assign(calibrated.responses.begin(), calibrated.responses.end());
        geometry.measuredDirectResponse.assign(calibrated.directResponse.begin(), calibrated.directResponse.end());
      }
      coherentFusion = std::make_unique<CoherentBeamBank>(geometry);
      config.coherentFusion = geometry;
      residentProductRoute = config.residentProductTemplate.has_value();
    }
    if (residentRequired && !residentProductRoute)
      throw std::invalid_argument("Selected coherent statistic/bearing history requires a qualified resident backend");
    detection::validateDopplerRidgeAdmission(residentProductRoute,
      config.trackerDopplerRidge.enabled);
    detection::validateTemporalRouteAdmission(residentProductRoute,
      config.advanced.trackerBeamToleranceConfigured,
      config.advanced.detectorIntegrationCpis>1||config.advanced.trackBeforeDetect,config.stages.tracking);
    track_before_detect::validateBeamHistory(config.advanced.detectorIntegrationCpis,
      config.advanced.trackerBeamToleranceConfigured?0u:UINT32_MAX);
    const auto effectiveReferenceMode = conditioning::resolveAtscReferenceMode(
      definition.standard, definition.atscReferenceMode,
      config.offlineReplay, config.arrayReference);
    if (effectiveReferenceMode == "pilot_aided")
    {
      atscPilot = std::make_unique<conditioning::AtscPilotQuality>(
        definition.outputRateHz);
      atscPilotProjector =
        std::make_unique<conditioning::AtscPilotProjector>(
          conditioning::AtscPilotProjector::Config{
            definition.outputRateHz});
    }
    health.illuminatorId = definition.id;
    health.frequencyHz = definition.frequencyHz;
    if (!samples || samples < 2000 || captureRateHz % definition.outputRateHz)
      throw std::invalid_argument("Multi-illuminator CPI or sample-rate ratio is invalid");
    if (config.surveillanceChannels.empty())
      throw std::invalid_argument("Multi-illuminator surveillance topology is empty");
    std::vector<bool> surveillanceSeen(channelCount), referenceSeen(channelCount);
    for (const uint32_t channel : config.surveillanceChannels)
    {
      if (channel >= channelCount || surveillanceSeen[channel])
        throw std::invalid_argument("Multi-illuminator surveillance topology is invalid");
      surveillanceSeen[channel] = true;
    }
    if (config.arrayReference)
    {
      if (config.referenceChannels.size() < 2)
        throw std::invalid_argument("Multi-illuminator array reference needs multiple channels");
      for (const uint32_t channel : config.referenceChannels)
      {
        if (channel >= channelCount || referenceSeen[channel])
          throw std::invalid_argument("Multi-illuminator reference topology is invalid");
        referenceSeen[channel] = true;
      }
    }
    else if (config.referenceChannel >= channelCount ||
        surveillanceSeen[config.referenceChannel])
      throw std::invalid_argument("Multi-illuminator dedicated reference topology is invalid");
    if (config.experimentalFmReferenceQualityFilter && definition.standard == "FM")
      fmReferenceFilter = std::make_unique<
        conditioning::FmReferenceQualityFilter>(definition.outputRateHz);
    ownedChannels.reserve(channelCount);
    for (std::size_t channel = 0; channel < channelCount; ++channel) {
      ownedChannels.push_back(std::make_unique<IqData>(samples));
      // First-touch the one reusable FP64 CPI outside live capture. Partial
      // and complete input fill this same owner, with no intermediate deque.
      ownedChannels.back()->resize_contiguous_for_write(samples);
      ownedChannels.back()->clear();
      ownedChannels.back()->take_storage_metrics();
    }
    channels.reserve(config.surveillanceChannels.size());
    for (const uint32_t channel : config.surveillanceChannels)
      channels.push_back(ownedChannels[channel].get());
    if (config.arrayReference)
      for (const uint32_t channel : config.referenceChannels)
        referenceChannels.push_back(ownedChannels[channel].get());

    ownedAmbiguity.reserve(channels.size());
    for (std::size_t channel = 0; channel < channels.size(); ++channel)
    {
      ownedAmbiguity.push_back(pipeline::makeAmbiguity(config.delayMin,
        config.delayMax, config.dopplerMin, config.dopplerMax,
        definition.outputRateHz, samples, true));
      ambiguity.push_back(ownedAmbiguity.back().get());
    }
    // CAF block rounding changes the actual Doppler spacing. Keep input
    // cadence/sample ranges unchanged, but use the same effective CPI as the
    // single-lane path for centroid frequency spans and tracker hypotheses.
    const auto plan = resolveRadarPlan(config, *ambiguity.front(),
      definition.outputRateHz, definition.frequencyHz, samples, residentProductRoute);
    products = std::make_unique<pipeline::ProductStages>(plan);
    // Reference conditioning is causal but currently host-side. AUTO must skip
    // the resident frontend in that topology and continue to the portable GPU
    // worker, which consumes the conditioned reference. Treating this as a
    // failed resident allocation incorrectly suppressed every GPU fallback.
    const bool residentReferenceCompatible =
      !fmReferenceFilter && !atscPilotProjector &&
      config.reference.protectionRelativeSteering.empty() &&
      config.reference.commonModeDecorrelation == 0;
    if (config.stages.clutter && residentRequired &&
        !residentReferenceCompatible)
      throw std::invalid_argument(
        "Selected resident products do not support configured host reference conditioning");
    if (!residentReferenceCompatible)
      residentProductRoute = false;
    if (config.stages.clutter && config.accelerationMode == "gpu-resident" &&
        !residentReferenceCompatible)
      throw std::invalid_argument(
        "gpu-resident does not support configured host reference conditioning; use auto or gpu");
    if (config.stages.clutter && shouldAttemptResidentFrontend(
          config.accelerationMode, residentSelectable,
          residentReferenceCompatible)) {
      residentAttempted = true;
      residentStatus.requested = config.accelerationMode;
      residentStatus.active = "cpu";
      residentStatus.state = "fallback";
      try {
        if (!residentBudget)
          throw std::runtime_error("No selectable FP64 resident device");
        const auto& shape = *ownedAmbiguity.front();
        resident::Config frontend = residentProductRoute ?
          *config.residentProductTemplate : resident::Config{};
        frontend.geometry = {shape.get_nfft(), shape.get_n_doppler_bins(),
          shape.get_n_delay_bins(), uint32_t(channels.size()), config.delayMin,
          samples, uint32_t(config.clutterDelayMax - config.clutterDelayMin),
          config.clutterDelayMin};
        frontend.inputChannels = uint32_t(channelCount);
        frontend.correlation = shape.get_n_corr();
        frontend.sampleRate = definition.outputRateHz;
        frontend.middle = shape.get_doppler_middle();
        frontend.spectrumBins = 1;
        frontend.epoch = uint64_t(laneIndex) + 1;
        frontend.device = residentDevice;
        frontend.surveillanceChannels = config.surveillanceChannels;
        frontend.delayAxis.assign(shape.result()->delay.begin(), shape.result()->delay.end());
        frontend.dopplerAxis.assign(shape.result()->doppler.begin(), shape.result()->doppler.end());
        frontend.reference.channels = config.arrayReference ? config.referenceChannels :
          std::vector<uint32_t>{config.referenceChannel};
        frontend.reference.mode = config.arrayReference ?
          resident::ReferenceMode::capturedArray : resident::ReferenceMode::capturedDedicated;
        frontend.reference.analysisSamples = config.reference.analysisSamples;
        frontend.reference.analysisInterval = config.reference.analysisInterval;
        frontend.reference.powerIterations = config.reference.powerIterations;
        frontend.reference.covarianceSmoothing = config.reference.covarianceSmoothing;
        frontend.reference.diagonalLoading = config.reference.diagonalLoading;
        frontend.reference.bandwidthHz = definition.outputRateHz;
        frontend.reference.centreHz = definition.frequencyHz;
        if (residentProductRoute) {
          const auto& geometry = *config.coherentFusion;
          frontend.fusion.frequencyHz = definition.frequencyHz;
          frontend.fusion.directPathAzimuthRad = geometry.directPathAzimuthRad;
          frontend.fusion.manifoldResponses.assign(geometry.measuredResponses.begin(), geometry.measuredResponses.end());
          frontend.fusion.directResponse.assign(geometry.measuredDirectResponse.begin(), geometry.measuredDirectResponse.end());
          frontend.detector = plan.residentDetector();
          frontend.tracker = plan.residentTracker(frontend.tracker.capacity);
          const auto bearingFile = definition.bearingManifoldFile.empty() ?
            config.bearingManifoldFile : definition.bearingManifoldFile;
          if (frontend.bearing.enabled && !bearingFile.empty()) {
            bearingCalibration = bearing_manifold::loadBearing(
              bearingFile, definition.frequencyHz, frontend.bearing.scanBeams,
              uint32_t(channels.size()), config.manifoldIdentity);
            frontend.bearing.manifoldResponses = bearingCalibration->responses;
          }
          config.bearingPublication.cpiSeconds = shape.get_cpi();
          // Recompute before clamping: rescaling a previously capped 90-degree
          // value would understate uncertainty at another lane's frequency.
          double aperture = 0;
          for (const auto& left : geometry.positionsM)
            for (const auto& right : geometry.positionsM)
              aperture = std::max(aperture, std::sqrt(
                std::pow(left[0]-right[0], 2) + std::pow(left[1]-right[1], 2) +
                std::pow(left[2]-right[2], 2)));
          config.bearingPublication.minimumUncertaintyDeg = std::min(90.0,
            .443 * speedOfLight / definition.frequencyHz / aperture * 180 / std::acos(-1.0));
        }
        frontend.presence = resident::wire::mandatoryPresence(frontend) |
          resident::bit(resident::Section::filtered) |
          resident::bit(resident::Section::fullReference);
        residentAllocationLimit = residentBudget->allocationLimit(laneIndex,
          resident::makeLayout(frontend, uint64_t(sysconf(_SC_PAGESIZE))).bytes);
        GpuProcessOptions options;
        options.offlineTransportTrace = config.offlineReplay;
        options.qualificationLane = laneIndex;
        options.allocationLimitBytes = residentAllocationLimit;
        options.startupMs = gpuCpiStartupDeadlineMs(
          config.cpiSeconds * 1000.0, options.startupMs);
        if (!config.offlineReplay)
          options.frameMs = gpuCpiFrameDeadlineMs(
            config.cpiSeconds * 1000.0, options.frameMs);
        std::lock_guard<std::mutex> lease(residentBudget->execution);
        residentFrontend = std::make_unique<resident::Coordinator>(frontend, options);
        residentStatus = residentFrontend->status();
        residentStatus.requested = config.accelerationMode;
      } catch (const std::exception& error) {
        residentStatus.reason = std::string("Resident lane unavailable: ") + error.what();
        if (std::string(error.what()).find("allocation") != std::string::npos)
          residentStatus.reason = "aggregate-gpu-capacity-exhausted: " + residentStatus.reason;
        residentFrontend.reset();
        if (residentRequired) throw;
        residentProductRoute = false;
      }
    }
    if (residentRequired && !residentFrontend)
      throw std::invalid_argument("Selected resident products require an available qualified resident backend");
    if (!residentFrontend) residentProductRoute = false;
    initializePortableAcceleration();
    preferBorrowedStages = config.stages.clutter && config.borrowedClutter.value_or(
      !config.arrayReference && config.surveillanceChannels.size() == 1);
    const bool borrowed = preferBorrowedStages && !residentFrontend &&
      !(acceleration && acceleration->clutterAvailable());
    borrowedStages = borrowed;
    stages = std::make_unique<pipeline::RawIqStages>(
      pipeline::RawIqStages::Config{config.clutterDelayMin,
        config.clutterDelayMax, samples, config.surveillanceChannels.size(),
        config.workers, borrowed, config.reference, config.stages.clutter});
    directFloatBorrowed = borrowed && !fmReferenceFilter &&
      !config.arrayReference && config.surveillanceChannels.size() == 1;
  }

  void initializePortableAcceleration()
  {
      if (acceleration || !shouldAttemptPortableFrontend(config.accelerationMode,
            bool(residentFrontend), residentRequired)) return;
      const auto& shape = *ownedAmbiguity.front();
      const int64_t clutterBins = int64_t(config.clutterDelayMax) -
        config.clutterDelayMin;
      if (clutterBins <= 0 || uint64_t(clutterBins) > UINT32_MAX)
        throw std::invalid_argument("Multi-illuminator clutter width is invalid");
      // Offer complete lane geometry on every platform. Capability, memory,
      // Precision and runtime qualification decide each stage independently.
      const Acceleration::BackendFactory boundedFactory =
        [budget = residentBudget, laneIndex = qualificationLaneIndex,
          backendFactory = portableBackendFactory,
          cpiSeconds = config.cpiSeconds, offline = config.offlineReplay](const GpuGeometry& geometry,
            const std::string& device) {
          try {
          if (!budget) throw std::runtime_error("GPU lane has no aggregate reservation");
          GpuProcessOptions options;
          options.offlineTransportTrace = offline;
          options.qualificationLane = laneIndex;
          options.allocationLimitBytes = budget->allocationLimit(laneIndex,
            gpuProcessMappingBytes(geometry));
          options.startupMs = gpuCpiStartupDeadlineMs(
            cpiSeconds * 1000.0, options.startupMs);
          if (!offline)
            options.frameMs = gpuCpiFrameDeadlineMs(
              cpiSeconds * 1000.0, options.frameMs);
          std::unique_ptr<GpuBackend> worker;
          {
            std::lock_guard<std::mutex> lease(budget->execution);
            if (!gpu_worker_lifetime::replacementAvailable())
              throw std::runtime_error("aggregate-gpu-capacity-exhausted: retired resident worker has not exited");
            worker = backendFactory ? backendFactory(geometry, device) :
              createGpuProcess(geometry, device, options);
          }
          return leaseGpuLane(std::move(worker), budget);
          } catch (const std::exception& error) {
            const std::string reason = error.what();
            if (reason.rfind("aggregate-gpu-capacity-exhausted", 0) != 0 &&
                (reason.find("allocation") != std::string::npos ||
                 reason.find("memory") != std::string::npos ||
                 reason.find("reservation") != std::string::npos))
              throw std::runtime_error("aggregate-gpu-capacity-exhausted: " + reason);
            throw;
          }
        };
      acceleration = std::make_unique<Acceleration>(config.accelerationMode,
        GpuGeometry{shape.get_nfft(), shape.get_n_doppler_bins(),
          shape.get_n_delay_bins(), uint32_t(channels.size()), config.delayMin,
          config.stages.clutter ? samples : 0u,
          config.stages.clutter ? uint32_t(clutterBins) : 0u, config.clutterDelayMin},
        shape.get_n_corr(), definition.outputRateHz,
        shape.get_doppler_middle(), config.accelerationDevice, boundedFactory);
  }

  void selectCpuStages()
  {
    // Recovery owns the CPI boundary. If the previous worker failed, recreate
    // and numerically requalify it before choosing the borrowed CPU-clutter
    // route; otherwise that safe fallback would never call processClutter()
    // again and the lane could not return to GPU without a process restart.
    if (acceleration) acceleration->beginFrame();
    const bool borrowed = preferBorrowedStages && !residentFrontend &&
      !(acceleration && acceleration->clutterAvailable());
    if (borrowed == borrowedStages) return;
    stages->setBorrowed(borrowed);
    borrowedStages = borrowed;
    directFloatBorrowed = borrowed && !fmReferenceFilter &&
      !config.arrayReference && config.surveillanceChannels.size() == 1;
  }

  void clear(const char* reason = "explicit_reset")
  {
    pendingCount = 0;
    positioned = false;
    pendingFirstInputSample = 0;
    resetAdaptiveState(reason);
  }

  void resetAdaptiveState(const char* reason = "clutter_reference_rejected")
  {
    if (residentFrontend) {
      std::lock_guard<std::mutex> lease(residentBudget->execution);
      residentFrontend->resetInput();
    }
    residentReferenceWeights.clear();
    stages->resetReference();
    if (atscPilot) atscPilot->reset();
    if (atscPilotProjector) atscPilotProjector->reset();
    atscPilotProjectionMetrics.reset();
    products->reset(reason);
    if (++processingEpoch == 0) processingEpoch = 1;
    inputProcessing = {};
    collectStorageMetrics();
    inputProcessing = {};
  }

  void collectStorageMetrics()
  {
    const auto collect = [&](IqData& channel) {
      const auto costs = channel.take_storage_metrics();
      inputProcessing.contiguousAllocations += costs.allocations;
      inputProcessing.dequeMaterializations += costs.materializations;
      inputProcessing.materializedSamples += costs.materializedSamples;
      inputProcessing.materializationMs += costs.materializationMs;
    };
    for (auto& channel : ownedChannels) collect(*channel);
    collect(reference); collect(conditionedReference); collect(projectedReference);
  }

  void resetTracker() { products->resetTracker(); }

  std::vector<RadarFrame> consume(
    const iq::CoherentSamples& input)
  {
    profileActive=streamProfile.enabled();profileStages={};
    stream_profile::WorkRecord profileRecord;
    if(profileActive){profileRecord.sequence=++profileSequence;
      profileRecord.first=input.firstInputSample;profileRecord.identity=definition.frequencyHz;
      profileRecord.begin=stream_profile::Activity::capture(true);
      profileRecord.time.fill(stream_profile::Clock::now());}
    input.validateStorage();
    // FP64 bank ingress is initially CPU-only. The portable worker stores FP32;
    // resident precision admission needs paired evidence before this expands.
    if (input.isDouble() && config.accelerationMode != "cpu")
      throw std::invalid_argument("Canonical FP64 input requires CPU acceleration until backend precision qualification");
    if (input.channelCount() != ownedChannels.size() ||
        input.inputSamplesPerOutput != decimation)
      throw std::invalid_argument("Channelized illuminator topology changed");
    if (!input.channelCount()) return {};
    const auto channelSize = [](const auto& view) { return view.size(); };
    const std::size_t count = input.visitChannel(0, channelSize);
    for (std::size_t channel=0;channel<input.channelCount();++channel)
      if (input.visitChannel(channel, channelSize) != count)
        throw std::invalid_argument("Channelized illuminator channels are unpaired");
    if (!count) return {};
    if (!input.hasFirstInputSample)
      throw std::invalid_argument("Channelized illuminator has no sample position");
    if (count > (std::numeric_limits<uint64_t>::max() - input.firstInputSample) / decimation)
      throw std::overflow_error("Canonical input sample interval overflow");
    if (positioned)
    {
      if (pendingCount > (std::numeric_limits<uint64_t>::max() -
          pendingFirstInputSample) / uint64_t(decimation))
        throw std::overflow_error("Multi-illuminator sample position overflow");
      const uint64_t expected = pendingFirstInputSample +
        pendingCount * uint64_t(decimation);
      if (input.firstInputSample != expected)
      {
        // The channelizer has already discarded its pre-gap FIR history.  Do
        // the same for clutter, tracker, and partial-CPI state so no adaptive
        // evidence crosses a missing-sample boundary.
        clear("input_discontinuity");
        pendingFirstInputSample = input.firstInputSample;
        positioned = true;
      }
    }
    else
    {
      pendingFirstInputSample = input.firstInputSample;
      positioned = true;
    }
    std::vector<RadarFrame> result;
    std::size_t offset = 0;
    while (offset < count)
    {
      // A rejected stage changes ownership only before a fresh CPI, never
      // between clutter cancellation and its matching ambiguity operation.
      selectCpuStages();
      const std::size_t observed = std::min<std::size_t>(
        samples - pendingCount, count - offset);
      if (atscPilot)
      {
        const auto qualityStart = CostClock::now();
        input.visitChannel(config.referenceChannel, [&](const auto& channel) {
          atscPilot->observe(observed,
            [&](std::size_t index) { return channel[offset + index]; },
            pendingFirstInputSample + uint64_t(pendingCount) * decimation,
            decimation, input.groupDelayInputSamples);
        });
        inputProcessing.pilotQualityMs += elapsedMs(qualityStart);
      }
      // Normal live capture produces an exact CPI per lane. Load that block
      // straight into the retained DSP buffers instead of first widening it
      // into the partial-CPI accumulator and then copying it a second time.
      if (pendingCount == 0 && count - offset >= samples)
      {
        if (directFloatBorrowed && !input.isDouble())
        {
          if (auto frame = process_float_cpi(input, offset,
                pendingFirstInputSample))
            result.push_back(std::move(*frame));
          pendingFirstInputSample += uint64_t(samples) * decimation;
          offset += samples;
          continue;
        }
        const auto ingestionStart = CostClock::now();
        inputProcessing.path = "contiguous_double";
        for (std::size_t channel = 0; channel < ownedChannels.size(); ++channel)
        {
          auto& destination = ownedChannels[channel]->resize_contiguous_for_write(samples);
          input.copyChannelTo(channel, destination.data(), samples, offset);
        }
        if (input.isDouble()) inputProcessing.copiedSamples += uint64_t(samples) * ownedChannels.size();
        else inputProcessing.widenedSamples += uint64_t(samples) * ownedChannels.size();
        inputProcessing.contiguousSamples += uint64_t(samples) * ownedChannels.size();
        inputProcessing.ingestionMs += elapsedMs(ingestionStart);
        if (auto frame = process_loaded_cpi(pendingFirstInputSample))
          result.push_back(std::move(*frame));
        pendingFirstInputSample += uint64_t(samples) * decimation;
        offset += samples;
        continue;
      }
      const std::size_t copied = std::min<std::size_t>(
        samples - pendingCount, count - offset);
      const auto ingestionStart = CostClock::now();
      inputProcessing.path = "partial_contiguous_double";
      for (std::size_t channel = 0; channel < ownedChannels.size(); ++channel)
      {
        auto& destination = ownedChannels[channel]->resize_contiguous_for_write(samples);
        input.copyChannelTo(channel, destination.data() + pendingCount, copied, offset);
      }
      if (input.isDouble()) inputProcessing.copiedSamples += uint64_t(copied) * ownedChannels.size();
      else inputProcessing.widenedSamples += uint64_t(copied) * ownedChannels.size();
      inputProcessing.contiguousSamples += uint64_t(copied) * ownedChannels.size();
      inputProcessing.ingestionMs += elapsedMs(ingestionStart);
      pendingCount += copied;
      offset += copied;
      if (pendingCount == samples)
      {
        if (auto frame = process_loaded_cpi(pendingFirstInputSample))
          result.push_back(std::move(*frame));
        pendingFirstInputSample += uint64_t(samples) * decimation;
        pendingCount = 0;
      }
    }
    if(profileActive){profileRecord.time[1]=profileRecord.time[2]=profileRecord.time[3]=stream_profile::Clock::now();
      profileRecord.stages=profileStages;streamProfile.record(profileRecord);}
    profileActive=false;
    return result;
  }

  qualification_observer::Frame qualificationFrame(uint64_t first) const
  {
    qualification_observer::Frame f;
    f.resolvedProductPlan=products->planIdentity();
    f.lane=definition.id;f.sequence=qualificationSequence;
    f.captureEpoch=liveTemporalIdentity?liveTemporalIdentity->captureEpoch:0;
    f.samplePositionAvailable=liveTemporalIdentity&&liveTemporalIdentity->samplePositionAvailable;
    f.captureFirst=first;f.captureEnd=first+uint64_t(samples)*decimation;
    f.frequencyHz=definition.frequencyHz;f.sampleRateHz=definition.outputRateHz;
    f.inputSamplesPerOutput=decimation;f.channelIds=config.surveillanceChannels;
    f.referenceChannelIds=config.arrayReference?config.referenceChannels:std::vector<uint32_t>{config.referenceChannel};
    if(liveTemporalIdentity)f.actualGainDb=liveTemporalIdentity->inputGainDb;
    f.backend=residentFrontend?"vulkan-resident":acceleration?acceleration->status().active:"cpu";
    f.device=residentFrontend?residentStatus.device:acceleration?acceleration->status().device:"";
    f.productBackend=residentProductRoute?"vulkan-resident":"cpu";
    f.fusionMode=config.advanced.surveillanceFusionMode;f.integrationCpis=config.advanced.detectorIntegrationCpis;
    if(config.coherentFusion){f.beamCount=config.coherentFusion->beams;f.positionsM=config.coherentFusion->positionsM;
      f.directPathAzimuthRad=config.coherentFusion->directPathAzimuthRad;f.directPathNullDb=config.coherentFusion->directPathNullDb;}
    f.referenceAnalysisSamples=config.reference.analysisSamples;f.referenceAnalysisInterval=config.reference.analysisInterval;
    f.referencePowerIterations=config.reference.powerIterations;f.referenceSmoothing=config.reference.covarianceSmoothing;
    f.referenceLoading=config.reference.diagonalLoading;
    f.detectionManifoldFile=definition.detectionManifoldFile.empty()?config.detectionManifoldFile:definition.detectionManifoldFile;
    f.bearingManifoldFile=definition.bearingManifoldFile.empty()?config.bearingManifoldFile:definition.bearingManifoldFile;
    return f;
  }

  bool process_loaded_dsp(bool preserveSurveillance = false)
  {
    selectCpuStages();
    IqData* selectedReference = &reference;
    if (config.arrayReference)
      stages->reference().process_into(referenceChannels, reference);
    else
      selectedReference = ownedChannels[config.referenceChannel].get();
    if (fmReferenceFilter)
    {
      fmReferenceFilter->process(*selectedReference, conditionedReference);
      selectedReference = &conditionedReference;
    }
    IqData* ambiguityReference = selectedReference;
    const auto prepareAmbiguityReference = [&]() -> IqData* {
      if (atscPilot && atscPilotProjector)
      {
        const auto projectionStart = CostClock::now();
        const auto& pilot = atscPilot->metrics();
        // Clutter's worker already owns its input copy. Read unchanged host IQ
        // while it runs; projection still precedes every host clutter commit.
        atscPilotProjectionMetrics = atscPilotProjector->process(
          selectedReference->sample_view(),
          projectedReference.resize_contiguous_for_write(samples),
          pilot.cfoHz, pilot.available && pilot.pilotCandidate);
        inputProcessing.pilotProjectionMs += elapsedMs(projectionStart);
        if (atscPilotProjectionMetrics->applied)
          ambiguityReference = &projectedReference;
      }
      if(qualificationObserver&&qualificationObserver->observesSamples()) {
        const auto source = ambiguityReference->sample_view();
        qualificationReference.assign(source.begin(), source.end());
      }
      return ambiguityReference;
    };
    auto profileFiltered=profileActive?CostClock::now():CostClock::time_point{};
    const auto profileStart=profileFiltered;
    const bool accepted = pipeline::processFrontend(*stages, acceleration.get(),
      *selectedReference, channels, ambiguity, config.stages.clutter,
      preserveSurveillance, [&]{
        if(qualificationObserver&&qualificationObserver->observesClutter()){
          const bool gpu=config.stages.clutter&&acceleration&&acceleration->clutterTiming().gpuExecuted;
          const bool cpu=config.stages.clutter&&(!acceleration||acceleration->clutterTiming().cpuExecuted);
          const auto views=borrowedStages?stages->borrowed_views():std::vector<WienerHopf::FilteredView>{};
          qualificationClutter=qualification_observer::snapshotClutter(samples,channels.size(),
            [&](size_t i){return selectedReference->sample_view().at(i);},
            [&](size_t c,size_t i){return borrowedStages?views.at(c).filteredSurveillance[i]:channels.at(c)->sample_view().at(i);},
            config.stages.clutter,config.clutterDelayMin,config.clutterDelayMax,
            gpu?(cpu?"cpu+vulkan-qualification":"vulkan"):"cpu",gpu,cpu);
        }
        if(profileActive){profileFiltered=CostClock::now();
          profileStages[0]+=stream_profile::ms(profileFiltered,profileStart);}
      }, borrowedStages ? std::function<void()>([&] {
        process_borrowed_ambiguity(
          ambiguityReference == selectedReference ? nullptr : ambiguityReference);
      }) : std::function<void()>{}, nullptr, prepareAmbiguityReference);
    if(profileActive&&accepted)profileStages[1]+=stream_profile::ms(CostClock::now(),profileFiltered);
    clutterGpuExecuted = config.stages.clutter && acceleration &&
      acceleration->clutterTiming().gpuExecuted;
    clutterCpuExecuted = config.stages.clutter &&
      (!acceleration || acceleration->clutterTiming().cpuExecuted);
    return accepted;
  }

  void importResidentMaps()
  {
    const auto& output = residentFrontend->receivePrepared();
    // Prepared acknowledges completed GPU work even if later admission fails.
    residentGpuCompleted = true;
    const auto proof = resident::allocationProof({output.preparedEvidence.data + 256, 832});
    if (proof.limit > residentAllocationLimit)
      throw std::runtime_error("Resident lane exceeded its reserved allocation ceiling");
    const auto state = resident::referenceState(output.referenceCheckpoint);
    residentReferenceWeights.assign(state.weights.begin(), state.weights.begin() + state.weightCount);
    const auto& shape = residentFrontend->config().geometry;
    const uint64_t cells = uint64_t(shape.doppler) * shape.delays;
    for (uint32_t channel = 0; channel < shape.channels; ++channel) {
      auto* map = ambiguity[channel]->result();
      for (uint32_t row = 0; row < shape.doppler; ++row)
        for (uint32_t column = 0; column < shape.delays; ++column)
          map->data[row][column] = output.maps.data[channel * cells +
            uint64_t(column) * shape.doppler + (row + shape.doppler / 2 + 1) % shape.doppler];
    }
    if (residentProductRoute) {
      if (output.fused.size != cells)
        throw std::runtime_error("Resident fused product geometry differs");
      residentFused = std::make_shared<Map<std::complex<double>>>(*ownedAmbiguity.front()->result());
      for (uint32_t row = 0; row < shape.doppler; ++row)
        for (uint32_t col = 0; col < shape.delays; ++col)
          // The imaginary part encodes beam identity, not measured phase.
          residentFused->data[row][col] = {output.fused.data[size_t(row) * shape.delays + col].real(), 0};
    }
  }

  void applyQualificationCpuReset()
  {
    if (!replay_gpu_qualification::takeCpuOracleReset(qualificationLaneIndex)) return;
    // The independent CPU control must reset exactly the state discarded by
    // resident recovery, not drop input or reset the capture/channelizer.
    resetAdaptiveState("qualification_cpu_oracle_reset");
    replay_gpu_qualification::emit("history-reset", "host", qualificationLaneIndex,
      replay_gpu_qualification::current(), "cpu-oracle-reset");
  }

  std::optional<RadarFrame> process_loaded_cpi(uint64_t first)
  {
    applyQualificationCpuReset();
    if(qualificationObserver){++qualificationSequence;
      qualificationObserver->input(qualificationFrame(first),ownedChannels.size(),samples,
        [&](size_t c,size_t i){return ownedChannels.at(c)->sample_view().at(i);});}
    if (detectionCalibration || bearingCalibration) {
      if (!liveTemporalIdentity || liveTemporalIdentity->inputGainDb.size() != ownedChannels.size())
        throw std::runtime_error("Measured lane manifold requires frame-bound actual gain metadata");
      std::vector<double> gains;
      for (auto channel : config.surveillanceChannels)
        gains.push_back(liveTemporalIdentity->inputGainDb[channel]);
      if (detectionCalibration) bearing_manifold::validateActualGains(*detectionCalibration, gains);
      if (bearingCalibration) bearing_manifold::validateActualGains(*bearingCalibration, gains);
    }
    residentExecuted = false;
    residentGpuCompleted = false;
    residentProducts.reset();
    bool qualificationResidentRecovery = false;
    if (residentFrontend) {
      try {
        if (config.advanced.detectorIntegrationCpis > 1 && liveTemporalIdentity &&
            !liveTemporalIdentity->samplePositionAvailable)
          throw std::runtime_error("Resident temporal input sample lineage unavailable; temporal products cannot be admitted");
        std::lock_guard<std::mutex> lease(residentBudget->execution);
        residentFrontend->begin(ownedChannels, {++residentLogical, ++residentConsumed,
          detection::signalTimeMilliseconds(first + uint64_t(samples) * decimation,
            captureRate)});
        importResidentMaps();
        if (residentProductRoute)
          residentProducts = residentFrontend->format(
            detection::signalTimeMilliseconds(first + uint64_t(samples) * decimation, captureRate),
            config.bearingPublication);
        residentFrontend->commit();
        residentFrontend->published();
        residentExecuted = true;
        clutterGpuExecuted = true;
        clutterCpuExecuted = false;
      } catch (const std::exception& error) {
        // Canonical host IQ is unchanged by the worker. Reset all host temporal
        // state before retrying this CPI; never splice two reference histories.
        {
          std::lock_guard<std::mutex> lease(residentBudget->execution);
          residentFrontend->stop();
          residentFrontend.reset();
        }
        residentStatus.active = "cpu";
        residentStatus.state = "fallback";
        residentStatus.reason = std::string("Resident lane failed; state reset: ") + error.what();
        if (residentRequired) throw;
        residentProductRoute = false;
        residentProducts.reset();
        resetAdaptiveState("resident_frontend_failure");
        initializePortableAcceleration();
        qualificationResidentRecovery = true;
        if(replay_gpu_qualification::enabled())
          replay_gpu_qualification::emit("history-reset","resident",qualificationLaneIndex,
            replay_gpu_qualification::current(),{}, {},residentLogical,residentConsumed);
        selectCpuStages();
      }
    }
    if (residentExecuted) return finish_processed_cpi(first);
    if (!process_loaded_dsp()) {
      reject(first, "clutter_reference_rejected");
      resetAdaptiveState();
      return std::nullopt;
    }
    auto result=finish_processed_cpi(first);
    if(qualificationResidentRecovery&&replay_gpu_qualification::enabled())
      replay_gpu_qualification::emit("cpu-recovery-complete","resident",qualificationLaneIndex,
        replay_gpu_qualification::current(),{}, {},residentLogical,residentConsumed);
    return result;
  }

  void prepareStartup()
  {
    std::vector<IqData*> input;
    for (auto& channel : ownedChannels) input.push_back(channel.get());
    const auto prepareFrame = [&](unsigned frame) {
      startup::fillIq(input, samples, frame);
      // Deliberately stop before detection/tracking/temporal artifact output.
      if (!process_loaded_dsp())
        throw std::runtime_error("Startup clutter reference was rejected");
    };
    if (residentFrontend) {
      try {
        std::lock_guard<std::mutex> lease(residentBudget->execution);
        pipeline::ProductStages startupProducts{RadarPlan(products->config())};
        residentFrontend->prepareStartup([&](unsigned frame) {
          residentFrontend->fillStartupIq(input, samples, frame);
          residentFrontend->begin(ownedChannels, {uint64_t(frame)+1,
            uint64_t(frame)+1, 1000 + uint64_t(frame) * samples * 1000 /
              definition.outputRateHz});
          if (!process_loaded_dsp(true))
            throw std::runtime_error("Resident lane CPU startup oracle rejected IQ");
          ArrayReferenceSynthesizer::Metrics dedicated;
          residentFrontend->checkReference(config.arrayReference ? reference :
            *ownedChannels[config.referenceChannel], config.arrayReference ?
            stages->reference().get_metrics() : dedicated);
          residentFrontend->checkFiltered(channels);
          residentFrontend->checkMaps(ambiguity);
          const auto& output = residentFrontend->prepared();
          if (resident::allocationProof({output.preparedEvidence.data + 256, 832}).limit >
              residentAllocationLimit)
            throw std::runtime_error("Resident lane allocation receipt exceeds reservation");
          if (residentProductRoute) {
            const uint64_t timestamp = 1000 + uint64_t(frame) * samples * 1000 / definition.outputRateHz;
            std::vector<Map<std::complex<double>>*> maps;
            for (const auto& processor : ownedAmbiguity) maps.push_back(processor->result());
            std::shared_ptr<Map<std::complex<double>>> oracleMap;
            const auto oracleFusion = residentFrontend->fuseOracle(maps, fusion,
              coherentFusion.get(), oracleMap, stages->reference().get_metrics());
            auto oracle = startupProducts.process(oracleMap,
              {1, uint64_t(frame) * samples, uint64_t(frame + 1) * samples, timestamp, true},
              true, {}, &oracleFusion.beams, oracleFusion.spatialAt);
            auto& found = oracle.detections;
            auto& tracked = oracle.tracks;
            const auto products = residentFrontend->format(timestamp, config.bearingPublication);
            residentFrontend->checkProducts(products,
              oracleMap->to_json_km(timestamp, definition.outputRateHz),
              found ? found->to_json_km(timestamp, definition.outputRateHz) : std::string{},
              tracked ? tracked->to_json(timestamp) : std::string{},
              oracle.integration ? &*oracle.integration : nullptr, &startupProducts.tracker);
          } else residentFrontend->checkFrontendProducts();
          residentFrontend->commit();
        }, [&] { stages->resetReference(); });
        residentStatus = residentFrontend->status();
        residentStatus.requested = config.accelerationMode;
        residentStatus.reason = residentProductRoute ?
          "Qualified resident coherent products; lane-bound RF/site/calibration; serialized aggregate lease" :
          "Qualified resident reference/clutter/CAF; shared FP64 host products; serialized aggregate lease";
        if (config.accelerationMode == "auto") {
          // No diagnostic transaction may consume the new production namespace.
          residentStatus.cpuMs = 0; residentStatus.gpuMs = 0;
          residentStatus.reason += "; AUTO timing unavailable (private correctness qualification only)";
        }
      } catch (const std::exception& error) {
        residentFrontend.reset();
        residentStatus.active = "cpu";
        residentStatus.state = "fallback";
        residentStatus.reason = std::string("Resident lane qualification failed: ") + error.what();
        if (residentRequired) throw;
        residentProductRoute = false;
        stages->resetReference();
        initializePortableAcceleration();
        selectCpuStages();
      }
    }
    if (!residentFrontend) {
      if (acceleration && acceleration->startupPending())
        acceleration->prepareStartup(prepareFrame);
      else
        prepareFrame(0); // CPU/fallback must also finish first-use DSP work.
    }
    selectCpuStages(); // Finish any fallback workspace planning before capture.
    // Reset private reference state before live IQ. FFT planning happens here,
    // while capture is still dormant, and accepted device qualification stays.
    stages->resetReference();
    products->reset("startup_qualification_complete");
    if (atscPilot) atscPilot->reset();
    if (atscPilotProjector) atscPilotProjector->reset();
    atscPilotProjectionMetrics.reset();
    reference.clear();
    conditionedReference.clear();
    projectedReference.clear();
    qualificationReference.clear();
    qualificationClutter.reset();
    inputProcessing = {};
    for (auto& channel : ownedChannels) channel->clear();
    collectStorageMetrics();
    inputProcessing = {};
  }

  std::optional<RadarFrame> process_float_cpi(
    const iq::CoherentSamples& input,
    std::size_t offset, uint64_t first)
  {
    applyQualificationCpuReset();
    inputProcessing.path = "native_float_borrowed";
    if(qualificationObserver){++qualificationSequence;
      qualificationObserver->input(qualificationFrame(first),input.channelCount(),samples,
        [&](size_t c,size_t i){return std::complex<double>(input.channel(c).at(offset+i));});}
    const auto* rawReference = input.channel(config.referenceChannel).data() + offset;
    const std::complex<double>* projected = nullptr;
    if (atscPilot && atscPilotProjector) {
      const auto projectionStart = CostClock::now();
      const auto& pilot = atscPilot->metrics();
      atscPilotProjectionMetrics = atscPilotProjector->process(rawReference,
        samples, projectedReferenceContiguous, pilot.cfoHz,
        pilot.available && pilot.pilotCandidate);
      inputProcessing.pilotProjectionMs += elapsedMs(projectionStart);
      if (atscPilotProjectionMetrics->applied)
        projected = projectedReferenceContiguous.data();
    }
    if(qualificationObserver&&qualificationObserver->observesSamples()){qualificationReference.resize(samples);
      for(size_t i=0;i<samples;++i)qualificationReference[i]=projected?projected[i]:std::complex<double>(rawReference[i]);}
    std::vector<const std::complex<float>*> surveillance;
    surveillance.reserve(config.surveillanceChannels.size());
    for (const uint32_t channel : config.surveillanceChannels)
      surveillance.push_back(input.channel(channel).data() + offset);
    const auto profileClutterStart=profileActive?CostClock::now():CostClock::time_point{};
    if (!stages->clutter_float(
          rawReference,
          surveillance, samples)) {
      reject(first, "clutter_reference_rejected");
      resetAdaptiveState();
      return std::nullopt;
    }
    clutterGpuExecuted = false;
    clutterCpuExecuted = true;
    if(qualificationObserver&&qualificationObserver->observesClutter()){
      const auto views=stages->borrowed_views();
      qualificationClutter=qualification_observer::snapshotClutter(samples,surveillance.size(),
        [&](size_t i){return rawReference[i];},
        [&](size_t c,size_t i){return views.at(c).filteredSurveillance[i];},
        true,config.clutterDelayMin,config.clutterDelayMax,"cpu",false,true);
    }
    const auto profileAmbiguityStart=profileActive?CostClock::now():CostClock::time_point{};
    if(profileActive)profileStages[0]+=stream_profile::ms(profileAmbiguityStart,profileClutterStart);
    process_borrowed_ambiguity(nullptr, projected);
    if(profileActive)profileStages[1]+=stream_profile::ms(CostClock::now(),profileAmbiguityStart);
    return finish_processed_cpi(first);
  }

  void process_borrowed_ambiguity(const IqData* qualifiedReference = nullptr,
      const std::complex<double>* contiguousQualifiedReference = nullptr)
  {
    const auto views = stages->borrowed_views();
    if (views.size() != ambiguity.size() || views.empty())
      throw std::logic_error("Borrowed ambiguity view count changed");
    std::vector<const std::complex<double>*> surveillance;
    surveillance.reserve(views.size());
    for (const auto& view : views) {
      if (view.samples != views.front().samples ||
          view.delayMin != views.front().delayMin)
        throw std::logic_error("Borrowed ambiguity geometry changed");
      surveillance.push_back(view.filteredSurveillance);
    }
    const auto runAmbiguity = [&] {
      if (contiguousQualifiedReference)
        stages->ambiguity_borrowed_reference(contiguousQualifiedReference,
          samples, ambiguity);
      else if (qualifiedReference)
        stages->ambiguity_borrowed_reference(*qualifiedReference, ambiguity);
      else
        stages->ambiguity_borrowed(ambiguity);
    };
    const std::complex<double>* accelerationReference =
      contiguousQualifiedReference ? contiguousQualifiedReference : views.front().rotatedReference;
    if (acceleration && qualifiedReference)
    {
      const auto source = qualifiedReference->sample_view();
      accelerationReference = source.data();
      if (!accelerationReference) {
        const auto copyStart = CostClock::now();
        projectedReferenceContiguous.assign(source.begin(), source.end());
        accelerationReference = projectedReferenceContiguous.data();
        inputProcessing.copiedSamples += source.size();
        inputProcessing.ingestionMs += elapsedMs(copyStart);
      }
    }
    if (acceleration)
      acceleration->processBorrowed(accelerationReference,
        surveillance, views.front().samples,
        (qualifiedReference || contiguousQualifiedReference) ? 0 : views.front().delayMin,
        ambiguity, runAmbiguity);
    else
      runAmbiguity();
  }

  RadarFrame finish_processed_cpi(uint64_t first)
  {
    stream_profile::Duration profileProducts(profileActive,profileStages[2]);
    std::vector<Map<std::complex<double>>*> maps;
    maps.reserve(ambiguity.size());
    for (const auto& processor : ownedAmbiguity) maps.push_back(processor->result());
    if (nativeObserver) {
      NativeCafView view;
      view.illuminatorId = definition.id;
      view.frequencyHz = definition.frequencyHz;
      view.sampleRateHz = definition.outputRateHz;
      view.firstInputSample = first;
      view.endInputSample = first + uint64_t(samples - 1) * decimation;
      view.inputSamplesPerOutput = decimation;
      view.channelIds = config.surveillanceChannels;
      view.referenceChannelIds = config.arrayReference ?
        config.referenceChannels : std::vector<uint32_t>{config.referenceChannel};
      view.maps.assign(maps.begin(), maps.end());
      if (config.arrayReference)
        view.referenceWeights = residentExecuted ? residentReferenceWeights :
          stages->reference().get_metrics().weights;
      // Failure aborts publication; the latch survives reset so these maps
      // cannot be reused by a caller that catches an observer exception.
      nativeObserverFailed = true;
      nativeObserver(view);
      nativeObserverFailed = false;
    }
    if (residentProducts) fusedMap = residentFused;
    else {
      if (coherentFusion)
        coherentFusion->calibrate(residentExecuted ? residentReferenceWeights :
          stages->reference().get_metrics().directPathWeights);
      pipeline::fuseMaps(maps, fusion, coherentFusion.get(), fusedMap);
    }
    auto map = fusedMap;
    if(qualificationObserver){
      if(qualificationObserver->observesClutter()){
        if(residentExecuted)qualificationClutter=qualification_observer::residentClutter(
          residentFrontend->config(),residentFrontend->prepared(),residentFrontend->observedReference());
        if(!qualificationClutter)throw std::runtime_error("Production clutter observation missing");
        qualificationObserver->clutter(qualificationFrame(first),*qualificationClutter);
        qualificationClutter.reset();
      }
      auto products=residentExecuted?qualification_observer::residentProducts(
        residentFrontend->config(),residentFrontend->prepared(),
        residentFrontend->observedSharedReservationBytes(),residentBudget.get()):qualification_observer::hostProducts(maps,*map);
      if(residentExecuted&&!residentProductRoute){
        products.fused=[map,columns=map->get_nCols()](size_t i){return map->data.at(i/columns).at(i%columns);};
        products.fusedBeamIdsAvailable=false;
      }
      if(residentExecuted){const auto ref=residentFrontend->observedReference();
        products.referenceSamples=ref.size;products.reference=[ref](size_t i){return ref.data[i];};
        products.referenceWeights=residentReferenceWeights;
      }else{products.referenceSamples=qualificationReference.size();
        products.reference=[&](size_t i){return qualificationReference.at(i);};
        if(config.arrayReference)products.referenceWeights=stages->reference().get_metrics().weights;}
      qualificationObserver->products(qualificationFrame(first),products);
    }
    map->set_metrics();
    const uint64_t end = first + uint64_t(samples - 1) * decimation;
    const uint64_t timestamp = detection::signalTimeMilliseconds(
      first + uint64_t(samples) * decimation, captureRate);
    pipeline::Products processed;
    CfarDetector1D::Trials observedTrials;
    if (residentProducts) {
      processed.integration = resident::temporalPublication(residentFrontend->config(),
        *residentProducts,liveTemporalIdentity?liveTemporalIdentity->captureEpoch:processingEpoch,
        first,first+uint64_t(samples)*decimation);
      // Native products remain authoritative; these empty containers only
      // preserve the existing publication/storage interface.
      processed.detections = std::make_unique<Detection>(
        std::vector<double>{}, std::vector<double>{}, std::vector<double>{});
      processed.tracks = std::make_unique<Track>();
    } else {
      processed = products->process(map, {
        liveTemporalIdentity ? liveTemporalIdentity->captureEpoch : processingEpoch,
        first, first + uint64_t(samples) * decimation, timestamp,
        !liveTemporalIdentity || liveTemporalIdentity->samplePositionAvailable},
        true, {}, nullptr, {}, qualificationObserver ? &observedTrials : nullptr);
    }
    if (qualificationObserver) {
      if (residentProducts) {
        const auto& native=residentFrontend->prepared();
        if(config.stages.detection&&!native.detectorObserved)
          throw std::runtime_error("Resident replay lacks evaluated detector population");
        qualificationObserver->detector(qualificationFrame(first),config.stages.detection,
          config.stages.detection&&(!processed.integration||processed.integration->ready),
          "vulkan-resident",native.evaluatedCells,native.cfarCount);
      } else qualificationObserver->detector(qualificationFrame(first),config.stages.detection,
        config.stages.detection&&(!processed.integration||processed.integration->ready),
        "cpu",observedTrials.evaluated,observedTrials.accepted,observedTrials.nonfinite);
    }
    ++health.acceptedCpis;
    health.hasLastCpi = true;
    health.lastAccepted = true;
    health.lastFirstInputSample = first;
    health.lastEndInputSample = end;
    health.lastReason.clear();
    RadarFrame result;
    result.illuminatorId = definition.id;
    result.frequencyHz = definition.frequencyHz;
    if (config.coherentFusion) {
      result.transmitterLocation = definition.transmitterLocation;
      result.directPathAzimuthRad = config.coherentFusion->directPathAzimuthRad;
    }
    result.firstInputSample = first;
    result.endInputSample = end;
    result.inputSamplesPerOutput = decimation;
    result.channelIds = config.surveillanceChannels;
    result.referenceChannelIds = config.arrayReference ?
      config.referenceChannels : std::vector<uint32_t>{config.referenceChannel};
    result.sampleRateHz = definition.outputRateHz;
    collectStorageMetrics();
    result.inputProcessing = inputProcessing;
    result.detectionEnabled = config.stages.detection;
    result.trackingEnabled = config.stages.tracking;
    result.detectorIntegration = std::move(processed.integration);
    inputProcessing = {};
    result.atscReferenceRequested = definition.atscReferenceMode;
    if (definition.atscReferenceMode == "automatic" && !atscPilot)
      result.atscReferenceReason = definition.standard == "ATSC-3.0" ?
        "atsc3_reconstruction_not_qualified" :
        "pilot_projection_requires_dedicated_reference";
    if (atscPilot)
    {
      result.atscPilot = atscPilot->metrics();
      result.atscPilotProjection = atscPilotProjectionMetrics;
      result.atscReferenceEffective = atscPilotProjectionMetrics &&
        atscPilotProjectionMetrics->applied ? "pilot_aided" : "raw";
      result.atscReferenceReason = atscPilotProjectionMetrics ?
        atscPilotProjectionMetrics->reason : "pilot_not_observed";
    }
    result.trackerDiagnostics = products->tracker.frame_diagnostics();
    result.trackerMinimumSnrDb = config.trackerMinimumSnrDb;
    result.trackerMinimumSnrOverridden =
      config.trackerMinimumSnrDbByIlluminator.count(definition.id) != 0;
    result.map = std::move(map);
    result.residentProducts = std::move(residentProducts);
    result.residentTemporalExecuted = result.residentProducts &&
      result.detectorIntegration && result.detectorIntegration->availableCpis > 0;
    result.detections = std::move(processed.detections);
    result.tracks = std::move(processed.tracks);
    // Keep the existing transport schema while execution metadata records
    // disabled stages. Empty containers cannot invent detections or tracks.
    if (!result.detections) result.detections = std::make_unique<Detection>(
      std::vector<double>{}, std::vector<double>{}, std::vector<double>{});
    if (!result.tracks) result.tracks = std::make_unique<Track>();
    if (atscPilot)
    {
      atscPilot->reset();
      atscPilotProjectionMetrics.reset();
    }
    if (residentExecuted || config.accelerationMode == "gpu-resident" ||
        (!acceleration && residentAttempted))
    {
      result.acceleration = residentStatus;
      result.clutterAcceleration = residentStatus;
      result.acceleration.gpuExecutionBackend = result.clutterAcceleration.gpuExecutionBackend =
        residentGpuCompleted ? "vulkan-resident" : "";
      result.clutterGpuExecuted = result.ambiguityGpuExecuted = residentGpuCompleted;
      result.clutterCpuExecuted = result.ambiguityCpuExecuted = !residentExecuted;
    }
    else if (acceleration)
    {
      result.acceleration = acceleration->status();
      result.clutterAcceleration = acceleration->clutterStatus();
      if (residentAttempted && !residentFrontend) {
        result.acceleration.reason += "; " + residentStatus.reason;
        result.clutterAcceleration.reason += "; " + residentStatus.reason;
      }
      result.clutterGpuExecuted = clutterGpuExecuted;
      result.clutterCpuExecuted = clutterCpuExecuted;
      result.ambiguityGpuExecuted = acceleration->ambiguityGpuExecuted();
      result.ambiguityCpuExecuted = acceleration->ambiguityCpuExecuted();
    }
    else
    {
      result.acceleration.requested = "cpu";
      result.acceleration.active = "cpu";
      result.acceleration.state = "selected";
      result.clutterAcceleration = result.acceleration;
      result.clutterGpuExecuted = false;
      result.clutterCpuExecuted = clutterCpuExecuted;
      result.ambiguityGpuExecuted = false;
      result.ambiguityCpuExecuted = true;
    }
    return result;
  }

  void reject(uint64_t first, std::string reason)
  {
    ++health.rejectedCpis;
    health.hasLastCpi = true;
    health.lastAccepted = false;
    health.lastFirstInputSample = first;
    health.lastEndInputSample = first + uint64_t(samples - 1) * decimation;
    health.lastReason = std::move(reason);
  }
};

RadarBank::~RadarBank() = default;

void RadarBank::setNativeCafObserver(NativeCafObserver observer)
{
  if (inputStarted_)
    throw std::logic_error("Cannot replace the native CAF observer after input");
  for (auto& context : contexts_) context->nativeObserver = observer;
}

void RadarBank::setQualificationObserver(
  std::shared_ptr<qualification_observer::Observer> observer)
{
  if(inputStarted_||!config_.offlineReplay||!observer)
    throw std::logic_error("Qualification observer requires offline replay before input");
  for(auto& context:contexts_){
    context->qualificationObserver=observer;
    if(context->residentFrontend)context->residentFrontend->enableReplayObserver(true,true,observer->observesClutter());
  }
}

RadarBank::RadarBank(uint32_t captureRateHz,
  std::size_t channelCount,
  const std::vector<iq::SignalDefinition>& illuminators,
  RadarConfig config, Acceleration::BackendFactory backendFactory)
  : captureRateHz_(captureRateHz), channelCount_(channelCount),
    config_(std::move(config))
{
  if (!captureRateHz_ || !channelCount_ || illuminators.empty() ||
      !std::isfinite(config_.cpiSeconds) || config_.cpiSeconds <= 0)
    throw std::invalid_argument("Multi-illuminator radar configuration is empty");
  config_.stages.validate();
  validateAdvancedFeatures(config_.advanced);
  const auto laneMatches = [&](const std::string& id) {
    return std::count_if(illuminators.begin(), illuminators.end(),
      [&](const auto& lane) { return lane.id == id; });
  };
  if (!std::isfinite(config_.trackerMinimumSnrDb) ||
      config_.trackerMinimumSnrDb < -100 || config_.trackerMinimumSnrDb > 100)
    throw std::invalid_argument("Multi-illuminator tracker birth SNR is invalid");
  for (const auto& threshold : config_.trackerMinimumSnrDbByIlluminator)
    if (laneMatches(threshold.first) != 1 || !std::isfinite(threshold.second) ||
        threshold.second < -100 || threshold.second > 100)
      throw std::invalid_argument("Per-illuminator tracker birth SNR is invalid");
  if (config_.experimentalFmReferenceQualityFilter &&
      std::none_of(illuminators.begin(), illuminators.end(),
        [](const auto& lane) { return lane.standard == "FM"; }))
    throw std::invalid_argument(
      "Experimental FM reference quality filtering requires an FM lane");
  const std::size_t available = blah2::available_cpu_threads();
  const std::size_t outerMaximum = std::min(
    std::max<std::size_t>(1, available - 1), illuminators.size());
  const std::size_t outerWorkers = config_.laneWorkers ?
    std::min(config_.laneWorkers, outerMaximum) : outerMaximum;
  auto laneConfig = config_;
  laneConfig.workers = std::max<std::size_t>(1,
    std::min(config_.workers, available / outerWorkers));
  contexts_.reserve(illuminators.size());
  std::shared_ptr<resident::LaneBudget> residentBudget;
  bool residentSelectable = config_.accelerationMode == "gpu-resident";
  std::string residentDevice = config_.accelerationDevice;
#ifdef VECTORWARP_RESIDENT_GPU
  constexpr bool residentBuilt = true;
#else
  constexpr bool residentBuilt = false;
#endif
  if (config_.accelerationMode != "cpu")
    residentBudget = std::make_shared<resident::LaneBudget>(uint32_t(illuminators.size()),
      config_.residentMaximumSessions, config_.residentMemoryMaximumBytes);
  if (config_.accelerationMode == "gpu-resident" ||
      (config_.accelerationMode == "auto" && !backendFactory && residentBuilt)) {
    try {
      // The native caller may already have pinned the exact resident ID. AUTO
      // uses the same isolated enumeration and strict selector, never a vendor.
      if (config_.accelerationMode == "auto")
        residentDevice = selectResidentGpuDevice(queryGpuDriverStatus(),
          config_.accelerationDevice);
      residentSelectable = true;
    } catch (const std::exception&) {
      if (config_.accelerationMode == "gpu-resident") throw;
      // Portable-stage AUTO retains its own precise device/fallback receipt.
    }
  }
  for (const auto& illuminator : illuminators)
    contexts_.push_back(std::make_unique<Context>(captureRateHz_,
      channelCount_, illuminator, laneConfig, backendFactory, residentBudget,
      uint32_t(contexts_.size()), residentSelectable, residentDevice));
  executor_ = std::make_unique<utility::FixedExecutor>(outerWorkers);
}

void RadarBank::prepareStartup()
{
  if (inputStarted_)
    throw std::logic_error("Cannot prepare multi-illuminator startup after input");
  if (startupPrepared_) return;
  // Qualify with the same bounded lane concurrency used by live processing.
  // Resident transactions use the aggregate lease; portable workers may
  // overlap within the lane executor and retain independent transaction locks.
  // Independent CPU oracles use those same already-budgeted lane workers. No
  // capture input is admitted until every lane has completed successfully.
  const auto started = std::chrono::steady_clock::now();
  executor_->parallel_for(contexts_.size(),
    [&](std::size_t lane) { contexts_[lane]->prepareStartup(); });
  const auto milliseconds = std::chrono::duration<double, std::milli>(
    std::chrono::steady_clock::now() - started).count();
  std::cout << "[StartupTiming] phase=radar-lane-qualification lanes="
    << contexts_.size() << " workers=" << executor_->size()
    << " elapsed_ms=" << milliseconds << '\n';
  startupPrepared_ = true;
}

std::vector<RadarFrame> RadarBank::process(
  std::vector<iq::SignalBlock> input,
  std::optional<LiveTemporalIdentity> liveIdentity)
{
  for (const auto& context : contexts_)
    if (context->nativeObserverFailed)
      throw std::logic_error("Cannot reuse a lane after a native CAF observer failure");
  if (input.size() != contexts_.size())
    throw std::invalid_argument("Multi-illuminator output set changed");
  inputStarted_ = true;
  std::unordered_map<std::string, iq::SignalBlock*> byId;
  for (auto& illuminator : input)
    if (!byId.emplace(illuminator.id, &illuminator).second)
      throw std::invalid_argument("Multi-illuminator output IDs are duplicated");
  // Validate the complete identity set before advancing any adaptive context.
  for (const auto& context : contexts_)
  {
    const auto found = byId.find(context->definition.id);
    if (found == byId.end() ||
        found->second->frequencyHz != context->definition.frequencyHz)
      throw std::invalid_argument("Multi-illuminator output identity changed");
  }
  try
  {
    std::vector<std::vector<RadarFrame>> perContext(contexts_.size());
    executor_->parallel_for(contexts_.size(), [&](std::size_t index) {
      auto& context = *contexts_[index];
      const auto& previous = context.liveTemporalIdentity;
      if (context.positioned && (bool(previous) != bool(liveIdentity) ||
          (previous && liveIdentity &&
           (previous->captureEpoch != liveIdentity->captureEpoch ||
            previous->inputGainDb != liveIdentity->inputGainDb ||
            previous->samplePositionAvailable != liveIdentity->samplePositionAvailable))))
        context.clear("capture_identity_changed");
      context.liveTemporalIdentity = liveIdentity;
      perContext[index] = contexts_[index]->consume(
        byId.at(contexts_[index]->definition.id)->samples);
    });
    std::vector<RadarFrame> result;
    for (auto& frames : perContext)
      for (auto& frame : frames) result.push_back(std::move(frame));
    return result;
  }
  catch (...)
  {
    reset();
    throw;
  }
}

std::vector<LaneHealthStatus> RadarBank::laneHealth() const
{
  std::vector<LaneHealthStatus> result;
  result.reserve(contexts_.size());
  for (const auto& context : contexts_) result.push_back(context->health);
  return result;
}

void RadarBank::reset()
{
  for (auto& context : contexts_) context->clear();
}

} // namespace blah2::pipeline
