#pragma once

#include "process/detection/PostClutterBootstrap.h"
#include "process/integration/TemporalEvidence.h"
#include "process/integration/TemporalShadowTracker.h"
#include "process/integration/LiveWeakEvidenceArtifact.h"

#include <condition_variable>
#include <array>
#include <atomic>
#include <mutex>
#include <optional>
#include <thread>

namespace blah2::integration {

// One observation-only worker, one preallocated input slot, no waiting queue.
// The radar thread only tries to copy already-produced complex CAFs. It never
// waits for worker completion or consumes these scores for detection/tracking.
class LiveTemporalShadow {
public:
  using Complex = std::complex<double>;
  using ComplexMap = Map<Complex>;
  enum class DropReason : std::size_t {
    lockContention, workerBusy, minimumInterval, invalidMetadata,
    invalidCopy, copyOverrun, copyException, none
  };
  inline static constexpr std::array<const char*, std::size_t(DropReason::none)>
    dropReasonNames{{"lock_contention", "worker_busy", "minimum_interval",
      "invalid_metadata", "invalid_copy", "copy_overrun", "copy_exception"}};
  struct DropCounts {
    std::array<uint64_t, std::size_t(DropReason::none)> counts{};
    uint64_t operator[](DropReason reason) const {
      return counts[std::size_t(reason)];
    }
    uint64_t total() const noexcept {
      uint64_t result = 0; for (const auto count : counts) result += count;
      return result;
    }
  };
  class DropLedger {
  public:
    DropLedger() { for (auto& count : counts_) count.store(0); }
    void record(DropReason reason) noexcept {
      if (reason != DropReason::none)
        counts_[std::size_t(reason)].fetch_add(1, std::memory_order_relaxed);
    }
    DropCounts snapshot() const noexcept {
      DropCounts result;
      for (std::size_t index = 0; index < counts_.size(); ++index)
        result.counts[index] = counts_[index].load(std::memory_order_relaxed);
      return result;
    }
  private:
    std::array<std::atomic<uint64_t>, std::size_t(DropReason::none)> counts_;
  };
  static DropReason admissionDropReason(bool lockAcquired, bool workerBusy,
    bool minimumIntervalPending, bool validMetadata) noexcept {
    if (!lockAcquired) return DropReason::lockContention;
    if (workerBusy) return DropReason::workerBusy;
    if (minimumIntervalPending) return DropReason::minimumInterval;
    return validMetadata ? DropReason::none : DropReason::invalidMetadata;
  }
  struct Config {
    bool enabled = false;
    bool associationSustain = false;
    // Select one disjoint bank for production survival credit. Other banks
    // remain shadow-only so overlapping depths cannot double-count CPIs.
    uint32_t associationSustainCpis = 4;
    detection::PostClutterBootstrap::Config bootstrap;
    TemporalEvidence::Config temporal;
    TemporalShadowTracker::Config scoring;
    uint32_t maximumCells = 524288;
    uint32_t maximumWorkMs = 20;
    uint32_t minimumIntervalMs = 100;
    std::string artifactPath;
    uint64_t artifactMaximumBytes = 512ULL << 20;
    uint32_t artifactMaximumFrames = 16;
    // Exact CAF collection only; no covariance/temporal CPU computation. This
    // is the bounded qualification path for production-sized weak evidence.
    bool artifactOnly = false;
  };
  struct Frame {
    uint64_t captureEpoch = 0, sequence = 0;
    uint64_t firstSample = 0, endSample = 0; // Exact half-open capture interval.
    uint64_t tuningHz = 0, configurationEpoch = 0;
    uint32_t sampleRateHz = 0;
    bool samplePositionAvailable = false;
    uint64_t timestampMs = 0;
    uint32_t mapSampleRateHz = 0; // CAF delay-bin rate, possibly decimated.
  };
  struct Evidence {
    Frame frame;
    uint64_t generation = 0;
    detection::PostClutterBootstrap::Result bootstrap;
    TemporalEvidence::Result temporal;
    std::optional<TemporalShadowTracker::Status> scoring;
  };
  struct ScoringLaneStatus {
    uint32_t depth = 0;
    bool ready = false, evaluated = false;
    uint64_t evaluatedWindows = 0, skippedOverlappingWindows = 0;
    uint64_t candidates = 0, truncatedCandidates = 0;
    uint64_t cumulativeAssociations = 0, cumulativePromotions = 0,
      cumulativeMisses = 0;
    uint64_t latestEvaluatedSequence = 0;
    uint64_t tracks = 0, activeTracks = 0, coastingTracks = 0;
    uint32_t latestCandidateCount = 0, candidateExportLimit = 0;
    uint32_t confirmationM = 0, confirmationN = 0, deleteAfter = 0;
    double baseCpiSeconds = 0, updateIntervalSeconds = 0;
    TrackerFrameDiagnostics tracker;
    // Bounded full-precision candidates from the most recently evaluated
    // disjoint window.  These are diagnostics for paired recovery scoring;
    // they never feed the production detector or tracker.
    std::vector<TemporalShadowTracker::Candidate> latestCandidates;
  };
  struct Status {
    bool enabled = false, disabled = false, current = false, busy = false;
    bool snapshotAvailable = false, resultAvailable = false;
    // Lightweight gauges/candidates remain observable after the production
    // consumer takes ownership of the full map payload.
    bool monitorResultAvailable = false;
    uint64_t accepted = 0, dropped = 0, rejected = 0, resets = 0;
    // Monotonic process-local identity for every semantic reset, including a
    // statistic-family change that does not change the capture generation.
    uint64_t resetSerial = 0;
    std::string lastResetReason;
    uint64_t budgetExhaustions = 0;
    DropCounts dropReasons;
    uint64_t generation = 0;
    uint64_t residentBytesBound = 0;
    double copyMs = 0, maximumCopyMs = 0;
    uint64_t copyOverruns = 0;
    uint32_t copyOverrunStreak = 0;
    std::string disableReason;
    uint32_t sourceRows = 0, sourceColumns = 0;
    uint32_t observedRows = 0, observedColumns = 0, observedColumnOffset = 0;
    Frame frame;
    uint32_t availableCpis = 0;
    std::array<bool, 4> bankReady{};
    uint64_t trainedCells = 0, fallbackCells = 0, invalidCells = 0;
    double workMs = 0, peakPower = 0;
    bool scoringEnabled = false;
    std::array<ScoringLaneStatus, 3> scoring{};
    bool artifactEnabled = false, artifactDisabled = false, artifactComplete = false;
    uint64_t artifactFrames = 0, artifactBytes = 0;
    double artifactWriteMs = 0;
    Frame artifactLastFrame;
  };
  struct CopyPolicyDecision {
    bool admit = false, latch = false, hardOverrun = false;
    uint32_t nextStreak = 0;
  };

