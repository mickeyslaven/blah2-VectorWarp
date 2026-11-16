#pragma once

#include "WienerHopfReplayContract.h"
#include "process/detection/PostClutterBootstrap.h"
#include <numeric>

namespace blah2::resident_weak_score {
using ComplexMap = integration::DirectPathCoherentShadow::ComplexMap;
using Bootstrap = detection::PostClutterBootstrap;
using Frame = integration::coherent_replay::ValidatedSnapshot;
constexpr uint32_t maximumFrames = 20, maximumCells = 65536;
constexpr uint64_t maximumInputBytes = 512ULL << 20;
constexpr uint32_t guard = 2, training = 12;
constexpr double contrastThresholdDb = 10;

struct Surface {
  std::string name;
  uint32_t rows = 0, columns = 0;
  bool ready = true;
  std::vector<double> power;
  std::vector<uint8_t> valid;
  uint64_t fallbackCells = 0;
};

inline Surface amplitudeSurface(const std::string& name, ComplexMap& map,
    const std::vector<uint8_t>& valid) {
  Surface out{name, map.get_nRows(), map.get_nCols(), true, {}, valid};
  if (valid.size() != uint64_t(out.rows) * out.columns)
    throw std::invalid_argument("Statistic validity shape differs");
  for (const auto& row : map.data) for (auto value : row) {
    const auto power = std::norm(value);
    if (!std::isfinite(power)) throw std::invalid_argument("Nonfinite statistic power");
    out.power.push_back(power);
  }
  return out;
}

struct Measurement {
  bool available = false;
  uint32_t trainingCells = 0;
  double baselinePower = 0, power = 0, baselineLocalMean = 0, localMean = 0;
  double baselineContrastDb = NAN, contrastDb = NAN;
};

// Both methods use exactly the same CUT and local training-cell intersection.
// No truth coordinate, target peak search, or method-specific training mask is
// allowed to select a better background for one side of the pair.
inline Measurement measure(const Surface& baseline, const Surface& arm, uint64_t cell) {
  if (baseline.rows != arm.rows || baseline.columns != arm.columns)
    throw std::invalid_argument("Paired surface geometry differs");
  Measurement out;
  if (!arm.ready || cell >= arm.valid.size() || !arm.valid[cell] || !baseline.valid[cell]) return out;
  const uint32_t row = uint32_t(cell / arm.columns), column = uint32_t(cell % arm.columns);
  const int first = std::max(0, int(column) - int(guard + training));
  const int last = std::min(int(arm.columns) - 1, int(column) + int(guard + training));
  for (int c = first; c <= last; ++c) {
    if (std::abs(c - int(column)) <= int(guard)) continue;
    const auto index = uint64_t(row) * arm.columns + unsigned(c);
    if (!arm.valid[index] || !baseline.valid[index]) continue;
    out.localMean += arm.power[index]; out.baselineLocalMean += baseline.power[index];
    ++out.trainingCells;
  }
  if (out.trainingCells < 2 || !(out.localMean > 0) || !(out.baselineLocalMean > 0) ||
      !std::isfinite(out.localMean) || !std::isfinite(out.baselineLocalMean)) return out;
  out.localMean /= out.trainingCells; out.baselineLocalMean /= out.trainingCells;
  out.power = arm.power[cell]; out.baselinePower = baseline.power[cell];
  // Zero CUTs are available non-selections; their log contrast is represented
  // as JSON null rather than nonstandard -Infinity.
  if (out.power > 0) out.contrastDb = 10 * (std::log10(out.power) - std::log10(out.localMean));
  if (out.baselinePower > 0)
    out.baselineContrastDb = 10 * (std::log10(out.baselinePower) - std::log10(out.baselineLocalMean));
  out.available = true;
  return out;
}

struct Summary {
  uint64_t validCells = 0, pairedCells = 0, selectedCells = 0, baselineSelectedCells = 0;
};
inline Summary summarize(const Surface& baseline, const Surface& arm) {
  Summary out;
  if (!arm.ready) return out;
  for (uint64_t cell = 0; cell < arm.valid.size(); ++cell) {
    out.validCells += arm.valid[cell] != 0;
    const auto value = measure(baseline, arm, cell);
    if (!value.available) continue;
    ++out.pairedCells;
    out.selectedCells += value.contrastDb >= contrastThresholdDb;
    out.baselineSelectedCells += value.baselineContrastDb >= contrastThresholdDb;
  }
  return out;
}

class Scorer {
  std::unique_ptr<integration::coherent_replay::ShadowReader> coherent_;
  integration::cancellation_replay::Cohort cohort_;
  integration::EvidenceWorkBudget budget_{std::chrono::steady_clock::now() + std::chrono::seconds(60)};
  uint32_t anchor_;
public:
  explicit Scorer(uint32_t anchor) : anchor_(anchor) {}
  std::string coherentReason;
  std::vector<Surface> append(const Frame& frame) {
    budget_.check();
    const auto& g = frame.config.geometry;
    if (!g.doppler || !g.delays || uint64_t(g.doppler) * g.delays > maximumCells ||
        g.clutterSamples > 2000000)
      throw std::invalid_argument("Scorer exceeds 65536 cells or 2M pilot samples");
    cohort_.append(frame.sidecar, g.clutterSamples);
    if (!coherent_) {
      integration::DirectPathCoherentShadow::Config config;
      config.channelIds = frame.config.surveillanceChannels;
      config.anchorChannelId = anchor_; config.maximumCells = maximumCells;
      config.temporal.delayMotionBins = 0; config.temporal.dopplerMotionBins = 0;
      config.spatialCoefficients.assign(config.channelIds.size(),
        {1 / std::sqrt(double(config.channelIds.size())), 0});
      coherent_ = std::make_unique<integration::coherent_replay::ShadowReader>(config);
    }
    // Compute every complete surface before the caller evaluates any labels.
    auto coherent = coherent_->append(frame); coherentReason = coherent.reason;
    Surface baseline{"baseline_current_power", g.doppler, g.delays, true, {}, {}};
    for (const auto& row : coherent.powerFallback->data) for (auto value : row) {
      if (!std::isfinite(value.real()) || value.real() < 0)
        throw std::invalid_argument("Recorded current power is invalid");
      baseline.power.push_back(value.real()); // Already power: never square it.
      baseline.valid.push_back(1);
    }
    std::vector<Surface> result; result.push_back(std::move(baseline));
    auto maps = integration::coherent_replay::channelMaps(frame);
    std::vector<ComplexMap*> pointers; for (auto& map : maps) pointers.push_back(&map);
    for (auto covariance : {Bootstrap::Covariance::diagonal, Bootstrap::Covariance::shrinkage}) {
      Bootstrap::Config config; config.covariance = covariance;
      auto bootstrap = Bootstrap(config).process(pointers, &budget_);
      auto surface = amplitudeSurface("bootstrap_" + bootstrap.covariance,
        *bootstrap.map, bootstrap.statisticFamily);
      surface.fallbackCells = bootstrap.fallbackCells;
      result.push_back(std::move(surface));
    }
    for (size_t bank = 0; bank < coherent.banks.size(); ++bank) {
      auto& value = coherent.banks[bank];
      const auto name = "direct_path_coherent_" + std::to_string(2u << bank);
      if (value.ready) result.push_back(amplitudeSurface(name, *value.map, value.valid));
      else result.push_back({name, g.doppler, g.delays, false, {}, {}});
    }
    return result;
  }
};
} // namespace blah2::resident_weak_score
