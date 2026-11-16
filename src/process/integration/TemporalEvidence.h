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
    bool retainRecentPower = false;
  };

  struct BankResult
  {
    std::string name;
    uint32_t depth = 0;
    bool ready = false;
    std::unique_ptr<Map<std::complex<double>>> map;
    // Epoch and CPI window are identical for every cell in a bank. Store them
    // once and retain only the per-cell selected motion.
    EvidenceLineage window;
    std::vector<EvidenceMotion> motion;

    EvidenceLineage lineageAt(uint64_t cell) const
    {
      if (!ready || cell >= motion.size()) return {};
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
    // Optional FP64 current/prior snapshots for one pre-registered tracker
    // path. Observation-only users do not pay for these copies.
    std::array<std::vector<double>, 2> recentPower;

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
        !std::isfinite(config_.delayDriftPerHz) || config_.delayDriftPerHz > 0)
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
    uint64_t sequence, const EvidenceWorkBudget* budget = nullptr)
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
    Frame current{epoch, sequence,
      std::vector<double>(uint64_t(rows) * columns)};
    for (uint32_t row = 0; row < rows; ++row)
      for (uint32_t column = 0; column < columns; ++column)
      {
        const double power = std::norm(map.data[row][column]);
        if (!(power >= 0) || !std::isfinite(power))
          throw std::invalid_argument("Temporal evidence input is not finite");
        current.power[uint64_t(row) * columns + column] = power;
      }
    frames_.push_back(std::move(current));
    while (frames_.size() > config_.maximumCpis) frames_.pop_front();

    Result result;
    result.epoch = epoch;
    result.sequence = sequence;
    result.availableCpis = static_cast<uint32_t>(frames_.size());
    if (config_.retainRecentPower)
      for (std::size_t age = 0; age < std::min<std::size_t>(2, frames_.size()); ++age)
        result.recentPower[age] = frames_[frames_.size() - 1 - age].power;
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
        output.window = lineage(output.depth, 0, 0);
        output.motion.resize(uint64_t(rows) * columns);
      }
    }

    for (uint32_t row = 0; row < rows; ++row)
      for (uint32_t column = 0; column < columns; ++column)
      {
        const uint64_t cell = uint64_t(row) * columns + column;
        if (budget && column % 32 == 0) budget->check();
        double selected = frames_.back().power[cell];
        uint8_t selectedBank = 0;
        if (result.banks[0].ready)
        {
          const double baseline = .5 * (frames_.back().power[cell] +
            frames_[frames_.size() - 2].power[cell]);
          write(result.banks[0], cell, row, column, baseline,
            lineage(2, 0, 0));
          selected = std::max(0.0, baseline);
          selectedBank = 1;
        }
        for (std::size_t bank = 1; bank < result.banks.size(); ++bank)
        {
          auto& output = result.banks[bank];
          if (!output.ready) continue;
          const uint32_t depth = output.depth;
          double best = -1;
          EvidenceLineage bestLineage;
          for (int dr = -int(config_.dopplerMotionBins);
               dr <= int(config_.dopplerMotionBins); ++dr)
            for (int dc = -int(config_.delayMotionBins);
                 dc <= int(config_.delayMotionBins); ++dc)
            {
              double sum = 0;
              bool valid = true;
              for (uint32_t age = 0; age < depth; ++age)
              {
                const int rr = int(row) - dr * int(age);
                if (rr < 0 || rr >= int(rows)) {
                  valid = false;
                  break;
                }
                const int cc = track_before_detect::delayPathColumn(int(column),
                  dc, age, config_.delayDriftPerHz, doppler_[row], doppler_[rr]);
                if (cc < 0 || cc >= int(columns))
                {
                  valid = false;
                  break;
                }
                sum += frames_[frames_.size() - 1 - age].power[
                  uint64_t(rr) * columns + uint32_t(cc)];
              }
              const double mean = valid ? sum / depth : -1;
              if (mean > best)
              {
                best = mean;
                bestLineage = lineage(depth, dc, dr);
              }
            }
          // Fractional drift can leave every path outside the map. A zero
          // invalid cell cannot acquire a shorter, miscalibrated history.
          if (best < 0) best = 0;
          if (!std::isfinite(best))
            throw std::logic_error("Temporal evidence bank is not finite");
          write(output, cell, row, column, best, bestLineage);
          if (best > selected)
          {
            selected = best;
            selectedBank = static_cast<uint8_t>(bank + 1);
          }
        }
        result.map->data[row][column] =
          {std::sqrt(std::max(0.0, selected)), 0};
        result.selectedBank[cell] = selectedBank;
      }
    result.map->set_metrics();
    for (auto& bank : result.banks)
      if (bank.ready) bank.map->set_metrics();
    return result;
  }

private:
  struct Frame
  {
    uint64_t epoch;
    uint64_t sequence;
    std::vector<double> power;
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
    uint32_t column, double power, const EvidenceLineage& evidence)
  {
    output.map->data[row][column] = {std::sqrt(std::max(0.0, power)), 0};
    output.motion[cell] = {evidence.delayMotion, evidence.dopplerMotion};
  }

  Config config_;
  uint32_t rows_ = 0, columns_ = 0;
  std::deque<int> delay_;
  std::deque<double> doppler_;
  std::deque<Frame> frames_;
};

} // namespace blah2::integration

#endif
