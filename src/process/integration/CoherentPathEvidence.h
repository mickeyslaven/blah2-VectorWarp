#ifndef COHERENT_PATH_EVIDENCE_H
#define COHERENT_PATH_EVIDENCE_H

#include "data/Map.h"
#include "process/integration/TemporalEvidence.h"
#include "process/integration/RangeMigrationReplay.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <chrono>
#include <complex>
#include <cstdint>
#include <deque>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <vector>
#include <fftw3.h>

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
    // Twofold zero-padded slow-time frequency bank: 4/8/16 trials at 2/4/8
    // CPIs. One Nyquist endpoint only; offsets are aliases modulo 1/hop.
    // Histories remain immutable, and the full bank is charged/calibrated.
    bool refineDopplerPhase = false;
    // Opt-in research sampling, never changes the canonical power baseline.
    // center is the CAF heterodyne range ramp, not a target frequency.
    bool interpolateDelay = false;
    double heterodyneCenterHz = 0;
    bool measureTiming = false;
    EvidenceSourceDomain sourceDomain{};
  };

  struct Bank
  {
    uint32_t depth = 0;
    bool ready = false;
    std::unique_ptr<Map<std::complex<double>>> map;
    std::vector<double> coherence;
    std::vector<uint8_t> valid;
    std::vector<EvidenceMotion> motion;
    std::vector<double> dopplerOffsetHz;
    std::vector<double> whiteRangeNoiseGain;
    EvidenceLineage window;
    std::array<uint64_t, 8> firstSamples{};
    uint32_t sampleRateHz = 0;
    uint32_t phaseTrials = 1;
    double preparationMilliseconds = 0, reductionMilliseconds = 0;

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
        config_.beamCount > 360 || !std::isfinite(config_.heterodyneCenterHz) ||
        (config_.interpolateDelay && config_.beamCount))
      throw std::invalid_argument("Coherent temporal evidence configuration invalid");
  }

  static uint32_t phaseTrials(uint32_t depth, bool refine) { return refine ? 2 * depth : 1; }
  static uint32_t totalPhaseTrials(const Config& config) {
    uint32_t count = 0;
    for (uint32_t depth : {2u, 4u, 8u})
      if (depth <= config.maximumCpis) count += phaseTrials(depth, config.refineDopplerPhase);
    return count;
  }

  void reset()
  {
    frames_.clear(); rows_ = columns_ = 0; delay_.clear(); doppler_.clear();
    lockedHopSamples_ = 0;
  }

  std::array<Bank, 3> process(Map<std::complex<double>>& map, uint64_t epoch,
    uint64_t sequence, uint64_t firstSample, uint32_t sampleRateHz,
    const std::vector<uint32_t>* beams = nullptr, uint64_t endSample = 0)
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
    config_.sourceDomain.validate(map.delay, map.doppler, rows, columns);
    if (config_.interpolateDelay) {
      if (columns > 4096 || uint64_t(rows) * columns > 65536 || map.delay.size() != columns ||
          std::abs(config_.heterodyneCenterHz) > .5 * sampleRateHz)
        throw std::invalid_argument("Coherent interpolation requires bounded explicit range coordinates");
      for (uint32_t column = 1; column < columns; ++column)
        if (int64_t(map.delay[column]) != int64_t(map.delay[0]) + column)
          throw std::invalid_argument("Coherent interpolation requires contiguous sample-delay bins");
    }
    uint64_t phaseWork = 0;
    for (uint32_t depth : {2u, 4u, 8u})
      if (depth <= config_.maximumCpis) phaseWork += depth * phaseTrials(depth, config_.refineDopplerPhase);
    const uint64_t work = uint64_t(rows) * columns * phaseWork *
      (2 * config_.delayMotionBins + 1) * (2 * config_.dopplerMotionBins + 1);
    if (work > (uint64_t(1) << 30))
      throw std::invalid_argument("Coherent phase bank exceeds bounded path work");
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
    if (endSample && endSample <= firstSample)
      throw std::invalid_argument("Coherent temporal evidence sample range invalid");
    Frame frame{epoch, sequence, firstSample, endSample, sampleRateHz,
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
      bank.dopplerOffsetHz.assign(cells, 0);
      bank.whiteRangeNoiseGain.assign(cells, 0);
      bank.sampleRateHz = sampleRateHz;
      bank.phaseTrials = phaseTrials(bank.depth, config_.refineDopplerPhase);
      std::array<const Frame*, 8> history{};
      for (uint32_t age = 0; age < bank.depth; ++age)
      {
        history[age] = &frames_[frames_.size() - 1 - age];
        bank.firstSamples[age] = frames_[frames_.size() - 1 - age].firstSample;
      }
      for (uint32_t row = 0; row < rows; ++row)
      {
        const auto started = config_.measureTiming ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        // Phase depends on row/path/age, never on delay column. Cache the
        // rotations once per row rather than calling sin/cos in every cell.
        const auto rotations = phaseRotations(row, bank.depth, history);
        const auto paths = rowPaths(row, bank.depth, history);
        const auto prepared = config_.measureTiming ? std::chrono::steady_clock::now() : started;
        accumulate(bank, row, paths.paths, rotations);
        if (config_.measureTiming) {
          bank.preparationMilliseconds += std::chrono::duration<double, std::milli>(prepared - started).count();
          bank.reductionMilliseconds += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - prepared).count();
        }
      }
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
  struct Frame { uint64_t epoch, sequence, firstSample, endSample; uint32_t sampleRateHz;
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
    for (auto& row : result->data) std::fill(row.begin(), row.end(), std::complex<double>{});
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
    {
      const auto& frame = frames_[frames_.size() - 1 - age];
      result.sequences[age] = frame.sequence;
      result.firstSamples[age] = frame.firstSample;
      result.endSamples[age] = frame.endSample;
    }
    return result;
  }

  using Rotations = std::vector<std::array<std::complex<double>, 8>>;
  static constexpr uint32_t tile = 128;
  // Same FFTW backend as canonical Ambiguity. One bounded batch per path/tile,
  // cached per immutable depth; no cell-level allocation or phase fitting.
  struct PhaseTransform {
    std::vector<std::complex<double>> values;
    fftw_plan plan = nullptr;
    static std::mutex& plannerMutex() { static std::mutex mutex; return mutex; }
    PhaseTransform(uint32_t depth, int direction) : values(2 * depth * tile) {
      const int length = int(2 * depth);
      auto* data = reinterpret_cast<fftw_complex*>(values.data());
      std::lock_guard<std::mutex> lock(plannerMutex());
      plan = fftw_plan_many_dft(1, &length, int(tile), data, nullptr, int(tile), 1,
        data, nullptr, int(tile), 1, direction > 0 ? FFTW_BACKWARD : FFTW_FORWARD,
        FFTW_ESTIMATE);
      if (!plan) throw std::runtime_error("Coherent slow-time FFT planning failed");
    }
    ~PhaseTransform() { std::lock_guard<std::mutex> lock(plannerMutex()); if (plan) fftw_destroy_plan(plan); }
    PhaseTransform(const PhaseTransform&) = delete;
    PhaseTransform& operator=(const PhaseTransform&) = delete;
  };
  struct Path {
    int dr = 0, dc = 0, first = 0, last = 0;
    std::array<int, 8> shifts{};
    std::array<const std::complex<double>*, 8> values{};
    std::array<const uint32_t*, 8> beams{};
    std::array<std::complex<double>, 8> rangePhase{};
    double noiseGain = 1;
  };
  struct RowPaths {
    std::vector<Path> paths;
    std::vector<std::vector<std::complex<double>>> samples;
  };
  RowPaths rowPaths(uint32_t row, uint32_t depth,
    const std::array<const Frame*, 8>& history) const
  {
    RowPaths result;
    result.paths.reserve((2 * config_.dopplerMotionBins + 1) * (2 * config_.delayMotionBins + 1));
    result.samples.reserve((2 * config_.dopplerMotionBins + 1) * depth);
    const int firstSourceColumn = config_.sourceDomain.firstColumn(delay_);
    for (int dr = -int(config_.dopplerMotionBins); dr <= int(config_.dopplerMotionBins); ++dr) {
      std::array<const std::complex<double>*, 8> values{};
      std::array<int, 8> first{}, last{};
      std::array<double, 8> physical{}, gain{};
      for (uint32_t age = 0; age < depth; ++age) {
        const int older = int(row) - dr * int(age);
        if (older < 0 || older >= int(rows_)) break;
        if (!config_.sourceDomain.allowsDoppler(dopplerHz(uint32_t(older)))) break;
        values[age] = history[age]->cells.data() + uint64_t(older) * columns_;
        first[age] = firstSourceColumn;
        last[age] = int(columns_); gain[age] = 1;
        if (!config_.interpolateDelay) continue;
        physical[age] = -config_.delayDriftPerHz * .5 *
          (dopplerHz(row) + dopplerHz(uint32_t(older))) * age;
        if (!physical[age]) continue;
        const auto kernel = RangeMigrationReplay::prepareProfile(physical[age],
          config_.heterodyneCenterHz, history[age]->sampleRateHz);
        first[age] = std::max(0, int(std::ceil(RangeMigrationReplay::radius - physical[age])));
        if (config_.sourceDomain.enabled)
          first[age] = std::max(first[age], firstSourceColumn - int(kernel.firstOffset()));
        last[age] = std::min(int(columns_), int(std::ceil(double(columns_) - RangeMigrationReplay::radius - physical[age])));
        if (first[age] + physical[age] < RangeMigrationReplay::radius) ++first[age];
        if (last[age] - 1 + physical[age] + RangeMigrationReplay::radius >= columns_) --last[age];
        auto& samples = result.samples.emplace_back(columns_);
        for (int column = first[age]; column < last[age]; ++column)
          samples[column] = RangeMigrationReplay::sampleProfile(history[age]->cells,
            columns_, uint32_t(older), uint32_t(column), kernel);
        values[age] = samples.data(); gain[age] = kernel.whiteNoiseGain();
      }
      for (int dc = -int(config_.delayMotionBins); dc <= int(config_.delayMotionBins); ++dc) {
        Path path; path.dr = dr; path.dc = dc; path.last = int(columns_); path.noiseGain = 0;
        for (uint32_t age = 0; age < depth; ++age) {
          const int older = int(row) - dr * int(age);
          if (older < 0 || older >= int(rows_) || !values[age]) { path.last = 0; break; }
          const int shift = config_.interpolateDelay ? -dc * int(age) : track_before_detect::delayPathColumn(0, dc, age,
            config_.delayDriftPerHz, dopplerHz(row), dopplerHz(uint32_t(older)));
          path.first = std::max(path.first, first[age] - shift);
          path.last = std::min(path.last, last[age] - shift);
          path.shifts[age] = shift;
          path.values[age] = values[age]; path.noiseGain += gain[age] / depth;
          path.rangePhase[age] = std::polar(1., config_.interpolateDelay ?
            -2 * std::acos(-1.) * config_.heterodyneCenterHz * (physical[age] + shift) / history[age]->sampleRateHz : 0);
          if (config_.beamCount) path.beams[age] = history[age]->beams.data() + uint64_t(older) * columns_;
        }
        if (path.first < path.last) result.paths.push_back(path);
      }
    }
    return result;
  }
  double phaseOffsetHz(uint32_t trial, uint32_t depth) const {
    if (!trial) return 0;
    const int index = trial <= depth ? int(trial) - int(depth) - 1 : int(trial) - int(depth);
    return double(index) * frames_.back().sampleRateHz / (double(2 * depth) * double(lockedHopSamples_));
  }
  Rotations phaseRotations(uint32_t row, uint32_t depth,
    const std::array<const Frame*, 8>& history) const
  {
    constexpr double pi = 3.14159265358979323846;
    Rotations result(2 * config_.dopplerMotionBins + 1);
    for (int dr = -int(config_.dopplerMotionBins); dr <= int(config_.dopplerMotionBins); ++dr) {
      double cycles = 0;
      for (uint32_t age = 0; age < depth; ++age) {
        const int older = int(row) - dr * int(age);
        if (older < 0 || older >= int(rows_)) break;
        if (age) cycles += .5 * (dopplerHz(uint32_t(older + dr)) + dopplerHz(uint32_t(older))) *
          double(history[age - 1]->firstSample - history[age]->firstSample) / history[age]->sampleRateHz;
        result[dr + int(config_.dopplerMotionBins)][age] = std::polar(1.,
          config_.phaseDirection * 2 * pi * cycles);
      }
    }
    return result;
  }

  void accumulate(Bank& bank, uint32_t row,
    const std::vector<Path>& paths, const Rotations& rotations)
  {
    // Contiguous columns let the compiler vectorize the bounded reductions.
    // Each cell retains newest-to-oldest addition and dr/dc/trial tie order.
    std::array<std::array<double, tile>, 8> real{}, imaginary{};
    std::array<double, tile> scale{}, power{}, sumReal{}, sumImaginary{}, best{},
      pathBest{}, pathReal{}, pathImaginary{}, pathCoherence{}, pathOffset{};
    std::array<uint8_t, tile> usable{};
    const double rootDepth = std::sqrt(double(bank.depth));
    PhaseTransform* transform = nullptr;
    if (config_.refineDopplerPhase) {
      auto& cached = transforms_[bank.depth == 2 ? 0 : bank.depth == 4 ? 1 : 2];
      if (!cached) cached = std::make_unique<PhaseTransform>(bank.depth, config_.phaseDirection);
      transform = cached.get();
    }
    for (uint32_t begin = 0; begin < columns_; begin += tile) {
      const uint32_t count = std::min(tile, columns_ - begin);
      best.fill(-1);
      for (const auto& path : paths) {
        const int first = std::max(int(begin), path.first) - int(begin);
        const int last = std::min(int(begin + count), path.last) - int(begin);
        if (first >= last) continue;
        scale.fill(0); power.fill(0); usable.fill(1);
        for (uint32_t age = 0; age < bank.depth; ++age) {
          const auto* source = path.values[age] + int(begin) + first + path.shifts[age];
          for (int c = first; c < last; ++c)
            scale[c] = std::max(scale[c], std::max(std::abs(source[c - first].real()), std::abs(source[c - first].imag())));
          if (config_.beamCount)
            for (int c = first; c < last; ++c) {
              const auto newest = path.beams[0][begin + c];
              usable[c] &= newest && path.beams[age][int(begin) + c + path.shifts[age]] == newest;
            }
        }
        for (uint32_t age = 0; age < bank.depth; ++age) {
          const auto* source = path.values[age] + int(begin) + first + path.shifts[age];
          for (int c = first; c < last; ++c) {
            const double divisor = scale[c] > 0 ? scale[c] : 1.;
            real[age][c] = source[c - first].real() / divisor;
            imaginary[age][c] = source[c - first].imag() / divisor;
            power[c] += real[age][c] * real[age][c] + imaginary[age][c] * imaginary[age][c];
          }
        }
        const auto& phases = rotations[path.dr + int(config_.dopplerMotionBins)];
        if (transform) {
          std::fill(transform->values.begin(), transform->values.end(), std::complex<double>{});
          for (uint32_t age = 0; age < bank.depth; ++age) {
            const auto phase = phases[age] * path.rangePhase[age];
            for (int c = first; c < last; ++c)
              transform->values[age * tile + c] = std::complex<double>{real[age][c], imaginary[age][c]} * phase;
          }
          fftw_execute(transform->plan);
        }
        pathBest.fill(-1);
        for (uint32_t trial = 0; trial < bank.phaseTrials; ++trial) {
          sumReal.fill(0); sumImaginary.fill(0);
          const double offsetHz = phaseOffsetHz(trial, bank.depth);
          if (transform) {
            const int index = !trial ? 0 : trial <= bank.depth ? int(trial) - int(bank.depth) - 1 : int(trial) - int(bank.depth);
            const auto bin = uint32_t((index + int(bank.phaseTrials)) % int(bank.phaseTrials));
            for (int c = first; c < last; ++c) {
              const auto z = transform->values[bin * tile + c];
              sumReal[c] = z.real(); sumImaginary[c] = z.imag();
            }
          } else for (uint32_t age = 0; age < bank.depth; ++age) {
            const auto phase = phases[age] * path.rangePhase[age];
            for (int c = first; c < last; ++c) {
              sumReal[c] += real[age][c] * phase.real() - imaginary[age][c] * phase.imag();
              sumImaginary[c] += real[age][c] * phase.imag() + imaginary[age][c] * phase.real();
            }
          }
          for (int c = first; c < last; ++c) {
            if (!usable[c] || !(power[c] > 0)) continue;
            const double sumPower = sumReal[c] * sumReal[c] + sumImaginary[c] * sumImaginary[c];
            const double coherence = sumPower / (double(bank.depth) * power[c]);
            if (coherence < config_.minimumCoherence || coherence > 1 + 1e-9) continue;
            if (sumPower <= pathBest[c]) continue;
            pathBest[c] = sumPower; pathReal[c] = sumReal[c]; pathImaginary[c] = sumImaginary[c];
            pathCoherence[c] = coherence; pathOffset[c] = offsetHz;
          }
        }
        for (int c = first; c < last; ++c) {
          if (pathBest[c] < 0) continue;
          const double amplitude = std::sqrt(pathBest[c] / double(bank.depth)) * scale[c];
          if (!std::isfinite(amplitude) || amplitude <= best[c]) continue;
          best[c] = amplitude;
          const uint64_t cell = uint64_t(row) * columns_ + begin + c;
          bank.map->data[row][begin + c] = std::complex<double>{pathReal[c] / rootDepth, pathImaginary[c] / rootDepth} * scale[c];
          bank.valid[cell] = 1; bank.coherence[cell] = std::min(1., pathCoherence[c]);
          bank.motion[cell] = {int8_t(path.dc), int8_t(path.dr)};
          bank.dopplerOffsetHz[cell] = pathOffset[c];
          bank.whiteRangeNoiseGain[cell] = path.noiseGain;
        }
      }
    }
  }

  Config config_;
  uint32_t rows_ = 0, columns_ = 0;
  std::deque<int> delay_; std::deque<double> doppler_;
  std::deque<Frame> frames_;
  uint64_t lockedHopSamples_ = 0;
  std::array<std::unique_ptr<PhaseTransform>, 3> transforms_;
};

} // namespace blah2::integration

#endif
