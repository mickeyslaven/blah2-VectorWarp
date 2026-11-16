#include "process/analysis/MultiIlluminatorRadar.h"
#include "process/integration/TemporalEvidenceArtifact.h"
#include "process/ambiguity/Acceleration.h"
#include "process/ambiguity/GpuResidentCoordinator.h"
#include "process/ambiguity/GpuDeviceSelection.h"
#include "process/ambiguity/ResidentLaneBudget.h"
#include "process/ambiguity/GpuLaneLease.h"
#include "process/analysis/StartupIq.h"
#include "process/conditioning/FmReferenceQualityFilter.h"
#include "process/utility/ProcessingThreads.h"

#include <algorithm>
#include <cmath>
#include <chrono>
#include <limits>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <utility>
#include <unistd.h>

namespace blah2::multi_illuminator
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
  const channelizer::IlluminatorDefinition& definition)
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
  // Both currently supported shadow inputs have discarded complex CAF phase.
  // A real-valued sqrt(power) map cannot supply measured phase coherence.
  if (config.temporalShadowCoherentPathEvidence)
    throw std::invalid_argument(
      "Coherent temporal evidence requires a phase-preserving complex input; "
      "fused and post-clutter bootstrap maps contain only magnitude");
  if (config.surveillanceFusionMode != "noncoherent" &&
      config.surveillanceFusionMode != "coherent_beam_bank" &&
      config.surveillanceFusionMode != "noise_whitened_beam_bank")
    throw std::invalid_argument("Unknown surveillance fusion statistic");
  if (config.surveillanceManifoldAutomation)
    throw std::invalid_argument(
      "Multi-illuminator mode does not support detection-manifold automation");
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
  // Each lane currently owns a CPU tracker.  A gpu_capacity inherited from a
  // single-illuminator profile therefore has no effect here, but it must not
  // make an otherwise valid multi-illuminator configuration unstartable.
  if (config.trackerAccelerationModel != "hypothesis_bank" &&
      config.trackerAccelerationModel != "adaptive_gate")
    throw std::invalid_argument(
      "Multi-illuminator tracker initiation mode is invalid");
  if ((config.trackGuidedShadow || config.trackGuidedPromotion ||
      config.trackGuidedSustain) && !config.trackBeforeDetect)
    throw std::invalid_argument(
      "Track-guided support requires track-before-detect history");
  if (config.bearingFrequencyResolvedShadow || config.bearingManifoldRecorder)
    throw std::invalid_argument(
      "Bearing calibration workers require an explicitly lane-bound artifact identity");
  if (config.temporalShadow &&
      (!config.temporalShadowOfflineReplay ||
       config.temporalShadowIlluminatorId.empty() ||
       config.temporalShadowArtifactPath.empty() ||
       config.temporalShadowArtifactMaximumBytes < (64ULL << 20) ||
       config.temporalShadowArtifactMaximumBytes > (1ULL << 40) ||
       (config.temporalShadowInputStatistic != "fused" &&
        config.temporalShadowInputStatistic != "post_clutter_bootstrap") ||
       (config.temporalShadowBootstrapCovariance != "diagonal" &&
        config.temporalShadowBootstrapCovariance != "shrinkage" &&
        config.temporalShadowBootstrapCovariance != "robust_shrinkage") ||
       (config.temporalShadowMaximumCpis != 4 &&
        config.temporalShadowMaximumCpis != 8) ||
       config.temporalShadowDelayMotionBins > 2 ||
       config.temporalShadowDopplerMotionBins > 16))
    throw std::invalid_argument(
      "Multi-illuminator temporal shadow configuration is invalid");
}

struct MultiIlluminatorRadar::Context
{
  channelizer::IlluminatorDefinition definition;
  uint32_t captureRate = 0, samples = 0, decimation = 0;
  RadarConfig config;
  std::vector<std::vector<std::complex<double>>> pending;
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
  std::unique_ptr<production_replay::RawIqStages> stages;
  std::vector<std::unique_ptr<Ambiguity>> ownedAmbiguity;
  std::vector<Ambiguity*> ambiguity;
  std::unique_ptr<Acceleration> acceleration;
  std::shared_ptr<resident::LaneBudget> residentBudget;
  std::unique_ptr<resident::Coordinator> residentFrontend;
  AccelerationStatus residentStatus;
  std::vector<std::complex<double>> residentReferenceWeights;
  uint64_t residentLogical = 0, residentConsumed = 0, residentAllocationLimit = 0;
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
  CfarDetector1D detector;
  std::unique_ptr<detection::DetectorIntegration> detectorIntegration;
  DopplerRidgeScrubber ridgeScrubber;
  DopplerRidgeShadowWindow ridgeShadowWindow;
  RadarFrame::InputProcessingMetrics inputProcessing;
  std::unique_ptr<Centroid> centroid;
  Interpolate interpolate{true, true};
  Tracker tracker;
  std::unique_ptr<Tracker> comparisonTracker;
  bool directFloatBorrowed = false;
  std::unique_ptr<conditioning::FmReferenceQualityFilter> fmReferenceFilter;
  std::unique_ptr<conditioning::AtscPilotQuality> atscPilot;
  std::unique_ptr<conditioning::AtscPilotProjector> atscPilotProjector;
  std::optional<conditioning::AtscPilotProjector::Metrics>
    atscPilotProjectionMetrics;
  std::unique_ptr<integration::TemporalEvidence> temporalShadow;
  std::unique_ptr<integration::LiveTemporalShadow> liveTemporalShadow;
  std::optional<LiveTemporalIdentity> liveTemporalIdentity;
  std::unique_ptr<integration::TemporalEvidenceArtifact> temporalArtifact;
  std::unique_ptr<detection::PostClutterBootstrap> postClutterBootstrap;
  uint64_t temporalShadowEpoch = 1;
  uint64_t temporalShadowSequence = 0;
  uint64_t lastTrackedTemporalSequence = 0;
  uint64_t lastTrackedTemporalEpoch = 0;
  NativeCafObserver nativeObserver;
  bool nativeObserverFailed = false;
  LaneHealthStatus health;

