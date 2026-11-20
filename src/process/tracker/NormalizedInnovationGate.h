#ifndef NORMALIZED_INNOVATION_GATE_H
#define NORMALIZED_INNOVATION_GATE_H

#include <array>
#include <cmath>
#include <stdexcept>

namespace blah2::tracker
{

// CPU numerical contract for a two-dimensional delay/Doppler innovation gate.
// Covariances are row-major.  The area ceiling prevents uncertain tracks from
// buying arbitrarily large association volume even when their NIS is small.
class NormalizedInnovationGate
{
public:
  struct Config { double maximumNis = 9.210340371976184; double maximumArea = 100; double loading = 1e-9; };
  struct Result { bool valid = false; bool accepted = false; double nis = 0; double area = 0; };

  NormalizedInnovationGate() : NormalizedInnovationGate(Config{}) {}

  explicit NormalizedInnovationGate(Config config) : config_(config)
  {
    if (!(config_.maximumNis > 0) || !(config_.maximumArea > 0) ||
        !(config_.loading > 0) || !std::isfinite(config_.maximumNis) ||
        !std::isfinite(config_.maximumArea) || !std::isfinite(config_.loading))
      throw std::invalid_argument("Normalized innovation gate configuration invalid");
  }

  Result evaluate(const std::array<double, 2>& residual,
    const std::array<double, 4>& predictionCovariance,
    const std::array<double, 4>& measurementCovariance) const
  {
    const double a = predictionCovariance[0] + measurementCovariance[0] + config_.loading;
    const double b = .5 * (predictionCovariance[1] + predictionCovariance[2] +
      measurementCovariance[1] + measurementCovariance[2]);
    const double d = predictionCovariance[3] + measurementCovariance[3] + config_.loading;
    const double determinant = a * d - b * b;
    Result result;
    if (!(a > 0) || !(d > 0) || !(determinant > 0) ||
        !std::isfinite(determinant) || !std::isfinite(residual[0]) ||
        !std::isfinite(residual[1])) return result;
    result.nis = (d * residual[0] * residual[0] - 2 * b * residual[0] * residual[1] +
      a * residual[1] * residual[1]) / determinant;
    constexpr double pi = 3.14159265358979323846;
    result.area = pi * config_.maximumNis * std::sqrt(determinant);
    result.valid = std::isfinite(result.nis) && result.nis >= 0 && std::isfinite(result.area);
    result.accepted = result.valid && result.nis <= config_.maximumNis &&
      result.area <= config_.maximumArea;
    return result;
  }

private:
  Config config_;
};

} // namespace blah2::tracker

#endif
