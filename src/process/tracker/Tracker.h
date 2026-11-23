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
#include "ConfirmationContract.h"
#include "AssociationContract.h"

#include <stdint.h>
#include <memory>
#include <array>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <deque>
#include <map>

struct TrackerRecoveryConfig {
  bool stationaryClutter = false;
  uint32_t persistenceCpis = 60;
  double maximumDopplerHz = 0; // zero covers the full configured Doppler map
  uint32_t maximumStationaryCells = 65536;
};

struct DopplerRidgeConfig
{
  bool enabled = false;
  uint32_t persistenceCpis = 12;
  double toleranceHz = 3.0;
  double minimumDelaySeparationKm = .25;
};

// Offline replay policy. Existing tracks always associate against the
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
  uint64_t pairInnovationRejected=0, pairSpatialRejected=0;
  uint64_t associations=0, misses=0, promotions=0, tentativeExpired=0, inactivityDeleted=0;
  uint64_t birthCandidates=0, birthSnrRejected=0, birthRidgeRejected=0,
    birthExclusiveRejected=0, birthDopplerCombRejected=0, birthRangeCombRejected=0, initiated=0;
  uint64_t lineageResets=0, pairLineageRejected=0, birthLineageRejected=0,
    creditedCpis=0, independentCredits=0, birthCapacityRejected=0;
  uint64_t stationaryBirthRejected=0, stationaryQuarantined=0,
    stationaryCells=0, stationaryCapacityRejected=0, stationaryBirthCapacityRejected=0;
};

// One field order supplies JSON, the resident ABI and device counter storage.
// Entries are append-only within an ABI; bool noOp is encoded separately.
struct TrackerDiagnosticField {const char* name; const char* memberName;
  uint64_t TrackerFrameDiagnostics::*member;};
#define TRACKER_DIAGNOSTIC(member, name) {name, #member, &TrackerFrameDiagnostics::member}
inline constexpr TrackerDiagnosticField trackerDiagnosticFields[]={
  TRACKER_DIAGNOSTIC(detections,"detections"),
  TRACKER_DIAGNOSTIC(tracksBefore,"tracks_before"),
  TRACKER_DIAGNOSTIC(tracksAfter,"tracks_after"),
  TRACKER_DIAGNOSTIC(pairs,"candidate_track_pairs"),
  TRACKER_DIAGNOSTIC(pairClaimed,"pair_already_claimed"),
  TRACKER_DIAGNOSTIC(pairDelayRejected,"pair_delay_rejected"),
  TRACKER_DIAGNOSTIC(pairDopplerRejected,"pair_doppler_rejected"),
  TRACKER_DIAGNOSTIC(pairInnovationRejected,"pair_innovation_rejected"),
  TRACKER_DIAGNOSTIC(pairSpatialRejected,"pair_spatial_rejected"),
  TRACKER_DIAGNOSTIC(pairInGate,"pair_in_gate"),
  TRACKER_DIAGNOSTIC(pairLineageRejected,"pair_lineage_rejected"),
  TRACKER_DIAGNOSTIC(associations,"associations"),
  TRACKER_DIAGNOSTIC(misses,"misses"),
  TRACKER_DIAGNOSTIC(promotions,"promotions"),
  TRACKER_DIAGNOSTIC(tentativeExpired,"tentative_expired"),
  TRACKER_DIAGNOSTIC(inactivityDeleted,"inactivity_deleted"),
  TRACKER_DIAGNOSTIC(birthCandidates,"birth_candidates"),
  TRACKER_DIAGNOSTIC(birthSnrRejected,"birth_snr_rejected"),
  TRACKER_DIAGNOSTIC(birthRidgeRejected,"birth_ridge_rejected"),
  TRACKER_DIAGNOSTIC(birthDopplerCombRejected,"birth_doppler_comb_rejected"),
  TRACKER_DIAGNOSTIC(birthRangeCombRejected,"birth_range_comb_rejected"),
  TRACKER_DIAGNOSTIC(initiated,"initiated"),
  TRACKER_DIAGNOSTIC(lineageResets,"lineage_resets"),
  TRACKER_DIAGNOSTIC(birthLineageRejected,"birth_lineage_rejected"),
  TRACKER_DIAGNOSTIC(birthCapacityRejected,"birth_capacity_rejected"),
  TRACKER_DIAGNOSTIC(independentCredits,"independent_credits"),
  TRACKER_DIAGNOSTIC(creditedCpis,"credited_source_cpis"),
  TRACKER_DIAGNOSTIC(stationaryBirthRejected,"stationary_birth_rejected"),
  TRACKER_DIAGNOSTIC(stationaryBirthCapacityRejected,"stationary_birth_capacity_rejected"),
  TRACKER_DIAGNOSTIC(stationaryQuarantined,"stationary_quarantined"),
  TRACKER_DIAGNOSTIC(stationaryCells,"stationary_cells"),
  TRACKER_DIAGNOSTIC(stationaryCapacityRejected,"stationary_cell_capacity_rejected"),
  TRACKER_DIAGNOSTIC(birthExclusiveRejected,"birth_exclusive_rejected")
};
#undef TRACKER_DIAGNOSTIC
inline constexpr size_t trackerDiagnosticCount=sizeof(trackerDiagnosticFields)/sizeof(*trackerDiagnosticFields);