  Context(uint32_t captureRateHz, std::size_t channelCount,
    channelizer::IlluminatorDefinition value, RadarConfig radar,
    const Acceleration::BackendFactory& backendFactory,
    std::shared_ptr<resident::LaneBudget> budget, uint32_t laneIndex,
    bool residentSelectable)
    : definition(std::move(value)), captureRate(captureRateHz),
      samples(uint32_t(std::llround(radar.cpiSeconds * definition.outputRateHz))),
      decimation(captureRateHz / definition.outputRateHz),
      config(scaleRadarConfigForLane(applyLaneTopology(std::move(radar), definition),
        definition.outputRateHz)),
      pending(channelCount),
      reference(samples), conditionedReference(samples), projectedReference(samples),
      detector(config.pfa, config.guardCells, config.trainingCells,
        config.minimumDelay, config.minimumDopplerHz,
        config.advanced.detectorIntegrationCpis),
      ridgeScrubber(config.dopplerRidgeScrubber),
      ridgeShadowWindow(config.dopplerRidgeScrubber.shadowMaximumCpis),
      tracker(config.trackerM, config.trackerN, config.trackerDelete,
        config.cpiSeconds, config.trackerMaximumAcceleration,
        speedOfLight / definition.outputRateHz,
        speedOfLight / definition.frequencyHz, config.trackerMinimumSnrDb,
        config.advanced.trackerAccelerationModel == "adaptive_gate",
        config.trackerDopplerRidge, config.trackerNormalizedInnovationGate)
  {
    residentBudget = std::move(budget);
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
          identity, bearing_manifold::DetectionUse::liveProduction);
        const auto& calibrated = *detectionCalibration;
        geometry.measuredResponses.assign(calibrated.responses.begin(), calibrated.responses.end());
        geometry.measuredDirectResponse.assign(calibrated.directResponse.begin(), calibrated.directResponse.end());
      }
      coherentFusion = std::make_unique<CoherentBeamBank>(geometry);
      residentProductRoute = config.residentProductTemplate.has_value();
    }
    if (residentRequired && !residentProductRoute)
      throw std::invalid_argument("Selected coherent statistic/bearing history requires a qualified resident backend");
    if (residentProductRoute && config.dopplerRidgeScrubber.mode != "off")
      throw std::invalid_argument("Resident coherent products do not accept a host-only ridge background statistic");
    if (config.advanced.detectorIntegrationCpis > 1)
      detectorIntegration = std::make_unique<detection::DetectorIntegration>(
        config.advanced.detectorIntegrationCpis,
        config.advanced.detectorIntegrationMotionBins,
        track_before_detect::delayDriftPerHz(samples, definition.frequencyHz));
    conditioning::validateAtscReferenceMode(definition.standard,
      definition.atscReferenceMode);
    if (definition.atscReferenceMode == "pilot_aided")
    {
      if (config.arrayReference)
        throw std::invalid_argument(
          "ATSC pilot_aided does not support array reference");
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
    if (config.advanced.temporalShadow &&
        definition.id == config.advanced.temporalShadowIlluminatorId)
    {
      temporalShadow = std::make_unique<integration::TemporalEvidence>(
        integration::TemporalEvidence::Config{
          config.advanced.temporalShadowMaximumCpis,
          config.advanced.temporalShadowDelayMotionBins,
          config.advanced.temporalShadowDopplerMotionBins});
      temporalArtifact = std::make_unique<integration::TemporalEvidenceArtifact>(
        config.advanced.temporalShadowArtifactPath,
        config.advanced.temporalShadowArtifactMaximumBytes,
        config.advanced.temporalShadowMaximumCpis,
        config.advanced.temporalShadowDelayMotionBins,
        config.advanced.temporalShadowDopplerMotionBins,
        uint64_t(samples) * decimation,
        decimation,
        config.advanced.temporalShadowInputStatistic,
        config.advanced.temporalShadowBootstrapCovariance);
      if (config.advanced.temporalShadowInputStatistic ==
          "post_clutter_bootstrap")
      {
        auto bootstrap = detection::PostClutterBootstrap::Config{};
        bootstrap.covariance =
          config.advanced.temporalShadowBootstrapCovariance == "robust_shrinkage" ?
            detection::PostClutterBootstrap::Covariance::robustShrinkage :
          config.advanced.temporalShadowBootstrapCovariance == "shrinkage" ?
            detection::PostClutterBootstrap::Covariance::shrinkage :
            detection::PostClutterBootstrap::Covariance::diagonal;
        postClutterBootstrap =
          std::make_unique<detection::PostClutterBootstrap>(bootstrap);
      }
    }
    ownedChannels.reserve(channelCount);
    for (std::size_t channel = 0; channel < channelCount; ++channel)
      ownedChannels.push_back(std::make_unique<IqData>(samples));
    channels.reserve(config.surveillanceChannels.size());
    for (const uint32_t channel : config.surveillanceChannels)
      channels.push_back(ownedChannels[channel].get());
    if (config.arrayReference)
      for (const uint32_t channel : config.referenceChannels)
        referenceChannels.push_back(ownedChannels[channel].get());

    ownedAmbiguity.reserve(channels.size());
    for (std::size_t channel = 0; channel < channels.size(); ++channel)
    {
      ownedAmbiguity.push_back(std::make_unique<Ambiguity>(config.delayMin,
        config.delayMax, config.dopplerMin, config.dopplerMax,
        definition.outputRateHz, samples, true));
      ambiguity.push_back(ownedAmbiguity.back().get());
    }
    // CAF block rounding changes the actual Doppler spacing. Keep input
    // cadence/sample ranges unchanged, but use the same effective CPI as the
    // single-lane path for centroid frequency spans and tracker hypotheses.
    centroid = std::make_unique<Centroid>(config.centroidRadius,
      *config.centroidDopplerRadius, 1.0 / ambiguity.front()->get_cpi());
    resetTracker();
    if (config.liveTemporalShadow.enabled &&
        definition.id == config.liveTemporalShadowIlluminatorId) {
      auto shadowConfig = config.liveTemporalShadow;
      // The top-level parser binds scoring to the unchannelized geometry.
      // Rebind it after per-lane delay scaling and topology selection so its
      // CFAR and tracker counts are directly comparable with this lane.
      shadowConfig.scoring.pfa = config.pfa;
      shadowConfig.scoring.guardCells = config.guardCells;
      shadowConfig.scoring.trainingCells = config.trainingCells;
      shadowConfig.scoring.minimumDelay = config.minimumDelay;
      shadowConfig.scoring.minimumDopplerHz = config.minimumDopplerHz;
      shadowConfig.scoring.confirmationM = config.trackerM;
      shadowConfig.scoring.confirmationN = config.trackerN;
      shadowConfig.scoring.deleteAfter = config.trackerDelete;
      shadowConfig.scoring.cpiSeconds = ambiguity.front()->get_cpi();
      shadowConfig.scoring.maximumAccelerationHzPerSecond =
        config.trackerMaximumAcceleration;
      shadowConfig.scoring.rangeResolutionM =
        speedOfLight / definition.outputRateHz;
      shadowConfig.scoring.wavelengthM =
        speedOfLight / definition.frequencyHz;
      if (!shadowConfig.scoring.minimumInitiationSnrOverridden)
        shadowConfig.scoring.minimumInitiationSnrDb = config.trackerMinimumSnrDb;
      shadowConfig.scoring.adaptiveGate =
        config.advanced.trackerAccelerationModel == "adaptive_gate";
      liveTemporalShadow = std::make_unique<integration::LiveTemporalShadow>(
        shadowConfig, *ambiguity.front()->result(),
        config.surveillanceChannels);
    }
    if (config.accelerationMode == "gpu-resident" ||
        (config.accelerationMode == "auto" && residentSelectable)) {
      residentAttempted = true;
      residentStatus.requested = config.accelerationMode;
      residentStatus.active = "cpu";
      residentStatus.state = "fallback";
      try {
        if (!residentBudget)
          throw std::runtime_error("No selectable FP64 resident device");
        if (fmReferenceFilter || atscPilotProjector)
          throw std::runtime_error("Configured reference conditioning requires the common host frontend");
        if (!config.reference.protectionRelativeSteering.empty() ||
            config.reference.commonModeDecorrelation != 0)
          throw std::runtime_error("Configured reference statistic is absent from the resident input contract");
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
        frontend.device = config.accelerationDevice;
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
          frontend.detector.guard = config.guardCells;
          frontend.detector.train = config.trainingCells;
          frontend.detector.minDelay = config.minimumDelay;
          frontend.detector.centroidDelay = config.centroidRadius;
          frontend.detector.centroidDoppler = *config.centroidDopplerRadius;
          frontend.detector.centroidDopplerResolution = 1 / shape.get_cpi();
          frontend.tracker.cpi = shape.get_cpi();
          frontend.tracker.rangeRes = speedOfLight / definition.outputRateHz;
          frontend.tracker.lambda = speedOfLight / definition.frequencyHz;
          frontend.tracker.minInitiationSnrDb = config.trackerMinimumSnrDb;
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
        options.allocationLimitBytes = residentAllocationLimit;
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
    // A failed/excess resident reservation cannot escape into an independent
    // portable worker with another allocation ceiling and execution lease.
    if (!residentFrontend && !residentAttempted && config.accelerationMode != "cpu" &&
        config.accelerationMode != "gpu-resident")
    {
      const auto& shape = *ownedAmbiguity.front();
      const int64_t clutterBins = int64_t(config.clutterDelayMax) -
        config.clutterDelayMin;
      if (clutterBins <= 0 || uint64_t(clutterBins) > UINT32_MAX)
        throw std::invalid_argument("Multi-illuminator clutter width is invalid");
      // Offer complete lane geometry on every platform. Capability, memory,
      // Precision and runtime qualification decide each stage independently.
      const Acceleration::BackendFactory boundedFactory =
        [budget = residentBudget, laneIndex, backendFactory](const GpuGeometry& geometry,
            const std::string& device) {
          try {
          if (!budget) throw std::runtime_error("GPU lane has no aggregate reservation");
          GpuProcessOptions options;
          options.allocationLimitBytes = budget->allocationLimit(laneIndex,
            gpuProcessMappingBytes(geometry));
          std::unique_ptr<GpuBackend> worker;
          {
            std::lock_guard<std::mutex> lease(budget->execution);
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
          samples, uint32_t(clutterBins), config.clutterDelayMin},
        shape.get_n_corr(), definition.outputRateHz,
        shape.get_doppler_middle(), config.accelerationDevice, boundedFactory);
    }
    preferBorrowedStages = config.borrowedClutter.value_or(
      !config.arrayReference && config.surveillanceChannels.size() == 1);
    const bool borrowed = preferBorrowedStages && !residentFrontend &&
      !(acceleration && acceleration->clutterAvailable());
    borrowedStages = borrowed;
    stages = std::make_unique<production_replay::RawIqStages>(
      production_replay::RawIqStages::Config{config.clutterDelayMin,
        config.clutterDelayMax, samples, config.surveillanceChannels.size(),
        config.workers, borrowed, config.reference, true});
    directFloatBorrowed = borrowed && !fmReferenceFilter &&
      !config.arrayReference && config.surveillanceChannels.size() == 1;
  }

  void selectCpuStages()
  {
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
    if (temporalShadow) temporalShadow->reset();
    if (detectorIntegration) detectorIntegration->reset();
    if (liveTemporalShadow) liveTemporalShadow->invalidate();
    if (++temporalShadowEpoch == 0) temporalShadowEpoch = 1;
    temporalShadowSequence = 0;
    lastTrackedTemporalSequence = 0;
    lastTrackedTemporalEpoch = 0;
    ridgeScrubber.reset();
    ridgeShadowWindow.reset(reason);
    inputProcessing = {};
    resetTracker();
  }

  void resetTracker()
  {
    tracker = Tracker(config.trackerM, config.trackerN,
      config.trackerDelete, ambiguity.front()->get_cpi(),
      config.trackerMaximumAcceleration,
      speedOfLight / definition.outputRateHz,
      speedOfLight / definition.frequencyHz, config.trackerMinimumSnrDb,
      config.advanced.trackerAccelerationModel == "adaptive_gate",
      config.trackerDopplerRidge, config.trackerNormalizedInnovationGate);
    tracker.configureRecovery(config.trackerRecovery);
    comparisonTracker = config.dopplerRidgeScrubber.mode == "off" ? nullptr :
      std::make_unique<Tracker>(config.trackerM, config.trackerN,
        config.trackerDelete, ambiguity.front()->get_cpi(),
        config.trackerMaximumAcceleration,
        speedOfLight / definition.outputRateHz,
        speedOfLight / definition.frequencyHz, config.trackerMinimumSnrDb,
        config.advanced.trackerAccelerationModel == "adaptive_gate",
        config.trackerDopplerRidge, config.trackerNormalizedInnovationGate);
    if (comparisonTracker) comparisonTracker->configureRecovery(config.trackerRecovery);
  }

  std::vector<RadarFrame> consume(
    const channelizer::SubbandChannelizer::Output& input)
  {
    if (input.channels.size() != pending.size() ||
        input.inputSamplesPerOutput != decimation)
      throw std::invalid_argument("Channelized illuminator topology changed");
    if (input.channels.empty()) return {};
    const std::size_t count = input.channels.front().size();
    for (const auto& channel : input.channels)
      if (channel.size() != count)
        throw std::invalid_argument("Channelized illuminator channels are unpaired");
    if (!count) return {};
    if (!input.hasFirstInputSample)
      throw std::invalid_argument("Channelized illuminator has no sample position");
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
        const auto& channel = input.channels[config.referenceChannel];
        atscPilot->observe(observed,
          [&](std::size_t index) { return channel[offset + index]; },
          pendingFirstInputSample + uint64_t(pendingCount) * decimation,
          decimation, input.groupDelayInputSamples);
        inputProcessing.pilotQualityMs += elapsedMs(qualityStart);
      }
      // Normal live capture produces an exact CPI per lane. Load that block
      // straight into the retained DSP buffers instead of first widening it
      // into the partial-CPI accumulator and then copying it a second time.
      if (pendingCount == 0 && count - offset >= samples)
      {
        if (directFloatBorrowed)
        {
          if (auto frame = process_float_cpi(input, offset,
                pendingFirstInputSample))
            result.push_back(std::move(*frame));
          pendingFirstInputSample += uint64_t(samples) * decimation;
          offset += samples;
          continue;
        }
        const auto ingestionStart = CostClock::now();
        for (std::size_t channel = 0; channel < pending.size(); ++channel)
        {
          auto& destination = ownedChannels[channel]->resize_for_write(samples);
          std::transform(input.channels[channel].begin() + offset,
            input.channels[channel].begin() + offset + samples,
            destination.begin(), [](const std::complex<float> value) {
              return std::complex<double>(value);
            });
        }
        inputProcessing.widenedSamples += uint64_t(samples) * pending.size();
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
      inputProcessing.path = "partial_double";
      for (std::size_t channel = 0; channel < pending.size(); ++channel)
      {
        if (pending[channel].empty()) pending[channel].resize(samples);
        std::transform(input.channels[channel].begin() + offset,
          input.channels[channel].begin() + offset + copied,
          pending[channel].begin() + pendingCount,
          [](const std::complex<float> value) {
            return std::complex<double>(value);
          });
      }
      inputProcessing.widenedSamples += uint64_t(copied) * pending.size();
      inputProcessing.ingestionMs += elapsedMs(ingestionStart);
      pendingCount += copied;
      offset += copied;
      if (pendingCount == samples)
      {
        const auto copyStart = CostClock::now();
        for (std::size_t channel = 0; channel < pending.size(); ++channel)
          ownedChannels[channel]->assign_complex(pending[channel].data(), samples);
        inputProcessing.copiedSamples += uint64_t(samples) * pending.size();
        inputProcessing.ingestionMs += elapsedMs(copyStart);
        if (auto frame = process_loaded_cpi(pendingFirstInputSample))
          result.push_back(std::move(*frame));
        pendingFirstInputSample += uint64_t(samples) * decimation;
        pendingCount = 0;
      }
    }
    return result;
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
    if (atscPilot && atscPilotProjector)
    {
      const auto projectionStart = CostClock::now();
      const auto& pilot = atscPilot->metrics();
      // Successful projection overwrites the complete CPI; rejected projection
      // is never consumed. Retain storage instead of freeing/zeroing millions
      // of samples before each call, including while reacquiring pilot lock.
      atscPilotProjectionMetrics = atscPilotProjector->process(
        selectedReference->view_data(),
        projectedReference.resize_for_write(projectedReference.get_length()),
        pilot.cfoHz, pilot.available && pilot.pilotCandidate);
      inputProcessing.pilotProjectionMs += elapsedMs(projectionStart);
      if (atscPilotProjectionMetrics->applied)
        ambiguityReference = &projectedReference;
    }
    const auto runClutter = [&] {
      return stages->clutter(*selectedReference, channels);
    };
    if (!(acceleration ? acceleration->processClutter(*selectedReference,
          channels, runClutter) : runClutter()))
      return false;
    clutterGpuExecuted = acceleration && acceleration->clutterTiming().gpuExecuted;
    clutterCpuExecuted = !acceleration || acceleration->clutterTiming().cpuExecuted;
    if (borrowedStages)
      process_borrowed_ambiguity(
        ambiguityReference == selectedReference ? nullptr : ambiguityReference);
    else {
      const auto runAmbiguity = [&] {
        stages->ambiguity(*ambiguityReference, channels, ambiguity, preserveSurveillance);
      };
      if (acceleration)
        acceleration->process(ambiguityReference->view_data(), channels,
          ambiguity, runAmbiguity);
      else
        runAmbiguity();
    }
    return true;
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

  std::optional<RadarFrame> process_loaded_cpi(uint64_t first)
  {
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
    if (residentFrontend) {
      try {
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
        selectCpuStages();
      }
    }
    if (residentExecuted) return finish_processed_cpi(first);
    if (!process_loaded_dsp()) {
      reject(first, "clutter_reference_rejected");
      resetAdaptiveState();
      return std::nullopt;
    }
    return finish_processed_cpi(first);
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
        residentFrontend->prepareStartup([&](unsigned frame) {
          startup::fillIq(input, samples, frame);
          residentFrontend->begin(ownedChannels, {++residentLogical,
            ++residentConsumed, 1000 + uint64_t(frame) * samples * 1000 /
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
            coherentFusion->calibrate(stages->reference().get_metrics().directPathWeights);
            auto oracleMap = coherentFusion->process(maps);
            oracleMap->set_metrics();
            auto found = detection::detectProducts(*oracleMap, detector, *centroid, interpolate, {});
            auto tracked = tracker.process(found.get(), timestamp);
            const auto products = residentFrontend->format(timestamp, config.bearingPublication);
            residentFrontend->checkProducts(products,
              oracleMap->to_json_km(timestamp, definition.outputRateHz),
              found->to_json_km(timestamp, definition.outputRateHz), tracked->to_json(timestamp));
          } else residentFrontend->checkFrontendProducts();
          residentFrontend->commit();
        }, [&] { stages->resetReference(); });
        residentStatus = residentFrontend->status();
        residentStatus.requested = config.accelerationMode;
        residentStatus.reason = residentProductRoute ?
          "Qualified resident coherent products; lane-bound RF/site/calibration; serialized aggregate lease" :
          "Qualified resident reference/clutter/CAF; shared FP64 host products; serialized aggregate lease";
        if (config.accelerationMode == "auto") {
          std::array<double, 3> cpuTimes{}, gpuTimes{};
          for (unsigned sample = 0; sample < cpuTimes.size(); ++sample) {
            startup::fillIq(input, samples, 100 + sample);
            auto started = CostClock::now();
            if (!process_loaded_dsp(true))
              throw std::runtime_error("AUTO CPU timing reference rejected");
            cpuTimes[sample] = elapsedMs(started);
            startup::fillIq(input, samples, 100 + sample);
            started = CostClock::now();
            residentFrontend->begin(ownedChannels, {++residentLogical,
              ++residentConsumed, 10000 + sample});
            importResidentMaps();
            residentFrontend->commit(); residentFrontend->published();
            gpuTimes[sample] = elapsedMs(started);
          }
          std::sort(cpuTimes.begin(), cpuTimes.end());
          std::sort(gpuTimes.begin(), gpuTimes.end());
          residentStatus.cpuMs = cpuTimes[1]; residentStatus.gpuMs = gpuTimes[1];
          residentFrontend->resetInput(); stages->resetReference();
          // Correctness qualification selects AUTO. Timing is diagnostic;
          // a short startup speed race must not silently select CPU.
        }
      } catch (const std::exception& error) {
        residentFrontend.reset();
        residentStatus.active = "cpu";
        residentStatus.state = "fallback";
        residentStatus.reason = std::string("Resident lane qualification failed: ") + error.what();
        if (residentRequired) throw;
        residentProductRoute = false;
        stages->resetReference();
        selectCpuStages();
        prepareFrame(0);
      }
    }
    else if (acceleration && acceleration->startupPending())
      acceleration->prepareStartup(prepareFrame);
    else
      prepareFrame(0); // CPU/fallback must also finish first-use DSP work.
    selectCpuStages(); // Finish any fallback workspace planning before capture.
    // Reset private reference state before live IQ. FFT planning happens here,
    // while capture is still dormant, and accepted device qualification stays.
    stages->resetReference();
    resetTracker();
    if (atscPilot) atscPilot->reset();
    if (atscPilotProjector) atscPilotProjector->reset();
    atscPilotProjectionMetrics.reset();
    reference.clear();
    conditionedReference.clear();
    projectedReference.clear();
    inputProcessing = {};
    for (auto& channel : ownedChannels) channel->clear();
  }

  std::optional<RadarFrame> process_float_cpi(
    const channelizer::SubbandChannelizer::Output& input,
    std::size_t offset, uint64_t first)
  {
    inputProcessing.path = "native_float_borrowed";
    const auto* rawReference = input.channels[config.referenceChannel].data() + offset;
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
    std::vector<const std::complex<float>*> surveillance;
    surveillance.reserve(config.surveillanceChannels.size());
    for (const uint32_t channel : config.surveillanceChannels)
      surveillance.push_back(input.channels[channel].data() + offset);
    if (!stages->clutter_float(
          rawReference,
          surveillance, samples)) {
      reject(first, "clutter_reference_rejected");
      resetAdaptiveState();
      return std::nullopt;
    }
    clutterGpuExecuted = false;
    clutterCpuExecuted = true;
    process_borrowed_ambiguity(nullptr, projected);
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
      const auto copyStart = CostClock::now();
      const auto& source = qualifiedReference->view_data();
      projectedReferenceContiguous.assign(source.begin(), source.end());
      accelerationReference = projectedReferenceContiguous.data();
      inputProcessing.copiedSamples += source.size();
      inputProcessing.ingestionMs += elapsedMs(copyStart);
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
    std::vector<Map<std::complex<double>>*> maps;
    maps.reserve(ambiguity.size());
    for (const auto& processor : ownedAmbiguity) maps.push_back(processor->result());
    if (liveTemporalShadow) {
      // Consume only the immediately preceding, exactly identified result.
      // The worker is asynchronous, so late/stale evidence is never applied
      // to a newer kinematic state.
      if (config.liveTemporalShadow.associationSustain) {
        auto evidence = liveTemporalShadow->takeLatest();
        if (evidence && evidence->frame.captureEpoch == lastTrackedTemporalEpoch &&
            evidence->frame.sequence == lastTrackedTemporalSequence &&
            evidence->scoring) {
          const auto laneIndex = config.liveTemporalShadow.associationSustainCpis == 2 ?
            0u : config.liveTemporalShadow.associationSustainCpis == 4 ? 1u : 2u;
          const auto& lane = evidence->scoring->lanes[laneIndex];
          if (lane.evaluated && lane.latestEvaluatedSequence == evidence->frame.sequence) {
            std::vector<TrackerWeakSupportCandidate> support;
            support.reserve(lane.latestCandidates.size());
            for (const auto& candidate : lane.latestCandidates)
              support.push_back({candidate.delay, candidate.dopplerHz,
                candidate.lineage.epoch, candidate.lineage.newestSequence,
                candidate.lineage.depth, candidate.lineage.sequences});
            tracker.applyWeakSupport(support);
          }
        }
      }
      // Lane positions remain in capture-rate units after channelization.
      // The worker requires a half-open interval, unlike the inclusive last
      // sample exported in RadarFrame. Configuration is immutable per context.
      const uint64_t offeredEpoch = liveTemporalIdentity ?
        liveTemporalIdentity->captureEpoch : temporalShadowEpoch;
      liveTemporalShadow->offerCpu({offeredEpoch,
        ++temporalShadowSequence, first, first + uint64_t(samples) * decimation,
        definition.frequencyHz, temporalShadowEpoch, captureRate,
        !liveTemporalIdentity || liveTemporalIdentity->samplePositionAvailable,
        liveTemporalIdentity ? liveTemporalIdentity->timestampMs : 0,
        definition.outputRateHz}, maps);
    }
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
    else if (coherentFusion) {
      coherentFusion->calibrate(residentExecuted ? residentReferenceWeights :
        stages->reference().get_metrics().directPathWeights);
      fusedMap = coherentFusion->process(maps);
    } else {
    if (!fusedMap || fusedMap.use_count() != 1)
      fusedMap = std::make_shared<Map<std::complex<double>>>(
        maps.front()->get_nRows(), maps.front()->get_nCols());
    fusion.process_into(maps, *fusedMap);
    }
    auto map = fusedMap;
    map->set_metrics();
    std::shared_ptr<integration::TemporalEvidence::Result> shadow;
    std::optional<detection::PostClutterBootstrap::Result> bootstrap;
    if (temporalShadow)
    {
      Map<std::complex<double>>* shadowInput = map.get();
      if (postClutterBootstrap)
      {
        bootstrap.emplace(postClutterBootstrap->process(maps));
        shadowInput = bootstrap->map.get();
      }
      shadow = std::make_shared<integration::TemporalEvidence::Result>(
        temporalShadow->process(*shadowInput, temporalShadowEpoch,
          ++temporalShadowSequence));
      temporalArtifact->write(*shadowInput, *shadow,
        bootstrap ? bootstrap->statisticFamily : std::vector<uint8_t>{},
        first, first + uint64_t(samples - 1) * decimation);
    }
    const uint64_t end = first + uint64_t(samples - 1) * decimation;
    const uint64_t timestamp = detection::signalTimeMilliseconds(
      first + uint64_t(samples) * decimation, captureRate);
    std::optional<RadarFrame::DetectorIntegrationMetrics> integrationMetrics;
    if (detectorIntegration && !residentProducts) {
      integrationMetrics = detectorIntegration->process(map,
        liveTemporalIdentity ? liveTemporalIdentity->captureEpoch : temporalShadowEpoch,
        first, first + uint64_t(samples) * decimation,
        !liveTemporalIdentity || liveTemporalIdentity->samplePositionAvailable);
    }
    const auto detect = [&](const std::vector<std::vector<double>>* background) {
      return detection::detectProducts(*map, detector, *centroid, interpolate,
        integrationMetrics, background);
    };
    std::unique_ptr<Detection> rawDetections, scrubbedDetections;
    std::unique_ptr<Track> rawTracks, scrubbedTracks;
    const auto scrubberMode = config.dopplerRidgeScrubber.mode;
    const bool compare = scrubberMode == "active" ||
      (scrubberMode == "shadow" && ridgeShadowWindow.available());
    double backgroundMs = 0, comparisonDetectionMs = 0, comparisonTrackingMs = 0;
    if (residentProducts) {
      // Storage helpers only. Public records remain the immutable resident
      // products, never a reconstruction of GPU tracker state on the host.
      rawDetections = std::make_unique<Detection>(std::vector<double>{}, std::vector<double>{}, std::vector<double>{});
      rawTracks = std::make_unique<Track>();
    }
    else if (!compare)
    {
      rawDetections = detect(nullptr);
      rawTracks = detection::trackProducts(*rawDetections, tracker, timestamp, integrationMetrics, &detector);
    }
    else
    {
      auto stageStart = CostClock::now();
      const auto& background = ridgeScrubber.process(map.get(),
        config.minimumDelay);
      backgroundMs = elapsedMs(stageStart);
      stageStart = CostClock::now();
      rawDetections = detect(nullptr);
      const double rawDetectionMs = elapsedMs(stageStart);
      stageStart = CostClock::now();
      scrubbedDetections = detect(&background);
      const double scrubbedDetectionMs = elapsedMs(stageStart);
      if (scrubberMode == "active")
      {
        scrubbedTracks = detection::trackProducts(*scrubbedDetections, tracker, timestamp, integrationMetrics, &detector);
        stageStart = CostClock::now();
        rawTracks = detection::trackProducts(*rawDetections, *comparisonTracker, timestamp, integrationMetrics, &detector);
        comparisonTrackingMs = elapsedMs(stageStart);
        comparisonDetectionMs = rawDetectionMs;
      }
      else
      {
        rawTracks = detection::trackProducts(*rawDetections, tracker, timestamp, integrationMetrics, &detector);
        stageStart = CostClock::now();
        scrubbedTracks = detection::trackProducts(*scrubbedDetections,
          *comparisonTracker, timestamp, integrationMetrics, &detector);
        comparisonTrackingMs = elapsedMs(stageStart);
        comparisonDetectionMs = scrubbedDetectionMs;
      }
      if (scrubberMode == "shadow")
        ridgeShadowWindow.observe(backgroundMs + comparisonDetectionMs + comparisonTrackingMs);
    }
    if (liveTemporalShadow) {
      lastTrackedTemporalSequence = temporalShadowSequence;
      lastTrackedTemporalEpoch = liveTemporalIdentity ?
        liveTemporalIdentity->captureEpoch : temporalShadowEpoch;
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
    result.inputProcessing = inputProcessing;
    result.detectorIntegration = std::move(integrationMetrics);
    inputProcessing = {};
    result.atscReferenceRequested = definition.atscReferenceMode;
    if (atscPilot)
    {
      result.atscPilot = atscPilot->metrics();
      result.atscPilotProjection = atscPilotProjectionMetrics;
      result.atscReferenceEffective = atscPilotProjectionMetrics &&
        atscPilotProjectionMetrics->applied ? "pilot_aided" : "raw";
      result.atscReferenceReason = atscPilotProjectionMetrics ?
        atscPilotProjectionMetrics->reason : "pilot_not_observed";
    }
    result.trackerDiagnostics = tracker.frame_diagnostics();
    result.trackerMinimumSnrDb = config.trackerMinimumSnrDb;
    result.trackerMinimumSnrOverridden =
      config.trackerMinimumSnrDbByIlluminator.count(definition.id) != 0;
    if (liveTemporalShadow) result.liveTemporalShadow = liveTemporalShadow->status();
    if (scrubberMode != "off")
    {
      const auto& metrics = ridgeScrubber.metrics();
      RadarFrame::RidgeScrubberMetrics comparison;
      comparison.mode = scrubberMode;
      comparison.comparisonAvailable = compare;
      comparison.state = scrubberMode == "active" ? "continuous" :
        ridgeShadowWindow.available() ? "running" : "completed";
      comparison.reason = comparison.state == "completed" ?
        "shadow_window_complete" : "contiguous_comparison";
      comparison.epoch = ridgeShadowWindow.epoch();
      comparison.resetReason = ridgeShadowWindow.resetReason();
      if (scrubberMode == "shadow") {
        comparison.observedCpis = ridgeShadowWindow.observed();
        comparison.maximumCpis = ridgeShadowWindow.maximum();
        comparison.totalComparisonMs = ridgeShadowWindow.totalMs();
      }
      comparison.backgroundMs = backgroundMs;
      comparison.comparisonDetectionMs = comparisonDetectionMs;
      comparison.comparisonTrackingMs = comparisonTrackingMs;
      comparison.comparisonMs = backgroundMs + comparisonDetectionMs + comparisonTrackingMs;
      comparison.observedRows = metrics.observedRows;
      comparison.qualifiedRows = metrics.qualifiedRows;
      comparison.maximumAppliedDb = metrics.maximumAppliedDb;
      comparison.qualifiedDopplerHz = metrics.qualifiedDopplerHz;
      comparison.rawDetections = rawDetections->get_nDetections();
      comparison.scrubbedDetections = scrubbedDetections ? scrubbedDetections->get_nDetections() : 0;
      comparison.rawTracks = rawTracks->get_n();
      comparison.scrubbedTracks = scrubbedTracks ? scrubbedTracks->get_n() : 0;
      comparison.rawOperationalTracks = rawTracks->get_nState("ACTIVE") +
        rawTracks->get_nState("COASTING");
      comparison.scrubbedOperationalTracks = scrubbedTracks ?
        scrubbedTracks->get_nState("ACTIVE") +
        scrubbedTracks->get_nState("COASTING") : 0;
      result.ridgeScrubber = std::move(comparison);
      if (scrubberMode == "shadow" && compare && !ridgeShadowWindow.available()) {
        // Release only private observation state. No production map, detector,
        // tracker or configured mode is reset or replaced on completion.
        comparisonTracker.reset();
        ridgeScrubber.reset();
      }
    }
    result.map = std::move(map);
    result.residentProducts = std::move(residentProducts);
    result.residentTemporalExecuted = result.residentProducts &&
      config.advanced.detectorIntegrationCpis > 1;
    if (scrubberMode == "active")
    {
      result.detections = std::move(scrubbedDetections);
      result.tracks = std::move(scrubbedTracks);
    }
    else
    {
      result.detections = std::move(rawDetections);
      result.tracks = std::move(rawTracks);
    }
    if (atscPilot)
    {
      atscPilot->reset();
      atscPilotProjectionMetrics.reset();
    }
    if (shadow)
    {
      result.temporalShadowActive = true;
      result.temporalShadowSequence = shadow->sequence;
      result.temporalShadowAvailableCpis = shadow->availableCpis;
      result.temporalShadowMaximumCpis = config.advanced.temporalShadowMaximumCpis;
      result.temporalShadowInputStatistic = postClutterBootstrap ?
        "post_clutter_bootstrap" : "fused";
      if (bootstrap)
      {
        result.temporalShadowBootstrapTrainedCells = bootstrap->trainedCells;
        result.temporalShadowBootstrapFallbackCells = bootstrap->fallbackCells;
        result.temporalShadowBootstrapInvalidCells = bootstrap->invalidCells;
        result.temporalShadowBootstrapStatisticFamily =
          bootstrap->statisticFamily;
      }
      for (uint32_t row = 0; row < shadow->map->get_nRows(); ++row)
        for (uint32_t column = 0; column < shadow->map->get_nCols(); ++column)
        {
          result.temporalShadowPeakPower = std::max(
            result.temporalShadowPeakPower,
            std::norm(shadow->map->data[row][column]));
          const auto lineage = shadow->lineageAt(
            uint64_t(row) * shadow->map->get_nCols() + column);
          if (lineage.depth > 1) ++result.temporalShadowIntegratedCells;
        }
      for (std::size_t bank = 0; bank < shadow->banks.size(); ++bank)
        result.temporalShadowBankReady[bank] = shadow->banks[bank].ready;
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

MultiIlluminatorRadar::~MultiIlluminatorRadar() = default;

void MultiIlluminatorRadar::setNativeCafObserver(NativeCafObserver observer)
{
  if (inputStarted_)
    throw std::logic_error("Cannot replace the native CAF observer after input");
  for (auto& context : contexts_) context->nativeObserver = observer;
}

std::optional<integration::LiveTemporalShadow::Status>
MultiIlluminatorRadar::liveTemporalShadowStatus() const
{
  for (const auto& context : contexts_)
    if (context->liveTemporalShadow) return context->liveTemporalShadow->status();
  return std::nullopt;
}

std::unique_ptr<integration::LiveTemporalShadow::Evidence>
MultiIlluminatorRadar::takeLiveTemporalEvidence()
{
  for (const auto& context : contexts_)
    if (context->liveTemporalShadow) return context->liveTemporalShadow->takeLatest();
  return {};
}

MultiIlluminatorRadar::MultiIlluminatorRadar(uint32_t captureRateHz,
  std::size_t channelCount,
  const std::vector<channelizer::IlluminatorDefinition>& illuminators,
  RadarConfig config, Acceleration::BackendFactory backendFactory)
  : captureRateHz_(captureRateHz), channelCount_(channelCount),
    config_(std::move(config))
{
  if (!captureRateHz_ || !channelCount_ || illuminators.empty() ||
      !std::isfinite(config_.cpiSeconds) || config_.cpiSeconds <= 0)
    throw std::invalid_argument("Multi-illuminator radar configuration is empty");
  validateAdvancedFeatures(config_.advanced);
  integration::LiveTemporalShadow::validate(config_.liveTemporalShadow);
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
  if (config_.liveTemporalShadow.enabled &&
      (config_.offlineReplay || config_.advanced.temporalShadow ||
       laneMatches(config_.liveTemporalShadowIlluminatorId) != 1))
    throw std::invalid_argument(
      "Live temporal shadow requires live capture and exactly one enabled illuminator");
  if (config_.experimentalFmReferenceQualityFilter &&
      (!config_.offlineReplay || std::none_of(illuminators.begin(), illuminators.end(),
        [](const auto& lane) { return lane.standard == "FM"; })))
    throw std::invalid_argument(
      "Experimental FM reference quality filtering requires replay and an FM lane");
  if (config_.advanced.temporalShadow &&
      std::count_if(illuminators.begin(), illuminators.end(),
        [&](const auto& illuminator) {
          return illuminator.id ==
            config_.advanced.temporalShadowIlluminatorId;
        }) != 1)
    throw std::invalid_argument(
      "Temporal shadow illuminator must match exactly one enabled lane");
  const std::size_t available = blah2::available_cpu_threads();
  const std::size_t outerMaximum = std::min(
    std::max<std::size_t>(1, available - 1), illuminators.size());
  const std::size_t outerWorkers = config_.laneWorkers ?
    std::min(config_.laneWorkers, outerMaximum) : outerMaximum;
  config_.workers = std::max<std::size_t>(1,
    std::min(config_.workers, available / outerWorkers));
  contexts_.reserve(illuminators.size());
  std::shared_ptr<resident::LaneBudget> residentBudget;
  bool residentSelectable = config_.accelerationMode == "gpu-resident";
  if (config_.accelerationMode != "cpu")
    residentBudget = std::make_shared<resident::LaneBudget>(uint32_t(illuminators.size()),
      config_.residentMaximumSessions, config_.residentMemoryMaximumBytes);
  if (config_.accelerationMode == "gpu-resident" ||
      (config_.accelerationMode == "auto" && !backendFactory)) {
    try {
      // The native caller may already have pinned the exact resident ID. AUTO
      // uses the same isolated enumeration and strict selector, never a vendor.
      if (config_.accelerationMode == "auto")
        config_.accelerationDevice = selectResidentGpuDevice(queryGpuDriverStatus(),
          config_.accelerationDevice);
      residentSelectable = true;
    } catch (const std::exception&) {
      if (config_.accelerationMode == "gpu-resident") throw;
      // Portable-stage AUTO retains its own precise device/fallback receipt.
    }
  }
  for (const auto& illuminator : illuminators)
    contexts_.push_back(std::make_unique<Context>(captureRateHz_,
      channelCount_, illuminator, config_, backendFactory, residentBudget,
      uint32_t(contexts_.size()), residentSelectable));
  executor_ = std::make_unique<utility::FixedExecutor>(outerWorkers);
}

void MultiIlluminatorRadar::prepareStartup()
{
  if (inputStarted_)
    throw std::logic_error("Cannot prepare multi-illuminator startup after input");
  if (startupPrepared_) return;
  // Serialize lane qualification: every lane must finish, without multiplying
  // peak CPU/GPU qualification work or admitting a producer between lanes.
  for (auto& context : contexts_) context->prepareStartup();
  startupPrepared_ = true;
}

std::vector<RadarFrame> MultiIlluminatorRadar::process(
  std::vector<channelizer::IlluminatorOutput> input,
  std::optional<LiveTemporalIdentity> liveIdentity)
{
  for (const auto& context : contexts_)
    if (context->nativeObserverFailed)
      throw std::logic_error("Cannot reuse a lane after a native CAF observer failure");
  if (input.size() != contexts_.size())
    throw std::invalid_argument("Multi-illuminator output set changed");
  inputStarted_ = true;
  std::unordered_map<std::string, channelizer::IlluminatorOutput*> byId;
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

std::vector<LaneHealthStatus> MultiIlluminatorRadar::laneHealth() const
{
  std::vector<LaneHealthStatus> result;
  result.reserve(contexts_.size());
  for (const auto& context : contexts_) result.push_back(context->health);
  return result;
}

void MultiIlluminatorRadar::reset()
{
  for (auto& context : contexts_) context->clear();
}

} // namespace blah2::multi_illuminator
