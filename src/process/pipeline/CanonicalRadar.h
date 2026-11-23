#ifndef CANONICAL_RADAR_H
#define CANONICAL_RADAR_H

#include "process/pipeline/RawIqStages.h"
#include "process/pipeline/RadarPipeline.h"
#include "process/analysis/MultiIlluminatorHealth.h"
#include "process/ambiguity/Acceleration.h"
#include "data/CanonicalSignal.h"
#include "process/conditioning/AtscPilotQuality.h"
#include "process/conditioning/AtscPilotProjector.h"
#include "process/detection/Centroid.h"
#include "process/detection/CfarDetector1D.h"
#include "process/detection/Interpolate.h"
#include "process/detection/DetectorIntegration.h"
#include "process/fusion/Noncoherent.h"
#include "process/fusion/CoherentBeamBank.h"
#include "process/ambiguity/GpuResidentCoordinator.h"
#include "process/analysis/ReplayCoherentObserver.h"
#include "process/ambiguity/BearingManifold.h"
#include "process/tracker/Tracker.h"
#include "process/utility/FixedExecutor.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cmath>
#include <functional>
#include <memory>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace blah2::pipeline
{
using multi_illuminator::LaneHealthStatus;

inline uint32_t resolveCpiSamples(uint32_t rateHz, double seconds)
{
  const double samples = double(rateHz) * seconds;
  if (!rateHz || !std::isfinite(seconds) || seconds <= 0 ||
      !std::isfinite(samples) || samples < 1 ||
      samples > double(std::numeric_limits<uint32_t>::max()))
    throw std::invalid_argument("Invalid CPI sample geometry");
  return uint32_t(std::llround(samples));
}

inline double resolveReferenceSpectrumBandwidth(
    const std::optional<double>& configured, uint32_t outputRateHz)
{
  return configured.value_or(double(outputRateHz));
}

inline uint32_t resolveReferenceSpectrumBins(uint32_t samples,
    uint32_t outputRateHz, double bandwidthHz)
{
  const double resolution = double(outputRateHz) / samples;
  const uint32_t stride = uint32_t(std::min<double>(samples,
    std::max(1.0, std::ceil(bandwidthHz / resolution))));
  return 1 + (samples - 1) / stride;
}

// Shared advanced behavior and remaining resident-only capabilities. Callers
// pass effective values so a lane cannot silently ignore configured behavior.
struct AdvancedFeatureConfig
{
  std::string surveillanceFusionMode = "noncoherent";
  uint32_t detectorIntegrationCpis = 1;
  uint32_t detectorIntegrationMotionBins = 0;
  bool trackBeforeDetect = false;
  bool trackerBeamToleranceConfigured = false;
  bool trackerGpuCapacityConfigured = false;
  // Resolved normalized tracking policy; independent of ingress arrangement.
  std::string trackerAccelerationModel = "adaptive_gate";
  bool stationaryClutter = false;
  bool bearing = false;
  bool bearingManifoldRecorder = false;
  bool residentDiagnostics = false;
};

// Throws with a feature-specific diagnostic if a configured algorithm would
// otherwise be ignored by the canonical CPU products.
void validateAdvancedFeatures(const AdvancedFeatureConfig& config);

inline bool requiresResidentProducts(const AdvancedFeatureConfig& config)
{
  return config.surveillanceFusionMode == "noise_whitened_beam_bank" ||
    config.bearing || config.trackerBeamToleranceConfigured || config.residentDiagnostics;
}

// AUTO chooses an implementation without changing configured radar features.
// Required resident products cannot fall back to different host algorithms.
inline bool selectRequiredResidentRoute(const std::string& mode, bool required)
{
  if (required && mode != "auto" && mode != "gpu-resident")
    throw std::invalid_argument("Selected radar features require a qualified resident backend; select auto or gpu-resident");
  return mode == "gpu-resident" || (mode == "auto" && required);
}

// Pure route selector kept public for deterministic regression coverage. A
// host-conditioned reference remains GPU-capable through the portable worker,
// but cannot enter the resident frontend until that input contract exists.
inline bool shouldAttemptResidentFrontend(const std::string& accelerationMode,
  bool residentSelectable, bool residentReferenceCompatible, bool residentBuilt = true)
{
  return residentReferenceCompatible &&
    (accelerationMode == "gpu-resident" ||
     (accelerationMode == "auto" && residentSelectable && residentBuilt));
}

inline bool shouldAttemptPortableFrontend(const std::string& accelerationMode,
  bool residentActive, bool residentRequired)
{
  return !residentActive && !residentRequired &&
    (accelerationMode == "auto" || accelerationMode == "gpu");
}

// The isolated portable worker must return a full filtered CPI to the host
// before the same samples are uploaded again for CAF.  AUTO therefore keeps
// clutter on the host and uses the portable worker for CAF only; this removes
// a topology-imposed round trip without disabling GPU acceleration.  Explicit
// GPU mode remains available for qualification and controlled comparisons.
inline bool shouldOfferPortableGpuClutter(const std::string& accelerationMode,
  bool clutterEnabled)
{
  return clutterEnabled && accelerationMode == "gpu";
}

inline bool shouldAttemptTemporalProducts(const std::string& mode,
  bool fp64Selectable, bool coherent, bool detection, uint32_t depth,
  bool portableClutterRequired = false)
{
  return (mode == "auto" || mode == "gpu" || mode == "gpu-resident") &&
    fp64Selectable && !coherent && detection && !portableClutterRequired &&
    (depth == 2 || depth == 4 || depth == 8);
}

struct RadarConfig
{
  pipeline::Stages stages;
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
  uint32_t trackerBeamTolerance = UINT32_MAX;
  bool trackerNormalizedInnovationGate = true;
  TrackerRecoveryConfig trackerRecovery;
  // Explicit per-statistic calibration overrides for tracker birth only.
  // Missing lanes inherit the global threshold; detector PFA is unchanged.
  std::unordered_map<std::string, double> trackerMinimumSnrDbByIlluminator;
  // Existing canonical capacity only; its owner must outlive this bank.
  utility::FixedExecutor* observationExecutor = nullptr;
  DopplerRidgeConfig trackerDopplerRidge;
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
  // Shared lane ceiling; each worker still clamps allocations to the selected
  // device and heap policy. Three production lanes retain enough headroom for
  // the exact FP64-refined clutter path without weakening smaller-GPU guards.
  uint64_t residentMemoryMaximumBytes = 3ULL << 30;
  // Empty selects the production topology policy. Tests may force either
  // mathematically equivalent full-quality Wiener-Hopf workspace.
  std::optional<bool> borrowedClutter;
  // Experimental sample masking changes radar sensitivity. Never infer it
  // from a waveform label; explicit admission is identical live and replay.
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
  // Optional publication diagnostics share the canonical reference and maps.
  std::optional<double> referenceSpectrumBandwidthHz;
  resident::BearingConfig manifoldEvidence;
  AdvancedFeatureConfig advanced;
};

// Public for deterministic native contract tests and replay tooling. Throws
// if a projection cannot be represented by the processor's bounded types.
RadarConfig scaleRadarConfigForLane(RadarConfig config,
  uint32_t laneSampleRateHz);
RadarConfig applyLaneTopology(RadarConfig config, const iq::SignalDefinition& definition);

// Resolve physical units once, after canonical ambiguity geometry is known.
// CPU, portable GPU, resident GPU and startup oracles consume this same plan.
// Spatial metadata is a qualified product capability; an unavailable requested
// association gate is refused, never removed from the configured policy.
RadarPlan resolveRadarPlan(const RadarConfig& config, const Ambiguity& ambiguity,
  uint32_t sampleRateHz, uint64_t frequencyHz, uint32_t samples,
  bool spatialEvidence = false);

struct RadarFrame
{
  using DetectorIntegrationMetrics = detection::IntegrationMetrics;
  struct InputProcessingMetrics {
    std::string path = "loaded_double";
    uint64_t widenedSamples = 0, copiedSamples = 0;
    uint64_t contiguousSamples = 0, contiguousAllocations = 0;
    uint64_t dequeMaterializations = 0, materializedSamples = 0;
    double materializationMs = 0;
    double ingestionMs = 0, pilotQualityMs = 0, pilotProjectionMs = 0;
  };
  std::string illuminatorId, resolvedProductPlan, resolvedFrontendPlan;
  uint64_t frequencyHz = 0;
  std::optional<std::array<double, 3>> transmitterLocation;
  std::optional<double> directPathAzimuthRad;
  uint64_t firstInputSample = 0, endInputSample = 0;
  // Opaque publication binding supplied by the caller; never a radar feature.
  uint64_t publicationSequence = 0;
  uint32_t inputSamplesPerOutput = 1;
  std::vector<uint32_t> channelIds, referenceChannelIds;
  uint32_t sampleRateHz = 0;
  std::string atscReferenceRequested = "raw";
  std::string atscReferenceEffective = "raw";
  std::string atscReferenceReason = "raw_configured";
  std::optional<conditioning::AtscPilotQuality::Metrics> atscPilot;
  std::optional<conditioning::AtscPilotProjector::Metrics> atscPilotProjection;
  std::optional<DetectorIntegrationMetrics> detectorIntegration;
  InputProcessingMetrics inputProcessing;
  std::shared_ptr<Map<std::complex<double>>> map;
  bool detectionEnabled = true, trackingEnabled = true;
  std::unique_ptr<Detection> detections;
  std::unique_ptr<Track> tracks;
  // Immutable native products from the same admitted resident algorithm.
  // They retain the lane's private temporal/tracker state; no host adapter
  // reconstructs or mixes another lane's tracker history.
  std::optional<resident::Coordinator::Publication> residentProducts;
  std::string referenceSpectrum, manifoldEvidence;
  struct ResidentDiagnostics {
    std::shared_ptr<const resident::Config> config;
    uint64_t logical = 0, consumed = 0;
    std::vector<uint32_t> causalTrace;
    std::array<uint8_t, 32> moduleHash{};
    std::vector<uint8_t> snapshot;
    ResidentDiagnostics(std::shared_ptr<const resident::Config> configuration,
        uint64_t logicalId, uint64_t consumedSequence, const resident::OutputViews& prepared)
      : config(std::move(configuration)), logical(logicalId), consumed(consumedSequence) {
      if (prepared.causalTrace.size)
        causalTrace.assign(prepared.causalTrace.data, prepared.causalTrace.data + prepared.causalTrace.size);
      if (prepared.preparedEvidence.size >= 192)
        std::copy_n(prepared.preparedEvidence.data + 160, moduleHash.size(), moduleHash.begin());
    }
  };
  // These values own their bytes. No prepared/shared GPU view escapes staging;
  // releasing the device means transfer to this frame, not transport success.
  std::optional<ResidentDiagnostics> residentDiagnostics;
  bool residentTemporalExecuted = false;
  std::string temporalDevice;
  TrackerFrameDiagnostics trackerDiagnostics;
  double trackerMinimumSnrDb = 0;
  bool trackerMinimumSnrOverridden = false;
  AccelerationStatus acceleration;
  AccelerationStatus clutterAcceleration;
  bool clutterGpuExecuted = false;
  bool clutterCpuExecuted = true;
  bool ambiguityGpuExecuted = false;
  bool ambiguityCpuExecuted = true;
};

// Synchronous read-only native CAF view; pointers are valid only during the
// observer call, before fusion discards channel phase. Never a startup oracle.
struct NativeCafView {
  std::string illuminatorId, resolvedFrontendPlan;
  uint64_t frequencyHz = 0;
  uint32_t sampleRateHz = 0;
  uint64_t firstInputSample = 0, endInputSample = 0;
  uint32_t inputSamplesPerOutput = 1;
  std::vector<uint32_t> channelIds, referenceChannelIds;
  std::vector<const Map<std::complex<double>>*> maps;
  std::vector<std::complex<double>> referenceWeights;
  std::optional<std::complex<double>> directPathPilot;
  std::optional<uint32_t> pilotAnchorChannel;
};
using NativeCafObserver = std::function<void(const NativeCafView&)>;

// Identity of the captured batch used for live evidence and publication. No
// external truth enters the radar; timestamp matches its native frame product.
struct LiveTemporalIdentity {
  uint64_t timestampMs = 0, captureEpoch = 0;
  bool samplePositionAvailable = false;
  std::vector<double> inputGainDb;
  uint64_t publicationSequence = 0;
  // Monotonic input progress, separate from the publication/event timestamp.
  std::optional<uint64_t> signalTimestampMs;
  // Authenticated raw capture CPI, not the decimated output-sample grid.
  // Kept optional for standalone canonical-signal consumers without capture.
  std::optional<std::array<uint64_t, 2>> captureSamples;
};

// One adaptive context per canonical signal. Ingress adapters assemble exact
// coherent blocks; normal processing uses RadarPipeline regardless of origin.
// The object owns no receiver, channelizer or publication transport.
class RadarBank
{
public:
  RadarBank(uint32_t captureRateHz, std::size_t channelCount,
    const std::vector<iq::SignalDefinition>& illuminators,
    RadarConfig config, Acceleration::BackendFactory backendFactory = {});
  ~RadarBank();

  // Complete lane DSP qualification without capturing or publishing samples.
  // Must be called before process(); a repeated pre-input call is harmless.
  void prepareStartup();
  // Install before input. Absent observers perform no allocation or copying.
  void setNativeCafObserver(NativeCafObserver observer);
  void setQualificationObserver(std::shared_ptr<qualification_observer::Observer> observer);

  std::vector<RadarFrame> process(
    std::vector<iq::SignalBlock> input,
    std::optional<LiveTemporalIdentity> liveIdentity = std::nullopt);
  // One owned product CPI may remain per lane. The next call stages its
  // frontend before joining the preceding products. drain() completes EOF.
  std::vector<RadarFrame> processPipelined(
    std::vector<iq::SignalBlock> input,
    std::optional<LiveTemporalIdentity> liveIdentity = std::nullopt);
  std::vector<RadarFrame> drain();
  std::vector<uint64_t> pendingPublicationSequences() const;
  // Snapshot after process() returns. Counters include rejected CPIs, which do
  // not produce RadarFrame objects and therefore cannot be inferred from the
  // aggregate acceleration status.
  std::vector<LaneHealthStatus> laneHealth() const;
  // Live ingress may retain the receiver's native FP32 representation when
  // every direct consumer has a qualified resident frontend. Receiver and OS
  // identity are deliberately absent from this capability query.
  bool prefersNativeFloatCapture() const;
  void reset();

private:
  std::vector<RadarFrame> processImpl(std::vector<iq::SignalBlock> input,
    std::optional<LiveTemporalIdentity> liveIdentity, bool deferProducts);
  struct Context;
  uint32_t captureRateHz_ = 0;
  std::size_t channelCount_ = 0;
  const RadarConfig config_;
  std::vector<std::unique_ptr<Context>> contexts_;
  std::unique_ptr<utility::FixedExecutor> executor_;
  std::unique_ptr<utility::FixedExecutor> productExecutor_;
  bool startupPrepared_ = false;
  bool inputStarted_ = false;
};

} // namespace blah2::pipeline

#endif
