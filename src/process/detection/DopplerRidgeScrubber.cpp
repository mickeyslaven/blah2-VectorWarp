#include "process/detection/DopplerRidgeScrubber.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>

DopplerRidgeScrubber::DopplerRidgeScrubber(DopplerRidgeScrubberConfig config)
  : config_(std::move(config))
{
  if (config_.mode != "off" && config_.mode != "shadow" &&
      config_.mode != "active")
    throw std::invalid_argument("Doppler ridge scrubber mode must be off, shadow, or active");
  if (config_.persistenceCpis < 2 ||
      !std::isfinite(config_.minimumExcessDb) || config_.minimumExcessDb <= 0 ||
      !std::isfinite(config_.maximumSubtractionDb) ||
      config_.maximumSubtractionDb <= 0 ||
      !std::isfinite(config_.dopplerToleranceHz) ||
      config_.dopplerToleranceHz < 0 || !config_.neighborhoodRows ||
      !config_.delayStride || !config_.shadowMaximumCpis ||
      config_.shadowMaximumCpis > 65535)
    throw std::invalid_argument("Invalid Doppler ridge scrubber configuration");
}

double DopplerRidgeScrubber::median(std::vector<double>& values)
{
  if (values.empty()) return 0;
  const auto middle = values.begin() + values.size() / 2;
  std::nth_element(values.begin(), middle, values.end());
  return *middle;
}

void DopplerRidgeScrubber::reset()
{
  rowMedianPower_.clear();
  localBaselinePower_.clear();
  learnedExcessPower_.clear();
  learnedCellExcessPower_.clear();
  backgroundPower_.clear();
  persistence_.clear();
  misses_.clear();
  ridgeCandidate_.clear();
  doppler_.clear();
  scratch_.clear();
  metrics_ = {};
}

void DopplerRidgeScrubber::ensureTopology(Map<std::complex<double>>* map)
{
  const std::size_t rows = map->get_nRows();
  bool changed = rows != doppler_.size();
  if (!changed)
    for (std::size_t row = 0; row < rows; ++row)
      if (doppler_[row] != map->doppler[row]) { changed = true; break; }
  if (!changed) return;
  doppler_.assign(map->doppler.begin(), map->doppler.end());
  rowMedianPower_.assign(rows, 0);
  localBaselinePower_.assign(rows, 0);
  learnedExcessPower_.assign(rows, 0);
  learnedCellExcessPower_.assign(rows,
    std::vector<double>(map->get_nCols(), 0));
  backgroundPower_.assign(rows,
    std::vector<double>(map->get_nCols(), 0));
  persistence_.assign(rows, 0);
  misses_.assign(rows, 0);
  ridgeCandidate_.assign(rows, false);
}

