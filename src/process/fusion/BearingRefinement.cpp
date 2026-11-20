#include "BearingRefinement.h"
#include "process/ambiguity/BearingSteering.h"
#include "process/ambiguity/ResidentFusionWeights.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace blah2::fusion {
namespace {
using F = std::complex<float>;
using D = std::complex<double>;
using Matrix = std::array<D, 64>;
constexpr double twoPi = 6.283185307179586476925286766559;
template<class T> bool finite(std::complex<T> value) {
  return std::isfinite(value.real()) && std::isfinite(value.imag());
}
// Match explicit resident component operations. Maps/conventional beams stay
// FP32; only the small adaptive solve and its quadratic form use FP64.
template<class T> std::complex<T> multiply(std::complex<T> a, std::complex<T> b) {
  return {a.real() * b.real() - a.imag() * b.imag(),
    a.real() * b.imag() + a.imag() * b.real()};
}
template<class T> T power(std::complex<T> a) { return a.real() * a.real() + a.imag() * a.imag(); }
D divide(D a, D b) {
  const double denominator = power(b);
  return {(a.real() * b.real() + a.imag() * b.imag()) / denominator,
    (a.imag() * b.real() - a.real() * b.imag()) / denominator};
}
// Partial row pivoting, with first-row tie preference. A finite tiny pivot
// selects the specified conventional fallback; nonfinite algebra fails closed.
bool invert(Matrix& covariance, Matrix& inverse, uint32_t channels,
    BearingRefinement::Result& result) {
  for (uint32_t c = 0; c < channels; ++c) inverse[c * 8 + c] = {1, 0};
  for (uint32_t pivot = 0; pivot < channels; ++pivot) {
    uint32_t choice = pivot;
    double magnitude = power(covariance[pivot * 8 + pivot]);
    for (uint32_t row = pivot + 1; row < channels; ++row) {
      const double candidate = power(covariance[row * 8 + pivot]);
      if (!std::isfinite(candidate)) { result.outcome = BearingRefinement::Outcome::invalidInput; return false; }
      if (candidate > magnitude) { magnitude = candidate; choice = row; }
    }
    if (!std::isfinite(magnitude)) { result.outcome = BearingRefinement::Outcome::invalidInput; return false; }
    if (!(magnitude > 1e-20)) { result.singularFallback = true; return false; }
    if (choice != pivot) {
      ++result.pivotSwaps;
      for (uint32_t c = 0; c < channels; ++c) {
        std::swap(covariance[pivot * 8 + c], covariance[choice * 8 + c]);
        std::swap(inverse[pivot * 8 + c], inverse[choice * 8 + c]);
      }
    }
    const D divisor = covariance[pivot * 8 + pivot];
    for (uint32_t c = 0; c < channels; ++c) {
      covariance[pivot * 8 + c] = divide(covariance[pivot * 8 + c], divisor);
      inverse[pivot * 8 + c] = divide(inverse[pivot * 8 + c], divisor);
    }
    for (uint32_t row = 0; row < channels; ++row) if (row != pivot) {
      const D multiplier = covariance[row * 8 + pivot];
      for (uint32_t c = 0; c < channels; ++c) {
        covariance[row * 8 + c] -= multiply(multiplier, covariance[pivot * 8 + c]);
        inverse[row * 8 + c] -= multiply(multiplier, inverse[pivot * 8 + c]);
      }
    }
    for (uint32_t row = 0; row < channels; ++row)
      for (uint32_t c = 0; c < channels; ++c)
        if (!finite(covariance[row * 8 + c]) || !finite(inverse[row * 8 + c])) {
          result.outcome = BearingRefinement::Outcome::invalidInput; return false;
        }
  }
  return true;
}
}

