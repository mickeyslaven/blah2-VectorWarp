#include "CoherentBeamBank.h"

#include <cmath>
#include <limits>
#include <stdexcept>

namespace
{
constexpr double SPEED_OF_LIGHT = 299792458.0;
constexpr double TWO_PI = 6.283185307179586476925286766559;
}

CoherentBeamBank::CoherentBeamBank(const Config& config)
{
  if (!std::isfinite(config.frequencyHz) || config.frequencyHz <= 0)
    throw std::invalid_argument("Coherent fusion requires a positive RF frequency");
  if (config.beams < 4 || config.beams > 360)
    throw std::invalid_argument("Coherent fusion beam count must be from 4 through 360");
  if (config.positionsM.empty())
    throw std::invalid_argument("Coherent fusion requires antenna positions");

  const double waveNumber = TWO_PI * config.frequencyHz / SPEED_OF_LIGHT;
  const double normalization = 1.0 / std::sqrt(double(config.positionsM.size()));
  weights_.resize(config.beams);
  for (uint32_t beam = 0; beam < config.beams; ++beam)
  {
    const double azimuth = TWO_PI * double(beam) / double(config.beams);
    const double ux = std::cos(azimuth);
    const double uy = std::sin(azimuth);
    auto& weights = weights_[beam];
    weights.reserve(config.positionsM.size());
    for (const auto& position : config.positionsM)
    {
      for (double coordinate : position)
        if (!std::isfinite(coordinate))
          throw std::invalid_argument("Coherent fusion antenna positions must be finite");
      const double phase = waveNumber * (position[0] * ux + position[1] * uy);
      weights.push_back(normalization * std::polar(1.0, phase));
    }
  }
}

std::unique_ptr<Map<std::complex<double>>> CoherentBeamBank::process(
  const std::vector<Map<std::complex<double>> *>& maps) const
{
  if (maps.empty() || maps.front() == nullptr || maps.size() != weights_.front().size())
    throw std::invalid_argument("Coherent fusion map count does not match antenna geometry");
  const uint32_t rows = maps.front()->get_nRows();
  const uint32_t columns = maps.front()->get_nCols();
  auto fused = std::make_unique<Map<std::complex<double>>>(rows, columns);
  fused->delay = maps.front()->delay;
  fused->doppler = maps.front()->doppler;
  for (auto *map : maps)
    if (!map || map->get_nRows() != rows || map->get_nCols() != columns)
      throw std::invalid_argument("Ambiguity-map dimensions do not match");

  for (uint32_t row = 0; row < rows; ++row)
    for (uint32_t column = 0; column < columns; ++column)
    {
      double bestPower = -1;
      for (const auto& weights : weights_)
      {
        std::complex<double> beamValue{0, 0};
        for (std::size_t channel = 0; channel < maps.size(); ++channel)
          beamValue += weights[channel] * maps[channel]->data[row][column];
        const double power = std::norm(beamValue);
        if (power > bestPower)
          bestPower = power;
      }
      // Match the existing noncoherent fusion contract: downstream stages
      // consume a real, nonnegative amplitude rather than a complex sample.
      fused->data[row][column] = {std::sqrt(bestPower), 0};
    }
  return fused;
}
