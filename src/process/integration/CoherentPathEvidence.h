#ifndef COHERENT_PATH_EVIDENCE_H
#define COHERENT_PATH_EVIDENCE_H

#include "data/Map.h"
#include "process/integration/TemporalEvidence.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstdint>
#include <deque>
#include <limits>
#include <memory>
#include <stdexcept>
#include <vector>

namespace blah2::integration
{

// Offline replay contract for Doppler-phase-compensated complex integration.
// It is deliberately separate from the accepted noncoherent TBD banks.  A
// result is emitted only when the selected path passes a measured coherence
// floor; every depth/sign combination is a separate empirical-null stratum.
class CoherentPathEvidence
{
public:
  struct Config
  {
    uint32_t maximumCpis = 8;
    uint32_t delayMotionBins = 2;
    uint32_t dopplerMotionBins = 4;
    // Zero derives and then locks the interval from supplied frame lineage.
    // A positive value is an explicit replay contract which every lineage hop
    // must match; CPI duration is never implicitly treated as the hop.
    double cpiHopSeconds = 0;
    double minimumCoherence = .6;
    int phaseDirection = 1;
    double delayDriftPerHz = 0;
    // Zero declares one immutable spatial projection. Selected-beam inputs
    // require a known, identical beam along the entire coherent path.
    uint32_t beamCount = 0;
  };

  struct Bank
  {
    uint32_t depth = 0;
    bool ready = false;
    std::unique_ptr<Map<std::complex<double>>> map;
    std::vector<double> coherence;
    std::vector<uint8_t> valid;
    std::vector<EvidenceMotion> motion;
    EvidenceLineage window;
    std::array<uint64_t, 8> firstSamples{};
    uint32_t sampleRateHz = 0;

    EvidenceLineage lineageAt(uint64_t cell) const
    {
      if (!ready || cell >= motion.size() || !valid[cell]) return {};
      auto result = window;
      result.delayMotion = motion[cell].delay;
      result.dopplerMotion = motion[cell].doppler;
      return result;
    }
  };

  CoherentPathEvidence() : CoherentPathEvidence(Config{}) {}

  explicit CoherentPathEvidence(Config config) : config_(config)
  {
    if ((config_.maximumCpis != 2 && config_.maximumCpis != 4 && config_.maximumCpis != 8) ||
        config_.delayMotionBins > 2 || config_.dopplerMotionBins > 16 ||
        !(config_.cpiHopSeconds >= 0) || !std::isfinite(config_.cpiHopSeconds) ||
        !(config_.minimumCoherence >= 0 && config_.minimumCoherence <= 1) ||
        !std::isfinite(config_.minimumCoherence) ||
        (config_.phaseDirection != -1 && config_.phaseDirection != 1) ||
        !std::isfinite(config_.delayDriftPerHz) || config_.delayDriftPerHz > 0 ||
        config_.beamCount > 360)
      throw std::invalid_argument("Coherent temporal evidence configuration invalid");
  }

  void reset()
  {
    frames_.clear(); rows_ = columns_ = 0; delay_.clear(); doppler_.clear();
    lockedHopSamples_ = 0;
  }