  static CopyPolicyDecision copyPolicy(double milliseconds,
    uint32_t priorStreak) noexcept {
    if (!std::isfinite(milliseconds) || milliseconds > 10.0)
      return {false, true, true, priorStreak + 1};
    if (milliseconds > 2.0) {
      const uint32_t streak = priorStreak + 1;
      return {false, streak >= 3, false, streak};
    }
    return {true, false, false, 0};
  }

  static void validate(const Config& config) {
    detection::PostClutterBootstrap bootstrap(config.bootstrap);
    TemporalEvidence temporal(config.temporal);
    TemporalShadowTracker::validate(config.scoring);
    LiveWeakEvidenceArtifact::validate({config.artifactPath,
      config.artifactMaximumBytes, config.artifactMaximumFrames});
    if (config.artifactOnly && (!config.enabled || config.artifactPath.empty()))
      throw std::invalid_argument("Live weak-evidence capture-only mode requires enabled artifact output");
    if (!config.maximumCells || config.maximumCells > 524288 ||
        !config.maximumWorkMs || config.maximumWorkMs > 150 ||
        config.minimumIntervalMs < 2 * config.maximumWorkMs ||
        config.minimumIntervalMs > 10000)
      throw std::invalid_argument("Live temporal shadow bounds are invalid");
    if (config.scoring.enabled &&
        (config.scoring.maximumCpis != config.temporal.maximumCpis ||
         config.scoring.delayMotionBins != config.temporal.delayMotionBins ||
         config.scoring.dopplerMotionBins != config.temporal.dopplerMotionBins))
      throw std::invalid_argument(
        "Live temporal scoring must use the exact temporal motion family");
    if (config.associationSustain &&
        (!config.enabled || !config.scoring.enabled ||
         (config.associationSustainCpis != 2 &&
          config.associationSustainCpis != 4 &&
          config.associationSustainCpis != 8) ||
         config.temporal.maximumCpis < config.associationSustainCpis))
      throw std::invalid_argument(
        "Temporal association sustain requires one enabled 2/4/8-CPI scoring bank");
  }