// Startup/replay oracle view only. No resident checkpoint is fed back into
// inference and no evaluation truth is present.
struct TrackerQualificationState {
  std::string id;
  uint32_t state=0,spatial=0,source=0;
  double delay=0,doppler=0,acceleration=0,missedSeconds=0;
  uint64_t inactive=0,measurements=0,updates=0,epoch=0,highWaterSequence=0;
  bool accelerationEstimated=false,everConfirmed=false,lineageKnown=false;
  std::vector<uint8_t> confirmationVotes;
  Detection::Evidence measurementEvidence{};
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
  uint32_t maximumEvidenceDepth=1;
  uint32_t spatialBeams=0, spatialFineFactor=1, spatialTolerance=UINT32_MAX;
  // The latest spatial observation is retained across prediction/coast,
  // exactly as resident history carries its last spatial word forward.
  std::unordered_map<std::string,uint32_t> trackSpatial;
  struct QuarantineSample {double delay,doppler;bool associated;};
  std::unordered_map<std::string,std::deque<QuarantineSample>> quarantineHistory;
  std::unordered_set<std::string> previouslyConfirmed;

  /// NIS radius squared for the normalized gate. 4/pi gives the same area as
  /// the legacy +/-1-bin rectangle when covariance is the identity.
  double associationMaximumNis;

  BirthExclusivityConfig birthExclusivity;

  /// @brief Index of detections already updated.
  std::vector<bool> doNotInitiate;
  std::vector<tracker_policy::Candidate> policyCandidates,policyPredictions;
  std::deque<int> nativeDelay;
  std::deque<double> nativeDoppler;
  double dopplerRow(double value) const;
  bool stationaryKey(double delay,double doppler,std::pair<int64_t,int64_t>& key) const;

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
  bool frameHasEvidence = false;
  bool validEvidence(const Detection::Evidence&, bool requireCurrentEpoch = true) const;
  bool freshEvidence(const std::string&, const Detection::Evidence&) const;
  bool creditEvidence(const std::string&, const Detection::Evidence*);
  void appendVote(uint64_t index, bool credit);
  void promote(uint64_t index, bool credit);
  bool stationary(double delay, double doppler) const;
  bool quarantine(uint64_t index,double delay,double doppler) const;
  bool stationaryCapacityBlocked(double delay, double doppler) const;
  void pruneStationary();
  void learnStationary(Detection& detection);
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
  std::unique_ptr<Track> process(Detection *detection, uint64_t timestamp);
  void configureRecovery(TrackerRecoveryConfig config);
  void configureMaximumEvidenceDepth(uint32_t depth);
  void configureGrid(const std::deque<int>& delay,const std::deque<double>& doppler);
  void configureSpatialAssociation(uint32_t beams,uint32_t fineFactor,uint32_t tolerance);
  uint32_t spatialForTrack(const std::string& id) const {
    const auto found=trackSpatial.find(id);return found==trackSpatial.end()?0u:found->second;
  }
  std::vector<TrackerQualificationState> qualificationState();


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