  std::array<Bank, 3> process(Map<std::complex<double>>& map, uint64_t epoch,
    uint64_t sequence, uint64_t firstSample, uint32_t sampleRateHz,
    const std::vector<uint32_t>* beams = nullptr)
  {
    const uint32_t rows = map.get_nRows(), columns = map.get_nCols();
    if (!rows || !columns || map.data.size() != rows ||
        std::any_of(map.data.begin(), map.data.end(), [&](const auto& row) {
          return row.size() != columns;
        }) || (map.delay.size() != 2 && map.delay.size() != columns) ||
        std::any_of(map.doppler.begin(), map.doppler.end(),
          [](double hz) { return !std::isfinite(hz); }) || (map.doppler.size() != 2 &&
        map.doppler.size() != rows) ||
        (!frames_.empty() && (rows != rows_ || columns != columns_ ||
          map.delay != delay_ || map.doppler != doppler_ ||
          epoch != frames_.back().epoch || sequence <= frames_.back().sequence ||
          sequence != frames_.back().sequence + 1 ||
          sampleRateHz != frames_.back().sampleRateHz ||
          firstSample <= frames_.back().firstSample ||
          !validHop(firstSample - frames_.back().firstSample, sampleRateHz))))
      throw std::invalid_argument("Coherent temporal evidence lineage changed");
    if (!sampleRateHz)
      throw std::invalid_argument("Coherent temporal evidence sample rate invalid");
    if (config_.beamCount && (!beams || beams->size() != uint64_t(rows) * columns ||
        std::any_of(beams->begin(), beams->end(), [&](uint32_t beam) {
          return beam > config_.beamCount;
        })))
      throw std::invalid_argument("Coherent temporal evidence beam identity invalid");
    if (config_.delayDriftPerHz) {
      if (map.doppler.size() != rows)
        throw std::invalid_argument("Coherent delay drift requires an explicit Doppler axis");
      track_before_detect::validateDelayDrift(config_.delayDriftPerHz,
        std::vector<double>(map.doppler.begin(), map.doppler.end()));
    }
    Frame frame{epoch, sequence, firstSample, sampleRateHz,
      std::vector<std::complex<double>>(uint64_t(rows) * columns),
      config_.beamCount ? *beams : std::vector<uint32_t>{}};
    for (uint32_t row = 0; row < rows; ++row)
      for (uint32_t column = 0; column < columns; ++column)
      {
        const auto value = map.data[row][column];
        if (!std::isfinite(value.real()) || !std::isfinite(value.imag()))
          throw std::invalid_argument("Coherent temporal evidence input invalid");
        frame.cells[uint64_t(row) * columns + column] = value;
      }
    // Rejected input must not establish a timing contract. Lock the hop only
    // after all samples passed validation, alongside accepting the frame.
    const uint64_t acceptedHop = frames_.empty() ? 0 :
      firstSample - frames_.back().firstSample;
    rows_ = rows; columns_ = columns; delay_ = map.delay; doppler_ = map.doppler;
    frames_.push_back(std::move(frame));
    if (!lockedHopSamples_) lockedHopSamples_ = acceptedHop;
    while (frames_.size() > config_.maximumCpis) frames_.pop_front();

    std::array<Bank, 3> result;
    const std::array<uint32_t, 3> depths{2, 4, 8};
    for (std::size_t index = 0; index < result.size(); ++index)
    {
      auto& bank = result[index]; bank.depth = depths[index];
      bank.ready = bank.depth <= config_.maximumCpis && bank.depth <= frames_.size();
      if (!bank.ready) continue;
      bank.map = makeMap();
      const auto cells = uint64_t(rows) * columns;
      bank.coherence.assign(cells, 0); bank.valid.assign(cells, 0);
      bank.motion.resize(cells); bank.window = lineage(bank.depth, 0, 0);
      bank.sampleRateHz = sampleRateHz;
      for (uint32_t age = 0; age < bank.depth; ++age)
        bank.firstSamples[age] = frames_[frames_.size() - 1 - age].firstSample;
      for (uint32_t row = 0; row < rows; ++row)
        for (uint32_t column = 0; column < columns; ++column)
          accumulate(bank, row, column);
      bank.map->set_metrics();
    }
    return result;
  }

  // Compatibility entry point for bounded unit tools which declare an
  // explicit fixed hop. Production replay must provide capture lineage above.
  std::array<Bank, 3> process(Map<std::complex<double>>& map, uint64_t epoch,
    uint64_t sequence)
  {
    if (!(config_.cpiHopSeconds > 0))
      throw std::invalid_argument("Coherent temporal evidence needs frame lineage time");
    constexpr uint32_t compatibilityRate = 1000000000;
    return process(map, epoch, sequence, uint64_t(std::llround(
      config_.cpiHopSeconds * double(sequence) * compatibilityRate)),
      compatibilityRate);
  }

private:
  struct Frame { uint64_t epoch, sequence, firstSample; uint32_t sampleRateHz;
    std::vector<std::complex<double>> cells; std::vector<uint32_t> beams; };

  bool validHop(uint64_t hop, uint32_t sampleRateHz) const
  {
    if (!hop || !sampleRateHz) return false;
    if (!lockedHopSamples_)
    {
      if (config_.cpiHopSeconds > 0)
      {
        const double expected = config_.cpiHopSeconds * sampleRateHz;
        if (!std::isfinite(expected) || expected < 1 ||
            std::abs(expected - std::round(expected)) > 1e-9)
          return false;
        // llround has a signed return type; very large explicit contracts
        // must fail closed before conversion rather than overflow that type.
        if (expected >= double(std::numeric_limits<int64_t>::max()))
          return false;
        return hop == uint64_t(std::llround(expected));
      }
      return true;
    }
    return hop == lockedHopSamples_;
  }

  std::unique_ptr<Map<std::complex<double>>> makeMap() const
  {
    auto result = std::make_unique<Map<std::complex<double>>>(rows_, columns_);
    result->delay = delay_; result->doppler = doppler_; return result;
  }

  double dopplerHz(uint32_t row) const
  {
    if (doppler_.size() == rows_) return doppler_[row];
    if (rows_ == 1) return doppler_.front();
    return doppler_.front() + (doppler_.back() - doppler_.front()) *
      double(row) / double(rows_ - 1);
  }

