#ifndef TEMPORAL_EVIDENCE_H
#define TEMPORAL_EVIDENCE_H

#include "data/Map.h"
#include "process/integration/EvidenceWorkBudget.h"
#include "../../../core/TrackBeforeDetect.private.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstdint>
#include <deque>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace blah2::integration
{

struct EvidenceLineage
{
  uint64_t epoch = 0;
  uint64_t newestSequence = 0;
  uint8_t depth = 0;
  int8_t delayMotion = 0;
  int8_t dopplerMotion = 0;
  std::array<uint64_t, 8> sequences{};

  std::vector<uint64_t> supportSequences() const
  {
    return {sequences.begin(), sequences.begin() + depth};
  }
};

struct EvidenceMotion
{
  int8_t delay = 0;
  int8_t doppler = 0;
};

// Experimental confirmation may credit only disjoint CPI windows. This is
// deliberately stricter than finding one new CPI in a rolling window: one old
// impulse plus fresh background must not manufacture persistence.
class ConfirmationLedger
{
public:
  explicit ConfirmationLedger(std::size_t maximumTracks = 1024,
    uint64_t historyCpis = 256)
    : maximumTracks_(maximumTracks), historyCpis_(historyCpis)
  {
    if (!maximumTracks_ || historyCpis_ < 8)
      throw std::invalid_argument("Confirmation ledger bounds are invalid");
  }

  bool credit(const std::string& track, const EvidenceLineage& lineage)
  {
    if (track.empty() || !lineage.depth || lineage.depth > 8) return false;
    if (lineage.newestSequence != lineage.sequences[0]) return false;
    for (uint32_t index = 1; index < lineage.depth; ++index)
      if (lineage.sequences[index - 1] == 0 ||
          lineage.sequences[index] + 1 != lineage.sequences[index - 1])
        return false;
    auto found = tracks_.find(track);
    if (found == tracks_.end())
    {
      if (tracks_.size() >= maximumTracks_) return false;
      found = tracks_.emplace(track, State{}).first;
    }
    auto& state = found->second;
    if (state.initialized && lineage.epoch < state.epoch) return false;
    if (!state.initialized || state.epoch != lineage.epoch)
    {
      state = State{};
      state.epoch = lineage.epoch;
      state.initialized = true;
    }
    else if (lineage.newestSequence <= state.highWaterSequence) return false;
    const uint64_t floor = lineage.newestSequence > historyCpis_ ?
      lineage.newestSequence - historyCpis_ : 0;
    for (auto iterator = state.credited.begin();
         iterator != state.credited.end();)
      if (*iterator < floor) iterator = state.credited.erase(iterator);
      else ++iterator;
    const auto support = lineage.supportSequences();
    if (std::any_of(support.begin(), support.end(), [&](uint64_t sequence) {
          return state.credited.count(sequence) != 0;
        }))
      return false;
    state.credited.insert(support.begin(), support.end());
    state.highWaterSequence = lineage.newestSequence;
    return true;
  }

  void erase(const std::string& track) { tracks_.erase(track); }
  void clear() { tracks_.clear(); }

private:
  struct State
  {
    uint64_t epoch = 0;
    uint64_t highWaterSequence = 0;
    bool initialized = false;
    std::unordered_set<uint64_t> credited;
  };
  std::size_t maximumTracks_;
  uint64_t historyCpis_;
  std::unordered_map<std::string, State> tracks_;
};

class TemporalEvidence
{
public:
  struct Config
  {
    uint32_t maximumCpis = 8;
    uint32_t delayMotionBins = 2;
    uint32_t dopplerMotionBins = 4;
    double delayDriftPerHz = 0;
    uint32_t beamCount = 0, beamTolerance = UINT32_MAX;
  };

  struct BankResult
  {
    std::string name;
    uint32_t depth = 0;
    bool ready = false;
    std::unique_ptr<Map<std::complex<double>>> map;
    // CFAR consumes the exact FP64 mean, not norm(sqrt(mean)): that extra
    // rounding can reverse a strict threshold decision by one ULP. The map
    // remains an amplitude representation for display/interpolation only.
    std::vector<double> detectorPower;
    // Epoch and CPI window are identical for every cell in a bank. Store them
    // once and retain only the per-cell selected motion.
    EvidenceLineage window;
    std::vector<EvidenceMotion> motion;
    // Empty means every cell is valid. Allocate only if a complete motion
    // path is unavailable; invalid cells are not measured zero-power noise.
    std::vector<uint8_t> validCells;

    EvidenceLineage lineageAt(uint64_t cell) const
    {
      if (!ready || cell >= motion.size() ||
          (!validCells.empty() &&
            (cell >= validCells.size() || !validCells[cell]))) return {};
      auto result = window;
      result.delayMotion = motion[cell].delay;
      result.dopplerMotion = motion[cell].doppler;
      return result;
    }
  };

  struct Result
  {
    std::unique_ptr<Map<std::complex<double>>> map;
    // 0=current, 1=baseline2, 2=motion2, 3=motion4, 4=motion8.
    std::vector<uint8_t> selectedBank;
    // baseline2, motion2, motion4 and motion8 remain separate for the
    // production engine's depth-specific CFAR calibration.
    std::array<BankResult, 4> banks;
    uint64_t epoch = 0;
    uint64_t sequence = 0;
    uint32_t availableCpis = 0;
    EvidenceLineage lineageAt(uint64_t cell) const
    {
      if (cell >= selectedBank.size()) return {};
      const auto selected = selectedBank[cell];
      if (selected > 0 && selected <= banks.size())
        return banks[selected - 1].lineageAt(cell);
      EvidenceLineage result;
      result.epoch = epoch; result.newestSequence = sequence; result.depth = 1;
      result.sequences[0] = sequence;
      return result;
    }
  };

  TemporalEvidence() : TemporalEvidence(Config{}) {}

  explicit TemporalEvidence(Config config) : config_(config)
  {
    if ((config_.maximumCpis != 2 && config_.maximumCpis != 4 &&
         config_.maximumCpis != 8) ||
        config_.delayMotionBins > 2 || config_.dopplerMotionBins > 16 ||
        !std::isfinite(config_.delayDriftPerHz) || config_.delayDriftPerHz > 0 ||
        (config_.beamTolerance != UINT32_MAX && (!config_.beamCount ||
          config_.beamTolerance > config_.beamCount / 2 || config_.maximumCpis == 2)))
      throw std::invalid_argument("Temporal evidence configuration invalid");
  }

  void reset()
  {
    frames_.clear();
    rows_ = columns_ = 0;
    delay_.clear();
    doppler_.clear();
  }

  Result process(Map<std::complex<double>>& map, uint64_t epoch,
    uint64_t sequence, const EvidenceWorkBudget* budget = nullptr,
    const std::vector<uint32_t>* beams = nullptr)
  {
    const uint32_t rows = map.get_nRows(), columns = map.get_nCols();
    if (!rows || !columns || (!frames_.empty() &&
        (rows != rows_ || columns != columns_ || map.delay != delay_ ||
         map.doppler != doppler_ || epoch != frames_.back().epoch ||
         sequence != frames_.back().sequence + 1)))
      throw std::invalid_argument(
        "Temporal evidence epoch, sequence, or geometry changed");
    rows_ = rows;
    columns_ = columns;
    delay_ = map.delay;
    doppler_ = map.doppler;
    if (config_.delayDriftPerHz)
      track_before_detect::validateDelayDrift(config_.delayDriftPerHz,
        std::vector<double>(doppler_.begin(), doppler_.end()));
    const bool beamGated = config_.beamTolerance != UINT32_MAX;
    if (beamGated && (!beams || beams->size() != uint64_t(rows) * columns ||
        std::any_of(beams->begin(), beams->end(), [&](uint32_t beam) {
          return beam > config_.beamCount;
        })))
      throw std::invalid_argument("Temporal beam history is absent or outside the configured bank");
    Frame current{epoch, sequence,
      std::vector<double>(uint64_t(rows) * columns), beamGated ? *beams : std::vector<uint32_t>{}};
    const auto copyPower = [&](std::size_t row) {
      for (uint32_t column = 0; column < columns; ++column)
      {
        if (budget && column % 32 == 0) budget->check();
        const double power = std::norm(map.data[row][column]);
        if (!(power >= 0) || !std::isfinite(power))
          throw std::invalid_argument("Temporal evidence input is not finite");
        current.power[uint64_t(row) * columns + column] = power;
      }
    };
    if (budget && budget->executor) budget->forEach(rows, copyPower);
    else for (uint32_t row = 0; row < rows; ++row) copyPower(row);
    frames_.push_back(std::move(current));
    while (frames_.size() > config_.maximumCpis) frames_.pop_front();

    Result result;
    result.epoch = epoch;
    result.sequence = sequence;
    result.availableCpis = static_cast<uint32_t>(frames_.size());
    result.map = makeMap(rows, columns);
    result.selectedBank.resize(uint64_t(rows) * columns, 0);
    const std::array<const char*, 4> names{
      "baseline2", "motion2", "motion4", "motion8"};
    const std::array<uint32_t, 4> depths{2, 2, 4, 8};
    for (std::size_t bank = 0; bank < result.banks.size(); ++bank)
    {
      auto& output = result.banks[bank];
      output.name = names[bank];
      output.depth = depths[bank];
      output.ready = depths[bank] <= frames_.size() &&
        depths[bank] <= config_.maximumCpis;
      if (output.ready)
      {
        output.map = makeMap(rows, columns);
        output.detectorPower.resize(uint64_t(rows) * columns);
        output.window = lineage(output.depth, 0, 0);
        output.motion.resize(uint64_t(rows) * columns);
      }
    }

    std::array<const double*, 8> powerByAge{};
    std::array<const uint32_t*, 8> beamByAge{};
    for (std::size_t age = 0; age < frames_.size(); ++age)
    {
      powerByAge[age] = frames_[frames_.size() - 1 - age].power.data();
      beamByAge[age] = frames_[frames_.size() - 1 - age].beams.data();
    }
    const auto compatible = [&](uint64_t cell, uint32_t age, uint64_t older) {
      if (!beamGated) return true;
      const uint32_t left = beamByAge[0][cell], right = beamByAge[age][older];
      if (!left || !right) return true;
      const uint32_t delta = left > right ? left - right : right - left;
      return std::min(delta, config_.beamCount - delta) <= config_.beamTolerance;
    };
    const uint32_t readyDepth = frames_.size() >= 8 ? 8 : frames_.size() >= 4 ? 4 : 2;
    const bool stationary = !config_.delayMotionBins &&
      !config_.dopplerMotionBins && config_.delayDriftPerHz == 0 && !beamGated;
    if (stationary && result.banks[0].ready) {
      writeStationary(result, powerByAge, rows, columns, budget);
      return result;
    }
    using Metrics = Map<std::complex<double>>::MetricAccumulator;
    Metrics selectedMetrics;
    std::array<Metrics, 4> bankMetrics{};
    for (uint32_t row = 0; row < rows; ++row)
    {
      for (uint32_t column = 0; column < columns; ++column)
      {
        const uint64_t cell = uint64_t(row) * columns + column;
        if (budget && column % 32 == 0) budget->check();
        double selected = powerByAge[0][cell];
        uint8_t selectedBank = 0;
        std::array<double, 4> bankLevels{};
        if (result.banks[0].ready)
        {
          const bool valid = compatible(cell, 1, cell);
          const double baseline = valid ? .5 * (powerByAge[0][cell] + powerByAge[1][cell]) : 0;
          if (!std::isfinite(baseline))
            throw std::logic_error("Temporal evidence bank is not finite");
          write(result.banks[0], cell, row, column, baseline,
            {});
          bankLevels[0] = Metrics::level(result.banks[0].map->data[row][column]);
          if (valid) bankMetrics[0].addLevel(bankLevels[0]);
          else {
            auto& validity = result.banks[0].validCells;
            if (validity.empty()) validity.resize(uint64_t(rows) * columns, 1);
            validity[cell] = 0;
          }
          selected = std::max(0.0, baseline);
          selectedBank = 1;
        }
        // A path's 2/4/8-CPI statistics share the same newest-to-oldest
        // prefix. Sum each source sample once, in exactly the former order,
        // and snapshot its mean at each depth. Do not use rolling subtraction:
        // that would change rounding and strict-threshold decisions.
        std::array<double, 3> bestPower{-1, -1, -1};
        std::array<EvidenceMotion, 3> bestMotion{};
        if (result.banks[1].ready)
          for (int dr = -int(config_.dopplerMotionBins);
               dr <= int(config_.dopplerMotionBins); ++dr)
            for (int dc = -int(config_.delayMotionBins);
                 dc <= int(config_.delayMotionBins); ++dc)
            {
              double sum = 0;
              for (uint32_t age = 0; age < readyDepth; ++age)
              {
                const int rr = int(row) - dr * int(age);
                if (rr < 0 || rr >= int(rows)) break;
                const int cc = track_before_detect::delayPathColumn(int(column),
                  dc, age, config_.delayDriftPerHz, doppler_[row], doppler_[rr]);
                if (cc < 0 || cc >= int(columns)) break;
                if (!compatible(cell, age, uint64_t(rr) * columns + uint32_t(cc))) break;
                sum += powerByAge[age][uint64_t(rr) * columns + uint32_t(cc)];
                const uint32_t depth = age + 1;
                if (depth == 2 || depth == 4 || depth == 8) {
                  const std::size_t bank = depth == 2 ? 0 : depth == 4 ? 1 : 2;
                  const double mean = sum / depth;
                  if (track_before_detect::temporalPathBetter(mean, bestPower[bank], false)) {
                    bestPower[bank] = mean;
                    bestMotion[bank] = {int8_t(dc), int8_t(dr)};
                  }
                }
              }
            }
        for (std::size_t bank = 1; bank < result.banks.size(); ++bank)
        {
          auto& output = result.banks[bank];
          if (!output.ready) continue;
          double best = bestPower[bank - 1];
          // Fractional drift can leave every complete path outside the map.
          // Retain a finite placeholder, but exclude it from metrics, CFAR,
          // and lineage rather than treating it as measured background.
          if (best < 0) {
            if (output.validCells.empty())
              output.validCells.resize(uint64_t(rows) * columns, 1);
            output.validCells[cell] = 0;
            best = 0;
          }
          if (!std::isfinite(best))
            throw std::logic_error("Temporal evidence bank is not finite");
          write(output, cell, row, column, best, bestMotion[bank - 1]);
          if (output.validCells.empty() || output.validCells[cell]) {
            bankLevels[bank] = Metrics::level(output.map->data[row][column]);
            bankMetrics[bank].addLevel(bankLevels[bank]);
          }
          if (best > selected)
          {
            selected = best;
            selectedBank = static_cast<uint8_t>(bank + 1);
          }
        }
        result.map->data[row][column] =
          {std::sqrt(std::max(0.0, selected)), 0};
        result.selectedBank[cell] = selectedBank;
        // Selection copies one complete bank cell, so its level is exactly
        // that bank's level. Each map still reduces in row-major order.
        selectedMetrics.addLevel(selectedBank ? bankLevels[selectedBank - 1] :
          Metrics::level(result.map->data[row][column]));
      }
    }
    selectedMetrics.apply(*result.map);
    for (std::size_t bank = 0; bank < result.banks.size(); ++bank)
      if (result.banks[bank].ready) bankMetrics[bank].apply(*result.banks[bank].map);
    return result;
  }

private:
  static void writeStationary(Result& result,
      const std::array<const double*, 8>& power, uint32_t rows,
      uint32_t columns, const EvidenceWorkBudget* budget) {
    // Zero motion makes baseline2 and motion2 identical. Reuse their exact
    // value/metric, and the selected bank's value, without approximating sums
    // or adding a cache whose contents could survive a failed CPI.
    using Metrics = Map<std::complex<double>>::MetricAccumulator;
    Metrics selectedMetrics, twoMetrics, fourMetrics, eightMetrics;
    const bool four = result.banks[2].ready, eight = result.banks[3].ready;
    const auto compute = [&](uint32_t begin, uint32_t end, double* levels) {
    for (uint32_t row = begin; row < end; ++row) {
      if (!std::isfinite(result.map->doppler[row]))
        throw std::invalid_argument("Fractional delay path invalid");
      auto* selectedRow = result.map->data[row].data();
      auto* baselineRow = result.banks[0].map->data[row].data();
      auto* twoRow = result.banks[1].map->data[row].data();
      auto* fourRow = four ? result.banks[2].map->data[row].data() : nullptr;
      auto* eightRow = eight ? result.banks[3].map->data[row].data() : nullptr;
      for (uint32_t column = 0; column < columns; ++column) {
        if (budget && column % 32 == 0) budget->check();
        const uint64_t cell = uint64_t(row) * columns + column;
        double sum = power[0][cell] + power[1][cell];
        double selectedPower = sum / 2;
        if (!std::isfinite(selectedPower))
          throw std::logic_error("Temporal evidence bank is not finite");
        std::complex<double> selected{std::sqrt(std::max(0.0, selectedPower)), 0};
        double selectedLevel = Metrics::level(selected);
        const uint64_t levelCell = (uint64_t(row - begin) * columns + column) * 3;
        if (levels) levels[levelCell] = selectedLevel;
        else twoMetrics.addLevel(selectedLevel);
        baselineRow[column] = twoRow[column] = selected;
        result.banks[0].detectorPower[cell] = selectedPower;
        result.banks[1].detectorPower[cell] = selectedPower;
        uint8_t selectedBank = 1;
        if (four) {
          sum += power[2][cell]; sum += power[3][cell];
          const double mean = sum / 4;
          if (!std::isfinite(mean))
            throw std::logic_error("Temporal evidence bank is not finite");
          const std::complex<double> value{std::sqrt(std::max(0.0, mean)), 0};
          const double level = Metrics::level(value);
          if (levels) levels[levelCell + 1] = level;
          else fourMetrics.addLevel(level);
          fourRow[column] = value;
          result.banks[2].detectorPower[cell] = mean;
          if (mean > selectedPower) {
            selected = value; selectedPower = mean;
            selectedLevel = level; selectedBank = 3;
          }
        }
        if (eight) {
          for (uint32_t age = 4; age < 8; ++age) sum += power[age][cell];
          const double mean = sum / 8;
          if (!std::isfinite(mean))
            throw std::logic_error("Temporal evidence bank is not finite");
          const std::complex<double> value{std::sqrt(std::max(0.0, mean)), 0};
          const double level = Metrics::level(value);
          if (levels) levels[levelCell + 2] = level;
          else eightMetrics.addLevel(level);
          eightRow[column] = value;
          result.banks[3].detectorPower[cell] = mean;
          if (mean > selectedPower) {
            selected = value; selectedLevel = level; selectedBank = 4;
          }
        }
        selectedRow[column] = selected;
        result.selectedBank[cell] = selectedBank;
        if (!levels) selectedMetrics.addLevel(selectedLevel);
      }
    }
    };
    if (budget && budget->executor)
      orderedRowTiles(rows, columns, 3, *budget, compute,
        [&](uint32_t begin, uint32_t end, const double* levels) {
          for (uint64_t cell = 0; cell < uint64_t(end - begin) * columns; ++cell) {
            const auto selected = result.selectedBank[uint64_t(begin) * columns + cell];
            selectedMetrics.addLevel(levels[3 * cell + (selected == 1 ? 0 : selected - 2)]);
            twoMetrics.addLevel(levels[3 * cell]);
            if (four) fourMetrics.addLevel(levels[3 * cell + 1]);
            if (eight) eightMetrics.addLevel(levels[3 * cell + 2]);
          }
        });
    else compute(0, rows, nullptr);
    selectedMetrics.apply(*result.map);
    twoMetrics.apply(*result.banks[0].map);
    twoMetrics.apply(*result.banks[1].map);
    if (four) fourMetrics.apply(*result.banks[2].map);
    if (eight) eightMetrics.apply(*result.banks[3].map);
  }

  struct Frame
  {
    uint64_t epoch;
    uint64_t sequence;
    std::vector<double> power;
    std::vector<uint32_t> beams;
  };

  std::unique_ptr<Map<std::complex<double>>> makeMap(
    uint32_t rows, uint32_t columns) const
  {
    auto result = std::make_unique<Map<std::complex<double>>>(rows, columns);
    result->delay = delay_;
    result->doppler = doppler_;
    return result;
  }

  EvidenceLineage lineage(uint32_t depth, int delayMotion,
    int dopplerMotion) const
  {
    EvidenceLineage result;
    result.epoch = frames_.back().epoch;
    result.newestSequence = frames_.back().sequence;
    result.depth = uint8_t(depth);
    result.delayMotion = int8_t(delayMotion);
    result.dopplerMotion = int8_t(dopplerMotion);
    for (uint32_t age = 0; age < depth; ++age)
      result.sequences[age] = frames_[frames_.size() - 1 - age].sequence;
    return result;
  }

  static void write(BankResult& output, uint64_t cell, uint32_t row,
    uint32_t column, double power, const EvidenceMotion& motion)
  {
    output.map->data[row][column] = {std::sqrt(std::max(0.0, power)), 0};
    output.detectorPower[cell] = power;
    output.motion[cell] = motion;
  }

  Config config_;
  uint32_t rows_ = 0, columns_ = 0;
  std::deque<int> delay_;
  std::deque<double> doppler_;
  std::deque<Frame> frames_;
};

} // namespace blah2::integration

#endif
