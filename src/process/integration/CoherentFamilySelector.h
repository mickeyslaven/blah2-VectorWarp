#pragma once

#include "process/integration/DirectPathCoherentExperiment.h"
#include "process/detection/EmpiricalNullCalibrator.h"
#include <map>
#include <set>

namespace blah2::integration {

// Offline-only consumer of the canonical per-rate temporal owners. One sample
// is ONE prescribed terminal-CPI cell in an independent raw window, never a
// cell, rate, depth or overlapping CPI counted as an independent null draw.
class CoherentFamilySelector {
public:
  using Experiment = DirectPathCoherentExperiment;
  struct Contract {
    Experiment::Config experiment;
    std::deque<int> delay;
    std::deque<double> doppler;
    std::array<double, 3> rates{};
    uint32_t row = 0, column = 0, sampleRateHz = 0;
    uint64_t cpiSamples = 0, hopSamples = 0;
    uint32_t fitWindows = 128, jointWindows = 128, evaluationWindows = 256;
    double alpha = .05;
    // Content hashes of the complete upstream plan, frozen generator (seeds,
    // injection/readout, reset, confidence and unavailable policy), and splits.
    std::string planSha256, generatorSha256, fitSplitSha256, jointSplitSha256;
  };
  struct Window { uint64_t epoch = 0, firstSample = 0, endSample = 0; };
  struct Family {
    Experiment::Result::Cell raw;
    double rowMedian = 0, ratio = 0, phaseOffsetHz = 0;
    uint32_t validCount = 0, rateIndex = 0, bankIndex = 0;
    uint32_t row = 0, column = 0;
    int delay = 0;
    double dopplerHz = 0, rate = 0;
    bool available = false;
  };
  struct Observation {
    std::string contractSha256;
    Window window;
    // NC first, then coherent; within each family: declared rate, then depth.
    std::array<Family, 18> families{};
  };
  struct Winner {
    Family family;
    double marginalP = 1, standardized = 0;
    bool available = false;
  };
  struct Decision {
    Winner candidate, noncoherent;
    double candidateP = 1, noncoherentP = 1;
    double candidateAttainableP = 1, noncoherentAttainableP = 1;
    bool candidateDetected = false, noncoherentDetected = false;
    uint32_t availableFamilies = 0, availableBankTrialUpperBound = 0;
    uint32_t declaredPerReadoutTrials = 837, declaredNcTrials = 81;
    std::string calibrationSha256;
    Window window;
  };

