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
#include "process/ambiguity/ManifoldEvidence.h"
#include "process/spectrum/SpectrumAnalyser.h"
#include "process/analysis/StartupIq.h"
#include "process/analysis/ReplayCoherentObserverViews.h"
#include "process/conditioning/FmReferenceQualityFilter.h"
#include "process/conditioning/FrontendPlan.h"
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
  if (config.bearingManifoldRecorder && config.surveillanceFusionMode == "noncoherent")
    throw std::invalid_argument("Manifold evidence requires explicit coherent lane geometry");
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
  uint64_t residentRequalifications = 0;
  uint64_t residentLogical = 0, residentConsumed = 0, residentAllocationLimit = 0;
  uint32_t qualificationLaneIndex = 0;
  bool residentExecuted = false;
  bool residentGpuCompleted = false;
  bool residentAttempted = false;
  bool residentProductRoute = false;
  bool residentRequired = false;
  std::optional<resident::Coordinator::Publication> residentProducts;
  std::shared_ptr<const resident::Config> residentConfiguration;
  std::optional<RadarFrame::ResidentDiagnostics> residentDiagnostics;
  std::unique_ptr<SpectrumAnalyser> referenceSpectrumAnalyser;
  std::string referenceSpectrum;
  std::optional<std::complex<double>> directPathPilot;
  std::unique_ptr<Detection> residentDetections;
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
  std::optional<temporal_gpu::Config> temporalCandidate;
  uint64_t temporalReservationBytes = 0;
  uint64_t temporalMappingBytes = 0;
  NativeCafObserver nativeObserver;
  bool nativeObserverFailed = false;
  LaneHealthStatus health;
  struct StagedCpi {
    RadarFrame frame;
    pipeline::Position position;
    std::optional<qualification_observer::Frame> qualification;
    bool residentDetectorObserved = false;
    uint64_t residentEvaluated = 0, residentCfar = 0;
    std::vector<std::shared_ptr<Map<std::complex<double>>>> manifoldMaps;
    std::vector<std::complex<double>> referenceWeights;
    std::vector<double> actualGainDb;
    uint64_t publicationTimestamp = 0;
  };
  utility::FixedExecutor* productExecutor = nullptr;
  std::future<void> pendingProduct;
  stream_profile::Worker productProfile{"canonical-products"};
  uint64_t pendingPublication = 0;
  std::optional<RadarFrame> completedProduct;
  std::vector<RadarFrame> readyProducts;

  void collectPending()
  {
    if (!pendingProduct.valid()) return;
    pendingProduct.get();
    if (!completedProduct) throw std::logic_error("CPI product task returned no frame");
    auto& frame = *completedProduct;
    ++health.acceptedCpis;
    health.hasLastCpi = true;
    health.lastAccepted = true;
    health.lastFirstInputSample = frame.firstInputSample;
    health.lastEndInputSample = frame.endInputSample;
    health.lastReason.clear();
    readyProducts.push_back(std::move(frame));
    completedProduct.reset();
  }

  void discardPending() noexcept
  {
    if (pendingProduct.valid()) try { pendingProduct.get(); } catch (...) {}
    completedProduct.reset();
    readyProducts.clear();
  }

  ~Context() { discardPending(); }

  Context(uint32_t captureRateHz, std::size_t channelCount,
    iq::SignalDefinition value, RadarConfig radar,
    const Acceleration::BackendFactory& backendFactory,
    std::shared_ptr<resident::LaneBudget> budget, uint32_t laneIndex,
    bool residentSelectable, const std::string& residentDevice,
    std::size_t temporalWorkers, std::size_t clutterWorkerSlots)
    : definition(std::move(value)), captureRate(captureRateHz),
      samples(resolveCpiSamples(definition.outputRateHz, radar.cpiSeconds)),
      decimation(captureRateHz / definition.outputRateHz),
      config(scaleRadarConfigForLane(applyLaneTopology(std::move(radar), definition),
        definition.outputRateHz)),
      reference(samples), conditionedReference(samples), projectedReference(samples)
  {
    if (definition.frontendPlan) definition.frontendPlan->validateRoles(config.arrayReference ?
      config.referenceChannels : std::vector<uint32_t>{config.referenceChannel}, config.surveillanceChannels);
    residentBudget = std::move(budget);
    portableBackendFactory = backendFactory;
    qualificationLaneIndex = laneIndex;
    residentRequired = requiresResidentProducts(config.advanced);
    (void)selectRequiredResidentRoute(config.accelerationMode, residentRequired);
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
    // Host-conditioned reference needs the portable GPU frontend. Skipping
    // incompatible residency must preserve that independent fallback.
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
    if (residentSelectable && !residentReferenceCompatible)
      std::cerr << "GPU resident frontend skipped lane=" << gpuDiagnosticString(definition.id)
        << " reason=host-reference-conditioning pilot=" << bool(atscPilotProjector)
        << " fm=" << bool(fmReferenceFilter)
        << " temporal_backend=unqualified\n";
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
        const double spectrumBandwidth = resolveReferenceSpectrumBandwidth(
          config.referenceSpectrumBandwidthHz, definition.outputRateHz);
        frontend.spectrumBins = resolveReferenceSpectrumBins(samples,
          definition.outputRateHz, spectrumBandwidth);
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
        frontend.reference.bandwidthHz = spectrumBandwidth;
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
        // The qualified temporal/detector boundary does not require spatial
        // refinement or resident tracking. Keep ridge/recovery on ProductStages.
        if (!coherentFusion && config.stages.detection)
          frontend.detector = plan.residentDetector();
        frontend.presence = resident::wire::mandatoryPresence(frontend) |
          resident::bit(resident::Section::filtered) |
          resident::bit(resident::Section::fullReference);
        if (config.advanced.bearingManifoldRecorder)
          frontend.presence |= resident::bit(resident::Section::detectionCells);
        if (config.residentProductTemplate)
          frontend.presence |= config.residentProductTemplate->presence &
            resident::bit(resident::Section::causalTrace);
        residentAllocationLimit = residentBudget->allocationLimit(laneIndex,
          resident::makeLayout(frontend, uint64_t(sysconf(_SC_PAGESIZE))).bytes);
        GpuProcessOptions options;
        options.offlineTransportTrace = config.offlineReplay;
        options.qualificationLane = laneIndex;
        options.allocationLimitBytes = residentAllocationLimit;
        options.startupMs = gpuStartupDeadlineMs(config.cpiSeconds * 1000.0,
          options.startupMs, config.offlineReplay, residentRequired);
        if (!config.offlineReplay)
          options.frameMs = gpuCpiFrameDeadlineMs(
            config.cpiSeconds * 1000.0, options.frameMs);
        std::lock_guard<std::mutex> lease(residentBudget->execution);
        residentFrontend = std::make_unique<resident::Coordinator>(frontend, options);
        residentConfiguration = std::make_shared<const resident::Config>(frontend);
        residentStatus = residentFrontend->status();
        residentStatus.requested = config.accelerationMode;
      } catch (const std::exception& error) {
        residentStatus.reason = std::string("Resident lane unavailable: ") + error.what();
        if (std::string(error.what()).find("allocation") != std::string::npos)
          residentStatus.reason = "aggregate-gpu-capacity-exhausted: " + residentStatus.reason;
        residentFrontend.reset();
        std::cerr << "GPU resident frontend fallback lane=" << gpuDiagnosticString(definition.id)
          << " phase=construct reason=" << gpuDiagnosticString(residentStatus.reason) << '\n';
        if (residentRequired) throw;
        residentProductRoute = false;
      }
    }
    if (residentRequired && !residentFrontend)
      throw std::invalid_argument("Selected resident products require an available qualified resident backend");
    if (!residentFrontend) residentProductRoute = false;
    // Temporal admission is capability- and budget-driven for AUTO as well as
    // explicit GPU mode. The aggregate lane budget below reserves both stage
    // mappings before either worker starts, so unsupported or over-budget
    // hardware cleanly retains the identical host ProductStages path.
    // A separate temporal worker must not reserve capacity required by an
    // actually selected portable clutter worker. AUTO does not offer portable
    // clutter because its filtered-IQ download/re-upload is slower than host
    // clutter; it therefore remains eligible for the temporal GPU worker.
    // Basing this on configured clutter alone disabled both routes and caused
    // the measured CPU-clutter + CPU-temporal regression.
    const bool portableClutterRequired = !residentFrontend &&
      shouldOfferPortableGpuClutter(config.accelerationMode,
        config.stages.clutter);
    if (shouldAttemptTemporalProducts(config.accelerationMode, residentSelectable,
          bool(coherentFusion), config.stages.detection,
          plan.products().integrationCpis, portableClutterRequired) &&
        residentBudget && residentBudget->additionalWorkerAvailable(laneIndex)) {
      temporal_gpu::Config candidate;
      candidate.sampleRate = definition.outputRateHz;
      candidate.device = residentDevice;
      candidate.delayAxis.assign(ambiguity.front()->result()->delay.begin(), ambiguity.front()->result()->delay.end());
      candidate.dopplerAxis.assign(ambiguity.front()->result()->doppler.begin(), ambiguity.front()->result()->doppler.end());
      candidate.detector = plan.residentDetector();
      candidate.delayDriftPerHz = plan.products().delayDriftPerHz;
      temporal_gpu::validate(candidate);
      // Two independent stage workers share, rather than duplicate, the lane's
      // existing aggregate reservation. Size temporal's share from its complete
      // geometry so a small map worker does not strand portable clutter memory.
      const auto reservation = temporal_gpu::memoryReservation(candidate,
        uint64_t(sysconf(_SC_PAGESIZE)));
      if (reservation.bytes() < residentBudget->bytesPerSession &&
          residentBudget->bytesPerSession - reservation.bytes() >= (16ULL << 20)) {
        temporalCandidate = std::move(candidate);
        temporalReservationBytes = reservation.bytes();
        temporalMappingBytes = reservation.mappingBytes;
      }
    }
    initializePortableAcceleration();
    preferBorrowedStages = config.stages.clutter && config.borrowedClutter.value_or(
      !config.arrayReference && config.surveillanceChannels.size() == 1);
    const bool borrowed = preferBorrowedStages && !residentFrontend &&
      !(acceleration && acceleration->clutterAvailable());
    borrowedStages = borrowed;
    stages = std::make_unique<pipeline::RawIqStages>(
      pipeline::RawIqStages::Config{config.clutterDelayMin,
        config.clutterDelayMax, samples, config.surveillanceChannels.size(),
        config.workers, borrowed, config.reference, config.stages.clutter,
        config.stages.detection && config.advanced.detectorIntegrationCpis > 1 ? temporalWorkers : 1,
        clutterWorkerSlots});
    directFloatBorrowed = borrowed && !fmReferenceFilter &&
      !config.arrayReference && config.surveillanceChannels.size() == 1;
    if (config.referenceSpectrumBandwidthHz)
      referenceSpectrumAnalyser = std::make_unique<SpectrumAnalyser>(samples,
        *config.referenceSpectrumBandwidthHz, definition.frequencyHz, definition.outputRateHz);
  }

  uint64_t signalTimestamp(uint64_t first) const
  {
    return liveTemporalIdentity && liveTemporalIdentity->signalTimestampMs ?
      *liveTemporalIdentity->signalTimestampMs : detection::signalTimeMilliseconds(
        first + uint64_t(samples) * decimation, captureRate);
  }

  uint64_t publicationTimestamp(uint64_t first) const
  {
    return liveTemporalIdentity ? liveTemporalIdentity->timestampMs : signalTimestamp(first);
  }

  void initializePortableAcceleration()
  {
      if (acceleration || !shouldAttemptPortableFrontend(config.accelerationMode,
            bool(residentFrontend), residentRequired)) return;
      const auto& shape = *ownedAmbiguity.front();
      // Portable GPU clutter would copy a full filtered CPI back to the host,
      // then upload it to this same worker again for CAF.  AUTO avoids that
      // duplicate transport and retains GPU CAF; explicit GPU mode still
      // exposes portable clutter for qualification and controlled use.
      const bool portableGpuClutter = shouldOfferPortableGpuClutter(
        config.accelerationMode, config.stages.clutter);
      const int64_t clutterBins = int64_t(config.clutterDelayMax) -
        config.clutterDelayMin;
      if (clutterBins <= 0 || uint64_t(clutterBins) > UINT32_MAX)
        throw std::invalid_argument("Multi-illuminator clutter width is invalid");
      // Offer complete lane geometry on every platform. Capability, memory,
      // Precision and runtime qualification decide each stage independently.
      const Acceleration::BackendFactory boundedFactory =
        [budget = residentBudget, laneIndex = qualificationLaneIndex,
          backendFactory = portableBackendFactory,
          temporalReservation = temporalReservationBytes,
          cpiSeconds = config.cpiSeconds, offline = config.offlineReplay](const GpuGeometry& geometry,
            const std::string& device) {
          try {
          if (!budget) throw std::runtime_error("GPU lane has no aggregate reservation");
          GpuProcessOptions options;
          options.offlineTransportTrace = offline;
          options.qualificationLane = laneIndex;
          options.allocationLimitBytes = budget->allocationLimit(laneIndex,
            gpuProcessMappingBytes(geometry), temporalReservation);
          options.startupMs = gpuStartupDeadlineMs(
            cpiSeconds * 1000.0, options.startupMs, offline);
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
          portableGpuClutter ? samples : 0u,
          portableGpuClutter ? uint32_t(clutterBins) : 0u, config.clutterDelayMin},
        shape.get_n_corr(), definition.outputRateHz,
        shape.get_doppler_middle(), config.accelerationDevice, boundedFactory,
        Acceleration::TimeSource{}, config.observationExecutor);
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

  void clear(const char* reason = "explicit_reset", bool retainProducts = false)
  {
    if (retainProducts) collectPending();
    else discardPending();
    pendingCount = 0;
    positioned = false;
    pendingFirstInputSample = 0;
    resetAdaptiveState(reason);
  }

  void resetAdaptiveState(const char* reason = "clutter_reference_rejected")
  {
    // Product history belongs exclusively to its task until completion.
    collectPending();
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

  void resetTracker() { collectPending(); products->resetTracker(); }

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
    // Preserve FP64 ingress through the host/reference owners. Each backend's
    // existing numerical qualification and precision guard admits its stage;
    // unsupported clutter must not disable independently qualified GPU CAF.
    if (input.channelCount() != ownedChannels.size() ||
        input.inputSamplesPerOutput != decimation)
      throw std::invalid_argument("Channelized illuminator topology changed");
    if (!input.channelCount()) return {};
    const auto channelSize = [](const auto& view) { return view.size(); };
    const std::size_t count = input.visitChannel(0, channelSize);
    for (std::size_t channel=0;channel<input.channelCount();++channel)
      if (input.visitChannel(channel, channelSize) != count)
        throw std::invalid_argument("Channelized illuminator channels are unpaired");
    if (!count) {
      auto result = std::move(readyProducts);
      readyProducts.clear();
      return result;
    }
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
        clear("input_discontinuity", true);
        pendingFirstInputSample = input.firstInputSample;
        positioned = true;
      }
    }
    else
    {
      pendingFirstInputSample = input.firstInputSample;
      positioned = true;
    }
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
          process_float_cpi(input, offset, pendingFirstInputSample);
          pendingFirstInputSample += uint64_t(samples) * decimation;
          offset += samples;
          continue;
        }
        if (residentFrontend && input.borrowed)
        {
          process_resident_borrowed_cpi(input, offset,
            pendingFirstInputSample);
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
        process_loaded_cpi(pendingFirstInputSample);
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
        process_loaded_cpi(pendingFirstInputSample);
        pendingFirstInputSample += uint64_t(samples) * decimation;
        pendingCount = 0;
      }
    }
    if(profileActive){profileRecord.time[1]=profileRecord.time[2]=profileRecord.time[3]=stream_profile::Clock::now();
      profileRecord.stages=profileStages;streamProfile.record(profileRecord);}
    profileActive=false;
    auto result = std::move(readyProducts);
    readyProducts.clear();
    return result;
  }

  qualification_observer::Frame qualificationFrame(uint64_t first) const
  {
    qualification_observer::Frame f;
    f.resolvedProductPlan=products->planIdentity();
    if (definition.frontendPlan) f.resolvedFrontendPlan = definition.frontendPlan->identity();
    f.lane=definition.id;f.sequence=qualificationSequence;
    f.captureEpoch=liveTemporalIdentity?liveTemporalIdentity->captureEpoch:0;
    f.samplePositionAvailable=liveTemporalIdentity&&liveTemporalIdentity->samplePositionAvailable;
    f.captureFirst=first;f.captureEnd=first+uint64_t(samples)*decimation;
    f.inputSampleInterval=std::array<uint64_t,2>{f.captureFirst,f.captureEnd};
    if(liveTemporalIdentity&&liveTemporalIdentity->captureSamples){
      const auto& capture=*liveTemporalIdentity->captureSamples;
      if(capture[1]<=capture[0]||capture[1]-capture[0]!=uint64_t(samples)*decimation||
          capture[0]>first||first-capture[0]>=decimation)
        throw std::invalid_argument("Qualification CPI lacks matching raw capture and canonical input intervals");
      f.captureFirst=capture[0];f.captureEnd=capture[1];
    }
    f.frequencyHz=definition.frequencyHz;f.sampleRateHz=definition.outputRateHz;
    f.inputSamplesPerOutput=decimation;f.channelIds=config.surveillanceChannels;
    f.referenceChannelIds=config.arrayReference?config.referenceChannels:std::vector<uint32_t>{config.referenceChannel};
    if(liveTemporalIdentity)f.actualGainDb=liveTemporalIdentity->inputGainDb;
    f.backend=residentFrontend?"vulkan-resident":acceleration?acceleration->status().active:"cpu";
    f.device=residentFrontend?residentStatus.device:acceleration?acceleration->status().device:"";
    f.productBackend=residentFrontend&&residentFrontend->config().detector.enabled?"vulkan-resident":"cpu";
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
    directPathPilot.reset();
    if (nativeObserver)
      directPathPilot = ArrayReferenceSynthesizer::direct_path_anchor_pilot(
        selectedReference->sample_view(), channels.front()->sample_view());
    if (referenceSpectrumAnalyser) {
      referenceSpectrumAnalyser->process(selectedReference);
      referenceSpectrum = selectedReference->to_json(publicationTimestamp(pendingFirstInputSample));
    }
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
          if (replay_gpu_qualification::clutterActive())
            qualificationClutter->checkpointProof = replay_gpu_qualification::state().clutterProof.encode();
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
    if (acceleration && liveTemporalIdentity)
      replay_gpu_qualification::requireClutterSteady(*acceleration, true);
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
    if (residentFrontend->config().detector.enabled) {
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

  template<class BeginResident,class SampleAt,class PrepareHost>
  void process_cpi(uint64_t first,BeginResident&& beginResident,
      SampleAt&& sampleAt,PrepareHost&& prepareHost)
  {
    applyQualificationCpuReset();
    if (acceleration) replay_gpu_qualification::requireClutterSteady(*acceleration, false);
    if(qualificationObserver){++qualificationSequence;
      qualificationObserver->input(qualificationFrame(first),ownedChannels.size(),samples,
        std::forward<SampleAt>(sampleAt));}
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
    residentDiagnostics.reset();
    residentDetections.reset();
    bool qualificationResidentRecovery = false;
    if (residentFrontend) {
      try {
        if (config.advanced.detectorIntegrationCpis > 1 && liveTemporalIdentity &&
            !liveTemporalIdentity->samplePositionAvailable)
          throw std::runtime_error("Resident temporal input sample lineage unavailable; temporal products cannot be admitted");
        std::lock_guard<std::mutex> lease(residentBudget->execution);
        beginResident({++residentLogical, ++residentConsumed,
          signalTimestamp(first)});
        importResidentMaps();
        if (residentFrontend->config().detector.enabled) {
          std::vector<double> gains;
          if (liveTemporalIdentity) for (const auto channel : config.surveillanceChannels)
            if (channel < liveTemporalIdentity->inputGainDb.size())
              gains.push_back(liveTemporalIdentity->inputGainDb[channel]);
          residentProducts = residentFrontend->format(publicationTimestamp(first),
            config.bearingPublication,config.advanced.bearingManifoldRecorder,
            config.manifoldEvidence,{gains.data(),gains.size()},
            !liveTemporalIdentity||liveTemporalIdentity->samplePositionAvailable,
            config.offlineReplay);
          referenceSpectrum = residentProducts->spectrum;
          if (!residentProductRoute)
            residentDetections = residentFrontend->hostDetections(
              !liveTemporalIdentity||liveTemporalIdentity->samplePositionAvailable);
        }
        else if (referenceSpectrumAnalyser)
          referenceSpectrum = residentFrontend->format(publicationTimestamp(first)).spectrum;
        const auto& prepared = residentFrontend->prepared();
        if (config.advanced.residentDiagnostics)
          residentDiagnostics.emplace(residentConfiguration, residentLogical, residentConsumed, prepared);
        residentFrontend->commit();
        // Product strings/maps and diagnostic bytes now own this CPI. Release
        // the device transaction before the next stage; socket errors remain
        // fail-stop and never trigger replay of an already committed frame.
        residentFrontend->published();
        if (residentDiagnostics && residentConfiguration->replay.maxBytes &&
            residentLogical <= residentConfiguration->replay.maxFrames) {
          if (!liveTemporalIdentity || !liveTemporalIdentity->samplePositionAvailable)
            throw std::runtime_error("Resident replay snapshot requires exact input lineage");
          residentDiagnostics->snapshot = residentFrontend->snapshot();
        }
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
        residentDetections.reset();
        resetAdaptiveState("resident_frontend_failure");
        if (residentRequalifications != UINT64_MAX) ++residentRequalifications;
        initializePortableAcceleration();
        qualificationResidentRecovery = true;
        if(replay_gpu_qualification::enabled())
          replay_gpu_qualification::emit("history-reset","resident",qualificationLaneIndex,
            replay_gpu_qualification::current(),{}, {},residentLogical,residentConsumed);
        selectCpuStages();
      }
    }
    if (residentExecuted) return queue_processed_cpi(first);
    prepareHost();
    if (!process_loaded_dsp()) {
      reject(first, "clutter_reference_rejected");
      resetAdaptiveState();
      return;
    }
    queue_processed_cpi(first);
    if(qualificationResidentRecovery&&replay_gpu_qualification::enabled())
      replay_gpu_qualification::emit("cpu-recovery-complete","resident",qualificationLaneIndex,
        replay_gpu_qualification::current(),{}, {},residentLogical,residentConsumed);
  }

  void process_loaded_cpi(uint64_t first)
  {
    process_cpi(first,
      [&](resident::Frame frame) { residentFrontend->begin(ownedChannels, frame); },
      [&](size_t channel,size_t index) {
        return ownedChannels.at(channel)->sample_view().at(index);
      }, [] {});
  }

  void process_resident_borrowed_cpi(const iq::CoherentSamples& input,
      std::size_t offset,uint64_t first)
  {
    bool hostPrepared=false;
    const auto prepareHost=[&] {
      if(hostPrepared)return;
      const auto started=CostClock::now();
      for(std::size_t channel=0;channel<ownedChannels.size();++channel){
        auto& destination=ownedChannels[channel]->resize_contiguous_for_write(samples);
        input.copyChannelTo(channel,destination.data(),samples,offset);
      }
      if(input.isDouble())
        inputProcessing.copiedSamples+=uint64_t(samples)*ownedChannels.size();
      else
        inputProcessing.widenedSamples+=uint64_t(samples)*ownedChannels.size();
      inputProcessing.contiguousSamples+=uint64_t(samples)*ownedChannels.size();
      inputProcessing.ingestionMs+=elapsedMs(started);
      inputProcessing.path=input.isDouble()?"resident_fallback_contiguous_double":
        "resident_fallback_contiguous_float";
      hostPrepared=true;
    };
    inputProcessing.path=input.isDouble()?"resident_borrowed_double":
      "resident_borrowed_float";
    process_cpi(first,
      [&](resident::Frame frame){residentFrontend->begin(input,offset,frame);},
      [&](size_t channel,size_t index){
        return input.visitChannel(channel,[&](const auto& values){
          return std::complex<double>(values.at(offset+index));
        });
      },prepareHost);
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
          if (residentFrontend->config().detector.enabled) {
            const uint64_t timestamp = 1000 + uint64_t(frame) * samples * 1000 / definition.outputRateHz;
            std::vector<Map<std::complex<double>>*> maps;
            for (const auto& processor : ownedAmbiguity) maps.push_back(processor->result());
            std::shared_ptr<Map<std::complex<double>>> oracleMap;
            const auto oracleFusion = residentFrontend->fuseOracle(maps, fusion,
              coherentFusion.get(), oracleMap, stages->reference().get_metrics());
            const auto products = residentFrontend->format(timestamp, config.bearingPublication);
            residentFrontend->checkProducts(products,
              oracleMap,
              startupProducts, timestamp,
              {1, uint64_t(frame) * samples, uint64_t(frame + 1) * samples, timestamp, true},oracleFusion);
          } else residentFrontend->checkFrontendProducts();
          residentFrontend->commit();
        }, [&] { stages->resetReference(); });
        residentStatus = residentFrontend->status();
        residentStatus.requested = config.accelerationMode;
        residentStatus.reason = residentProductRoute ?
          "Qualified resident coherent products; lane-bound RF/site/calibration; serialized aggregate lease" :
          residentFrontend->config().detector.enabled ?
          "Qualified resident temporal/CFAR; canonical host tracker; serialized aggregate lease" :
          "Qualified resident reference/clutter/CAF; shared FP64 host products; serialized aggregate lease";
        if (config.accelerationMode == "auto") {
          // No diagnostic transaction may consume the new production namespace.
          residentStatus.cpuMs = 0; residentStatus.gpuMs = 0;
          residentStatus.reason += "; AUTO timing unavailable (private correctness qualification only)";
        }
        std::cerr << "GPU resident qualification complete lane=" << gpuDiagnosticString(definition.id)
          << " temporal=" << residentFrontend->config().detector.enabled
          << " tracker=" << (residentFrontend->config().tracker.enabled ? "resident" : "host") << '\n';
      } catch (const std::exception& error) {
        residentFrontend.reset();
        residentStatus.active = "cpu";
        residentStatus.state = "fallback";
        residentStatus.reason = std::string("Resident lane qualification failed: ") + error.what();
        std::cerr << "GPU resident frontend fallback lane=" << gpuDiagnosticString(definition.id)
          << " phase=qualify reason=" << gpuDiagnosticString(residentStatus.reason) << '\n';
        if (residentRequired) throw;
        residentProductRoute = false;
        stages->resetReference();
        initializePortableAcceleration();
        selectCpuStages();
      }
    }
    if (!residentFrontend) {
      if (acceleration && acceleration->startupPending())
      {
        acceleration->prepareStartup(prepareFrame);
        acceleration->retryClutterQualificationAfterStartup();
      }
      else
        prepareFrame(0); // CPU/fallback must also finish first-use DSP work.
      if (temporalCandidate) {
        try {
          GpuProcessOptions options;
          options.allocationLimitBytes = residentBudget->allocationLimit(qualificationLaneIndex,
            temporalMappingBytes, residentBudget->bytesPerSession - temporalReservationBytes);
          options.startupMs = gpuStartupDeadlineMs(config.cpiSeconds * 1000.0, options.startupMs, config.offlineReplay);
          if (!config.offlineReplay) options.frameMs = gpuCpiFrameDeadlineMs(config.cpiSeconds * 1000.0, options.frameMs);
          options.offlineTransportTrace = config.offlineReplay; options.qualificationLane = qualificationLaneIndex;
          std::lock_guard<std::mutex> lease(residentBudget->execution);
          if (!gpu_worker_lifetime::replacementAvailable())
            throw std::runtime_error("Temporal GPU reservation retained by an unreaped worker");
          products->prepareTemporal(*temporalCandidate, options);
          std::cerr << "GPU temporal qualification complete lane=" << gpuDiagnosticString(definition.id)
            << " backend=vulkan-temporal tracker=host fusion=host rows=" << temporalCandidate->dopplerAxis.size()
            << " columns=" << temporalCandidate->delayAxis.size() << '\n';
        } catch (const std::exception& error) {
          std::cerr << "GPU temporal fallback lane=" << gpuDiagnosticString(definition.id)
            << " phase=qualify reason=" << gpuDiagnosticString(error.what()) << '\n';
        }
      }
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
    referenceSpectrum.clear();
    directPathPilot.reset();
    conditionedReference.clear();
    projectedReference.clear();
    qualificationReference.clear();
    qualificationClutter.reset();
    inputProcessing = {};
    for (auto& channel : ownedChannels) channel->clear();
    collectStorageMetrics();
    inputProcessing = {};
  }

  void process_float_cpi(
    const iq::CoherentSamples& input,
    std::size_t offset, uint64_t first)
  {
    applyQualificationCpuReset();
    inputProcessing.path = "native_float_borrowed";
    if(qualificationObserver){++qualificationSequence;
      qualificationObserver->input(qualificationFrame(first),input.channelCount(),samples,
        [&](size_t c,size_t i){return std::complex<double>(input.channel(c).at(offset+i));});}
    const auto* rawReference = input.channel(config.referenceChannel).data() + offset;
    directPathPilot.reset();
    if(nativeObserver){
      const auto anchor=input.channel(config.surveillanceChannels.front());
      directPathPilot=ArrayReferenceSynthesizer::direct_path_anchor_pilot(
        iq::SampleView{rawReference,samples},
        iq::SampleView{anchor.data()+offset,samples});
    }
    if(referenceSpectrumAnalyser){
      referenceSpectrumAnalyser->process(
        iq::SampleView{rawReference,samples},&reference);
      referenceSpectrum=reference.to_json(publicationTimestamp(first));
    }
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
      return;
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
    queue_processed_cpi(first);
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

  StagedCpi stage_processed_cpi(uint64_t first)
  {
    stream_profile::Duration profileProducts(profileActive,profileStages[2]);
    std::vector<Map<std::complex<double>>*> maps;
    maps.reserve(ambiguity.size());
    for (const auto& processor : ownedAmbiguity) maps.push_back(processor->result());
    if (nativeObserver) {
      NativeCafView view;
      view.illuminatorId = definition.id;
      if (definition.frontendPlan) view.resolvedFrontendPlan = definition.frontendPlan->identity();
      view.frequencyHz = definition.frequencyHz;
      view.sampleRateHz = definition.outputRateHz;
      view.firstInputSample = first;
      view.endInputSample = first + uint64_t(samples - 1) * decimation;
      view.inputSamplesPerOutput = decimation;
      view.channelIds = config.surveillanceChannels;
      view.referenceChannelIds = config.arrayReference ?
        config.referenceChannels : std::vector<uint32_t>{config.referenceChannel};
      view.directPathPilot = directPathPilot;
      if (directPathPilot) view.pilotAnchorChannel = config.surveillanceChannels.front();
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
      if(residentExecuted&&!residentFrontend->config().detector.enabled){
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
    const uint64_t timestamp = signalTimestamp(first);
    StagedCpi staged;
    staged.position = {liveTemporalIdentity ? liveTemporalIdentity->captureEpoch : processingEpoch,
      first, first + uint64_t(samples) * decimation, timestamp,
      !liveTemporalIdentity || liveTemporalIdentity->samplePositionAvailable};
    auto& result = staged.frame;
    result.referenceSpectrum = std::move(referenceSpectrum);
    result.residentDiagnostics = std::move(residentDiagnostics);
    staged.publicationTimestamp = publicationTimestamp(first);
    if (config.advanced.bearingManifoldRecorder && !residentProducts) {
      for (const auto* source : maps)
        staged.manifoldMaps.push_back(std::make_shared<Map<std::complex<double>>>(*source));
      staged.referenceWeights = stages->reference().get_metrics().weights;
      if (!liveTemporalIdentity || liveTemporalIdentity->inputGainDb.size() != ownedChannels.size())
        throw std::runtime_error("Manifold evidence requires frame-bound actual gains");
      for (const auto channel : config.surveillanceChannels)
        staged.actualGainDb.push_back(liveTemporalIdentity->inputGainDb[channel]);
    }
    if (residentProducts) {
      result.detectorIntegration = resident::temporalPublication(residentFrontend->config(),
        *residentProducts,staged.position.epoch,
        first,first+uint64_t(samples)*decimation);
    }
    if (qualificationObserver) {
      staged.qualification = qualificationFrame(first);
      if (residentProducts) {
        const auto& native=residentFrontend->prepared();
        staged.residentDetectorObserved = native.detectorObserved;
        staged.residentEvaluated = native.evaluatedCells;
        staged.residentCfar = native.cfarCount;
      }
    }
    result.illuminatorId = definition.id;
    result.resolvedProductPlan = products->planIdentity();
    if (definition.frontendPlan) result.resolvedFrontendPlan = definition.frontendPlan->identity();
    result.frequencyHz = definition.frequencyHz;
    if (config.coherentFusion) {
      result.transmitterLocation = definition.transmitterLocation;
      result.directPathAzimuthRad = config.coherentFusion->directPathAzimuthRad;
    }
    result.firstInputSample = first;
    result.endInputSample = end;
    result.publicationSequence = liveTemporalIdentity ? liveTemporalIdentity->publicationSequence : 0;
    result.inputSamplesPerOutput = decimation;
    result.channelIds = config.surveillanceChannels;
    result.referenceChannelIds = config.arrayReference ?
      config.referenceChannels : std::vector<uint32_t>{config.referenceChannel};
    result.sampleRateHz = definition.outputRateHz;
    collectStorageMetrics();
    result.inputProcessing = inputProcessing;
    result.detectionEnabled = config.stages.detection;
    result.trackingEnabled = config.stages.tracking;
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
    result.trackerMinimumSnrDb = config.trackerMinimumSnrDb;
    result.trackerMinimumSnrOverridden =
      config.trackerMinimumSnrDbByIlluminator.count(definition.id) != 0;
    result.map = std::move(map);
    result.residentProducts = std::move(residentProducts);
    if (result.residentProducts) result.manifoldEvidence = result.residentProducts->manifoldEvidence;
    result.detections = std::move(residentDetections);
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
      if (config.accelerationMode == "auto" && config.stages.clutter) {
        // This owner deliberately omits portable clutter geometry in AUTO.
        // Publish that route decision instead of implying missing config.
        result.clutterAcceleration.state = "selected";
        result.clutterAcceleration.reason =
          "AUTO selected CPU clutter to avoid the portable GPU filtered-IQ round trip";
      }
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
    return staged;
  }

  RadarFrame complete_processed_cpi(StagedCpi staged)
  {
    // Only this ordered product task may access temporal/detector/tracker
    // state. Every frontend-owned value it needs was captured before launch.
    auto result = std::move(staged.frame);
    stream_profile::WorkScope profile(productProfile, result.firstInputSample,
      staged.position.epoch, result.frequencyHz);
    auto& map = result.map;
    pipeline::Products processed;
    CfarDetector1D::Trials observedTrials;
    if (result.residentProducts) {
      processed.integration = std::move(result.detectorIntegration);
      if (result.detections) {
        result.residentProducts->trackerOnHost = true;
        processed.detections = std::move(result.detections);
        processed = products->track(map, staged.position, std::move(processed));
        if (processed.tracks)
          result.residentProducts->tracks = processed.tracks->to_json(staged.position.timestampMs);
      } else {
        processed.detections = std::make_unique<Detection>(
          std::vector<double>{}, std::vector<double>{}, std::vector<double>{});
        processed.tracks = std::make_unique<Track>();
      }
    } else {
      processed = products->process(map, staged.position, true, {}, nullptr, {},
        staged.qualification ? &observedTrials : nullptr, &stages->executor());
    }
    if (staged.qualification) {
      const auto& identity = *staged.qualification;
      if (result.residentProducts) {
        if(config.stages.detection&&!staged.residentDetectorObserved)
          throw std::runtime_error("Resident replay lacks evaluated detector population");
        qualificationObserver->detector(identity,config.stages.detection,
          config.stages.detection&&(!processed.integration||processed.integration->ready),
          "vulkan-resident",staged.residentEvaluated,staged.residentCfar);
      } else qualificationObserver->detector(identity,config.stages.detection,
        config.stages.detection&&(!processed.integration||processed.integration->ready),
        products->temporalAccelerated() ? "vulkan-temporal" : "cpu",
        observedTrials.evaluated,observedTrials.accepted,observedTrials.nonfinite);
      if (config.stages.detection && !processed.detections)
        throw std::runtime_error("Qualification replay lacks complete candidate output");
      if (processed.detections)
        qualificationObserver->temporalScores(identity,*processed.detections);
    }
    result.detectorIntegration = std::move(processed.integration);
    result.trackerDiagnostics = result.residentProducts && !result.residentProducts->trackerOnHost ?
      result.residentProducts->trackerDiagnostics : products->tracker.frame_diagnostics();
    result.residentTemporalExecuted = result.residentProducts &&
      result.detectorIntegration && result.detectorIntegration->availableCpis > 0;
    if (products->temporalAccelerated()) result.temporalDevice = products->temporalDevice();
    result.detections = std::move(processed.detections);
    result.tracks = std::move(processed.tracks);
    if (!staged.manifoldMaps.empty()) {
      std::vector<Map<std::complex<double>>*> maps;
      for (const auto& source : staged.manifoldMaps) maps.push_back(source.get());
      result.manifoldEvidence = manifold_evidence::cpu(staged.publicationTimestamp,
        definition.frequencyHz, definition.outputRateHz, *result.detections, maps,
        staged.referenceWeights, staged.actualGainDb);
    }
    if (!result.detections) result.detections = std::make_unique<Detection>(
      std::vector<double>{}, std::vector<double>{}, std::vector<double>{});
    if (!result.tracks) result.tracks = std::make_unique<Track>();
    profile.completed();
    return result;
  }

  void queue_processed_cpi(uint64_t first)
  {
    auto staged = std::make_shared<StagedCpi>(stage_processed_cpi(first));
    // N+1 owns its immutable map and diagnostics before we join N. Only then
    // may N+1 enter the same ordered temporal/detector/tracker owner.
    collectPending();
    pendingPublication = staged->frame.publicationSequence;
    pendingProduct = productExecutor->submit([this, staged] {
      completedProduct = complete_processed_cpi(std::move(*staged));
    });
  }

  void reject(uint64_t first, std::string reason)
  {
    collectPending();
    if (qualificationObserver)
      qualificationObserver->rejected(qualificationFrame(first), reason);
    ++health.rejectedCpis;
    health.hasLastCpi = true;
    health.lastAccepted = false;
    health.lastFirstInputSample = first;
    health.lastEndInputSample = first + uint64_t(samples - 1) * decimation;
    health.lastReason = std::move(reason);
  }
};

RadarBank::~RadarBank()
{
  // Join while all lane state and both executors still exist.
  for (auto& context : contexts_) context->discardPending();
}

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
  for (const auto& lane : illuminators)
    if (lane.frontendPlan) lane.frontendPlan->validate(captureRateHz_, channelCount_, lane);
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
  // Frontend and temporal work share each lane's bounded pool; overlap does
  // not allocate another inner DSP pool or increase this FFT worker budget.
  const std::size_t temporalWorkers = cap_fft_threads_for_lanes(8, outerWorkers, available);
  const std::size_t legacyClutterSlots = configured_clutter_worker_slots();
  const std::size_t totalClutterWorkers = configured_clutter_total_worker_budget();
  const auto clutterWorkerSlots = resolve_clutter_worker_slots(illuminators.size(),
    legacyClutterSlots, totalClutterWorkers, available);
  auto laneConfig = config_;
  laneConfig.workers = std::max<std::size_t>(1,
    std::min(config_.workers, temporalWorkers));
  contexts_.reserve(illuminators.size());
  std::shared_ptr<resident::LaneBudget> residentBudget;
  bool residentSelectable = config_.accelerationMode == "gpu-resident";
  const bool residentRequired = requiresResidentProducts(config_.advanced);
  (void)selectRequiredResidentRoute(config_.accelerationMode, residentRequired);
  std::string residentDevice = config_.accelerationDevice;
#ifdef VECTORWARP_RESIDENT_GPU
  constexpr bool residentBuilt = true;
#else
  constexpr bool residentBuilt = false;
#endif
  if (residentRequired && !residentBuilt)
    throw std::invalid_argument("Selected radar features require a qualified resident backend; this cohort does not provide it");
  if (config_.accelerationMode != "cpu")
    residentBudget = std::make_shared<resident::LaneBudget>(uint32_t(illuminators.size()),
      config_.residentMaximumSessions, config_.residentMemoryMaximumBytes);
  if (config_.accelerationMode == "gpu-resident" ||
      ((config_.accelerationMode == "auto" || config_.accelerationMode == "gpu") && !backendFactory && residentBuilt)) {
    try {
      // The native caller may already have pinned the exact resident ID. AUTO
      // uses the same isolated enumeration and strict selector, never a vendor.
      if (config_.accelerationMode != "gpu-resident")
        residentDevice = selectResidentGpuDevice(queryGpuDriverStatus(),
          config_.accelerationDevice);
      residentSelectable = true;
    } catch (const std::exception& error) {
      if (config_.accelerationMode == "gpu-resident" || residentRequired) throw;
      std::cerr << "GPU resident capability unavailable reason=" << gpuDiagnosticString(error.what()) << '\n';
      // Portable-stage AUTO retains its own precise device/fallback receipt.
    }
  }
  for (std::size_t lane = 0; lane < illuminators.size(); ++lane)
    contexts_.push_back(std::make_unique<Context>(captureRateHz_,
      channelCount_, illuminators[lane], laneConfig, backendFactory, residentBudget,
      uint32_t(contexts_.size()), residentSelectable, residentDevice, temporalWorkers,
      clutterWorkerSlots[lane]));
  executor_ = std::make_unique<utility::FixedExecutor>(outerWorkers);
  productExecutor_ = std::make_unique<utility::FixedExecutor>(outerWorkers);
  for (auto& context : contexts_) context->productExecutor = productExecutor_.get();
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
  return processImpl(std::move(input), std::move(liveIdentity), false);
}

std::vector<RadarFrame> RadarBank::processPipelined(
  std::vector<iq::SignalBlock> input,
  std::optional<LiveTemporalIdentity> liveIdentity)
{
  return processImpl(std::move(input), std::move(liveIdentity), true);
}

std::vector<RadarFrame> RadarBank::processImpl(
  std::vector<iq::SignalBlock> input,
  std::optional<LiveTemporalIdentity> liveIdentity, bool deferProducts)
{
  try
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
      const auto& expected = context->definition.frontendPlan;
      const auto& supplied = found->second->frontendPlan;
      if (bool(expected) != bool(supplied) || (expected && expected->identity() != supplied->identity()))
        throw std::invalid_argument("Canonical signal frontend plan changed");
      if (expected) expected->validate(found->second->samples);
    }
    std::vector<std::vector<RadarFrame>> perContext(contexts_.size());
    executor_->parallel_for(contexts_.size(), [&](std::size_t index) {
      auto& context = *contexts_[index];
      const auto& previous = context.liveTemporalIdentity;
      if (context.positioned && (bool(previous) != bool(liveIdentity) ||
          (previous && liveIdentity &&
           (previous->captureEpoch != liveIdentity->captureEpoch ||
            previous->inputGainDb != liveIdentity->inputGainDb ||
            previous->samplePositionAvailable != liveIdentity->samplePositionAvailable))))
        context.clear("capture_identity_changed", true);
      context.liveTemporalIdentity = liveIdentity;
      perContext[index] = contexts_[index]->consume(
        byId.at(contexts_[index]->definition.id)->samples);
      if (!deferProducts) {
        context.collectPending();
        for (auto& frame : context.readyProducts)
          perContext[index].push_back(std::move(frame));
        context.readyProducts.clear();
      }
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

std::vector<RadarFrame> RadarBank::drain()
{
  try {
    executor_->parallel_for(contexts_.size(), [&](std::size_t index) {
      contexts_[index]->collectPending();
    });
    std::vector<RadarFrame> result;
    for (auto& context : contexts_) {
      for (auto& frame : context->readyProducts) result.push_back(std::move(frame));
      context->readyProducts.clear();
    }
    return result;
  } catch (...) { reset(); throw; }
}

std::vector<uint64_t> RadarBank::pendingPublicationSequences() const
{
  std::vector<uint64_t> result;
  for (const auto& context : contexts_)
    if (context->pendingProduct.valid()) result.push_back(context->pendingPublication);
  return result;
}

std::vector<LaneHealthStatus> RadarBank::laneHealth() const
{
  std::vector<LaneHealthStatus> result;
  result.reserve(contexts_.size());
  for (const auto& context : contexts_) result.push_back(context->health);
  return result;
}

uint64_t RadarBank::requalificationCount() const noexcept
{
  uint64_t result = 0;
  for (const auto& context : contexts_) {
    const uint64_t counters[]{context->residentRequalifications,
      context->acceleration ? context->acceleration->requalificationCount() : 0};
    for (const auto value : counters) {
      if (value > UINT64_MAX - result) return UINT64_MAX;
      result += value;
    }
  }
  return result;
}

bool RadarBank::prefersNativeFloatCapture() const
{
  return startupPrepared_ && !contexts_.empty() &&
    std::all_of(contexts_.begin(), contexts_.end(),
    [](const auto& context) { return bool(context->residentFrontend); });
}

void RadarBank::reset()
{
  for (auto& context : contexts_) context->clear();
}

} // namespace blah2::pipeline
