#pragma once

#include "data/Map.h"
#include <array>
#include <limits>
#include <utility>

namespace blah2::integration {

// Offline numerical experiment, consumed only by rangeMigrationReplayInspect
// and its synthetic contract test. No runtime stage/configuration is added.
// Ambiguity stores FFTshift(DFT_b sum_n y[n] conj(x[n-delay])). Positive
// Doppler is exp(+i*2*pi*f*t); positive rate is increasing excess delay.
// A trial samples r[b, delay + rate*(b-(B-1)/2)*nCorr/fs], then repeats
// Ambiguity's forward DFT. Delay coordinates refer to the CPI block midpoint.
// This corrects inter-block walk, NOT motion within a correlation block.
class RangeMigrationReplay {
public:
  using Complex = std::complex<double>;
  struct Bank {
    double delayRateBinsPerSecond = 0;
    // Fixed-cell white range-noise variance relative to the zero-rate CAF.
    // Keep raw canonical amplitudes; compare |z|^2 / whiteNoiseGain.
    double whiteNoiseGain = 1;
    std::vector<Complex> cells; // Centered Doppler, then valid delay column.
  };
  struct Result {
    uint32_t firstColumn = 0, columns = 0, rows = 0;
    uint64_t complexMacBound = 0, evaluatedCells = 0;
    std::vector<Bank> banks; // All trials; no target-selected winner.
  };
  static constexpr uint32_t radius = 8, maximumTrials = 9;
  static constexpr uint64_t maximumMacs = 1u << 28;

  // Prepared once per constant fractional shift, then reused across a row.
  // Construction is closed: callers cannot supply inconsistent taps or gain.
  class ProfileKernel {
    friend class RangeMigrationReplay;
    double shift_ = 0, whiteNoiseGain_ = 0;
    int64_t first_ = 0;
    std::array<Complex, 2 * radius> weights_{};
    ProfileKernel() = default;
  public:
    double shift() const { return shift_; }
    double whiteNoiseGain() const { return whiteNoiseGain_; }
    int64_t firstOffset() const { return first_; }
    int64_t lastOffset() const { return first_ + 2 * radius - 1; }
  };

  // SAME finite-support Lanczos owner as migration. centerHz is the known
  // heterodyne range ramp, not target Doppler. Output retains the ramp at the
  // SAMPLED coordinate (integer column + shift). To express it at the integer
  // output column instead, the caller applies exp(-i*2*pi*centerHz*shift/fs)
  // once per path, not once per cell. No nearest-cell selection or phase fit.
  static ProfileKernel prepareProfile(double shift, double centerHz, uint32_t sampleRate) {
    if (!sampleRate || !std::isfinite(shift) || !std::isfinite(centerHz) ||
        std::abs(shift) > std::numeric_limits<uint32_t>::max())
      throw std::invalid_argument("Complex interpolation kernel is invalid");
    const auto scalar = interpolation(shift);
    ProfileKernel kernel;
    kernel.shift_ = shift;
    kernel.first_ = scalar.first;
    for (size_t tap = 0; tap < kernel.weights_.size(); ++tap) {
      const double phase = -2 * std::acos(-1.) * (centerHz / sampleRate) *
        (double(scalar.first + int64_t(tap)) - shift);
      if (!std::isfinite(phase))
        throw std::invalid_argument("Complex interpolation phase is not finite");
      kernel.weights_[tap] = scalar.weights[tap] * std::polar(1., phase);
      kernel.whiteNoiseGain_ += scalar.weights[tap] * scalar.weights[tap];
    }
    return kernel;
  }

