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
      config.powerIterations == 0 || !std::isfinite(config.covarianceSmoothing) ||
      config.covarianceSmoothing < 0 || config.covarianceSmoothing >= 1 ||
      !std::isfinite(config.diagonalLoading) || config.diagonalLoading < 0)
      throw std::invalid_argument("Invalid array-reference configuration");
  if (!std::isfinite(config.commonModeDecorrelation) ||
      config.commonModeDecorrelation < 0 || config.commonModeDecorrelation > .5 ||
      !std::isfinite(config.commonModeMinimumCoherence) ||
      config.commonModeMinimumCoherence < 0 || config.commonModeMinimumCoherence > 1)
    throw std::invalid_argument("Invalid common-mode decorrelation configuration");
  for (const auto value : config.protectionRelativeSteering)
    if (!std::isfinite(value.real()) || !std::isfinite(value.imag()))
      throw std::invalid_argument("Invalid target-protection steering");
  if (!std::isfinite(config.minimumProtectionRetention) ||
      config.minimumProtectionRetention <= 0 ||
      config.minimumProtectionRetention > 1)
    throw std::invalid_argument("Invalid target-protection retention");
}

void ArrayReferenceSynthesizer::decorrelate_surveillance_into(
  const IqData& reference, const IqData& surveillance, IqData& output)
{
  if (&output == &reference || &output == &surveillance)
    throw std::invalid_argument("Common-mode output aliases an input");
  const auto& referenceSamples = reference.view_data();
  const auto& surveillanceSamples = surveillance.view_data();
  if (referenceSamples.empty() || referenceSamples.size() != surveillanceSamples.size() ||
      surveillanceSamples.size() > output.get_n())
    throw std::invalid_argument("Common-mode samples are not aligned");
  double referencePower = 0, surveillancePower = 0;
  std::complex<double> cross{};
  for (std::size_t index = 0; index < referenceSamples.size(); ++index)
  {
    const auto r = referenceSamples[index], s = surveillanceSamples[index];
    if (!std::isfinite(r.real()) || !std::isfinite(r.imag()) ||
        !std::isfinite(s.real()) || !std::isfinite(s.imag()))
      throw std::invalid_argument("Nonfinite common-mode IQ");
    referencePower += std::norm(r);
    surveillancePower += std::norm(s);
    cross += std::conj(r) * s;
  }
  const double coherence = referencePower > 0 && surveillancePower > 0 ?
    std::min(1.0, std::norm(cross) / (referencePower * surveillancePower)) : 0;
  const double fraction = config.commonModeDecorrelation > 0 &&
    coherence >= config.commonModeMinimumCoherence && referencePower > 0 ?
    config.commonModeDecorrelation : 0;
  const auto coefficient = fraction > 0 ? fraction * cross / referencePower :
    std::complex<double>{};
  auto& result = output.resize_for_write(static_cast<uint32_t>(surveillanceSamples.size()));
  for (std::size_t index = 0; index < result.size(); ++index)
    result[index] = surveillanceSamples[index] - coefficient * referenceSamples[index];
  metrics.commonModeCoherence = coherence;
  metrics.commonModeFraction = fraction;
}

