#include "process/conditioning/AtscPilotProjector.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <stdexcept>

namespace blah2::conditioning {
namespace {
constexpr double pi = 3.14159265358979323846;
constexpr double atscSymbolRate = 4.5e6 * 684.0 / 286.0;

void renormalize(std::complex<double>& value) {
  const double magnitude = std::abs(value);
  if (magnitude > 0 && std::isfinite(magnitude)) value /= magnitude;
}

template <typename Sample, typename Output>
AtscPilotProjector::Metrics project(const AtscPilotProjector::Config& config,
  std::size_t samples, Sample sampleAt,
  Output& output, double cfoHz,
  bool pilotCandidate) {
  AtscPilotProjector::Metrics result; result.cfoHz = cfoHz;
  result.samples = samples;
  const std::size_t minimum = std::size_t(std::ceil(
    config.minimumDurationSeconds * config.sampleRateHz));
  if (samples < std::max<std::size_t>(2, minimum)) {
    result.reason = "insufficient_samples"; return result;
  }
  result.available = true;
  if (!pilotCandidate) { result.reason = "pilot_unqualified"; return result; }
  if (!std::isfinite(cfoHz) || std::abs(cfoHz) > config.maximumAbsoluteCfoHz) {
    result.reason = "cfo_unqualified"; return result;
  }
  const double frequency = -atscSymbolRate / 4 + cfoHz;
  const auto step = std::polar(1.0, 2 * pi * frequency / config.sampleRateHz);
  const std::size_t split = samples / 2;
  std::complex<double> oscillator{1,0}, training{}, heldout{};
  double power = 0;
  for (std::size_t i = 0; i < samples; ++i) {
    const std::complex<double> sample = sampleAt(i);
    if (!std::isfinite(sample.real()) || !std::isfinite(sample.imag())) {
      result.reason = "non_finite_input"; return result;
    }
    const auto projected = sample * std::conj(oscillator);
    if (i < split) training += projected; else heldout += projected;
    power += std::norm(sample);
    oscillator *= step;
    if ((i & 4095) == 4095) renormalize(oscillator);
  }
  const auto trainingAmplitude = training / double(split);
  const auto heldoutAmplitude = heldout / double(samples - split);
  const double residualPilot = std::norm(heldoutAmplitude - trainingAmplitude);
  result.heldoutSuppressionDb = 10 * std::log10(
    std::max(std::norm(heldoutAmplitude), 1e-30) /
    std::max(residualPilot, 1e-30));
  result.removedPowerFraction = std::norm(trainingAmplitude) /
    std::max(power / samples, 1e-30);
  if (!std::isfinite(result.heldoutSuppressionDb) ||
      !std::isfinite(result.removedPowerFraction) ||
      result.heldoutSuppressionDb < config.minimumHeldoutSuppressionDb) {
    result.reason = "heldout_suppression_unqualified"; return result;
  }
  if (result.removedPowerFraction <= std::numeric_limits<double>::epsilon() ||
      result.removedPowerFraction > config.maximumRemovedPowerFraction) {
    result.reason = "removed_power_unqualified"; return result;
  }
  result.qualified = true;
  output.resize(samples);
  oscillator = {1,0};
  for (std::size_t i = 0; i < samples; ++i) {
    output[i] = sampleAt(i) - trainingAmplitude * oscillator;
    oscillator *= step;
    if ((i & 4095) == 4095) renormalize(oscillator);
  }
  result.applied = true; result.reason = "qualified_pilot_projection";
  return result;
}

template <typename Sample>
std::optional<double> refineCfo(const AtscPilotProjector::Config& config,
  std::size_t samples, Sample sampleAt, double coarseCfoHz) {
  if (!samples || !std::isfinite(coarseCfoHz)) return std::nullopt;
  const std::size_t blockSamples = std::max<std::size_t>(256,
    std::size_t(std::llround(config.cfoBlockSeconds * config.sampleRateHz)));
  if (samples < blockSamples * 8) return std::nullopt;
  const double frequency = -atscSymbolRate / 4 + coarseCfoHz;
  const auto step = std::polar(1.0, 2 * pi * frequency / config.sampleRateHz);
  std::complex<double> oscillator{1,0}, block{}, previous{}, correlation{};
  std::size_t inBlock = 0, blocks = 0;
  for (std::size_t i = 0; i < samples; ++i) {
    const std::complex<double> sample = sampleAt(i);
    if (!std::isfinite(sample.real()) || !std::isfinite(sample.imag()))
      return std::nullopt;
    block += sample * std::conj(oscillator);
    oscillator *= step;
    if ((i & 4095) == 4095) renormalize(oscillator);
    if (++inBlock == blockSamples) {
      if (blocks) correlation += block * std::conj(previous);
      previous = block; block = {}; inBlock = 0; ++blocks;
    }
  }
  if (blocks < 8 || !(std::abs(correlation) > 0)) return std::nullopt;
  const double residual = std::arg(correlation) * config.sampleRateHz /
    (2 * pi * blockSamples);
  if (!std::isfinite(residual) ||
      std::abs(residual) >= .45 / config.cfoBlockSeconds)
    return std::nullopt;
  return coarseCfoHz + residual;
}
}

AtscPilotProjector::AtscPilotProjector(Config config): config_(config) {
  if (!config_.sampleRateHz ||
      !(config_.minimumHeldoutSuppressionDb >= 0) ||
      !(config_.maximumRemovedPowerFraction > 0 &&
        config_.maximumRemovedPowerFraction < 1) ||
      !(config_.maximumAbsoluteCfoHz > 0) ||
      !(config_.minimumDurationSeconds > 0) ||
      !(config_.cfoBlockSeconds > 0) ||
      !(config_.maximumCfoStepHz > 0) ||
      !config_.qualificationCpis || !config_.unlockFailures ||
      !std::isfinite(config_.minimumHeldoutSuppressionDb) ||
      !std::isfinite(config_.maximumRemovedPowerFraction) ||
      !std::isfinite(config_.maximumAbsoluteCfoHz) ||
      !std::isfinite(config_.minimumDurationSeconds) ||
      !std::isfinite(config_.cfoBlockSeconds) ||
      !std::isfinite(config_.maximumCfoStepHz) ||
      config_.sampleRateHz <= atscSymbolRate / 2)
    throw std::invalid_argument("Invalid ATSC pilot projector config");
}

void AtscPilotProjector::reset() {
  haveCfo_ = false; locked_ = false; cfoHz_ = 0;
  qualifiedCpis_ = 0; failedCpis_ = 0;
}

void AtscPilotProjector::fail() {
  if (++failedCpis_ >= config_.unlockFailures) reset();
}

bool AtscPilotProjector::acceptCfo(double value, Metrics& metrics) {
  if (!haveCfo_) {
    cfoHz_ = value; haveCfo_ = true; qualifiedCpis_ = 1;
  } else if (std::abs(value - cfoHz_) <= config_.maximumCfoStepHz) {
    cfoHz_ = .75 * cfoHz_ + .25 * value;
    ++qualifiedCpis_;
  } else {
    fail();
    if (!locked_) { cfoHz_ = value; haveCfo_ = true; qualifiedCpis_ = 1; }
  }
  failedCpis_ = 0;
  if (!locked_ && qualifiedCpis_ >= config_.qualificationCpis) locked_ = true;
  metrics.cfoHz = cfoHz_; metrics.cfoLocked = locked_;
  metrics.qualifiedCpis = qualifiedCpis_; metrics.failedCpis = failedCpis_;
  return locked_;
}

template<class Sample, class Output>
AtscPilotProjector::Metrics AtscPilotProjector::process_samples(
  std::size_t samples, Sample sample, Output& output, double cfoHz, bool pilotCandidate) {
  Metrics state; state.coarseCfoHz = cfoHz; state.samples = samples;
  if (!pilotCandidate) { fail(); state.reason = "pilot_unqualified"; return state; }
  const auto refined = refineCfo(config_, samples, sample, cfoHz);
  if (!refined) { fail(); state.reason = "cfo_refinement_unqualified"; return state; }
  state.residualCfoHz = *refined - cfoHz;
  if (!acceptCfo(*refined, state)) { state.reason = "cfo_lock_acquiring"; return state; }
  auto result = project(config_, samples, sample, output, cfoHz_, true);
  result.coarseCfoHz = cfoHz; result.residualCfoHz = *refined - cfoHz;
  result.cfoLocked = locked_; result.qualifiedCpis = qualifiedCpis_;
  result.failedCpis = failedCpis_;
  if (!result.applied) fail(); else failedCpis_ = 0;
  return result;
}

AtscPilotProjector::Metrics AtscPilotProjector::process(
  const std::deque<std::complex<double>>& input,
  std::deque<std::complex<double>>& output, double cfoHz, bool pilotCandidate) {
  return process_samples(input.size(), [&](std::size_t i) { return input[i]; },
    output, cfoHz, pilotCandidate);
}

AtscPilotProjector::Metrics AtscPilotProjector::process(IqSampleView input,
  std::vector<std::complex<double>>& output, double cfoHz, bool pilotCandidate) {
  return process_samples(input.size(), [&](std::size_t i) { return input[i]; },
    output, cfoHz, pilotCandidate);
}

AtscPilotProjector::Metrics AtscPilotProjector::process(
  const std::complex<float>* input, std::size_t samples,
  std::vector<std::complex<double>>& output, double cfoHz,
  bool pilotCandidate) {
  if (!input && samples) throw std::invalid_argument(
    "ATSC pilot projector float input is null");
  return process_samples(samples, [&](std::size_t i) {
    return std::complex<double>(input[i]);
  }, output, cfoHz, pilotCandidate);
}

} // namespace blah2::conditioning
