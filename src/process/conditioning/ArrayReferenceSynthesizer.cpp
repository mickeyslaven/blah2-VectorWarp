#include "ArrayReferenceSynthesizer.h"

#include <algorithm>
#include <cmath>
#include <deque>
#include <stdexcept>

namespace
{
void normalize(std::vector<std::complex<double>>& values)
{
  double power = 0;
  for (const auto& value : values) power += std::norm(value);
  const double norm = std::sqrt(std::max(power, 1e-30));
  for (auto& value : values) value /= norm;
}
}

ArrayReferenceSynthesizer::ArrayReferenceSynthesizer()
  : ArrayReferenceSynthesizer(Config{})
{
}

ArrayReferenceSynthesizer::ArrayReferenceSynthesizer(Config config)
  : config(config)
{
  if (config.analysisSamples < 64 || config.analysisInterval == 0 ||
      config.powerIterations == 0 || config.covarianceSmoothing < 0 ||
      config.covarianceSmoothing >= 1 || config.diagonalLoading < 0)
    throw std::invalid_argument("Invalid array-reference configuration");
}

std::unique_ptr<IqData> ArrayReferenceSynthesizer::process(
  const std::vector<IqData *>& channels)
{
  if (channels.empty() || !channels.front())
    throw std::invalid_argument("Array-reference channel is null");
  auto output = std::make_unique<IqData>(channels.front()->get_length());
  process_into(channels, *output);
  return output;
}

void ArrayReferenceSynthesizer::process_into(
  const std::vector<IqData *>& channels, IqData& output)
{
  if (channels.size() < 2)
    throw std::invalid_argument("Array reference requires multiple channels");
  if (!channels.front())
    throw std::invalid_argument("Array-reference channel is null");
  const std::size_t samples = channels.front()->view_data().size();
  for (const auto *channel : channels)
    if (!channel || channel->view_data().size() != samples)
      throw std::invalid_argument("Array-reference channels are not aligned");
  if (samples > output.get_n())
    throw std::invalid_argument("Array-reference output capacity is too small");
  std::vector<const std::deque<std::complex<double>>*> input;
  input.reserve(channels.size());
  for (const auto *channel : channels)
  {
    if (channel == &output)
      throw std::invalid_argument("Array-reference output aliases an input");
    input.push_back(&channel->view_data());
  }

  const std::size_t count = channels.size();
  const bool analyze = metrics.weights.size() != count ||
    frames++ % config.analysisInterval == 0;
  if (analyze)
  {
    std::vector<std::vector<std::complex<double>>> covariance(count,
      std::vector<std::complex<double>>(count));
    const std::size_t stride = std::max<std::size_t>(1,
      samples / config.analysisSamples);
    std::size_t observations = 0;
    for (std::size_t sample = 0; sample < samples; sample += stride)
    {
      for (std::size_t left = 0; left < count; left++)
        for (std::size_t right = 0; right < count; right++)
          covariance[left][right] += (*input[left])[sample] *
            std::conj((*input[right])[sample]);
      observations++;
    }
    double trace = 0;
    for (std::size_t left = 0; left < count; left++)
      for (std::size_t right = 0; right < count; right++)
      {
        covariance[left][right] /= static_cast<double>(observations);
        if (left == right) trace += covariance[left][right].real();
      }
    const double loading = config.diagonalLoading * trace / count;
    for (std::size_t channel = 0; channel < count; channel++)
      covariance[channel][channel] += loading;

    std::vector<std::complex<double>> weights(count,
      {1.0 / std::sqrt(static_cast<double>(count)), 0});
    if (metrics.weights.size() == count) weights = metrics.weights;
    for (uint32_t iteration = 0; iteration < config.powerIterations; iteration++)
    {
      std::vector<std::complex<double>> next(count);
      for (std::size_t left = 0; left < count; left++)
        for (std::size_t right = 0; right < count; right++)
          next[left] += covariance[left][right] * weights[right];
      normalize(next);
      weights = std::move(next);
    }
    if (metrics.weights.size() == count)
    {
      std::complex<double> alignment{};
      for (std::size_t channel = 0; channel < count; channel++)
        alignment += std::conj(metrics.weights[channel]) * weights[channel];
      if (std::abs(alignment) > 1e-12)
      {
        const auto phase = std::conj(alignment) / std::abs(alignment);
        for (auto& weight : weights) weight *= phase;
      }
      for (std::size_t channel = 0; channel < count; channel++)
        weights[channel] = config.covarianceSmoothing *
          metrics.weights[channel] + (1 - config.covarianceSmoothing) *
          weights[channel];
      normalize(weights);
    }
    std::vector<std::complex<double>> projected(count);
    for (std::size_t left = 0; left < count; left++)
      for (std::size_t right = 0; right < count; right++)
        projected[left] += covariance[left][right] * weights[right];
    std::complex<double> eigenvalue{};
    for (std::size_t channel = 0; channel < count; channel++)
      eigenvalue += std::conj(weights[channel]) * projected[channel];
    const double principal = std::max(0.0, eigenvalue.real() - loading);
    metrics.coherentFraction = principal / std::max(trace, 1e-30);
    metrics.coherentGainDb = 10 * std::log10(std::max(1e-12,
      principal / std::max(trace / count, 1e-30)));
    metrics.weights = std::move(weights);
    metrics.updates++;
  }

  auto& combined = output.resize_for_write(static_cast<uint32_t>(samples));
  double inputPower = 0;
  double outputPower = 0;
  for (std::size_t sample = 0; sample < samples; sample++)
  {
    combined[sample] = {};
    for (std::size_t channel = 0; channel < count; channel++)
    {
      combined[sample] += std::conj(metrics.weights[channel]) *
        (*input[channel])[sample];
      inputPower += std::norm((*input[channel])[sample]) / count;
    }
    outputPower += std::norm(combined[sample]);
  }
  const double scale = std::sqrt(inputPower / std::max(outputPower, 1e-30));
  for (auto& value : combined) value *= scale;
}