BearingRefinement::BearingRefinement(Config config,
    const std::vector<std::complex<double>>& referenceWeights,
    const std::array<double, 8>& channelScale, bool whiten) : config_(std::move(config)) {
  const auto& c = config_;
  if (!c.channels || c.channels > 8 || c.beams < 4 || c.beams > 127 ||
      c.exclusionBins > c.beams || c.method > 1 || c.referenceAnchor > 2 ||
      c.guardRadius > 3 || c.trainingRadius <= c.guardRadius || c.trainingRadius > 6 ||
      !std::isfinite(c.diagonalLoading) || !(c.diagonalLoading > 0) || c.diagonalLoading > 1 ||
      !std::isfinite(c.frequencyHz) || !(c.frequencyHz > 0) ||
      !std::isfinite(c.radiusM) || !(c.radiusM > 0) ||
      !std::isfinite(c.element0Rad) || !std::isfinite(c.elementStepRad) ||
      !std::isfinite(c.directPathAzimuthRad) || !std::isfinite(c.directPathNullDb) ||
      c.directPathNullDb < 0 || c.directPathNullDb > 40 || referenceWeights.size() != c.channels ||
      (!c.channelMapping.empty() && c.channelMapping.size() != c.channels) ||
      (!c.measuredResponses.empty() && c.measuredResponses.size() != uint64_t(c.beams) * c.channels))
    throw std::invalid_argument("Bearing refinement configuration invalid");
  std::array<bool, 8> used{};
  double referencePower = 0;
  for (uint32_t channel = 0; channel < c.channels; ++channel) {
    const auto physical = bearing_steering::physicalElement(channel, c.channelMapping);
    if (physical >= c.channels || used[physical] || !finite(referenceWeights[channel]) ||
        !std::isfinite(channelScale[channel]) || !(channelScale[channel] > 0))
      throw std::invalid_argument("Bearing refinement channel/reference identity invalid");
    used[physical] = true;
    referencePower += std::norm(referenceWeights[channel]);
  }
  if (!(referencePower > 0) || !std::isfinite(referencePower))
    throw std::invalid_argument("Bearing refinement reference has zero/nonfinite norm");
  for (const auto response : c.measuredResponses)
    if (!finite(response) || !(power(response) > 1e-12f))
      throw std::invalid_argument("Bearing refinement measured response invalid");
  const double waveNumber = twoPi * c.frequencyHz / 299792458.0;
  const double directAngle = std::fmod(std::fmod(c.directPathAzimuthRad, twoPi) + twoPi, twoPi);
  const auto directBeam = uint32_t(std::llround(directAngle * c.beams / twoPi)) % c.beams;
  const bool measured = !c.measuredResponses.empty(), fixed = c.referenceAnchor != 0;
  std::array<std::complex<double>, 8> anchor{};
  for (uint32_t channel = 0; channel < c.channels; ++channel) {
    const auto physical = bearing_steering::physicalElement(channel, c.channelMapping);
    const double element = c.element0Rad + physical * c.elementStepRad;
    anchor[channel] = !fixed ? referenceWeights[channel] : measured ?
      std::complex<double>(c.measuredResponses[directBeam * c.channels + channel]) :
      std::polar(1.0, -waveNumber * c.radiusM * std::cos(element - c.directPathAzimuthRad));
  }
  weights_.resize(uint64_t(c.beams) * c.channels);
  const double nullFraction = 1.0 - std::pow(10.0, -c.directPathNullDb / 20.0);
  for (uint32_t beam = 0; beam < c.beams; ++beam) {
    const double angle = twoPi * beam / c.beams;
    std::array<std::complex<double>, 8> steering{};
    for (uint32_t channel = 0; channel < c.channels; ++channel) {
      if (measured) {
        const F target = c.measuredResponses[beam * c.channels + channel];
        const F direct = c.measuredResponses[directBeam * c.channels + channel];
        steering[channel] = fixed ? std::complex<double>(target) :
          referenceWeights[channel] * std::complex<double>(target / direct);
      } else {
        const auto physical = bearing_steering::physicalElement(channel, c.channelMapping);
        const double element = c.element0Rad + physical * c.elementStepRad;
        const double relative = waveNumber * c.radiusM *
          (std::cos(element - angle) - std::cos(element - c.directPathAzimuthRad));
        steering[channel] = anchor[channel] * std::polar(1.0, -relative);
      }
    }
    if (c.referenceAnchor == 2 && !bearing_steering::normalizeModel(
        steering.data(), c.channels, referenceWeights.data(), true)) continue;
    const auto combined = resident_fusion::calibratedWeights(steering, c.channels,
      anchor.data(), channelScale.data(), nullFraction, whiten);
    for (uint32_t channel = 0; channel < c.channels; ++channel)
      weights_[beam * c.channels + channel] = F(std::conj(combined[channel]));
  }
}

