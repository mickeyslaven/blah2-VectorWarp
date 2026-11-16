/// @file Tracker.h
/// @class Tracker
/// @brief A class to implement a bistatic tracker.
/// @details Key functions are update, initiate, smooth and remove.
/// @details Update before initiate to avoid duplicate tracks.
/// @author 30hours
/// @todo Add smoothing capability.
/// @todo Fix units up.
/// @todo I don't think I callback the true CPI time from ambiguity.

#ifndef TRACKER_H
#define TRACKER_H

#include "data/Detection.h"
#include "data/Track.h"

#include <stdint.h>
#include <memory>
#include <array>
#include <string>
#include <unordered_map>
#include <vector>
#include <deque>
#include <map>

struct TrackerRecoveryConfig {
  bool stationaryClutter = false;
  uint32_t persistenceCpis = 60;
  double maximumDopplerHz = 0; // zero covers the full configured Doppler map
  uint32_t maximumStationaryCells = 65536;
  bool guidedShadow = false, guidedPromote = false, guidedSustain = false;
  uint32_t guidedMaximumTracks = 64;
};

struct TrackerGuidedPrediction {
  std::string trackId;
  double priorDelay = 0, priorDopplerHz = 0, delay = 0, dopplerHz = 0;
  bool operational = false;
};

struct TrackerGuidedCandidate {
  std::string trackId;
  Detection::Evidence evidence;
  bool valid = false, aboveThreshold = false;
};

struct DopplerRidgeConfig
{
  bool enabled = false;
  uint32_t persistenceCpis = 12;
  double toleranceHz = 3.0;
  double minimumDelaySeparationKm = .25;
};

// Optional shadow/replay policy. Existing tracks always associate against the
// complete candidate set first. Only unmatched new births are made exclusive
// around measurements actually claimed this frame and stronger selected births.
struct BirthExclusivityConfig
{
  bool enabled = false;
  double delayHalfWidthBins = 1.0;
  double dopplerHalfWidthBins = 1.0;
};

// Per-frame radar-only diagnostics. Pair rejection reasons are exclusive in
// order; they count candidate/track pairs, not unique detections.
struct TrackerFrameDiagnostics {
  bool noOp=false;
  uint64_t detections=0, tracksBefore=0, tracksAfter=0;
  uint64_t pairs=0, pairClaimed=0, pairDelayRejected=0, pairDopplerRejected=0, pairInGate=0;
  uint64_t pairInnovationRejected=0;
  uint64_t associations=0, misses=0, promotions=0, tentativeExpired=0, inactivityDeleted=0;
  uint64_t birthCandidates=0, birthSnrRejected=0, birthRidgeRejected=0,
    birthExclusiveRejected=0, initiated=0;
  uint64_t weakSupportCandidates=0, weakSupportInGate=0,
    weakSupportAccepted=0, weakSupportCreditedCpis=0,
    weakSupportLineageRejected=0;
  uint64_t lineageResets=0, pairLineageRejected=0, birthLineageRejected=0,
    creditedCpis=0, independentCredits=0, birthCapacityRejected=0;
  uint64_t stationaryBirthRejected=0, stationaryQuarantined=0,
    stationaryCells=0, stationaryCapacityRejected=0, stationaryBirthCapacityRejected=0;
  uint64_t guidedCandidates=0, guidedInvalid=0, guidedWouldRecover=0,
    guidedLineageRejected=0, guidedStationaryRejected=0,
    guidedPromotions=0, guidedSustained=0, guidedBudgetRejected=0;
};

// A deliberately imprecise, truth-blind support observation. It may extend
// the coast lifetime of an already operational track, but can never create,
// promote, reposition, or change the acceleration of a track.
struct TrackerWeakSupportCandidate {
  double delay = 0, dopplerHz = 0;
  uint64_t epoch = 0, newestSequence = 0;
  uint8_t depth = 0;
  std::array<uint64_t, 8> sequences{};
};

class Tracker
{
private:
  /// @brief Track initiation constant for M of N detections.
  uint32_t m;

  /// @brief Track initiation constant for M of N detections.
  uint32_t n;

  /// @brief Number of missed predictions to delete a tentative track.
  uint32_t nDelete;

  /// @brief True CPI time for acceleration resolution(s).
  double cpi;

  /// @brief Maximum acceleration to initiate track (Hz/s).
  double maxAccInit;

  /// @brief Minimum SNR allowed to start a new tentative track (dB).
  double minInitiationSnr;

  /// @brief Range resolution for kinematics equations (m).
  double rangeRes;

  /// @brief Wavelength for kinematics equations (m).
  double lambda;

  /// @brief Acceleration values to initiate track (Hz/s).
  std::vector<double> accInit;

  /// @brief Start one zero-acceleration candidate and estimate acceleration
  /// from associations instead of cloning every acceleration hypothesis.
  bool adaptiveGate;

  /// @brief Use a bounded 99% normalized innovation ellipse after the first
  /// acceleration-establishing association.  Residuals are expressed in the
  /// receiver's native delay and Doppler bins, so the contract is independent
  /// of SDR sample rate, carrier frequency, operating system and backend.
  bool normalizedInnovationGate;

  /// NIS radius squared for the normalized gate. 4/pi gives the same area as
  /// the legacy +/-1-bin rectangle when covariance is the identity.
  double associationMaximumNis;

