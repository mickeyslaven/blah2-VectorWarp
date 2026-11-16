#pragma once

#include "data/Detection.h"
#include "data/Track.h"
#include "process/detection/CfarDetector1D.h"
#include "process/detection/IntegratedCfar.h"
#include "process/integration/TemporalEvidence.h"
#include "process/tracker/Tracker.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <vector>

namespace blah2::integration {

// Read-only challenger that feeds the existing tracker from the existing
// temporal banks.  Each depth owns an independent tracker and consumes only
// disjoint windows.  This avoids treating overlapping 2/4/8-CPI windows as
// independent confirmation while preserving the production tracker math.
class TemporalShadowTracker {
public:
  struct Config {
    bool enabled = false;
    double pfa = 1e-5;
    int8_t guardCells = 2;
    int8_t trainingCells = 12;
    int32_t minimumDelay = 0;
    double minimumDopplerHz = 0;
    uint32_t maximumCandidates = 256;
    uint32_t maximumTracks = 512;
    uint64_t maximumAssociationPairs = 65536;
    uint32_t confirmationM = 2;
    uint32_t confirmationN = 3;
    uint32_t deleteAfter = 10;
    double cpiSeconds = .5;
    double maximumAccelerationHzPerSecond = 10;
    double rangeResolutionM = 1;
    double wavelengthM = 1;
    double minimumInitiationSnrDb = -100;
    // Inherit the production tracker threshold unless an experiment
    // explicitly requests a different shadow-only value.
    bool minimumInitiationSnrOverridden = false;
    bool adaptiveGate = true;
    uint32_t maximumCpis = 8;
    uint32_t delayMotionBins = 2;
    uint32_t dopplerMotionBins = 4;
  };

  struct Candidate {
    double delay = 0, dopplerHz = 0, snrDb = 0;
    EvidenceLineage lineage;
  };

  struct LaneStatus {
    uint32_t depth = 0;
    bool ready = false, evaluated = false;
    uint64_t evaluatedWindows = 0, skippedOverlappingWindows = 0;
    uint64_t candidates = 0, truncatedCandidates = 0;
    uint64_t cumulativeAssociations = 0, cumulativePromotions = 0,
      cumulativeMisses = 0;
    uint64_t latestEvaluatedSequence = 0;
    uint64_t tracks = 0, activeTracks = 0, coastingTracks = 0;
    TrackerFrameDiagnostics tracker;
    std::vector<Candidate> latestCandidates;
  };

  struct Status {
    bool enabled = false;
    uint64_t epoch = 0, sequence = 0;
    std::array<LaneStatus, 3> lanes{};
  };

  explicit TemporalShadowTracker(Config config) : config_(config) {
    validate(config_);
    status_.enabled = config_.enabled;
    for (std::size_t lane = 0; lane < lanes_.size(); ++lane) {
      const uint32_t depth = uint32_t(2u << lane);
      status_.lanes[lane].depth = depth;
      if (!config_.enabled || depth > config_.maximumCpis) continue;
      const double familyPfa = config_.pfa /
        detection::integrated_cfar::motionFamilyTrials(config_.maximumCpis,
          config_.delayMotionBins, config_.dopplerMotionBins);
      lanes_[lane].detector = std::make_unique<CfarDetector1D>(familyPfa,
        config_.guardCells, config_.trainingCells, config_.minimumDelay,
        config_.minimumDopplerHz, depth);
      lanes_[lane].tracker = std::make_unique<Tracker>(config_.confirmationM,
        config_.confirmationN, config_.deleteAfter,
        // Tracker's CPI parameter is the Doppler-bin resolution of the source
        // CAF. Disjoint temporal windows change update cadence, not the CAF
        // Doppler grid; elapsed kinematics already use the supplied time.
        config_.cpiSeconds,
        config_.maximumAccelerationHzPerSecond, config_.rangeResolutionM,
        config_.wavelengthM, config_.minimumInitiationSnrDb,
        config_.adaptiveGate);
    }
  }