  explicit CoherentFamilySelector(Contract contract)
    : contract_(std::move(contract)), fit_(contract_.fitWindows, 32),
      joint_(contract_.jointWindows, 32) {
    const auto& t = contract_.experiment.temporal;
    require(t.maximumCpis == 8 && t.delayMotionBins == 1 && t.dopplerMotionBins == 1 &&
      t.refineDopplerPhase && t.sourceDomain.enabled &&
      contract_.experiment.declaredSpatialTrials == 1 &&
      !contract_.experiment.fallbackBeamCount && !contract_.experiment.migrationRateCount &&
      contract_.experiment.migrationRates.empty(), "Selector requires declared 3-rate/2-4-8/837-trial plan");
    // Validate all numerical/gating fields through their canonical owner.
    Experiment validate(contract_.experiment, true);
    require(contract_.sampleRateHz && contract_.cpiSamples && contract_.hopSamples &&
      contract_.hopSamples <= (UINT64_MAX - contract_.cpiSamples) / 7 &&
      t.cpiHopSeconds * contract_.sampleRateHz == double(contract_.hopSamples),
      "Selector requires exact sample/hop identity");
    require(!contract_.delay.empty() && !contract_.doppler.empty() &&
      contract_.delay.size() <= contract_.experiment.maximumCells / contract_.doppler.size() &&
      contract_.row < contract_.doppler.size() && contract_.column < contract_.delay.size() &&
      contract_.evaluationWindows && contract_.fitWindows <= 65536 &&
      contract_.jointWindows <= 65536 && contract_.evaluationWindows <= 65536 &&
      std::isfinite(contract_.alpha) && contract_.alpha > 0 && contract_.alpha < 1,
      "Selector exposure or alpha invalid");
    t.sourceDomain.validate(contract_.delay, contract_.doppler,
      uint32_t(contract_.doppler.size()), uint32_t(contract_.delay.size()));
    require(t.sourceDomain.contains(contract_.delay[contract_.column], contract_.doppler[contract_.row]),
      "Selector prescribed readout outside source domain");
    std::set<double> rates;
    for (double rate : contract_.rates) require(std::isfinite(rate) && rates.insert(rate).second,
      "Selector rates must be distinct finite declared trials");
    require(rates.count(0), "Selector canonical rate bank must include zero");
    for (const auto* hash : {&contract_.planSha256, &contract_.generatorSha256,
        &contract_.fitSplitSha256, &contract_.jointSplitSha256})
      require(hash->size() == 64 && hash->find_first_not_of("0123456789abcdef") == std::string::npos,
        "Selector requires SHA256 contract identities");
    require(contract_.fitSplitSha256 != contract_.jointSplitSha256, "Selector A/B split identity overlaps");
    require(contract_.experiment.upstreamPlanSha256 == contract_.planSha256, "Selector upstream plan identity differs");
    contractHash_ = hashText(contractText());
    for (uint32_t r = 0; r < 3; ++r) {
      auto bound = contract_.experiment;
      bound.migrationRateIndex = r; bound.migrationRateCount = 3; bound.migrationRate = contract_.rates[r];
      producerHashes_[r] = Experiment::configurationSha256(bound);
    }
    fitHash_.update(contractHash_.data(), contractHash_.size());
    for (std::size_t i = 0; i < keys_.size(); ++i) keys_[i] = std::to_string(i);
  }

  const std::string& contractSha256() const { return contractHash_; }
  const std::string& fitSha256() const { return fitDigest_; }
  const std::string& calibrationSha256() const { return calibrationDigest_; }