  EvidenceLineage lineage(uint32_t depth, int delay, int doppler) const
  {
    EvidenceLineage result; result.epoch = frames_.back().epoch;
    result.newestSequence = frames_.back().sequence; result.depth = uint8_t(depth);
    result.delayMotion = int8_t(delay); result.dopplerMotion = int8_t(doppler);
    for (uint32_t age = 0; age < depth; ++age)
      result.sequences[age] = frames_[frames_.size() - 1 - age].sequence;
    return result;
  }

  void accumulate(Bank& bank, uint32_t row, uint32_t column)
  {
    constexpr double pi = 3.14159265358979323846;
    double best = -1, bestCoherence = 0; int bestDelay = 0, bestDoppler = 0;
    std::complex<double> bestValue{};
    for (int dr = -int(config_.dopplerMotionBins);
         dr <= int(config_.dopplerMotionBins); ++dr)
      for (int dc = -int(config_.delayMotionBins);
           dc <= int(config_.delayMotionBins); ++dc)
      {
        std::complex<double> sum{}; double power = 0, scale = 0, phaseCycles = 0;
        bool usable = true;
        for (uint32_t age = 0; age < bank.depth; ++age)
        {
          const int rr = int(row) - dr * int(age);
          if (rr < 0 || rr >= int(rows_)) { usable = false; break; }
          const int cc = track_before_detect::delayPathColumn(int(column), dc, age,
            config_.delayDriftPerHz, dopplerHz(row), dopplerHz(uint32_t(rr)));
          if (cc < 0 || cc >= int(columns_))
          { usable = false; break; }
          if (config_.beamCount) {
            const auto newestBeam = frames_.back().beams[uint64_t(row) * columns_ + column];
            if (!newestBeam || frames_[frames_.size() - 1 - age].beams[
                uint64_t(rr) * columns_ + uint32_t(cc)] != newestBeam)
            { usable = false; break; }
          }
          const auto value = frames_[frames_.size() - 1 - age].cells[
            uint64_t(rr) * columns_ + uint32_t(cc)];
          if (age)
          {
            const auto& newer = frames_[frames_.size() - age];
            const auto& older = frames_[frames_.size() - 1 - age];
            const double elapsed = double(newer.firstSample - older.firstSample) /
              newer.sampleRateHz;
            // Integrate every segment of the selected Doppler trajectory.
            // Explicit axes need not be uniformly spaced, so a single
            // endpoint average would not describe the intermediate path.
            phaseCycles += .5 * (dopplerHz(uint32_t(rr + dr)) +
              dopplerHz(uint32_t(rr))) * elapsed;
          }
          const double phase = config_.phaseDirection * 2 * pi *
            phaseCycles;
          const double magnitude = std::abs(value);
          if (!std::isfinite(magnitude)) { usable = false; break; }
          if (magnitude > scale) {
            const double ratio = scale / magnitude;
            sum *= ratio; power *= ratio * ratio; scale = magnitude;
          }
          if (scale > 0) {
            const auto normalized = value / scale;
            power += std::norm(normalized);
            sum += normalized * std::polar(1.0, phase);
          }
        }
        const double sumPower = std::norm(sum);
        if (!usable || !(power > 0) || !std::isfinite(power) ||
            !std::isfinite(sumPower)) continue;
        const double coherence = sumPower / (double(bank.depth) * power);
        const auto coherentValue = (sum / std::sqrt(double(bank.depth))) * scale;
        const double amplitude = std::abs(coherentValue);
        if (!std::isfinite(coherence) || coherence < 0 || coherence > 1 + 1e-9 ||
            !std::isfinite(amplitude)) continue;
        if (coherence >= config_.minimumCoherence && amplitude > best)
        { best = amplitude; bestCoherence = std::min(1.0, coherence);
          bestDelay = dc; bestDoppler = dr; bestValue = coherentValue; }
      }
    const uint64_t cell = uint64_t(row) * columns_ + column;
    if (best >= 0)
    {
      bank.map->data[row][column] = bestValue; bank.valid[cell] = 1;
      bank.coherence[cell] = bestCoherence;
      bank.motion[cell] = {int8_t(bestDelay), int8_t(bestDoppler)};
    }
    else bank.map->data[row][column] = {};
  }

  Config config_;
  uint32_t rows_ = 0, columns_ = 0;
  std::deque<int> delay_; std::deque<double> doppler_;
  std::deque<Frame> frames_;
  uint64_t lockedHopSamples_ = 0;
};

} // namespace blah2::integration

#endif