  LiveTemporalShadow(Config config, const ComplexMap& geometry,
    std::vector<uint32_t> channelIds)
    : config_(config), bootstrap_(config.bootstrap), temporal_(config.temporal),
      channelIds_(std::move(channelIds)) {
    validate(config_);
    sourceRows_ = uint32_t(geometry.data.size());
    sourceColumns_ = sourceRows_ ? uint32_t(geometry.data.front().size()) : 0;
    if (!sourceRows_ || !sourceColumns_ || sourceRows_ > 4096 ||
        sourceColumns_ > 4096 || geometry.delay.size() != sourceColumns_ ||
        geometry.doppler.size() != sourceRows_ ||
        !std::is_sorted(geometry.delay.begin(), geometry.delay.end()) ||
        std::any_of(geometry.doppler.begin(), geometry.doppler.end(),
          [](double value) { return !std::isfinite(value); }) ||
        std::any_of(geometry.data.begin(), geometry.data.end(),
          [&](const auto& row) { return row.size() != sourceColumns_; }))
      throw std::invalid_argument("Live temporal shadow source geometry is invalid");
    rows_ = sourceRows_;
    columns_ = rows_ ? std::min<uint32_t>(sourceColumns_,
      config_.maximumCells / rows_) : 0;
    if (columns_ != sourceColumns_ && !config_.artifactPath.empty())
      throw std::invalid_argument(
        "Live weak-evidence artifacts require complete source geometry");
    // Preserve every Doppler hypothesis and the lowest-delay contiguous ROI.
    // This bounds diagnostic work without quantizing or changing production
    // CAFs. Passive-radar delay axes are monotonic, with the small negative
    // clutter guard first and operational positive delays following it.
    if (columns_) {
      delay_.assign(geometry.delay.begin(), geometry.delay.begin() + columns_);
      doppler_ = geometry.doppler;
    }
    auto ids = channelIds_;
    std::sort(ids.begin(), ids.end());
    if (!rows_ || !columns_ || channelIds_.empty() || channelIds_.size() > 8 ||
        std::adjacent_find(ids.begin(), ids.end()) != ids.end() ||
        delay_.size() != columns_ || doppler_.size() != rows_ ||
        !sameSourceGeometry(geometry))
      throw std::invalid_argument("Live temporal shadow geometry is invalid or exceeds its cap");
    // Conservative bound: copied CAFs, eight power histories, old/new result
    // banks, masks, axes, row allocations and small covariance workspaces.
    status_.residentBytesBound = uint64_t(rows_) * columns_ *
      ((config_.artifactOnly ? 24 : 16) * channelIds_.size() + 384) +
      1024ULL * (rows_ + columns_) + (2ULL << 20);
    if (status_.residentBytesBound > (256ULL << 20))
      throw std::invalid_argument("Live temporal shadow memory cap exceeded");
    status_.enabled = config_.enabled;
    status_.scoringEnabled = config_.scoring.enabled;
    for (std::size_t lane = 0; lane < status_.scoring.size(); ++lane) {
      status_.scoring[lane].depth = uint32_t(2u << lane);
      status_.scoring[lane].candidateExportLimit =
        std::min<uint32_t>(config_.scoring.maximumCandidates, 256);
      status_.scoring[lane].confirmationM = config_.scoring.confirmationM;
      status_.scoring[lane].confirmationN = config_.scoring.confirmationN;
      status_.scoring[lane].deleteAfter = config_.scoring.deleteAfter;
      status_.scoring[lane].baseCpiSeconds = config_.scoring.cpiSeconds;
      status_.scoring[lane].updateIntervalSeconds =
        config_.scoring.cpiSeconds * double(2u << lane);
    }
    status_.sourceRows = sourceRows_; status_.sourceColumns = sourceColumns_;
    status_.observedRows = rows_; status_.observedColumns = columns_;
    status_.observedColumnOffset = columnOffset_;
    status_.artifactEnabled = !config_.artifactPath.empty();
    if (!config_.enabled) return;
    if (config_.scoring.enabled)
      scorer_ = std::make_unique<TemporalShadowTracker>(config_.scoring);
    for (std::size_t channel = 0; channel < channelIds_.size(); ++channel) {
      maps_.push_back(std::make_unique<ComplexMap>(rows_, columns_));
      maps_.back()->delay = delay_; maps_.back()->doppler = doppler_;
      views_.push_back(maps_.back().get());
    }
    if (config_.artifactOnly)
      residentRaw_.resize(std::size_t(rows_) * columns_ * channelIds_.size());
    worker_ = std::thread([this] { run(); });
  }