  // No history, CAF, path or phase-bank recomputation. Each row uses ONLY its
  // own family's finite, valid post-migration source-domain cells. The upper
  // median convention is frozen, including even-length rows and invalid cells.
  Observation readout(const std::array<const Experiment::Result*, 3>& results,
    Window window, const std::string& observedContract) const {
    require(observedContract == contractHash_, "Selector observed plan identity changed");
    Observation observation{observedContract, window, {}};
    for (uint32_t r = 0; r < 3; ++r) {
      require(results[r] && results[r]->sourceCount == 8 && results[r]->declaredPerCellTrials == 252,
        "Selector terminal eight-CPI exposure changed");
      const auto& result = *results[r]; const auto& producer = result.producer();
      require(producer.rateIndex == r && producer.rateCount == 3 && producer.rate == contract_.rates[r] &&
        producer.rateCorrectedFallback && producer.perRateTrials == 279 && producer.combinedTrials == 837 &&
        producer.upstreamPlanSha256 == contract_.planSha256 && producer.configurationSha256 == producerHashes_[r],
        "Selector rate identity or terminal eight-CPI exposure changed");
      for (uint32_t age = 0; age < 8; ++age) {
        const auto& source = result.sources[age];
        const auto& common = results[0]->sources[age];
        require(source.captureEpoch == window.epoch && source.sampleRateHz == contract_.sampleRateHz &&
          source.inputValid && source.samplePositionAvailable && source.sequence == common.sequence &&
          source.tuningHz == common.tuningHz && source.gainCalibrationEpoch == common.gainCalibrationEpoch &&
          source.firstSample >= window.firstSample &&
          source.firstSample - window.firstSample == (7 - age) * contract_.hopSamples &&
          source.endSample - source.firstSample == contract_.cpiSamples,
          "Selector terminal raw-window lineage changed");
      }
      for (uint32_t c = 0; c < 2; ++c) for (uint32_t bank = 0; bank < 3; ++bank) {
        auto& family = observation.families[c * 9 + r * 3 + bank];
        family.rateIndex = producer.rateIndex; family.bankIndex = bank; family.rate = producer.rate;
        family.row = contract_.row; family.column = contract_.column;
        family.delay = contract_.delay[contract_.column]; family.dopplerHz = contract_.doppler[contract_.row];
        const auto* map = c ? result.banks[bank].map.get() : result.noncoherent.banks[bank + 1].map.get();
        if (!map) continue;
        require(map->delay == contract_.delay && map->doppler == contract_.doppler &&
          map->data.size() == contract_.doppler.size() &&
          std::all_of(map->data.begin(), map->data.end(), [&](const auto& row) { return row.size() == contract_.delay.size(); }),
          "Selector axes changed");
        const auto get = [&](uint32_t column) {
          const auto cell = uint64_t(contract_.row) * contract_.delay.size() + column;
          return c ? result.coherentAt(bank, cell) : result.noncoherentAt(bank, cell);
        };
        std::vector<double> powers; powers.reserve(contract_.delay.size());
        for (uint32_t column = 0; column < contract_.delay.size(); ++column) {
          if (!contract_.experiment.temporal.sourceDomain.contains(contract_.delay[column], family.dopplerHz)) continue;
          const auto cell = get(column);
          if (cell.valid && std::isfinite(cell.power) && cell.power >= 0) powers.push_back(cell.power);
        }
        family.validCount = uint32_t(powers.size());
        if (powers.empty()) continue;
        require(powers.size() >= 3, "Selector normalization requires at least three valid cells");
        const auto middle = powers.begin() + powers.size() / 2;
        std::nth_element(powers.begin(), middle, powers.end()); family.rowMedian = *middle;
        family.raw = get(contract_.column);
        if (!family.raw.valid || !(family.rowMedian > 0) || !std::isfinite(family.rowMedian)) continue;
        require(family.raw.lineage.newestSequence == result.sources[0].sequence, "Selector family source sequence mismatch");
        family.ratio = family.raw.power / family.rowMedian;
        family.available = std::isfinite(family.ratio) && family.ratio >= 0;
        if (c && family.available) family.phaseOffsetHz = result.banks[bank].dopplerOffsetHz[
          uint64_t(contract_.row) * contract_.delay.size() + contract_.column];
      }
    }
    return observation;
  }