  static void validate(const Config& config) {
    if (!std::isfinite(config.pfa) || !(config.pfa > 0 && config.pfa < 1) ||
        config.guardCells < 0 || config.trainingCells <= 0 ||
        !std::isfinite(config.minimumDopplerHz) || config.minimumDopplerHz < 0 ||
        !config.maximumCandidates || config.maximumCandidates > 4096 ||
        !config.maximumTracks || config.maximumTracks > 4096 ||
        !config.maximumAssociationPairs || config.maximumAssociationPairs > 1048576 ||
        !config.confirmationM || config.confirmationM > config.confirmationN ||
        config.confirmationN > 255 || !config.deleteAfter ||
        !std::isfinite(config.cpiSeconds) || config.cpiSeconds <= 0 ||
        !std::isfinite(config.maximumAccelerationHzPerSecond) ||
        config.maximumAccelerationHzPerSecond < 0 ||
        !std::isfinite(config.rangeResolutionM) || config.rangeResolutionM <= 0 ||
        !std::isfinite(config.wavelengthM) || config.wavelengthM <= 0 ||
        !std::isfinite(config.minimumInitiationSnrDb) ||
        config.minimumInitiationSnrDb < -100 || config.minimumInitiationSnrDb > 100 ||
        (config.maximumCpis != 4 && config.maximumCpis != 8) ||
        config.delayMotionBins > 2 || config.dopplerMotionBins > 16)
      throw std::invalid_argument("Temporal shadow tracker configuration invalid");
  }

  void reset() {
    // A capture/pacing gap invalidates every temporal and tracker state, but
    // experiment counters are a process-lifetime ledger.  Erasing them here
    // makes a periodically reset lane look artificially idle and prevents a
    // defensible false-alarm/trial denominator.
    rebuildLanes();
    epoch_.reset();
    status_.epoch = 0;
    status_.sequence = 0;
  }

  Status process(const TemporalEvidence::Result& evidence, uint64_t timestampMs,
    const EvidenceWorkBudget* budget = nullptr) {
    status_.epoch = evidence.epoch;
    status_.sequence = evidence.sequence;
    if (!config_.enabled) return status_;
    if (!epoch_ || *epoch_ != evidence.epoch) {
      rebuildLanes();
      epoch_ = evidence.epoch;
    }
    for (std::size_t laneIndex = 0; laneIndex < lanes_.size(); ++laneIndex) {
      if (budget) budget->check();
      auto& lane = lanes_[laneIndex];
      auto& status = status_.lanes[laneIndex];
      const auto& bank = evidence.banks[laneIndex + 1]; // motion 2/4/8
      status.ready = bank.ready;
      status.evaluated = false;
      status.latestCandidates.clear();
      if (!bank.ready) continue;
      if (!lane.nextSequence) lane.nextSequence = bank.window.sequences[bank.depth - 1];
      if (evidence.sequence < *lane.nextSequence + bank.depth - 1) {
        ++status.skippedOverlappingWindows;
        continue;
      }
      if (evidence.sequence != *lane.nextSequence + bank.depth - 1)
        throw std::invalid_argument("Temporal shadow tracker sequence gap requires reset");
      lane.nextSequence = evidence.sequence + 1;
      uint64_t truncated = 0;
      auto detections = lane.detector->process(bank.map.get(), nullptr, budget,
        config_.maximumCandidates, &truncated);
      auto candidates = candidatesFrom(*detections, bank, budget);
      status.candidates += candidates.size() + truncated;
      status.truncatedCandidates += truncated;
      std::vector<double> delay, doppler, snr;
      delay.reserve(candidates.size()); doppler.reserve(candidates.size());
      snr.reserve(candidates.size());
      for (const auto& candidate : candidates) {
        delay.push_back(candidate.delay); doppler.push_back(candidate.dopplerHz);
        snr.push_back(candidate.snrDb);
      }
      Detection bounded(std::move(delay), std::move(doppler), std::move(snr));
      if (budget) budget->check();
      const uint64_t priorTracks = status.tracks;
      const uint64_t birthMultiplicity = Tracker::initiationMultiplicity(
        config_.cpiSeconds, config_.maximumAccelerationHzPerSecond,
        config_.adaptiveGate);
      const uint64_t maximumNewTracks = birthMultiplicity * candidates.size();
      if (priorTracks + maximumNewTracks > config_.maximumTracks ||
          (candidates.size() && priorTracks >
            config_.maximumAssociationPairs / candidates.size()))
        throw std::runtime_error("Temporal shadow tracker work bound exceeded");
      auto tracks = lane.tracker->process(&bounded, timestampMs);
      if (budget) budget->check();
      status.evaluated = true;
      ++status.evaluatedWindows;
      status.latestCandidates = std::move(candidates);
      status.tracker = lane.tracker->frame_diagnostics();
      status.latestEvaluatedSequence = evidence.sequence;
      status.cumulativeAssociations += status.tracker.associations;
      status.cumulativePromotions += status.tracker.promotions;
      status.cumulativeMisses += status.tracker.misses;
      status.tracks = tracks->get_n();
      status.activeTracks = tracks->get_nState("ACTIVE");
      status.coastingTracks = tracks->get_nState("COASTING");
    }
    return status_;
  }

