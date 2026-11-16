#pragma once

#include <armadillo>
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace blah2::clutter_refinement {

// Host FP64 covariance factor and solve for portable FP32 GPU RHS correlations. Use the same
// LAPACK-backed dense covariance algebra as WienerHopf, without requiring
// shaderFloat64, changing the covariance, or permitting approximate solves.
class DenseSolver {
  friend struct DenseSolverTestAccess;
  using Double = std::complex<double>;
  using Float = std::complex<float>;
  uint32_t samples_, bins_, channels_;
  std::vector<Double> column_;
  arma::cx_mat lower_, upper_, rhs_, intermediate_, solved_;
  bool ready_ = false;
  double smallest_ = 0, largest_ = 0;
  uint64_t compatibilityAttempts_ = 0, compatibilityFactors_ = 0;

  static bool finite(Double value) {
    return std::isfinite(value.real()) && std::isfinite(value.imag());
  }
  void factorLegacy(double scale) {
    // A failed Armadillo factor may resize/overwrite its output. Rebuild from
    // the untouched rounded covariance, never from the partial LAPACK factor.
    lower_.zeros(bins_, bins_);
    for (uint32_t row = 0; row < bins_; ++row)
      for (uint32_t col = 0; col <= row; ++col) {
        Double sum = column_[row-col];
        // Deliberately retain the pre-LAPACK row-wise subtraction order and
        // FP64 complex arithmetic. No loading, clipping, or precision change.
        for (uint32_t k = 0; k < col; ++k)
          sum -= lower_(row,k) * std::conj(lower_(col,k));
        if (row == col) {
          if (!std::isfinite(sum.real()) || sum.real() <= scale * 1e-12 ||
              std::abs(sum.imag()) > scale * 1e-5) {
            std::ostringstream detail;
            detail << "GPU clutter legacy Cholesky failed; guard=cholesky pivot row="
              << row << " pivot_real_relative=" << sum.real()/scale
              << " diagonal_imag_relative=" << std::abs(sum.imag())/scale;
            throw std::domain_error(detail.str());
          }
          lower_(row,col) = Double(std::sqrt(sum.real()), 0);
        } else {
          lower_(row,col) = sum / lower_(col,col);
        }
      }
  }
  void factorColumn(bool (*cholesky)(arma::cx_mat&)) {
    for (uint32_t i = 0; i < bins_; ++i) {
      if (!finite(column_[i]))
        throw std::domain_error("Nonfinite GPU clutter covariance; guard=cholesky");
    }
    const double scale = std::max(std::abs(column_[0]), 1e-30);
    // LAPACK treats a Hermitian diagonal as real. Retain the original
    // imaginary-diagonal and absolute-pivot rejection before that happens.
    if (column_[0].real() <= scale * 1e-12 ||
        std::abs(column_[0].imag()) > scale * 1e-5)
      throw std::domain_error("GPU clutter Cholesky failed; guard=cholesky diagonal");
    lower_.set_size(bins_, bins_);
    const Double diagonal(column_[0].real(), 0);
    for (uint32_t col = 0; col < bins_; ++col)
    {
      // Armadillo is column-major. Fill the two contiguous column ranges
      // without a branch per element; this constructs the identical Hermitian
      // Toeplitz matrix consumed by the unchanged FP64 Cholesky routine.
      for (uint32_t row = 0; row < col; ++row)
        lower_(row, col) = std::conj(column_[col-row]);
      lower_(col, col) = diagonal;
      for (uint32_t row = col + 1; row < bins_; ++row)
        lower_(row, col) = column_[row-col];
    }
    const bool compatibility = !cholesky(lower_);
    if (compatibility) {
      ++compatibilityAttempts_;
      factorLegacy(scale);
    }
    smallest_ = std::numeric_limits<double>::infinity(); largest_ = 0;
    for (uint32_t row = 0; row < bins_; ++row) {
      const Double value = lower_(row, row);
      const double pivot = value.real();
      if (!finite(value) || pivot <= 0 || pivot * pivot <= scale * 1e-12)
        throw std::domain_error("GPU clutter Cholesky failed; guard=cholesky pivot");
      smallest_ = std::min(smallest_, pivot); largest_ = std::max(largest_, pivot);
    }
    if (smallest_ < largest_ * 1e-4)
      throw std::domain_error("GPU clutter correlation is ill-conditioned; guard=conditioning");
    // Cache the conjugate transpose once per reference. Both correction
    // solves reuse it instead of allocating a dense transpose for each RHS.
    upper_ = lower_.t();
    ready_ = true;
    if (compatibility) ++compatibilityFactors_;
  }
  // Private test seam permits a deterministic destructive LAPACK failure;
  // production has no setting that can skip or replace the optimized factor.
  void factorWith(const Float* correlations, bool (*cholesky)(arma::cx_mat&)) {
    invalidate();
    if (!correlations) throw std::invalid_argument("Missing GPU clutter covariance");
    for (uint32_t i = 0; i < bins_; ++i)
      column_[i] = Double(correlations[i]) / double(samples_);
    factorColumn(cholesky);
  }
public:
  DenseSolver(uint32_t samples, uint32_t bins, uint32_t channels)
    : samples_(samples), bins_(bins), channels_(channels) {
    const uint64_t capacity = std::numeric_limits<size_t>::max() / sizeof(Double);
    const uint64_t vectors = uint64_t(bins) * (1 + 3 * uint64_t(channels));
    if (!samples || !bins || bins > samples || !channels || channels > 8 ||
        vectors > capacity || uint64_t(bins) * bins > (capacity-vectors)/2)
      throw std::invalid_argument("Invalid GPU clutter dense solver geometry");
    column_.resize(bins);
    lower_.set_size(bins, bins); upper_.set_size(bins, bins);
    rhs_.set_size(bins, channels); intermediate_.set_size(bins, channels);
    solved_.set_size(bins, channels);
  }
  void invalidate() noexcept { ready_ = false; }
  size_t storageBytes() const {
    return (column_.size() + 2 * size_t(bins_) * bins_ +
      3 * size_t(bins_) * channels_) * sizeof(Double);
  }
  uint64_t compatibilityAttempts() const noexcept { return compatibilityAttempts_; }
  uint64_t compatibilityFactors() const noexcept { return compatibilityFactors_; }
  void factor(const Float* correlations) {
    factorWith(correlations, [](arma::cx_mat& matrix) {
      return arma::chol(matrix, matrix, "lower");
    });
  }
  void factorCovariance(const Double* covariance, size_t count) {
    invalidate();
    if (!covariance || count != bins_)
      throw std::invalid_argument("GPU clutter FP64 covariance dimensions changed");
    // Host covariance is already sum(x[i]*conj(x[i-k])), unlike the GPU's
    // unnormalized inverse-FFT readback. Never cast to FP32 or divide by N here.
    std::copy_n(covariance, bins_, column_.begin());
    factorColumn([](arma::cx_mat& matrix) { return arma::chol(matrix, matrix, "lower"); });
  }
  const arma::cx_mat& solve(const Float* correlations) {
    if (!ready_) throw std::logic_error("GPU clutter covariance is not prepared");
    if (!correlations) throw std::invalid_argument("Missing GPU clutter correlations");
    // A failed solve invalidates the frame even when the caller catches it.
    ready_ = false;
    rhs_.set_size(bins_, channels_);
    for (size_t i = 0; i < rhs_.n_elem; ++i) {
      rhs_[i] = Double(correlations[bins_ + i]) / double(samples_);
      if (!finite(rhs_[i]))
        throw std::domain_error("Nonfinite GPU clutter RHS; guard=solve_residual");
    }
    const auto policy = arma::solve_opts::fast + arma::solve_opts::no_approx;
    if (!arma::solve(intermediate_, arma::trimatl(lower_), rhs_, policy) ||
        !arma::solve(solved_, arma::trimatu(upper_), intermediate_, policy) ||
        !solved_.is_finite())
      throw std::domain_error("GPU clutter triangular solve failed; guard=solve_residual");
    // Check against the original covariance (of the rounded reference), not L*L^H. Preserve
    // the old FP64 summation order and tolerance, including the tiny original
    // imaginary diagonal admitted by factor(). No covariance approximation.
    for (uint32_t channel = 0; channel < channels_; ++channel) {
      double residual = 0, rhsPower = 0;
      for (uint32_t row = 0; row < bins_; ++row) {
        Double calculated{};
        for (uint32_t col = 0; col < bins_; ++col)
          calculated += (row >= col ? column_[row-col] : std::conj(column_[col-row])) *
            solved_(col, channel);
        residual += std::norm(calculated - rhs_(row, channel));
        rhsPower += std::norm(rhs_(row, channel));
      }
      if (!std::isfinite(residual) || !std::isfinite(rhsPower) ||
          residual > std::max(rhsPower * 1e-10, 1e-20)) {
        std::ostringstream detail;
        detail << "GPU clutter solve residual is unstable; guard=solve_residual channel="
          << channel << " relative_rms=" << std::sqrt(residual / std::max(rhsPower, 1e-30))
          << " pivot_ratio=" << smallest_ / largest_;
        throw std::domain_error(detail.str());
      }
    }
    ready_ = true;
    return solved_;
  }
};
} // namespace blah2::clutter_refinement