  void addFit(const Observation& observation) {
    require(stage_ == Stage::fit && fitCount_ < contract_.fitWindows, "Selector fit is closed or full");
    const auto mask = validate(observation); recordWindow(observation.window);
    for (std::size_t i = 0; i < 18; ++i)
      if (observation.families[i].available) fit_.add(keys_[i], observation.families[i].ratio);
    availability_.insert(mask); hashObservation(fitHash_, observation); ++fitCount_;
  }
  void freezeFit() {
    require(stage_ == Stage::fit && fitCount_ == contract_.fitWindows, "Selector fit window count incomplete");
    fit_.freeze();
    for (const auto& key : keys_) fit_.minimumResolvableP(key); // All 18 strata must exist.
    fitDigest_ = fitHash_.finish(); jointHash_.update(fitDigest_.data(), fitDigest_.size());
    stage_ = Stage::joint;
  }
  void addJoint(const Observation& observation) {
    require(stage_ == Stage::joint && jointCount_ < contract_.jointWindows, "Selector joint calibration is closed or full");
    validateKnown(observation); recordWindow(observation.window);
    const auto winners = select(observation);
    joint_.add(candidateKey_, winners.first.standardized);
    joint_.add(ncKey_, winners.second.standardized);
    hashObservation(jointHash_, observation); ++jointCount_;
  }
  void freezeJoint() {
    require(stage_ == Stage::joint && jointCount_ == contract_.jointWindows, "Selector joint window count incomplete");
    joint_.freeze();
    // U cannot exceed max_i(1 - 1/(M_A,i+1)). Counting B samples at that
    // attainable endpoint (including ties) catches the large top-rank atom;
    // checking only 1/(M_B+1) would incorrectly admit impossible rare tails.
    double candidateMaximum = 0, ncMaximum = 0;
    for (std::size_t i = 0; i < 18; ++i) {
      const double maximum = 1 - fit_.upperTailP(keys_[i], std::numeric_limits<double>::max());
      candidateMaximum = std::max(candidateMaximum, maximum);
      if (i < 9) ncMaximum = std::max(ncMaximum, maximum);
    }
    candidateAttainable_ = joint_.upperTailP(candidateKey_, candidateMaximum);
    ncAttainable_ = joint_.upperTailP(ncKey_, ncMaximum);
    require(candidateAttainable_ <= contract_.alpha && ncAttainable_ <= contract_.alpha,
      "Selector requested alpha unattainable with frozen marginal/joint rank ties");
    calibrationDigest_ = jointHash_.finish(); stage_ = Stage::evaluation;
  }
  Decision evaluate(const Observation& observation) {
    require(stage_ == Stage::evaluation && evaluationCount_ < contract_.evaluationWindows,
      "Selector evaluation unavailable or exposure exhausted");
    validateKnown(observation); recordWindow(observation.window);
    const auto winners = select(observation);
    Decision decision;
    decision.candidate = winners.first; decision.noncoherent = winners.second;
    decision.candidateP = joint_.upperTailP(candidateKey_, winners.first.standardized);
    decision.noncoherentP = joint_.upperTailP(ncKey_, winners.second.standardized);
    decision.candidateAttainableP = candidateAttainable_; decision.noncoherentAttainableP = ncAttainable_;
    decision.candidateDetected = winners.first.available && decision.candidateP <= contract_.alpha;
    decision.noncoherentDetected = winners.second.available && decision.noncoherentP <= contract_.alpha;
    decision.calibrationSha256 = calibrationDigest_; decision.window = observation.window;
    for (std::size_t i = 0; i < 18; ++i) if (observation.families[i].available) {
      ++decision.availableFamilies;
      decision.availableBankTrialUpperBound += i < 9 ? 9 : 9 * (4u << (i % 3));
    }
    ++evaluationCount_; return decision;
  }

private:
  enum class Stage { fit, joint, evaluation };
  static void require(bool condition, const char* reason) {
    if (!condition) throw std::invalid_argument(reason);
  }
  static std::string hashText(const std::string& text) {
    Sha256 hash; hash.update(text.data(), text.size()); return hash.finish();
  }
  static std::ostringstream stream() {
    std::ostringstream s; s.imbue(std::locale::classic()); s << std::hexfloat; return s;
  }
  std::string contractText() const {
    auto s = stream(); const auto& c = contract_;
    s << "coherent-selector-v2 terminal8 one-cell upper-median minimum3 unavailable-omit known-mask NC-first rate-depth ties "
      << c.planSha256 << c.generatorSha256 << c.fitSplitSha256 << c.jointSplitSha256 << ' '
      << c.row << ' ' << c.column << ' ' << c.sampleRateHz << ' ' << c.cpiSamples << ' ' << c.hopSamples << ' '
      << c.fitWindows << ' ' << c.jointWindows << ' ' << c.evaluationWindows << ' ' << c.alpha << ' '
      << Experiment::configurationSha256(c.experiment) << ' ';
    s << "rates "; for (double value : c.rates) s << value << ' ';
    s << "delay "; for (int value : c.delay) s << value << ' ';
    s << "doppler "; for (double value : c.doppler) s << value << ' ';
    return s.str();
  }
  uint32_t validate(const Observation& observation) const {
    require(observation.contractSha256 == contractHash_, "Selector table/plan identity mismatch");
    const auto& w = observation.window;
    require(w.endSample > w.firstSample && w.endSample - w.firstSample ==
      7 * contract_.hopSamples + contract_.cpiSamples, "Selector independent window span invalid");
    uint32_t mask = 0;
    for (uint32_t i = 0; i < 18; ++i) {
      const auto& f = observation.families[i];
      if (!f.available) continue;
      require(f.raw.valid && f.raw.coherent == (i >= 9) && f.raw.lineage.depth == (2u << (i % 3)) &&
        f.rateIndex == (i % 9) / 3 && f.bankIndex == i % 3 && f.rate == contract_.rates[f.rateIndex] &&
        f.row == contract_.row && f.column == contract_.column && f.delay == contract_.delay[f.column] &&
        f.dopplerHz == contract_.doppler[f.row] && f.validCount >= 3 && f.validCount <= contract_.delay.size() &&
        std::isfinite(f.rowMedian) && f.rowMedian > 0 && std::isfinite(f.raw.power) && f.raw.power >= 0 &&
        std::isfinite(f.ratio) && f.ratio == f.raw.power / f.rowMedian && std::isfinite(f.phaseOffsetHz),
        "Selector family score/lineage invalid");
      const auto& lineage = f.raw.lineage;
      require(lineage.epoch == w.epoch && lineage.newestSequence >= 8 &&
        std::abs(int(lineage.delayMotion)) <= 1 && std::abs(int(lineage.dopplerMotion)) <= 1,
        "Selector family epoch or motion mismatch");
      for (uint32_t age = 0; age < lineage.depth; ++age)
        require(lineage.sequences[age] == lineage.newestSequence - age &&
          lineage.firstSamples[age] >= w.firstSample && lineage.endSamples[age] <= w.endSample &&
          lineage.firstSamples[age] - w.firstSample == (7 - age) * contract_.hopSamples &&
          lineage.endSamples[age] - lineage.firstSamples[age] == contract_.cpiSamples,
          "Selector family raw support mismatch");
      mask |= 1u << i;
    }
    return mask;
  }
  void validateKnown(const Observation& observation) const {
    require(availability_.count(validate(observation)), "Selector unseen availability mask");
  }
  void recordWindow(Window w) {
    auto& intervals = windows_[w.epoch]; const auto next = intervals.lower_bound(w.firstSample);
    require((next == intervals.end() || next->first >= w.endSample) &&
      (next == intervals.begin() || std::prev(next)->second <= w.firstSample),
      "Selector calibration/evaluation windows overlap or repeat");
    intervals.emplace(w.firstSample, w.endSample);
  }
  static void hashObservation(Sha256& hash, const Observation& observation) {
    auto s = stream(); const auto& w = observation.window;
    s << w.epoch << ' ' << w.firstSample << ' ' << w.endSample << ' ';
    for (const auto& f : observation.families) s << f.available << ' ' << (f.available ? f.ratio : 0) << ' ';
    const auto text = s.str(); hash.update(text.data(), text.size());
  }
  std::pair<Winner, Winner> select(const Observation& observation) const {
    Winner candidate, nc;
    for (std::size_t i = 0; i < 18; ++i) {
      const auto& family = observation.families[i]; if (!family.available) continue;
      const double p = fit_.upperTailP(keys_[i], family.ratio), u = 1 - p;
      const Winner winner{family, p, u, true};
      if (!candidate.available || u > candidate.standardized) candidate = winner;
      if (i < 9 && (!nc.available || u > nc.standardized)) nc = winner;
    }
    return {candidate, nc};
  }
  Contract contract_;
  detection::EmpiricalNullCalibrator fit_, joint_;
  std::array<std::string, 18> keys_;
  const std::string candidateKey_ = "joint18", ncKey_ = "jointNC9";
  std::string contractHash_, fitDigest_, calibrationDigest_;
  std::array<std::string, 3> producerHashes_;
  Sha256 fitHash_, jointHash_;
  Stage stage_ = Stage::fit;
  uint32_t fitCount_ = 0, jointCount_ = 0, evaluationCount_ = 0;
  double candidateAttainable_ = 1, ncAttainable_ = 1;
  std::set<uint32_t> availability_;
  std::map<uint64_t, std::map<uint64_t, uint64_t>> windows_;
};
} // namespace blah2::integration