  ~LiveTemporalShadow() {
    { std::lock_guard<std::mutex> lock(mutex_); stop_.store(true); }
    wake_.notify_one();
    if (worker_.joinable()) worker_.join();
  }
  LiveTemporalShadow(const LiveTemporalShadow&) = delete;
  LiveTemporalShadow& operator=(const LiveTemporalShadow&) = delete;

  bool offerCpu(const Frame& frame, const std::vector<ComplexMap*>& maps) noexcept {
    return offer(frame, [&](const auto hardDeadline) {
      if (maps.size() != maps_.size()) return CopyResult::invalid;
      for (std::size_t channel = 0; channel < maps.size(); ++channel) {
        if (!maps[channel] || !sameSourceGeometry(*maps[channel]))
          return CopyResult::invalid;
        for (uint32_t row = 0; row < rows_; ++row) {
          std::copy_n(maps[channel]->data[row].begin() + columnOffset_, columns_,
            maps_[channel]->data[row].begin());
          if (std::chrono::steady_clock::now() > hardDeadline)
            return CopyResult::hardOverrun;
        }
      }
      pendingResident_ = false;
      return CopyResult::success;
    });
  }

  // Native resident CAF layout: channel, delay, unshifted Doppler. This is the
  // identical index transform used by Coordinator::checkMaps; float input is
  // widened exactly, never reconstructed from quantized display dB.
  bool offerResident(const Frame& frame, const std::complex<float>* maps,
    std::size_t count) noexcept {
    return offer(frame, [&](const auto hardDeadline) {
      const std::size_t cells = std::size_t(rows_) * columns_;
      const std::size_t sourceCells = std::size_t(sourceRows_) * sourceColumns_;
      if (!maps || count != sourceCells * maps_.size()) return CopyResult::invalid;
      if (config_.artifactOnly) {
        for (std::size_t channel = 0; channel < maps_.size(); ++channel)
          for (uint32_t column = 0; column < columns_; ++column) {
            std::copy_n(maps + channel * sourceCells +
              std::size_t(column + columnOffset_) * sourceRows_, rows_,
              residentRaw_.begin() + channel * cells +
                std::size_t(column) * rows_);
            if (std::chrono::steady_clock::now() > hardDeadline)
              return CopyResult::hardOverrun;
          }
        pendingResident_ = true;
        return CopyResult::success;
      }
      pendingResident_ = false;
      for (std::size_t channel = 0; channel < maps_.size(); ++channel)
        for (uint32_t row = 0; row < rows_; ++row) {
          for (uint32_t column = 0; column < columns_; ++column)
            maps_[channel]->data[row][column] = maps[channel * sourceCells +
              std::size_t(column + columnOffset_) * sourceRows_ +
              (row + rows_ / 2 + 1) % rows_];
          if (std::chrono::steady_clock::now() > hardDeadline)
            return CopyResult::hardOverrun;
        }
      return CopyResult::success;
    });
  }

