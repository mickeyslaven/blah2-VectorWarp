#ifndef POST_CLUTTER_BOOTSTRAP_H
#define POST_CLUTTER_BOOTSTRAP_H

#include "data/Map.h"
#include "process/detection/IntegratedCfar.h"
#include "process/integration/EvidenceWorkBudget.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace blah2::detection
{

// CPU reference for the manifold-independent post-clutter statistic.  It is
// deliberately independent of a GPU API so every supported platform has the
// same numerical contract for the offline residentWeakEvidenceScore command.
class PostClutterBootstrap
{
public:
  // robustShrinkage is a offline portable contract.  The established
  // diagonal and fixed-shrinkage arms intentionally retain their exact math.
  enum class Covariance { diagonal, shrinkage, robustShrinkage };

  struct Config
  {
    Covariance covariance = Covariance::diagonal;
    uint32_t guardCells = 2;
    uint32_t trainingCells = 12;
    double diagonalLoading = 1e-3;
    // One is diagonal covariance; zero is the unshrunk sample covariance.
    double shrinkage = .75;
    // Bounded Huber power weighting followed by sample-count shrinkage.  These
    // are only used by robustShrinkage, whose explicit opt-in keeps the frozen
    // fixed-.75 arm a valid comparison baseline.
    double robustClip = 4.0;
    double robustShrinkageFloor = .10;
    double robustShrinkageCeiling = .90;
  };

  struct Result
  {
    std::unique_ptr<Map<std::complex<double>>> map;
    // 0 = invalid/no usable training background, 1 = requested covariance,
    // 2 = dimensionless diagonal fallback. The statistic family is never
    // silently mixed with raw power.
    std::vector<uint8_t> statisticFamily;
    uint64_t trainedCells = 0;
    uint64_t fallbackCells = 0;
    uint64_t invalidCells = 0;
    uint32_t channels = 0;
    std::string covariance;
  };

  PostClutterBootstrap() : PostClutterBootstrap(Config{}) {}

  explicit PostClutterBootstrap(Config config) : config_(config)
  {
    if (!config_.trainingCells || config_.trainingCells > 256)
      throw std::invalid_argument("Post-clutter bootstrap training-cell configuration invalid");
    if (config_.guardCells > 128)
      throw std::invalid_argument("Post-clutter bootstrap guard-cell configuration invalid");
    if (!(config_.diagonalLoading > 0) ||
        !std::isfinite(config_.diagonalLoading))
      throw std::invalid_argument("Post-clutter bootstrap diagonal-loading configuration invalid");
    if (!(config_.shrinkage >= 0 && config_.shrinkage <= 1) ||
        !std::isfinite(config_.shrinkage))
      throw std::invalid_argument("Post-clutter bootstrap shrinkage configuration invalid");
    if (!(config_.robustClip >= 1) || !std::isfinite(config_.robustClip))
      throw std::invalid_argument("Post-clutter bootstrap robust-clip configuration invalid");
    if (!(config_.robustShrinkageFloor >= 0 &&
          config_.robustShrinkageFloor <= config_.robustShrinkageCeiling &&
          config_.robustShrinkageCeiling <= 1) ||
        !std::isfinite(config_.robustShrinkageFloor) ||
        !std::isfinite(config_.robustShrinkageCeiling))
      throw std::invalid_argument("Post-clutter bootstrap robust-shrinkage configuration invalid");
  }

  Result process(const std::vector<Map<std::complex<double>>*>& maps,
    const integration::EvidenceWorkBudget* budget = nullptr) const
  {
    if (maps.empty() || maps.size() > 8 || maps.front() == nullptr)
      throw std::invalid_argument("Post-clutter bootstrap needs one to eight maps");
    const uint32_t rows = maps.front()->get_nRows();
    const uint32_t columns = maps.front()->get_nCols();
    for (auto* map : maps)
      if (!map || map->get_nRows() != rows || map->get_nCols() != columns ||
          map->delay != maps.front()->delay || map->doppler != maps.front()->doppler)
        throw std::invalid_argument("Post-clutter bootstrap map geometry changed");

    Result result;
    result.map = std::make_unique<Map<std::complex<double>>>(rows, columns);
    result.map->delay = maps.front()->delay;
    result.map->doppler = maps.front()->doppler;
    result.channels = static_cast<uint32_t>(maps.size());
    result.statisticFamily.resize(uint64_t(rows) * columns, 0);
    result.covariance = config_.covariance == Covariance::diagonal ? "diagonal" :
      config_.covariance == Covariance::shrinkage ? "shrinkage" :
      "robust_shrinkage";

    const std::size_t channels = maps.size();
    // A one-channel diagonal covariance is a scalar local-power estimate.
    // Preserve the general solver's operation order and statistic exactly,
    // without allocating a matrix, CUT vector and training vectors per cell.
    if (channels == 1 && config_.covariance == Covariance::diagonal)
    {
      // Every training ring reuses the same row samples. Cache their exact
      // complex covariance terms once; summation remains left-to-right for
      // every CUT, including the original guard and boundary exclusions.
      Map<std::complex<double>>::MetricAccumulator metrics;
      std::vector<uint32_t> trainedByRow(rows);
      const auto compute = [&](uint32_t begin, uint32_t end, double* levels) {
      std::vector<double> covarianceTerms(columns), covarianceSums;
      std::vector<uint8_t> finitePower(columns);
      for (uint32_t row = begin; row < end; ++row) {
        bool finiteRow = true;
        for (uint32_t column = 0; column < columns; ++column) {
          if (budget && column % 32 == 0) budget->check();
          const auto value = maps.front()->data[row][column];
          finitePower[column] = std::isfinite(std::norm(value));
          finiteRow = finiteRow && finitePower[column];
          covarianceTerms[column] = (value * std::conj(value)).real();
        }
        if (finiteRow)
          integrated_cfar::orderedTrainingSums(covarianceTerms,
            config_.guardCells, config_.trainingCells, covarianceSums, budget);
        for (uint32_t column = 0; column < columns; ++column)
        {
          if (budget && column % 32 == 0) budget->check();
          double covariance = 0;
          uint32_t samples = 0;
          bool valid = true;
          const int first = std::max<int>(0, int(column) -
            int(config_.guardCells + config_.trainingCells));
          const int leftEnd = std::max<int>(0, int(column) - int(config_.guardCells));
          const int rightBegin = std::min<int>(columns, int(column) + int(config_.guardCells) + 1);
          const int last = std::min<int>(columns, int(column) +
            int(config_.guardCells + config_.trainingCells) + 1);
          if (finiteRow) {
            covariance = covarianceSums[column];
            samples = uint32_t(leftEnd - first + last - rightBegin);
          } else {
            for (int training = first; training < leftEnd; ++training) {
              if (!finitePower[training]) { valid = false; break; }
              covariance += covarianceTerms[training]; ++samples;
            }
            if (valid)
              for (int training = rightBegin; training < last; ++training) {
                if (!finitePower[training]) { valid = false; break; }
                covariance += covarianceTerms[training]; ++samples;
              }
          }
          valid = valid && samples >= 2;
          double statistic = 0;
          uint8_t family = 0;
          if (valid)
          {
            covariance /= double(samples);
            const double scale = covariance;
            const auto cut = maps.front()->data[row][column];
            valid = scale > 0 && std::isfinite(scale) &&
              std::isfinite(cut.real()) && std::isfinite(cut.imag());
            if (valid)
            {
              const auto normalized = cut / std::sqrt(scale);
              statistic = std::norm(normalized) /
                (1.0 + config_.diagonalLoading);
              valid = statistic >= 0 && std::isfinite(statistic);
              if (valid) family = 1;
            }
          }
          trainedByRow[row] += family == 1;
          result.statisticFamily[uint64_t(row) * columns + column] = family;
          result.map->data[row][column] = {
            std::sqrt(std::max(0.0, statistic)), 0};
          if (levels) levels[uint64_t(row - begin) * columns + column] =
            Map<std::complex<double>>::MetricAccumulator::level(result.map->data[row][column]);
          else metrics.add(result.map->data[row][column]);
        }
      }
      };
      if (budget && budget->executor)
        integration::orderedRowTiles(rows, columns, 1, *budget, compute,
          [&](uint32_t begin, uint32_t end, const double* levels) {
            for (uint64_t cell = 0; cell < uint64_t(end - begin) * columns; ++cell)
              metrics.addLevel(levels[cell]);
          });
      else compute(0, rows, nullptr);
      for (const auto trained : trainedByRow) result.trainedCells += trained;
      result.invalidCells = uint64_t(rows) * columns - result.trainedCells;
      metrics.apply(*result.map);
      return result;
    }
    using Matrix = std::vector<std::complex<double>>;
    for (uint32_t row = 0; row < rows; ++row)
      for (uint32_t column = 0; column < columns; ++column)
      {
        if (budget && column % 32 == 0) budget->check();
        Matrix covariance(channels * channels, std::complex<double>{});
        std::vector<std::vector<std::complex<double>>> trainingVectors;
        uint32_t samples = 0;
        const int first = std::max<int>(0, int(column) -
          int(config_.guardCells + config_.trainingCells));
        const int last = std::min<int>(int(columns) - 1, int(column) +
          int(config_.guardCells + config_.trainingCells));
        for (int training = first; training <= last; ++training)
        {
          if (std::abs(training - int(column)) <= int(config_.guardCells))
            continue;
          std::vector<std::complex<double>> observation(channels);
          for (std::size_t channel = 0; channel < channels; ++channel)
            observation[channel] = maps[channel]->data[row][training];
          trainingVectors.push_back(std::move(observation));
          ++samples;
        }
        std::vector<std::complex<double>> cut(channels);
        for (std::size_t channel = 0; channel < channels; ++channel)
          cut[channel] = maps[channel]->data[row][column];

        double statistic = 0;
        uint8_t family = 0;
        bool valid = samples >= std::max<uint32_t>(2, uint32_t(channels));
        if (valid)
        {
          std::vector<double> powers;
          powers.reserve(trainingVectors.size());
          for (const auto& observation : trainingVectors)
          {
            double power = 0;
            for (const auto value : observation) power += std::norm(value);
            if (!std::isfinite(power)) { valid = false; break; }
            powers.push_back(power / double(channels));
          }
          double weightSum = 0;
          if (valid && config_.covariance == Covariance::robustShrinkage)
          {
            auto orderedPowers = powers;
            std::sort(orderedPowers.begin(), orderedPowers.end());
            const double median = orderedPowers[orderedPowers.size() / 2];
            valid = median > 0 && std::isfinite(median);
            if (valid)
              for (std::size_t sample = 0; sample < trainingVectors.size(); ++sample)
              {
                const double weight = std::min(1.0,
                  config_.robustClip * median / powers[sample]);
                weightSum += weight;
                for (std::size_t left = 0; left < channels; ++left)
                  for (std::size_t right = 0; right < channels; ++right)
                    covariance[left * channels + right] += weight *
                      trainingVectors[sample][left] *
                      std::conj(trainingVectors[sample][right]);
              }
          }
          else if (valid)
          {
            weightSum = double(trainingVectors.size());
            for (const auto& observation : trainingVectors)
              for (std::size_t left = 0; left < channels; ++left)
                for (std::size_t right = 0; right < channels; ++right)
                  covariance[left * channels + right] += observation[left] *
                    std::conj(observation[right]);
          }
          valid = valid && weightSum >= double(channels) &&
            std::isfinite(weightSum);
          if (!valid)
          {
            // Leave this CUT explicitly invalid; do not skip its accounting.
          }
          else
          {
          for (auto& value : covariance) value /= weightSum;
          double trace = 0;
          for (std::size_t channel = 0; channel < channels; ++channel)
            trace += covariance[channel * channels + channel].real();
          const double scale = trace / double(channels);
          valid = scale > 0 && std::isfinite(scale);
          if (valid)
          {
            // Normalize both covariance and CUT before solving. The output is
            // invariant to a global IQ scale and all conditioning checks are
            // relative to a matrix whose average diagonal is one.
            const double rootScale = std::sqrt(scale);
            for (auto& value : covariance) value /= scale;
            for (auto& value : cut) value /= rootScale;
            double diagonalFallback = 0;
            bool fallbackValid = true;
            for (std::size_t channel = 0; channel < channels; ++channel)
            {
              const double variance =
                covariance[channel * channels + channel].real() +
                config_.diagonalLoading;
              if (!(variance > 0) || !std::isfinite(variance))
              {
                fallbackValid = false;
                break;
              }
              diagonalFallback += std::norm(cut[channel]) / variance;
            }
            const double offDiagonalScale = config_.covariance == Covariance::diagonal ?
              0.0 : config_.covariance == Covariance::shrinkage ?
              1.0 - config_.shrinkage :
              1.0 - std::clamp(double(channels) / (weightSum + double(channels)),
                config_.robustShrinkageFloor, config_.robustShrinkageCeiling);
            for (std::size_t left = 0; left < channels; ++left)
              for (std::size_t right = 0; right < channels; ++right)
                if (left != right)
                  covariance[left * channels + right] *= offDiagonalScale;
            for (std::size_t channel = 0; channel < channels; ++channel)
              covariance[channel * channels + channel] +=
                config_.diagonalLoading;
            std::vector<std::complex<double>> solved;
            valid = solve(covariance, cut, channels, solved);
            if (valid)
            {
              std::complex<double> energy{};
              for (std::size_t channel = 0; channel < channels; ++channel)
                energy += std::conj(cut[channel]) * solved[channel];
              const double candidate = energy.real();
              valid = candidate >= 0 && std::isfinite(candidate) &&
                std::abs(energy.imag()) <=
                  1e-7 * std::max(1.0, candidate);
              if (valid)
              {
                statistic = candidate;
                family = 1;
              }
            }
            // A failed shrinkage solve remains in the same dimensionless
            // family. Never leak a partially validated solve or raw CUT power.
            if (!valid && fallbackValid &&
                std::isfinite(diagonalFallback) && diagonalFallback >= 0)
            {
              statistic = diagonalFallback;
              family = 2;
              valid = true;
            }
          }
          }
        }
        if (family == 1) ++result.trainedCells;
        else if (family == 2) ++result.fallbackCells;
        else ++result.invalidCells;
        result.statisticFamily[uint64_t(row) * columns + column] = family;
        // Map amplitude is sqrt(E), matching the existing detector's map
        // convention while leaving the unthresholded statistic available.
        result.map->data[row][column] = {
          std::sqrt(std::max(0.0, statistic)), 0};
      }
    result.map->set_metrics();
    return result;
  }

private:
  Config config_;

  static bool solve(std::vector<std::complex<double>> matrix,
    std::vector<std::complex<double>> right, std::size_t size,
    std::vector<std::complex<double>>& result)
  {
    double matrixScale = 0;
    for (const auto value : matrix)
      matrixScale = std::max(matrixScale, std::abs(value));
    const double epsilon = 64 * std::numeric_limits<double>::epsilon() *
      std::max(1.0, matrixScale) * double(size);
    for (std::size_t pivot = 0; pivot < size; ++pivot)
    {
      std::size_t choice = pivot;
      double magnitude = std::abs(matrix[pivot * size + pivot]);
      for (std::size_t row = pivot + 1; row < size; ++row)
        if (std::abs(matrix[row * size + pivot]) > magnitude)
        {
          choice = row;
          magnitude = std::abs(matrix[row * size + pivot]);
        }
      if (!(magnitude > epsilon) || !std::isfinite(magnitude)) return false;
      if (choice != pivot)
      {
        for (std::size_t column = 0; column < size; ++column)
          std::swap(matrix[pivot * size + column],
            matrix[choice * size + column]);
        std::swap(right[pivot], right[choice]);
      }
      const auto divisor = matrix[pivot * size + pivot];
      for (std::size_t column = pivot; column < size; ++column)
        matrix[pivot * size + column] /= divisor;
      right[pivot] /= divisor;
      for (std::size_t row = 0; row < size; ++row)
        if (row != pivot)
        {
          const auto multiplier = matrix[row * size + pivot];
          for (std::size_t column = pivot; column < size; ++column)
            matrix[row * size + column] -=
              multiplier * matrix[pivot * size + column];
          right[row] -= multiplier * right[pivot];
        }
    }
    result = std::move(right);
    return std::all_of(result.begin(), result.end(), [](const auto value) {
      return std::isfinite(value.real()) && std::isfinite(value.imag());
    });
  }
};

} // namespace blah2::detection

#endif
