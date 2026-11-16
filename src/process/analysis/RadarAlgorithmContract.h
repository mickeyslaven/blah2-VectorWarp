#pragma once

#include "process/ambiguity/GpuResidentBackend.h"
#include "process/analysis/MultiIlluminatorRadar.h"

#include <cmath>
#include <cstdint>
#include <stdexcept>

namespace blah2::radar_contract {

// Receiver- and OS-independent radar behavior. Capture, channelization,
// geometry qualification and GPU transport remain adapters around this
// contract; they must not invent detector or tracker defaults.
struct Algorithm {
  double cpiSeconds = 0;
  int8_t guardCells = 0, trainingCells = 0;
  int32_t minimumDelay = 0;
  double minimumDopplerHz = 0, pfa = 0;
  uint16_t centroidDelayRadius = 0, centroidDopplerRadius = 0;
  uint32_t integrationCpis = 1, integrationMotion = 0;
  uint32_t integrationBeamTolerance = UINT32_MAX;
  bool trackBeforeDetect = false;
  uint32_t trackerM = 0, trackerN = 0, trackerDelete = 0;
  double trackerMaximumAcceleration = 0, trackerRangeResolutionM = 0,
    trackerWavelengthM = 0, trackerMinimumSnrDb = -100;
  bool trackerAdaptiveGate = false, trackerStationaryClutter = false,
    trackerGuidedShadow = false, trackerGuidedPromote = false,
    trackerGuidedSustain = false, trackerNormalizedInnovationGate = true;
  uint32_t stationaryPersistenceCpis = 0, trackerGuidedMaximumTracks = 0;
  double stationaryMaximumDopplerHz = 0;
};

inline void validate(const Algorithm& value) {
  if (!std::isfinite(value.cpiSeconds) || value.cpiSeconds <= 0 ||
      value.guardCells < 0 || value.trainingCells < 1 ||
      !std::isfinite(value.minimumDopplerHz) || value.minimumDopplerHz < 0 ||
      !std::isfinite(value.pfa) || value.pfa <= 0 || value.pfa >= 1 ||
      !value.integrationCpis || !value.trackerM ||
      value.trackerM > value.trackerN || !value.trackerDelete ||
      !std::isfinite(value.trackerMaximumAcceleration) ||
      value.trackerMaximumAcceleration < 0 ||
      !std::isfinite(value.trackerRangeResolutionM) ||
      value.trackerRangeResolutionM <= 0 ||
      !std::isfinite(value.trackerWavelengthM) ||
      value.trackerWavelengthM <= 0 ||
      !std::isfinite(value.trackerMinimumSnrDb))
    throw std::invalid_argument("Unified radar algorithm contract is invalid");
  if ((value.trackerStationaryClutter &&
       (value.stationaryPersistenceCpis<2 || value.stationaryPersistenceCpis>65535)) ||
      !std::isfinite(value.stationaryMaximumDopplerHz) || value.stationaryMaximumDopplerHz<0 ||
      ((value.trackerGuidedPromote || value.trackerGuidedSustain) && !value.trackerGuidedShadow) ||
      (value.trackerGuidedShadow && (!value.trackerGuidedMaximumTracks || value.trackerGuidedMaximumTracks>65535)) ||
      (value.trackerGuidedPromote && value.trackerN<2*value.trackerM-1))
    throw std::invalid_argument("Unified tracker recovery contract is invalid");
}

inline TrackerRecoveryConfig hostTrackerRecovery(const Algorithm& value) {
  validate(value);
  TrackerRecoveryConfig output;
  output.stationaryClutter=value.trackerStationaryClutter;
  output.persistenceCpis=value.stationaryPersistenceCpis;
  output.maximumDopplerHz=value.stationaryMaximumDopplerHz;
  output.guidedShadow=value.trackerGuidedShadow;
  output.guidedPromote=value.trackerGuidedPromote;
  output.guidedSustain=value.trackerGuidedSustain;
  output.guidedMaximumTracks=value.trackerGuidedMaximumTracks;
  return output;
}

// Apply only behavior whose units are independent of receiver sample rate.
// Delay-cell radii, minimum delay, CPI sample count and centroid delay radius
// are intentionally resolved by the capture/lane adapter exactly once.  A
// resident adapter receives an Algorithm that is already resolved for its
// capture rate; a channelized adapter retains its requested physical contract
// until scaleRadarConfigForLane() resolves each lane.
inline void applyReceiverIndependent(Algorithm value,
  multi_illuminator::RadarConfig& output) {
  validate(value);
  output.pfa = value.pfa;
  output.minimumDopplerHz = value.minimumDopplerHz;
  output.centroidDopplerRadius = value.centroidDopplerRadius;
  output.trackerM = value.trackerM;
  output.trackerN = value.trackerN;
  output.trackerDelete = value.trackerDelete;
  output.trackerMaximumAcceleration = value.trackerMaximumAcceleration;
  output.trackerMinimumSnrDb = value.trackerMinimumSnrDb;
  output.trackerNormalizedInnovationGate = value.trackerNormalizedInnovationGate;
  output.trackerRecovery = hostTrackerRecovery(value);
  output.advanced.detectorIntegrationCpis = value.integrationCpis;
  output.advanced.detectorIntegrationMotionBins = value.integrationMotion;
  output.advanced.trackBeforeDetect = value.trackBeforeDetect;
  output.advanced.trackerBeamToleranceConfigured =
    value.integrationBeamTolerance != UINT32_MAX;
  output.advanced.trackerAccelerationModel = value.trackerAdaptiveGate ?
    "adaptive_gate" : "hypothesis_bank";
  output.advanced.stationaryClutter = value.trackerStationaryClutter;
  output.advanced.trackGuidedShadow = value.trackerGuidedShadow;
  output.advanced.trackGuidedPromotion = value.trackerGuidedPromote;
  output.advanced.trackGuidedSustain = value.trackerGuidedSustain;
}

inline resident::DetectorConfig residentDetector(Algorithm value,
  bool enabled) {
  validate(value);
  return {enabled, true, true, value.guardCells, value.trainingCells,
    value.minimumDelay, value.centroidDelayRadius, value.centroidDopplerRadius,
    value.pfa, value.minimumDopplerHz, 1 / value.cpiSeconds,
    value.integrationCpis, value.integrationMotion,
    value.integrationBeamTolerance};
}

inline resident::TrackerConfig residentTracker(Algorithm value, bool enabled,
  uint32_t capacity) {
  validate(value);
  return {enabled, capacity, value.trackerM, value.trackerN,
    value.trackerDelete, value.trackerAdaptiveGate ?
      resident::TrackerInitiationMode::adaptiveGate :
      resident::TrackerInitiationMode::hypothesisBank,
    value.cpiSeconds, value.trackerMaximumAcceleration,
    value.trackerRangeResolutionM, value.trackerWavelengthM,
    value.trackerMinimumSnrDb, value.trackerStationaryClutter,
    value.stationaryPersistenceCpis, value.stationaryMaximumDopplerHz,
    value.trackerGuidedShadow, value.trackerGuidedPromote,
    value.trackerGuidedSustain, value.trackerNormalizedInnovationGate,
    value.trackerGuidedMaximumTracks};
}

} // namespace blah2::radar_contract
