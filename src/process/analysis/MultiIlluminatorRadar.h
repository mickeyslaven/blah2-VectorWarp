#ifndef MULTI_ILLUMINATOR_RADAR_H
#define MULTI_ILLUMINATOR_RADAR_H

#include "process/analysis/ProductionRawIq.h"
#include "process/analysis/MultiIlluminatorHealth.h"
#include "process/ambiguity/Acceleration.h"
#include "process/conditioning/IlluminatorChannelizerBank.h"
#include "process/conditioning/AtscPilotQuality.h"
#include "process/conditioning/AtscPilotProjector.h"
#include "process/detection/Centroid.h"
#include "process/detection/CfarDetector1D.h"
#include "process/detection/DopplerRidgeScrubber.h"
#include "process/detection/DopplerRidgeShadowWindow.h"
#include "process/detection/Interpolate.h"
#include "process/detection/DetectorIntegration.h"
#include "process/fusion/Noncoherent.h"
#include "process/fusion/CoherentBeamBank.h"
#include "process/ambiguity/GpuResidentCoordinator.h"
#include "process/ambiguity/BearingManifold.h"
#include "process/integration/TemporalEvidence.h"
#include "process/integration/LiveTemporalShadow.h"
#include "process/detection/PostClutterBootstrap.h"
#include "process/tracker/Tracker.h"
#include "process/utility/FixedExecutor.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace blah2::multi_illuminator
{

// Shared advanced behavior and remaining resident-only capabilities. Callers
// pass effective values so a lane cannot silently ignore configured behavior.
struct AdvancedFeatureConfig
{
  std::string surveillanceFusionMode = "noncoherent";
  bool surveillanceManifoldAutomation = false;
  uint32_t detectorIntegrationCpis = 1;
  uint32_t detectorIntegrationMotionBins = 0;
  bool trackBeforeDetect = false;
  bool trackerBeamToleranceConfigured = false;
  bool trackerGpuCapacityConfigured = false;
  // Multi-illuminator mode is new and Balanced is its default tracking policy.
  std::string trackerAccelerationModel = "adaptive_gate";
  bool stationaryClutter = false;
  bool trackGuidedShadow = false;
  bool trackGuidedPromotion = false;
  bool trackGuidedSustain = false;
  bool bearing = false;
  bool bearingFrequencyResolvedShadow = false;
  bool bearingManifoldRecorder = false;
  // A single bounded lane may run the common temporal engine as an
  // observation-only arm. It never replaces the production map, detector or
  // tracker and is therefore safe to compare in exact-IQ replay.
  bool temporalShadow = false;
  // The current implementation is intentionally replay-only. It runs
  // synchronously and therefore must never be admitted on live acquisition.
  bool temporalShadowOfflineReplay = false;
  std::string temporalShadowIlluminatorId;
  std::string temporalShadowInputStatistic = "fused";
  std::string temporalShadowBootstrapCovariance = "diagonal";
  uint32_t temporalShadowMaximumCpis = 8;
  uint32_t temporalShadowDelayMotionBins = 2;
  uint32_t temporalShadowDopplerMotionBins = 4;
  std::string temporalShadowArtifactPath;
  uint64_t temporalShadowArtifactMaximumBytes = 4ULL << 30;
  // Reserved source contract, currently rejected: both allowed shadow inputs
  // discard complex phase. A phase-preserving CAF/beam source is required
  // before the standalone coherent engine can be connected to this lane.
  bool temporalShadowCoherentPathEvidence = false;
  int temporalShadowCoherentPhaseDirection = 1;
  // Runtime replay does not admit calibration samples.  Empirical null tails
  // require an externally validated, sample-disjoint negative-control split;
  // absent that split no calibrated decision can be claimed.
  uint32_t temporalShadowNullMinimumSamples = 32;
  double temporalShadowNullAlpha = 1e-3;
};

// Throws with a feature-specific diagnostic if a configured algorithm would
// otherwise be ignored by the multi-lane CPU radar.
void validateAdvancedFeatures(const AdvancedFeatureConfig& config);

struct RadarConfig
{
  double cpiSeconds = .5;
  int32_t delayMin = -10, delayMax = 100;
  int32_t dopplerMin = -200, dopplerMax = 200;
  int32_t clutterDelayMin = -10, clutterDelayMax = 101;
  // When present, saved delay bins describe this reference rate and are
  // projected independently into each lane's output sample rate.
  std::optional<uint32_t> delayReferenceRateHz;
  bool clutterFollowsAmbiguity = false;
  // Effective per-pass PFA, including the common contract's motion-trial
  // correction (nine trials for two-CPI one-bin motion).
  double pfa = 1e-5;
  int8_t guardCells = 2, trainingCells = 6;
  int32_t minimumDelay = 1;
  double minimumDopplerHz = 15;
  uint16_t centroidRadius = 6;
  // Unscaled Doppler bins. Empty inherits centroidRadius before delay-rate
  // projection, retaining the historical single-radius configuration.
  std::optional<uint16_t> centroidDopplerRadius;
  uint32_t trackerM = 3, trackerN = 6, trackerDelete = 10;
  double trackerMaximumAcceleration = 10;
  double trackerMinimumSnrDb = 5;
  bool trackerNormalizedInnovationGate = true;
  TrackerRecoveryConfig trackerRecovery;
  // Explicit per-statistic calibration overrides for tracker birth only.
  // Missing lanes inherit the global threshold; detector PFA is unchanged.
  std::unordered_map<std::string, double> trackerMinimumSnrDbByIlluminator;
  integration::LiveTemporalShadow::Config liveTemporalShadow;
  // Exactly one enabled lane may own the bounded live observer.
  std::string liveTemporalShadowIlluminatorId;
  DopplerRidgeConfig trackerDopplerRidge;
  DopplerRidgeScrubberConfig dopplerRidgeScrubber;
  std::size_t workers = 1;
  // Zero uses the bounded automatic lane executor. Benchmarks and constrained
  // deployments may lower this to trade latency for cache/RSS pressure.
  std::size_t laneWorkers = 0;
  // Qualified hardware acceleration is optional per lane. Auto falls back to
  // the identical CPU algorithm when the OS or device cannot support it.
  std::string accelerationMode = "cpu";
  std::string accelerationDevice = "auto";
  // Aggregate resident frontend ceilings, shared across configured lanes.
  uint32_t residentMaximumSessions = 8;
  uint64_t residentMemoryMaximumBytes = 2ULL << 30;
  // Empty selects the production topology policy. Tests may force either
  // mathematically equivalent full-quality Wiener-Hopf workspace.
  std::optional<bool> borrowedClutter;
  // Experimental sample masking changes radar sensitivity. Never infer it
  // from a waveform label, and never admit it on live acquisition.
  bool experimentalFmReferenceQualityFilter = false;
  bool offlineReplay = false;
  bool arrayReference = false;
  uint32_t referenceChannel = 0;
  std::vector<uint32_t> referenceChannels;
  std::vector<uint32_t> surveillanceChannels{1};
  ArrayReferenceSynthesizer::Config reference;
  std::optional<CoherentBeamBank::Config> coherentFusion;
  bearing_manifold::Identity manifoldIdentity;
  std::string detectionManifoldFile, bearingManifoldFile;
  std::optional<resident::Config> residentProductTemplate;
  gpu_result_json::BearingConfig bearingPublication;
  AdvancedFeatureConfig advanced;
};

// Public for deterministic native contract tests and replay tooling. Throws
// if a projection cannot be represented by the processor's bounded types.
RadarConfig scaleRadarConfigForLane(RadarConfig config,
  uint32_t laneSampleRateHz);

struct RadarFrame
{
  using DetectorIntegrationMetrics = detection::IntegrationMetrics;
  struct RidgeScrubberMetrics
  {
    std::string mode = "off";
    std::string state, reason, resetReason;
    bool comparisonAvailable = false;
    uint64_t epoch = 0;
    uint32_t observedCpis = 0, maximumCpis = 0;
    double backgroundMs = 0, comparisonDetectionMs = 0, comparisonTrackingMs = 0;
    double comparisonMs = 0, totalComparisonMs = 0;
    uint32_t observedRows = 0, qualifiedRows = 0;
    double maximumAppliedDb = 0;
    std::vector<double> qualifiedDopplerHz;
    uint64_t rawDetections = 0, scrubbedDetections = 0;
    uint64_t rawTracks = 0, scrubbedTracks = 0;
    uint64_t rawOperationalTracks = 0, scrubbedOperationalTracks = 0;
  };
  struct InputProcessingMetrics {
    std::string path = "loaded_double";
    uint64_t widenedSamples = 0, copiedSamples = 0;
    double ingestionMs = 0, pilotQualityMs = 0, pilotProjectionMs = 0;
  };
  std::string illuminatorId;
  uint64_t frequencyHz = 0;
  std::optional<std::array<double, 3>> transmitterLocation;
  std::optional<double> directPathAzimuthRad;
  uint64_t firstInputSample = 0, endInputSample = 0;
  uint32_t inputSamplesPerOutput = 1;
  std::vector<uint32_t> channelIds, referenceChannelIds;
  uint32_t sampleRateHz = 0;
  std::string atscReferenceRequested = "raw";
  std::string atscReferenceEffective = "raw";
  std::string atscReferenceReason = "raw_configured";
  std::optional<conditioning::AtscPilotQuality::Metrics> atscPilot;
  std::optional<conditioning::AtscPilotProjector::Metrics> atscPilotProjection;
  std::optional<RidgeScrubberMetrics> ridgeScrubber;
  std::optional<DetectorIntegrationMetrics> detectorIntegration;
  InputProcessingMetrics inputProcessing;
  std::shared_ptr<Map<std::complex<double>>> map;
  std::unique_ptr<Detection> detections;
  std::unique_ptr<Track> tracks;
  // Immutable native products from the same admitted resident algorithm.
  // They retain the lane's private temporal/tracker state; no host adapter
  // reconstructs or mixes another lane's tracker history.
  std::optional<resident::Coordinator::Publication> residentProducts;
  bool residentTemporalExecuted = false;
  TrackerFrameDiagnostics trackerDiagnostics;
  double trackerMinimumSnrDb = 0;
  bool trackerMinimumSnrOverridden = false;
  std::optional<integration::LiveTemporalShadow::Status> liveTemporalShadow;
  AccelerationStatus acceleration;
  AccelerationStatus clutterAcceleration;
  bool clutterGpuExecuted = false;
  bool clutterCpuExecuted = true;
  bool ambiguityGpuExecuted = false;
  bool ambiguityCpuExecuted = true;
  bool temporalShadowActive = false;
  uint64_t temporalShadowSequence = 0;
  uint32_t temporalShadowAvailableCpis = 0;
  uint32_t temporalShadowMaximumCpis = 0;
  double temporalShadowPeakPower = 0;
  uint64_t temporalShadowIntegratedCells = 0;
  uint64_t temporalShadowBootstrapTrainedCells = 0;
  uint64_t temporalShadowBootstrapFallbackCells = 0;
  uint64_t temporalShadowBootstrapInvalidCells = 0;
  std::vector<uint8_t> temporalShadowBootstrapStatisticFamily;
  std::string temporalShadowInputStatistic;
  std::array<bool, 4> temporalShadowBankReady{};
  bool temporalShadowCoherentActive = false;
  bool temporalShadowNullCalibrationFrozen = false;
  uint64_t temporalShadowCoherentValidCells = 0;
  // Per-cell empirical tails are diagnostic only; no global detection or
  // false-alarm decision is produced without a block-max/trial calibration.
  uint64_t temporalShadowNullTestedCells = 0;
  uint64_t temporalShadowNullSignificantCells = 0;
  uint64_t temporalShadowNullUnresolvedCells = 0;
};

// Synchronous read-only native CAF view; pointers are valid only during the
// observer call, before fusion discards channel phase. Never a startup oracle.
struct NativeCafView {
  std::string illuminatorId;
  uint64_t frequencyHz = 0;
  uint32_t sampleRateHz = 0;
  uint64_t firstInputSample = 0, endInputSample = 0;
  uint32_t inputSamplesPerOutput = 1;
  std::vector<uint32_t> channelIds, referenceChannelIds;
  std::vector<const Map<std::complex<double>>*> maps;
  std::vector<std::complex<double>> referenceWeights;
};
using NativeCafObserver = std::function<void(const NativeCafView&)>;

// Identity of the captured batch used for live evidence and publication. No
// external truth enters the radar; timestamp matches its native frame product.
struct LiveTemporalIdentity {
  uint64_t timestampMs = 0, captureEpoch = 0;
  bool samplePositionAvailable = false;
  std::vector<double> inputGainDb;
};

// Independent adaptive radar contexts fed by a coherent channelizer bank.
// The object owns no receiver and publishes no sockets, which makes the same
// processing path usable by live capture and deterministic replay.
class MultiIlluminatorRadar
{
public:
  MultiIlluminatorRadar(uint32_t captureRateHz, std::size_t channelCount,
    const std::vector<channelizer::IlluminatorDefinition>& illuminators,
    RadarConfig config, Acceleration::BackendFactory backendFactory = {});
  ~MultiIlluminatorRadar();

  // Complete lane DSP qualification without capturing or publishing samples.
  // Must be called before process(); a repeated pre-input call is harmless.
  void prepareStartup();
  // Install before input. Absent observers perform no allocation or copying.
  void setNativeCafObserver(NativeCafObserver observer);
  // Diagnostic inspection only; production never consumes these scores.
  std::optional<integration::LiveTemporalShadow::Status>
    liveTemporalShadowStatus() const;
  std::unique_ptr<integration::LiveTemporalShadow::Evidence>
    takeLiveTemporalEvidence();

  std::vector<RadarFrame> process(
    std::vector<channelizer::IlluminatorOutput> input,
    std::optional<LiveTemporalIdentity> liveIdentity = std::nullopt);
  // Snapshot after process() returns. Counters include rejected CPIs, which do
  // not produce RadarFrame objects and therefore cannot be inferred from the
  // aggregate acceleration status.
  std::vector<LaneHealthStatus> laneHealth() const;
  void reset();

private:
  struct Context;
  uint32_t captureRateHz_ = 0;
  std::size_t channelCount_ = 0;
  RadarConfig config_;
  std::vector<std::unique_ptr<Context>> contexts_;
  std::unique_ptr<utility::FixedExecutor> executor_;
  bool startupPrepared_ = false;
  bool inputStarted_ = false;
};

} // namespace blah2::multi_illuminator

#endif