  // Capture reset, invalid input, pressure or overload-backoff drops invalidate
  // old products immediately, including a worker completion still in flight.
  void invalidate() noexcept { generation_.fetch_add(1); }

  Status status() const {
    Status result;
    std::unique_lock<std::mutex> lock(mutex_, std::try_to_lock);
    if (lock.owns_lock()) {
      result = status_;
      result.snapshotAvailable = true;
      result.busy = busy_;
      const auto generation = generation_.load();
      result.generation = generation;
      result.resultAvailable = latest_ && latest_->generation == generation;
      result.monitorResultAvailable = statusGeneration_ == generation &&
        result.frame.sequence != 0;
      result.current = !busy_ && result.monitorResultAvailable;
      if (statusGeneration_ != generation) clearEvidenceGauges(result);
    } else result.busy = true;
    result.enabled = config_.enabled;
    result.disabled = disabled_.load();
    result.dropReasons = dropLedger_.snapshot();
    result.dropped = result.dropReasons.total();
    return result;
  }

  // Backend/replay inspection only. Ownership leaves the single output slot;
  // the monitor/API receives only Status, never these full-precision arrays.
  std::unique_ptr<Evidence> takeLatest() {
    std::unique_lock<std::mutex> lock(mutex_, std::try_to_lock);
    if (!lock.owns_lock() || busy_ || !latest_ || latest_->generation != generation_.load())
      return {};
    return std::move(latest_);
  }

private:
  enum class CopyResult { success, invalid, hardOverrun };
  bool recordDrop(DropReason reason) noexcept {
    dropLedger_.record(reason); invalidate(); return false;
  }
  static void clearEvidenceGauges(Status& status) {
    status.frame = {}; status.availableCpis = 0; status.bankReady = {};
    status.trainedCells = status.fallbackCells = status.invalidCells = 0;
    status.peakPower = 0;
    for (auto& lane : status.scoring) {
      lane.ready = lane.evaluated = false;
      lane.latestEvaluatedSequence = 0;
      lane.latestCandidateCount = 0;
      lane.tracks = lane.activeTracks = lane.coastingTracks = 0;
      lane.tracker = {}; lane.latestCandidates.clear();
    }
  }
  bool sameSourceGeometry(const ComplexMap& map) const {
    return map.data.size() == sourceRows_ &&
      map.delay.size() == sourceColumns_ && map.doppler.size() == sourceRows_ &&
      std::equal(delay_.begin(), delay_.end(), map.delay.begin() + columnOffset_) &&
      map.doppler == doppler_ &&
      std::all_of(map.data.begin(), map.data.end(),
        [&](const auto& row) { return row.size() == sourceColumns_; });
  }
  template<class Copy> bool offer(const Frame& frame, Copy copy) noexcept {
    if (!config_.enabled || disabled_.load() ||
        (config_.artifactOnly && artifactFinished_.load())) return false;
    const auto started = std::chrono::steady_clock::now();
    std::unique_lock<std::mutex> lock(mutex_, std::try_to_lock);
    const bool invalidMetadata =
        !frame.samplePositionAvailable || !frame.sequence ||
        !frame.sampleRateHz || !frame.tuningHz || !frame.configurationEpoch ||
        frame.endSample <= frame.firstSample ||
        (config_.scoring.enabled && !frame.timestampMs) ||
        (!config_.artifactPath.empty() && (!frame.timestampMs || !frame.mapSampleRateHz));
    const auto reason = admissionDropReason(lock.owns_lock(),
      lock.owns_lock() && busy_, lock.owns_lock() && started < nextOffer_,
      !invalidMetadata);
    if (reason != DropReason::none) return recordDrop(reason);
    try {
      const auto result = copy(started + std::chrono::milliseconds(10));
      const auto completed = std::chrono::steady_clock::now();
      const double copyMs = std::chrono::duration<double, std::milli>(
        completed - started).count();
      status_.copyMs = copyMs;
      status_.maximumCopyMs = std::max(status_.maximumCopyMs, copyMs);
      if (result == CopyResult::invalid) {
        return recordDrop(DropReason::invalidCopy);
      }
      auto decision = copyPolicy(copyMs, copyOverrunStreak_);
      if (result == CopyResult::hardOverrun)
        decision = {false, true, true, copyOverrunStreak_ + 1};
      if (!decision.admit) {
        ++status_.copyOverruns;
        copyOverrunStreak_ = decision.nextStreak;
        status_.copyOverrunStreak = copyOverrunStreak_;
        // Back off only after measured copy pressure.  A successful completed
        // observation must not create a wall-clock dead zone: bursty delivery
        // can otherwise discard a contiguous CPI even while the single worker
        // is idle, resetting every temporal bank for no resource reason.
        nextOffer_ = completed + std::chrono::milliseconds(config_.minimumIntervalMs);
        if (decision.latch) {
          disabled_.store(true);
          status_.disableReason = decision.hardOverrun ?
            "copy_hard_overrun" : "copy_soft_overrun_streak";
        }
        return recordDrop(DropReason::copyOverrun);
      }
      copyOverrunStreak_ = 0; status_.copyOverrunStreak = 0;
      pending_ = frame; pendingGeneration_ = generation_.load(); busy_ = true;
      ++status_.accepted; wake_.notify_one(); return true;
    } catch (...) {
      disabled_.store(true); status_.disableReason = "copy_exception";
      return recordDrop(DropReason::copyException);
    }
  }

