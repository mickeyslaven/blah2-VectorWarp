#ifndef MULTI_ILLUMINATOR_RADAR_H
#define MULTI_ILLUMINATOR_RADAR_H

#include "process/analysis/ProductionRawIq.h"
#include "process/conditioning/IlluminatorChannelizerBank.h"
#include "process/detection/Centroid.h"
#include "process/detection/CfarDetector1D.h"
#include "process/detection/Interpolate.h"
#include "process/fusion/Noncoherent.h"
#include "process/tracker/Tracker.h"
#include "process/utility/FixedExecutor.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace blah2::multi_illuminator
{

// Settings whose production implementations currently live in the
// single-illuminator GPU-resident pipeline.  Multi-illuminator callers must
// pass their effective values here so the lane radar cannot silently degrade
// to a simpler algorithm.
struct AdvancedFeatureConfig
{
  std::string surveillanceFusionMode = "noncoherent";
  bool surveillanceManifoldAutomation = false;
  uint32_t detectorIntegrationCpis = 1;
  uint32_t detectorIntegrationMotionBins = 0;
  bool trackBeforeDetect = false;
  bool trackerBeamToleranceConfigured = false;
  bool trackerGpuCapacityConfigured = false;
  std::string trackerAccelerationModel = "hypothesis_bank";
  bool stationaryClutter = false;
  bool trackGuidedShadow = false;
  bool trackGuidedPromotion = false;
  bool bearing = false;
  bool bearingFrequencyResolvedShadow = false;
  bool bearingManifoldRecorder = false;
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
  double pfa = 1e-5;
  int8_t guardCells = 2, trainingCells = 6, minimumDelay = 1;
  double minimumDopplerHz = 15;
  uint16_t centroidRadius = 6;
  uint32_t trackerM = 3, trackerN = 6, trackerDelete = 10;
  double trackerMaximumAcceleration = 10;
  double trackerMinimumSnrDb = 5;
  std::size_t workers = 1;
  // Zero uses the bounded automatic lane executor. Benchmarks and constrained
  // deployments may lower this to trade latency for cache/RSS pressure.
  std::size_t laneWorkers = 0;
  // Empty selects the production topology policy. Tests may force either
  // mathematically equivalent full-quality Wiener-Hopf workspace.
  std::optional<bool> borrowedClutter;
  bool arrayReference = false;
  uint32_t referenceChannel = 0;
  std::vector<uint32_t> referenceChannels;
  std::vector<uint32_t> surveillanceChannels{1};
  ArrayReferenceSynthesizer::Config reference;
  AdvancedFeatureConfig advanced;
};

struct RadarFrame
{
  std::string illuminatorId;
  uint64_t frequencyHz = 0;
  uint64_t firstInputSample = 0, endInputSample = 0;
  uint32_t sampleRateHz = 0;
  std::shared_ptr<Map<std::complex<double>>> map;
  std::unique_ptr<Detection> detections;
  std::unique_ptr<Track> tracks;
};

// Independent adaptive radar contexts fed by a coherent channelizer bank.
// The object owns no receiver and publishes no sockets, which makes the same
// processing path usable by live capture and deterministic replay.
class MultiIlluminatorRadar
{
public:
  MultiIlluminatorRadar(uint32_t captureRateHz, std::size_t channelCount,
    const std::vector<channelizer::IlluminatorDefinition>& illuminators,
    RadarConfig config);
  ~MultiIlluminatorRadar();

  std::vector<RadarFrame> process(
    std::vector<channelizer::IlluminatorOutput> input);
  void reset();

private:
  struct Context;
  uint32_t captureRateHz_ = 0;
  std::size_t channelCount_ = 0;
  RadarConfig config_;
  std::vector<std::unique_ptr<Context>> contexts_;
  std::unique_ptr<utility::FixedExecutor> executor_;
};

} // namespace blah2::multi_illuminator

#endif