BearingRefinement::Result BearingRefinement::refineAt(
    const std::vector<Map<std::complex<double>>*>& maps, uint32_t row, uint32_t column) const {
  const auto channels = config_.channels;
  if (maps.size() != channels || !maps.front())
    throw std::invalid_argument("Bearing refinement map count invalid");
  const uint32_t rows = maps.front()->get_nRows(), columns = maps.front()->get_nCols();
  if (row >= rows || column >= columns)
    throw std::out_of_range("Bearing refinement source cell outside map");
  for (auto* map : maps)
    if (!map || map->get_nRows() != rows || map->get_nCols() != columns || map->data.size() != rows)
      throw std::invalid_argument("Bearing refinement map geometry invalid");
  Result result;
  const auto read = [&](uint32_t r, uint32_t col, std::array<F, 8>& values) {
    for (uint32_t channel = 0; channel < channels; ++channel) {
      if (maps[channel]->data[r].size() != columns)
        throw std::invalid_argument("Bearing refinement row geometry invalid");
      const auto value = maps[channel]->data[r][col];
      values[channel] = F(value);
      if (!finite(value) || !finite(values[channel])) return false;
    }
    return true;
  };
  std::array<F, 8> values{};
  std::array<D, 8> filtered{};
  if (!read(row, column, values)) { result.outcome = Outcome::invalidInput; return result; }
  Matrix covariance{}, inverse{};
  if (config_.method == 1) {
    const int training = int(config_.trainingRadius), guard = int(config_.guardRadius);
    for (int dr = -training; dr <= training; ++dr)
      for (int dc = -training; dc <= training; ++dc) {
        if (std::abs(dr) <= guard && std::abs(dc) <= guard) continue;
        const int64_t r = int64_t(row) + dr, col = int64_t(column) + dc;
        if (r < 0 || col < 0 || r >= rows || col >= columns) continue;
        std::array<F, 8> sample{};
        if (!read(uint32_t(r), uint32_t(col), sample)) { result.outcome = Outcome::invalidInput; return result; }
        for (uint32_t a = 0; a < channels; ++a)
          for (uint32_t b = 0; b < channels; ++b)
            covariance[a * 8 + b] += multiply(D(sample[a]), std::conj(D(sample[b])));
        ++result.covarianceSamples;
      }
    // Only this bounded <=8x8 solve widens. FP32 CAF values can have finite
    // covariance but overflowing FP32 squared pivots at ordinary raw-ADC scale.
    double trace = 0;
    for (uint32_t channel = 0; channel < channels; ++channel) trace += covariance[channel * 8 + channel].real();
    const double loading = std::max(trace / std::max(double(result.covarianceSamples * channels), 1.0) *
      config_.diagonalLoading, 1e-12);
    for (uint32_t a = 0; a < channels; ++a) {
      for (uint32_t b = 0; b < channels; ++b) covariance[a * 8 + b] /= std::max(double(result.covarianceSamples), 1.0);
      covariance[a * 8 + a].real(covariance[a * 8 + a].real() + loading);
    }
    result.adaptiveUsed = invert(covariance, inverse, channels, result);
    if (result.outcome == Outcome::invalidInput) return result;
  }
  if (result.adaptiveUsed)
    for (uint32_t a = 0; a < channels; ++a)
      for (uint32_t b = 0; b < channels; ++b) filtered[a] += multiply(inverse[a * 8 + b], D(values[b]));
  if (result.adaptiveUsed)
    for (uint32_t channel = 0; channel < channels; ++channel)
      if (!finite(filtered[channel])) { result.outcome = Outcome::invalidInput; return result; }
  std::array<float, 127> scores{};
  for (uint32_t beam = 0; beam < config_.beams; ++beam) {
    float modelPower = 0;
    F conventional{};
    D numerator{}, denominator{};
    for (uint32_t channel = 0; channel < channels; ++channel) {
      const F weight = weights_[beam * channels + channel];
      modelPower += power(weight);
      if (result.adaptiveUsed) numerator += multiply(D(weight), filtered[channel]);
      else conventional += multiply(weight, values[channel]);
    }
    if (!std::isfinite(modelPower)) { result.outcome = Outcome::invalidInput; return result; }
    if (!(modelPower > 1e-20f)) { scores[beam] = -1; continue; }
    if (result.adaptiveUsed)
      for (uint32_t a = 0; a < channels; ++a) {
        D projected{};
        for (uint32_t b = 0; b < channels; ++b)
          projected += multiply(inverse[a * 8 + b], std::conj(D(weights_[beam * channels + b])));
        denominator += multiply(D(weights_[beam * channels + a]), projected);
      }
    if (!finite(numerator) || !finite(denominator) || !finite(conventional) ||
        (result.adaptiveUsed && !(denominator.real() > 0))) {
      result.outcome = Outcome::invalidInput; return result;
    }
    // An absolute denominator floor destroys the MVDR gain-scale invariance.
    const float score = result.adaptiveUsed ? float(power(numerator) / denominator.real()) : power(conventional);
    if (!std::isfinite(score)) { result.outcome = Outcome::invalidInput; return result; }
    scores[beam] = score;
  }
  std::array<uint32_t, 3> selected{};
  for (uint32_t peak = 0; peak < 3; ++peak) {
    float best = -1;
    for (uint32_t beam = 0; beam < config_.beams; ++beam) {
      bool excluded = false;
      for (uint32_t prior = 0; prior < peak; ++prior) {
        const uint32_t distance = beam > selected[prior] ? beam - selected[prior] : selected[prior] - beam;
        excluded = excluded || std::min(distance, config_.beams - distance) <= config_.exclusionBins;
      }
      if (!excluded && scores[beam] > best) { best = scores[beam]; selected[peak] = beam; }
    }
    if (best > 0) { result.beams[peak] = selected[peak] + 1; result.powers[peak] = best; }
    if (!peak && !(best > 0)) return result;
  }
  const auto ratio = [&](uint32_t peak) {
    return result.powers[peak] > 0 ? uint32_t(std::clamp(std::floor(
      31.0 * double(result.powers[peak]) / double(result.powers[0]) + .5), 0.0, 31.0)) : 0u;
  };
  result.spatial = 0x80000000u | result.beams[0] | (result.beams[1] << 7u) |
    (result.beams[2] << 14u) | (ratio(1) << 21u) | (ratio(2) << 26u);
  result.outcome = Outcome::valid;
  return result;
}
}