  BirthExclusivityConfig birthExclusivity;

  /// @brief Index of detections already updated.
  std::vector<bool> doNotInitiate;

  /// @brief POSIX timestamp of last update (ms).
  uint64_t timestamp;

  /// @brief Track data.
  Track track;
  TrackerFrameDiagnostics diagnostics;
  DopplerRidgeConfig dopplerRidge;
  double dopplerRidgeMinimumDelayBins = 0;
  struct DopplerRidgeState
  {
    double dopplerHz = 0;
    uint32_t streak = 0;
    uint32_t missedCpis = 0;
    bool seen = false;
  };
  std::vector<DopplerRidgeState> dopplerRidges;

  struct WeakSupportLineage {
    uint64_t epoch = 0, highWaterSequence = 0;
    bool initialized = false;
  };
  std::unordered_map<std::string, WeakSupportLineage> weakSupportLineage;
  TrackerFrameDiagnostics pendingWeakSupport;
  TrackerRecoveryConfig recovery;
  struct EvidenceLedger {
    uint64_t epoch = 0, highWaterSequence = 0;
    bool initialized = false;
    std::deque<bool> votes;
  };
  std::unordered_map<std::string, EvidenceLedger> evidenceLedgers;
  std::vector<bool> lineageNoBirth;
  uint64_t evidenceEpoch = 0, frameSequence = 0, frameOrdinal = 0;
  bool evidenceEpochInitialized = false;
  bool pendingLineageReset = false;
  struct StationaryCell { uint64_t last = 0; uint32_t streak = 0; };
  std::map<std::pair<int64_t,int64_t>,StationaryCell> stationaryCells;
  std::vector<TrackerGuidedCandidate> frameGuided;
  bool frameHasEvidence = false;
  bool validEvidence(const Detection::Evidence&, bool requireCurrentEpoch = true) const;
  bool freshEvidence(const std::string&, const Detection::Evidence&) const;
  bool creditEvidence(const std::string&, const Detection::Evidence*);
  void appendVote(uint64_t index, bool credit);
  void promote(uint64_t index, bool credit);
  bool stationary(double delay, double doppler) const;
  bool stationaryCapacityBlocked(double delay, double doppler) const;
  void pruneStationary();
  void learnStationary(Detection& detection);
  bool guidedRecovery(uint64_t index, Detection& prediction, bool& credit);
  void pruneLedgers();

  void updateDopplerRidges(Detection& detection);
  bool blocksDopplerRidgeBirth(double dopplerHz) const;

public:
  static uint64_t initiationMultiplicity(double cpi, double maximumAcceleration,
    bool adaptive);

  /// @brief Constructor.
  /// @param m Track initiation constant for M of N detections.
  /// @param n Track initiation constant for M of N detections.
  /// @param nDelete Number of missed predictions to delete a tentative track.
  /// @param cpi True CPI time for acceleration resolution(s).
  /// @param maxAccInit Maximum acceleration to initiate track (Hz/s).
  /// @param rangeRes Range resolution for kinematics equations (m).
  /// @param lambda Wavelength for kinematics equations (m).
  /// @return The object.
  Tracker(uint32_t m, uint32_t n, uint32_t nDelete, double cpi,
    double maxAccInit, double rangeRes, double lambda,
    double minInitiationSnr = -100, bool adaptiveGate = false,
    DopplerRidgeConfig dopplerRidge = {},
    bool normalizedInnovationGate = true,
    double associationMaximumNis = 9.210340371976184,
    BirthExclusivityConfig birthExclusivity = {}, TrackerRecoveryConfig recovery = {});

  /// @brief Destructor.
  /// @return Void.
  ~Tracker();

  // Clear temporal state without changing the algorithm's configuration.
  void reset(bool lineageDiscontinuity = false);

  const TrackerFrameDiagnostics& frame_diagnostics() const { return diagnostics; }

  /// @brief Run through key functions of tracker.
  /// @param detection Detection data for last CPI.
  /// @param timestamp POSIX timestamp (ms).
  /// @return Pointer to track data.
  std::unique_ptr<Track> process(Detection *detection, uint64_t timestamp,
    const std::vector<TrackerGuidedCandidate>& guided = {});
  void configureRecovery(TrackerRecoveryConfig config);
  std::vector<TrackerGuidedPrediction> guidedPredictions(uint64_t timestamp);

  /// Credit disjoint temporal evidence to existing ACTIVE/COASTING tracks.
  /// Returns the number of coast lifetimes reset. The current measurement,
  /// state history, association count, acceleration and promotion window are
  /// never modified.
  uint64_t applyWeakSupport(const std::vector<TrackerWeakSupportCandidate>& candidates);

  /// @brief Update tracks by associating detections.
  /// @param detection Detection data for last CPI.
  /// @param timestamp POSIX timestamp (ms).
  /// @return Void.
  void update(Detection *detection, uint64_t timestamp);

  /// @brief Predict next bistatic position using kinematics equations.
  /// @param current Current position of track.
  /// @param acc Acceleration hypothesis of track.
  /// @param T Time elapsed from previous CPI.
  /// @return Predicted position of track.
  Detection predict(Detection current, double acc, double T);

  /// @brief Initiate new tentative tracks from detections.
  /// @param detection Detection data for last CPI.
  /// @return Void.
  void initiate(Detection *detection);
};

#endif