  const Status& status() const { return status_; }

private:
  struct Lane {
    std::unique_ptr<CfarDetector1D> detector;
    std::unique_ptr<Tracker> tracker;
    std::optional<uint64_t> nextSequence;
  };

  std::vector<Candidate> candidatesFrom(Detection& detections,
    const TemporalEvidence::BankResult& bank,
    const EvidenceWorkBudget* budget) const {
    const auto delays = detections.get_delay();
    const auto dopplers = detections.get_doppler();
    const auto snrs = detections.get_snr();
    std::vector<Candidate> result;
    result.reserve(delays.size());
    for (std::size_t index = 0; index < delays.size(); ++index) {
      if (budget && (index & 31u) == 0) budget->check();
      const auto column = std::lower_bound(bank.map->delay.begin(),
        bank.map->delay.end(), int(std::llround(delays[index])));
      const auto row = std::lower_bound(bank.map->doppler.begin(),
        bank.map->doppler.end(), dopplers[index]);
      if (column == bank.map->delay.end() || row == bank.map->doppler.end() ||
          *column != int(std::llround(delays[index])) || *row != dopplers[index])
        throw std::logic_error("Temporal shadow detection is outside its source bank");
      const uint64_t cell = uint64_t(row - bank.map->doppler.begin()) *
        bank.map->delay.size() + uint64_t(column - bank.map->delay.begin());
      auto lineage = bank.lineageAt(cell);
      if (lineage.depth != bank.depth)
        throw std::logic_error("Temporal shadow candidate lost CPI lineage");
      result.push_back({delays[index], dopplers[index], snrs[index], lineage});
    }
    return result;
  }

  void rebuildLanes() {
    const auto preserved = status_;
    TemporalShadowTracker fresh(config_);
    lanes_ = std::move(fresh.lanes_);
    status_ = preserved;
    for (std::size_t lane = 0; lane < status_.lanes.size(); ++lane) {
      status_.lanes[lane].ready = false;
      status_.lanes[lane].evaluated = false;
      status_.lanes[lane].latestCandidates.clear();
      status_.lanes[lane].latestEvaluatedSequence = 0;
      status_.lanes[lane].tracker = {};
      status_.lanes[lane].tracks = status_.lanes[lane].activeTracks =
        status_.lanes[lane].coastingTracks = 0;
    }
  }

  Config config_;
  std::array<Lane, 3> lanes_{};
  std::optional<uint64_t> epoch_;
  Status status_;
};

} // namespace blah2::integration