  bool continuous(const Frame& frame, uint64_t generation) const {
    if (!previous_ || generation != previousGeneration_) return false;
    const auto& prior = *previous_;
    return frame.captureEpoch == prior.captureEpoch &&
      frame.configurationEpoch == prior.configurationEpoch &&
      frame.tuningHz == prior.tuningHz && frame.sampleRateHz == prior.sampleRateHz &&
      frame.mapSampleRateHz == prior.mapSampleRateHz &&
      prior.sequence != UINT64_MAX && frame.sequence == prior.sequence + 1 &&
      frame.firstSample == prior.endSample &&
      frame.endSample - frame.firstSample == prior.endSample - prior.firstSample;
  }

  void run() noexcept {
    while (!stop_.load()) {
      std::unique_lock<std::mutex> lock(mutex_);
      wake_.wait(lock, [&] { return stop_.load() || busy_; });
      if (stop_.load()) return;
      const auto frame = pending_; const auto generation = pendingGeneration_;
      const bool residentInput = pendingResident_;
      lock.unlock();
      const auto start = std::chrono::steady_clock::now();
      const EvidenceWorkBudget budget{start +
        std::chrono::milliseconds(config_.maximumWorkMs), &stop_};
      std::unique_ptr<Evidence> output;
      bool reset = !continuous(frame, generation), rejected = false;
      std::string resetReason = reset ? "input_discontinuity" : "";
      bool budgetExhausted = false;
      std::string failureReason;
      try {
        if (!config_.artifactOnly) {
          for (const auto& map : maps_)
            for (const auto& row : map->data) {
              budget.check();
              for (const auto value : row)
                if (!std::isfinite(value.real()) || !std::isfinite(value.imag()))
                  throw std::invalid_argument("Live temporal shadow CAF is not finite");
            }
          output = std::make_unique<Evidence>();
          output->frame = frame; output->generation = generation;
          output->bootstrap = bootstrap_.process(views_, &budget);
          // Covariance fallback/invalid masks change the statistic family. Never
          // combine incompatible families across a temporal window.
          const bool familyChanged = !family_.empty() &&
            family_ != output->bootstrap.statisticFamily;
          reset = reset || familyChanged;
          if (familyChanged) resetReason = "statistic_family_change";
          if (reset) {
            temporal_.reset();
            if (scorer_) scorer_->reset();
          }
          family_ = output->bootstrap.statisticFamily;
          output->temporal = temporal_.process(*output->bootstrap.map,
            frame.captureEpoch, frame.sequence, &budget);
          if (scorer_)
            output->scoring = scorer_->process(output->temporal,
              frame.timestampMs, &budget);
          budget.check();
          previous_ = frame; previousGeneration_ = generation;
        }
      } catch (const std::runtime_error& error) {
        temporal_.reset();
        if (scorer_) scorer_->reset();
        previous_.reset(); family_.clear(); output.reset();
        rejected = true; reset = true;
        // A cooperative deadline is load shedding, not corrupt evidence. Drop
        // the incomplete window and re-warm; numeric/work-bound failures still
        // latch so they cannot repeatedly affect live acquisition.
        if (std::string(error.what()) == "Temporal shadow work budget exhausted") {
          budgetExhausted = true;
          resetReason = "work_budget_exhausted";
        } else {
          failureReason = error.what();
          resetReason = "worker_runtime_error";
          disabled_.store(true);
        }
        invalidate();
      } catch (const std::exception& error) {
        temporal_.reset();
        if (scorer_) scorer_->reset();
        previous_.reset(); family_.clear(); output.reset();
        rejected = true; reset = true;
        failureReason = error.what();
        resetReason = "worker_exception";
        disabled_.store(true); invalidate();
      } catch (...) {
        temporal_.reset();
        if (scorer_) scorer_->reset();
        previous_.reset(); family_.clear(); output.reset();
        rejected = true; reset = true;
        failureReason = "unknown_worker_exception";
        resetReason = "unknown_worker_exception";
        disabled_.store(true); invalidate();
      }
      bool artifactFailed = false, artifactWritten = false;
      double artifactWriteMs = 0;
      if (!config_.artifactPath.empty() && !artifactDisabled_ && !artifactFinished_.load()) {
        const auto artifactStart = std::chrono::steady_clock::now();
        try {
          if (!artifact_) artifact_ = std::make_unique<LiveWeakEvidenceArtifact>(
            LiveWeakEvidenceArtifact::Config{config_.artifactPath,
              config_.artifactMaximumBytes, config_.artifactMaximumFrames},
            *maps_.front(), channelIds_, config_.bootstrap, config_.temporal);
          artifactWritten = artifact_->append(frame, generation, views_,
            residentInput ? &residentRaw_ : nullptr, &budget);
          if (artifactWritten && config_.artifactOnly) {
            previous_ = frame; previousGeneration_ = generation;
          }
          artifactFinished_.store(artifact_->complete());
          budget.check();
        } catch (...) {
          artifactDisabled_ = true; artifactFailed = true;
          // Export failure cannot change detection/tracking or trigger retries.
          if (config_.artifactOnly) { disabled_.store(true); invalidate(); }
        }
        artifactWriteMs = std::chrono::duration<double, std::milli>(
          std::chrono::steady_clock::now() - artifactStart).count();
      }
      const double workMs = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - start).count();
      lock.lock();
      status_.budgetExhaustions += budgetExhausted;
      if (!failureReason.empty()) status_.disableReason = failureReason;
      status_.workMs = workMs;
      status_.artifactDisabled = artifactDisabled_;
      status_.artifactComplete = artifactFinished_.load();
      status_.artifactWriteMs = artifactWriteMs;
      if (artifact_) {
        status_.artifactFrames = artifact_->frames(); status_.artifactBytes = artifact_->size();
      }
      if (artifactWritten) status_.artifactLastFrame = frame;
      if (artifactFailed && config_.artifactOnly) ++status_.rejected;
      if (reset) {
        ++status_.resets;
        ++status_.resetSerial;
        status_.lastResetReason = resetReason.empty() ?
          "unspecified_semantic_reset" : resetReason;
      }
      status_.rejected += rejected;
      latest_.reset();
      // Counters describe every completed scoring trial, including results
      // invalidated while the worker was running. Gauges remain current-only.
      if (scorer_)
        for (std::size_t lane = 0; lane < status_.scoring.size(); ++lane) {
          const auto& source = scorer_->status().lanes[lane];
          auto& target = status_.scoring[lane];
          target.evaluatedWindows = source.evaluatedWindows;
          target.skippedOverlappingWindows = source.skippedOverlappingWindows;
          target.candidates = source.candidates;
          target.truncatedCandidates = source.truncatedCandidates;
          target.cumulativeAssociations = source.cumulativeAssociations;
          target.cumulativePromotions = source.cumulativePromotions;
          target.cumulativeMisses = source.cumulativeMisses;
        }
      if (output && generation == generation_.load()) {
        statusGeneration_ = generation;
        status_.frame = frame; status_.availableCpis = output->temporal.availableCpis;
        status_.trainedCells = output->bootstrap.trainedCells;
        status_.fallbackCells = output->bootstrap.fallbackCells;
        status_.invalidCells = output->bootstrap.invalidCells;
        status_.peakPower = 0;
        for (const auto& row : output->temporal.map->data)
          for (const auto value : row)
            status_.peakPower = std::max(status_.peakPower, std::norm(value));
        for (std::size_t bank = 0; bank < status_.bankReady.size(); ++bank)
          status_.bankReady[bank] = output->temporal.banks[bank].ready;
        if (output->scoring)
          for (std::size_t lane = 0; lane < status_.scoring.size(); ++lane) {
            const auto& source = output->scoring->lanes[lane];
            auto& target = status_.scoring[lane];
            target.depth = source.depth; target.ready = source.ready;
            target.evaluated = source.evaluated;
            target.latestEvaluatedSequence = source.latestEvaluatedSequence;
            target.tracks = source.tracks;
            target.activeTracks = source.activeTracks;
            target.coastingTracks = source.coastingTracks;
            target.tracker = source.tracker;
            target.latestCandidateCount = uint32_t(source.latestCandidates.size());
            const auto count = std::min<std::size_t>(target.candidateExportLimit,
              source.latestCandidates.size());
            target.latestCandidates.assign(source.latestCandidates.begin(),
              source.latestCandidates.begin() + count);
          }
        latest_ = std::move(output);
      } else clearEvidenceGauges(status_);
      busy_ = false;
    }
  }