  static Complex sampleProfile(const std::vector<Complex>& profiles, uint32_t columns,
      uint32_t row, uint32_t column, const ProfileKernel& kernel) {
    const double coordinate = double(column) + kernel.shift_;
    if (!columns || profiles.size() % columns || row >= profiles.size() / columns ||
        coordinate < radius || coordinate + radius >= columns)
      throw std::invalid_argument("Complex coordinate lacks interpolation support");
    Complex value{};
    for (size_t tap = 0; tap < kernel.weights_.size(); ++tap) {
      const auto source = int64_t(column) + kernel.first_ + int64_t(tap);
      const auto z = profiles[uint64_t(row) * columns + size_t(source)];
      if (!std::isfinite(z.real()) || !std::isfinite(z.imag()))
        throw std::invalid_argument("Complex interpolation input is not finite");
      value += z * kernel.weights_[tap];
    }
    if (!std::isfinite(value.real()) || !std::isfinite(value.imag()))
      throw std::invalid_argument("Complex interpolation result is not finite");
    return value;
  }

  static Complex sampleProfile(const std::vector<Complex>& profiles, uint32_t columns,
      uint32_t row, double column, double centerHz, uint32_t sampleRate) {
    if (!std::isfinite(column) || column < 0 ||
        column > std::numeric_limits<uint32_t>::max())
      throw std::invalid_argument("Complex interpolation coordinate is invalid");
    const auto base = uint32_t(std::floor(column));
    return sampleProfile(profiles, columns, row, base,
      prepareProfile(column - base, centerHz, sampleRate));
  }

