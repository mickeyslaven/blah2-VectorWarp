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
  : waveNumber_(TWO_PI * config.frequencyHz / SPEED_OF_LIGHT),
    directPathAzimuthRad_(config.directPathAzimuthRad),
    directPathNullDb_(config.directPathNullDb),
    positionsM_(config.positionsM),
    measuredResponses_(config.measuredResponses)
{
  if (!std::isfinite(config.frequencyHz) || config.frequencyHz <= 0)
    throw std::invalid_argument("Coherent fusion requires a positive RF frequency");
  if (config.beams < 4 || config.beams > 360)
    throw std::invalid_argument("Coherent fusion beam count must be from 4 through 360");
  if (config.positionsM.empty())
    throw std::invalid_argument("Coherent fusion requires antenna positions");
  if (!std::isfinite(config.directPathAzimuthRad))
    throw std::invalid_argument("Coherent fusion direct-path azimuth must be finite");
  if (!std::isfinite(config.directPathNullDb) || config.directPathNullDb < 0 ||
      config.directPathNullDb > 40)
    throw std::invalid_argument("Coherent fusion direct-path null must be from 0 through 40 dB");
  if (!measuredResponses_.empty())
  {
    const uint64_t expected = uint64_t(config.beams) * config.positionsM.size();
    if (expected != measuredResponses_.size())
      throw std::invalid_argument("Coherent fusion measured manifold dimensions do not match the beam bank");
    for (const auto response : measuredResponses_)
      if (!std::isfinite(response.real()) || !std::isfinite(response.imag()) ||
          !(std::norm(response) > 1e-12))
        throw std::invalid_argument("Coherent fusion measured manifold contains a nonfinite or zero response");
    const double direct = std::fmod(std::fmod(config.directPathAzimuthRad, TWO_PI) +
      TWO_PI, TWO_PI);
    measuredDirectBeam_ = std::size_t(std::llround(direct * config.beams / TWO_PI)) %
      config.beams;
  }

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
      const double phase = waveNumber_ * (position[0] * ux + position[1] * uy);
      weights.push_back(normalization * std::polar(1.0, phase));
    }
  }
}

void CoherentBeamBank::calibrate(
  const std::vector<std::complex<double>>& directPathWeights)
{
  if (directPathWeights.size() != positionsM_.size())
    throw std::invalid_argument("Coherent calibration channel count does not match antenna geometry");
  double norm = 0;
  for (const auto weight : directPathWeights)
  {
    if (!std::isfinite(weight.real()) || !std::isfinite(weight.imag()))
      throw std::invalid_argument("Coherent calibration weights must be finite");
    norm += std::norm(weight);
  }
  if (!(norm > 0) || !std::isfinite(norm))
    throw std::invalid_argument("Coherent calibration weights have zero norm");
  const double scale = 1.0 / std::sqrt(norm);
  const double directX = std::cos(directPathAzimuthRad_);
  const double directY = std::sin(directPathAzimuthRad_);
  const double nullFraction = 1.0 - std::pow(10.0, -directPathNullDb_ / 20.0);
  for (std::size_t beam = 0; beam < weights_.size(); ++beam)
  {
    const double azimuth = TWO_PI * double(beam) / double(weights_.size());
    const double targetX = std::cos(azimuth);
    const double targetY = std::sin(azimuth);
    std::vector<std::complex<double>> steering(positionsM_.size());
    std::complex<double> projection{0, 0};
    if (measuredResponses_.empty())
    {
      for (std::size_t channel = 0; channel < positionsM_.size(); ++channel)
      {
        const auto& position = positionsM_[channel];
        const double relativePhase = waveNumber_ *
          (position[0] * (targetX - directX) +
           position[1] * (targetY - directY));
        steering[channel] = directPathWeights[channel] * std::polar(1.0, -relativePhase);
        projection += std::conj(directPathWeights[channel]) * steering[channel];
      }
    }
    else
    {
      double steeringNorm = 0;
      for (std::size_t channel = 0; channel < positionsM_.size(); ++channel)
      {
        const auto target = measuredResponses_[beam * positionsM_.size() + channel];
        const auto direct = measuredResponses_[measuredDirectBeam_ * positionsM_.size() + channel];
        steering[channel] = directPathWeights[channel] * target / direct;
        steeringNorm += std::norm(steering[channel]);
      }
      if (!(steeringNorm > 0) || !std::isfinite(steeringNorm))
        throw std::invalid_argument("Coherent fusion measured steering has zero norm");
      const double steeringScale = std::sqrt(norm / steeringNorm);
      for (std::size_t channel = 0; channel < positionsM_.size(); ++channel)
      {
        steering[channel] *= steeringScale;
        projection += std::conj(directPathWeights[channel]) * steering[channel];
      }
    }
    projection *= nullFraction / norm;
    for (std::size_t channel = 0; channel < positionsM_.size(); ++channel)
      weights_[beam][channel] = scale *
        std::conj(steering[channel] - projection * directPathWeights[channel]);
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
        if (power > bestPower) bestPower = power;
      }
      // Match the existing noncoherent fusion contract: downstream stages
      // consume a real, nonnegative amplitude rather than a complex sample.
      fused->data[row][column] = {std::sqrt(bestPower), 0};
    }
  return fused;
}

std::vector<double> CoherentBeamBank::powerBankAt(
  const std::vector<Map<std::complex<double>> *>& maps,
  uint32_t row, uint32_t column) const
{
  if (maps.empty() || maps.size() != weights_.front().size())
    throw std::invalid_argument("Coherent fusion map count does not match antenna geometry");
  if (row >= maps.front()->get_nRows() || column >= maps.front()->get_nCols())
    throw std::out_of_range("Coherent fusion cell is outside the ambiguity map");
  for (auto* map : maps)
    if (!map || map->get_nRows() != maps.front()->get_nRows() ||
        map->get_nCols() != maps.front()->get_nCols())
      throw std::invalid_argument("Ambiguity-map dimensions do not match");
  std::vector<std::complex<double>> channels;
  channels.reserve(maps.size());
  for (auto* map : maps) channels.push_back(map->data[row][column]);
  return powerBank(channels);
}

std::vector<double> CoherentBeamBank::powerBank(
  const std::vector<std::complex<double>>& channels) const
{
  if (channels.size() != weights_.front().size())
    throw std::invalid_argument("Coherent fusion channel vector does not match antenna geometry");
  std::vector<double> powers;
  powers.reserve(weights_.size());
  for (const auto& weights : weights_)
  {
    std::complex<double> beamValue{0, 0};
    for (std::size_t channel = 0; channel < channels.size(); ++channel)
      beamValue += weights[channel] * channels[channel];
    const double power = std::norm(beamValue);
    if (!std::isfinite(power))
      throw std::runtime_error("Coherent fusion produced non-finite beam power");
    powers.push_back(power);
  }
  return powers;
}