const std::vector<std::vector<double>>& DopplerRidgeScrubber::process(
  Map<std::complex<double>>* map, int32_t minimumDelay)
{
  if (!map) throw std::invalid_argument("Doppler ridge scrubber map is null");
  ensureTopology(map);
  metrics_ = {};
  for (auto& row : backgroundPower_) std::fill(row.begin(), row.end(), 0);
  if (config_.mode == "off") return backgroundPower_;

  const std::size_t rows = map->get_nRows(), columns = map->get_nCols();
  std::size_t firstColumn = 0;
  while (firstColumn < columns && map->delay[firstColumn] < minimumDelay)
    ++firstColumn;
  const std::size_t samples = firstColumn < columns
    ? 1 + (columns - 1 - firstColumn) / config_.delayStride : 0;
  scratch_.reserve(std::max<std::size_t>(samples,
    config_.neighborhoodRows * 2));

  // A target occupies only a few delay cells. The row median therefore
  // measures a broad horizontal ridge without being pulled up by an aircraft.
  for (std::size_t row = 0; row < rows; ++row)
  {
    scratch_.clear();
    for (std::size_t column = firstColumn; column < columns;
         column += config_.delayStride)
    {
      const double power = std::norm(map->data[row][column]);
      if (std::isfinite(power)) scratch_.push_back(power);
    }
    rowMedianPower_[row] = median(scratch_);
  }

  for (std::size_t row = 0; row < rows; ++row)
  {
    scratch_.clear();
    const std::size_t begin = row > config_.neighborhoodRows
      ? row - config_.neighborhoodRows : 0;
    const std::size_t end = std::min(rows, row + config_.neighborhoodRows + 1);
    for (std::size_t other = begin; other < end; ++other)
      if (other != row && std::abs(int64_t(other) - int64_t(row)) > 1 &&
          rowMedianPower_[other] > 0)
        scratch_.push_back(rowMedianPower_[other]);
    localBaselinePower_[row] = median(scratch_);
  }

  std::fill(ridgeCandidate_.begin(), ridgeCandidate_.end(), false);
  for (std::size_t source = 0; source < rows; ++source)
  {
    const double baseline = localBaselinePower_[source];
    const double observed = rowMedianPower_[source];
    if (!(baseline > 0) || !(observed > 0) ||
        observed / baseline < std::pow(10.0, config_.minimumExcessDb / 10.0))
      continue;
    for (std::size_t row = 0; row < rows; ++row)
      if (std::abs(doppler_[row] - doppler_[source]) <=
          config_.dopplerToleranceHz)
        ridgeCandidate_[row] = true;
  }

  for (std::size_t row = 0; row < rows; ++row)
  {
    const double baseline = localBaselinePower_[row];
    const double observed = rowMedianPower_[row];
    if (!(baseline > 0) || !(observed > 0)) continue;
    ++metrics_.observedRows;
    const bool ridge = ridgeCandidate_[row];
    if (ridge)
    {
      misses_[row] = 0;
      persistence_[row] = std::min(config_.persistenceCpis, persistence_[row] + 1);
      const double excess = std::max(0.0, observed - baseline);
      learnedExcessPower_[row] = learnedExcessPower_[row] > 0
        ? .8 * learnedExcessPower_[row] + .2 * excess : excess;
      auto& learned = learnedCellExcessPower_[row];
      for (std::size_t column = firstColumn; column < columns; ++column)
      {
        const double power = std::norm(map->data[row][column]);
        const double cellExcess = std::isfinite(power) ?
          std::max(0.0, power - baseline) : 0.0;
        learned[column] = persistence_[row] == 1 ? cellExcess :
          .8 * learned[column] + .2 * cellExcess;
      }
    }
    else
    {
      // Mains-related ridges observed on the live corpus blink and wander by
      // a row or two. Leak one persistence credit only after four misses so a
      // recurring ridge can qualify without treating a one-off event as one.
      if (++misses_[row] >= 4)
      {
        misses_[row] = 0;
        if (persistence_[row]) --persistence_[row];
        learnedExcessPower_[row] *= .95;
        for (double& value : learnedCellExcessPower_[row]) value *= .95;
      }
    }
    if (persistence_[row] < config_.persistenceCpis) continue;
    bool applied = false;
    double maximumAppliedDb = 0;
    for (std::size_t column = firstColumn; column < columns; ++column)
    {
      const double power = std::norm(map->data[row][column]);
      const double maximum = power *
        (1.0 - std::pow(10.0, -config_.maximumSubtractionDb / 10.0));
      backgroundPower_[row][column] = std::min(
        learnedCellExcessPower_[row][column], maximum);
      applied = applied || backgroundPower_[row][column] > 0;
      if (backgroundPower_[row][column] > 0)
        maximumAppliedDb = std::max(maximumAppliedDb,
          10.0 * std::log10(power / std::max(
            std::numeric_limits<double>::min(),
            power - backgroundPower_[row][column])));
    }
    if (!applied) continue;
    ++metrics_.qualifiedRows;
    metrics_.qualifiedDopplerHz.push_back(doppler_[row]);
    metrics_.maximumAppliedDb = std::max(metrics_.maximumAppliedDb,
      maximumAppliedDb);
  }
  return backgroundPower_;
}