  static Result process(const Map<Complex>& caf, uint32_t sampleRate,
      uint32_t correlationSamples, const std::vector<double>& rates) {
    const auto rows = caf.data.size(), columns = caf.delay.size();
    if (!sampleRate || !correlationSamples || rows < 3 || rows > 511 ||
        rows % 2 != 1 || columns <= 2 * radius || columns > 4096 ||
        rows * columns > 65536 || caf.doppler.size() != rows ||
        rates.empty() || rates.size() > maximumTrials)
      throw std::invalid_argument("Range migration requires a bounded full complex CAF");
    const double dt = double(correlationSamples) / sampleRate;
    const double spacing = 1.0 / (dt * rows), middle = caf.doppler[rows / 2];
    for (size_t row = 0; row < rows; ++row) {
      const double expected = middle + (double(row) - double(rows / 2)) * spacing;
      if (caf.data[row].size() != columns || !std::isfinite(caf.doppler[row]) ||
          std::abs(caf.doppler[row] - expected) > spacing * 1e-8)
        throw std::invalid_argument("Range migration rejects cropped/nonuniform Doppler axes");
      for (const auto value : caf.data[row])
        if (!std::isfinite(value.real()) || !std::isfinite(value.imag()) ||
            std::abs(value) > 1e100)
          throw std::invalid_argument("Range migration CAF sample invalid");
    }
    for (size_t column = 1; column < columns; ++column)
      if (int64_t(caf.delay[column]) != int64_t(caf.delay[0]) + int64_t(column))
        throw std::invalid_argument("Range migration needs contiguous sample-delay bins");
    double maximumShift = 0;
    bool hasBaseline = false;
    for (size_t trial = 0; trial < rates.size(); ++trial) {
      const auto rate = rates[trial];
      if (!std::isfinite(rate) || std::abs(rate) * dt > .25 ||
          std::find(rates.begin(), rates.begin() + trial, rate) != rates.begin() + trial)
        throw std::invalid_argument("Range migration rate duplicated or exceeds quarter-bin/block bound");
      hasBaseline |= rate == 0;
      maximumShift = std::max(maximumShift, std::abs(rate) * dt * (rows - 1) / 2);
    }
    if (!hasBaseline || maximumShift > 32)
      throw std::invalid_argument("Range migration needs zero-rate baseline and bounded walk");
    Result result;
    result.firstColumn = radius + uint32_t(std::ceil(maximumShift));
    if (2 * result.firstColumn >= columns)
      throw std::invalid_argument("Range migration has no common interpolation support");
    result.rows = uint32_t(rows);
    result.columns = uint32_t(columns) - 2 * result.firstColumn;
    result.evaluatedCells = rates.size() * rows * result.columns;
    result.complexMacBound = rows * rows * columns + rates.size() *
      rows * result.columns * (rows + 2 * radius);
    if (result.complexMacBound > maximumMacs)
      throw std::invalid_argument("Range migration exceeds offline work bound");

    // A small direct DFT avoids another FFT planner/worker lifecycle. The
    // declared operation ceiling is checked before any transform allocation.
    std::vector<Complex> inverse(rows * rows), profiles(rows * columns);
    const double tau = 2 * std::acos(-1.0);
    for (size_t time = 0; time < rows; ++time)
      for (size_t frequency = 0; frequency < rows; ++frequency)
        inverse[time * rows + frequency] = std::polar(1.0,
          tau * (double(frequency) - double(rows / 2)) * double(time) / rows);
    for (size_t time = 0; time < rows; ++time)
      for (size_t frequency = 0; frequency < rows; ++frequency)
        for (size_t column = 0; column < columns; ++column)
          profiles[time * columns + column] += caf.data[frequency][column] *
            (inverse[time * rows + frequency] / double(rows));

    for (const double rate : rates) {
      Bank bank;
      bank.delayRateBinsPerSecond = rate;
      bank.cells.resize(rows * result.columns);
      if (rate == 0) {
        for (size_t row = 0; row < rows; ++row)
          std::copy_n(caf.data[row].begin() + result.firstColumn, result.columns,
            bank.cells.begin() + row * result.columns);
      } else {
        bank.whiteNoiseGain = 0;
        for (size_t time = 0; time < rows; ++time) {
          const double shift = rate * dt * (double(time) - double(rows - 1) / 2);
          const auto kernel = interpolation(shift);
          std::array<Complex, 2 * radius> weights;
          double norm = 0;
          for (size_t tap = 0; tap < weights.size(); ++tap) {
            norm += kernel.weights[tap] * kernel.weights[tap];
            // Reference heterodyning leaves exp(+i*2*pi*middle*delay/fs)
            // on the range correlation. De-ramp before interpolation and
            // re-ramp at the OUTPUT delay to avoid inventing Doppler motion.
            weights[tap] = kernel.weights[tap] * std::polar(1.0,
              -tau * middle * double(kernel.first + int64_t(tap)) / sampleRate);
          }
          bank.whiteNoiseGain += norm / rows;
          for (size_t column = 0; column < result.columns; ++column) {
            Complex aligned{};
            for (size_t tap = 0; tap < kernel.weights.size(); ++tap) {
              const auto source = int64_t(column + result.firstColumn) + kernel.first + int64_t(tap);
              aligned += weights[tap] * profiles[time * columns + size_t(source)];
            }
            for (size_t frequency = 0; frequency < rows; ++frequency)
              bank.cells[frequency * result.columns + column] += aligned *
                std::conj(inverse[time * rows + frequency]);
          }
        }
      }
      result.banks.push_back(std::move(bank));
    }
    return result;
  }

private:
  struct Kernel { int64_t first; std::array<double, 2 * radius> weights{}; };
  static Kernel interpolation(double shift) {
    Kernel result{int64_t(std::floor(shift)) - int64_t(radius) + 1, {}};
    if (shift == std::round(shift)) {
      result.weights[radius - 1] = 1;
      return result;
    }
    const auto sinc = [](double x) {
      if (x == 0) return 1.0;
      const double angle = std::acos(-1.0) * x;
      return std::sin(angle) / angle;
    };
    double sum = 0;
    for (size_t tap = 0; tap < result.weights.size(); ++tap) {
      const double delta = shift - double(result.first + int64_t(tap));
      result.weights[tap] = sinc(delta) * sinc(delta / radius);
      sum += result.weights[tap];
    }
    // DC-preserving Lanczos-8 interpolation. Noise gain is reported, never
    // mistaken for a physical reduction of noise or a CFAR calibration.
    for (auto& weight : result.weights) weight /= sum;
    return result;
  }
};
} // namespace blah2::integration