  const Config config_;
  detection::PostClutterBootstrap bootstrap_;
  TemporalEvidence temporal_;
  std::unique_ptr<TemporalShadowTracker> scorer_;
  const std::vector<uint32_t> channelIds_;
  std::deque<int> delay_;
  std::deque<double> doppler_;
  uint32_t sourceRows_ = 0, sourceColumns_ = 0, columnOffset_ = 0;
  uint32_t rows_ = 0, columns_ = 0;
  std::vector<std::unique_ptr<ComplexMap>> maps_;
  std::vector<ComplexMap*> views_;
  std::vector<std::complex<float>> residentRaw_;
  std::unique_ptr<LiveWeakEvidenceArtifact> artifact_;
  bool artifactDisabled_ = false, pendingResident_ = false;
  uint32_t copyOverrunStreak_ = 0;
  std::atomic<bool> artifactFinished_{false};
  mutable std::mutex mutex_;
  std::condition_variable wake_;
  std::thread worker_;
  std::atomic<bool> stop_{false}, disabled_{false};
  std::atomic<uint64_t> generation_{0};
  DropLedger dropLedger_;
  bool busy_ = false;
  Frame pending_;
  uint64_t pendingGeneration_ = 0, previousGeneration_ = 0;
  uint64_t statusGeneration_ = 0;
  std::optional<Frame> previous_;
  std::vector<uint8_t> family_;
  std::chrono::steady_clock::time_point nextOffer_{};
  Status status_;
  std::unique_ptr<Evidence> latest_;
};

} // namespace blah2::integration