void ArrayReferenceSynthesizer::reset()
{
  metrics = Metrics{};
  frames = 0;
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
  if (channels.size() < 2 || channels.size() > 8)
    throw std::invalid_argument("Array reference requires two to eight channels");
  if (!channels.front())
    throw std::invalid_argument("Array-reference channel is null");
  const std::size_t samples = channels.front()->view_data().size();
  if (!samples) throw std::invalid_argument("Array-reference CPI is empty");
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
  if (!config.protectionRelativeSteering.empty() &&
      config.protectionRelativeSteering.size() != count)
    throw std::invalid_argument("Target-protection steering channel count differs");
  metrics.fallback = false;
  metrics.reason.clear();
  const bool analyze = metrics.directPathWeights.size() != count ||
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
    if (!std::isfinite(trace) || !std::isfinite(loading))
      throw std::invalid_argument("Nonfinite array-reference covariance");
    for (std::size_t channel = 0; channel < count; channel++)
      covariance[channel][channel] += loading;

    const auto beamPower = [&](const std::vector<std::complex<double>>& w) {
      std::complex<double> value{};
      for (std::size_t left = 0; left < count; ++left)
      {
        std::complex<double> projected{};
        for (std::size_t right = 0; right < count; ++right)
          projected += covariance[left][right] * w[right];
        value += std::conj(w[left]) * projected;
      }
      return std::max(0.0, value.real() - loading);
    };
    // Uniform/retained seeds can be exactly orthogonal to the field. Search
    // every basis seed, not just the largest diagonal (which can belong to
    // a separate subdominant mode). Match the bounded resident algorithm.
    std::vector<std::complex<double>> weights(count);
    double bestPower = -1;
    for (std::size_t seed = 0; seed < count; ++seed)
    {
      std::vector<std::complex<double>> candidate(count);
      candidate[seed] = 1;
      for (uint32_t iteration = 0; iteration < config.powerIterations; ++iteration)
      {
        std::vector<std::complex<double>> next(count);
        for (std::size_t left = 0; left < count; ++left)
          for (std::size_t right = 0; right < count; ++right)
            next[left] += covariance[left][right] * candidate[right];
        normalize(next);
        candidate = std::move(next);
      }
      const double power = beamPower(candidate);
      // Stable ties avoid selecting a different phase from rounding noise.
      if (bestPower < 0 || power > bestPower * (1 + 1e-12))
      {
        bestPower = power;
        weights = std::move(candidate);
      }
    }
    std::complex<double> alignment{};
    const bool retained = metrics.directPathWeights.size() == count;
    if (retained)
    {
      for (std::size_t channel = 0; channel < count; channel++)
        alignment += std::conj(metrics.directPathWeights[channel]) * weights[channel];
    }
    if (std::abs(alignment) <= 1e-12)
      for (const auto weight : weights)
        if (std::abs(weight) > 1e-12) { alignment = weight; break; }
    if (std::abs(alignment) > 0)
    {
      const auto phase = std::conj(alignment) / std::abs(alignment);
      for (auto& weight : weights) weight *= phase;
    }
    if (retained)
    {
      auto blended = weights;
      for (std::size_t channel = 0; channel < count; channel++)
        blended[channel] = config.covarianceSmoothing *
          metrics.directPathWeights[channel] + (1 - config.covarianceSmoothing) *
          weights[channel];
      normalize(blended);
      // Retaining smooth weights must not retain a phase null.
      if (beamPower(blended) >= 0.5 * beamPower(weights))
        weights = std::move(blended);
    }
    // Constrain the reference response to zero in one predeclared candidate
    // direction.  The target manifold is the measured direct-path manifold
    // rotated by its geometric relative phase, avoiding an uncalibrated ideal
    // array assumption.  This is deliberately applied before ECA/CAF, not as
    // a second projection of post-cancellation surveillance maps.
    metrics.directPathWeights = weights;
    const double directPrincipal = beamPower(metrics.directPathWeights);
    metrics.directPathCoherentFraction = directPrincipal / std::max(trace, 1e-30);
    metrics.directPathCoherentGainDb = 10 * std::log10(std::max(1e-12,
      directPrincipal / std::max(trace / count, 1e-30)));
    if (!config.protectionRelativeSteering.empty())
    {
      std::vector<std::complex<double>> target(count);
      std::complex<double> numerator{};
      double denominator = 0;
      for (std::size_t channel = 0; channel < count; ++channel)
      {
        target[channel] = metrics.directPathWeights[channel] *
          config.protectionRelativeSteering[channel];
        numerator += std::conj(target[channel]) * weights[channel];
        denominator += std::norm(target[channel]);
      }
      if (!(denominator > 1e-30))
        throw std::invalid_argument("Target-protection steering is degenerate");
      for (std::size_t channel = 0; channel < count; ++channel)
        weights[channel] -= target[channel] * numerator / denominator;
      double retainedPower = 0;
      for (const auto weight : weights) retainedPower += std::norm(weight);
      if (retainedPower < config.minimumProtectionRetention)
        throw std::runtime_error("Target-protection projection is near-degenerate");
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
  double directOutputPower = 0;
  for (std::size_t sample = 0; sample < samples; sample++)
  {
    combined[sample] = {};
    std::complex<double> directCombined{};
    for (std::size_t channel = 0; channel < count; channel++)
    {
      const auto value = (*input[channel])[sample];
      if (!std::isfinite(value.real()) || !std::isfinite(value.imag()))
        throw std::invalid_argument("Nonfinite array-reference IQ");
      combined[sample] += std::conj(metrics.weights[channel]) *
        value;
      directCombined += std::conj(metrics.directPathWeights[channel]) * value;
      inputPower += std::norm(value) / count;
    }
    outputPower += std::norm(combined[sample]);
    directOutputPower += std::norm(directCombined);
  }
  if (!std::isfinite(inputPower) || !std::isfinite(outputPower) ||
      !std::isfinite(directOutputPower))
    throw std::overflow_error("Array-reference power overflow");
  metrics.hasSignal = inputPower > 0;
  // Match the device energy gate, including CPIs between scheduled updates.
  // Explicit replay-only target protection must not be silently undone.
  if (inputPower > 0 && directOutputPower < 0.25 * inputPower)
  {
    std::size_t strongest = 0;
    double strongestPower = -1;
    for (std::size_t channel = 0; channel < count; ++channel)
    {
      double power = 0;
      for (const auto value : *input[channel]) power += std::norm(value);
      if (power > strongestPower) { strongest = channel; strongestPower = power; }
    }
    metrics.directPathWeights.assign(count, {});
    metrics.directPathWeights[strongest] = 1;
    metrics.fallback = true;
    metrics.reason = "strongest_channel";
    metrics.coherentFraction = metrics.coherentGainDb = 0;
    metrics.directPathCoherentFraction = metrics.directPathCoherentGainDb = 0;
    frames = 0;
    if (!config.protectionRelativeSteering.empty())
      throw std::runtime_error(
        "Target protection cannot preserve its null after direct-path fallback");
    std::copy(input[strongest]->begin(), input[strongest]->end(), combined.begin());
    metrics.weights = metrics.directPathWeights;
  }
  else if (!metrics.hasSignal)
  {
    metrics.fallback = true;
    metrics.reason = "zero_input";
    metrics.coherentFraction = metrics.coherentGainDb = 0;
    metrics.directPathCoherentFraction = metrics.directPathCoherentGainDb = 0;
    frames = 0;
  }
  // Unit-norm combining retains physical coherent gain. No per-CPI AGC.
}
