#include "ReplayEchoInjector.h"
#include "data/meta/Constants.h"
#include "process/conditioning/SamplePhase.h"
#include <cmath>
#include <complex>
#include <deque>
#include <algorithm>
#include <limits>
#include <stdexcept>

namespace blah2::analysis {
namespace {
constexpr double Tau = 6.283185307179586476925286766559;
}

ReplayEchoInjector::ReplayEchoInjector(const Config& config,
  uint32_t sampleRate, double frequencyHz, double directPathAzimuthRad,
  std::vector<std::array<double, 3>> positions, bool replayInput)
  : config_(config), sampleRate_(sampleRate), frequencyHz_(frequencyHz),
    directPathAzimuthRad_(directPathAzimuthRad), positions_(std::move(positions))
{
  if (!config_.enabled) return;
  if (!replayInput)
    throw std::invalid_argument("Echo injection is restricted to replay input");
  if (!sampleRate_ || !(frequencyHz_ > 0) || !std::isfinite(frequencyHz_) ||
      positions_.empty() || positions_.size() > 8 ||
      !std::isfinite(config_.amplitudeDb) || config_.amplitudeDb > 0 ||
      config_.amplitudeDb < -160 || !std::isfinite(config_.delayKm) ||
      config_.delayKm < 0 || !std::isfinite(config_.dopplerHz) ||
      std::abs(config_.dopplerHz) >= .5 * sampleRate_ ||
      !std::isfinite(config_.targetAzimuthRad) ||
      !std::isfinite(directPathAzimuthRad_))
    throw std::invalid_argument("Invalid replay echo-injection configuration");
  for (const auto& position : positions_)
    for (double coordinate : position)
      if (!std::isfinite(coordinate))
        throw std::invalid_argument("Replay echo-injection geometry must be finite");
  const long double delay = static_cast<long double>(config_.delayKm) *
    1000 * sampleRate_ / Constants::c;
  const uint64_t limit = maximumHistoryBytes /
    (sizeof(std::complex<double>) * positions_.size());
  if (!std::isfinite(delay) || delay > limit)
    throw std::invalid_argument("Replay echo delay exceeds bounded IQ history");
  delaySamples_ = uint64_t(std::llround(delay));
  history_.resize(positions_.size());
}

void ReplayEchoInjector::apply(const Frame& frame,
  const std::vector<IqData*>& channels, bool inject)
{
  if (!config_.enabled) return;
  if (channels.size() != positions_.size() || channels.empty())
    throw std::invalid_argument("Echo injection channel geometry changed");
  if (!frame.positioned || frame.end <= frame.first ||
      frame.end - frame.first > std::numeric_limits<uint32_t>::max())
    throw std::invalid_argument("Replay echo injection requires exact capture sample lineage");
  std::vector<std::deque<std::complex<double>>> sources;
  sources.reserve(channels.size());
  for (size_t channel = 0; channel < channels.size(); ++channel) {
    auto* iq = channels[channel];
    if (!iq || std::find(channels.begin(), channels.begin() + channel, iq) != channels.begin() + channel)
      throw std::invalid_argument("Echo injection channel is null or repeated");
    sources.push_back(iq->get_data());
    if (sources.back().size() != frame.end - frame.first)
      throw std::invalid_argument("Echo injection input differs from positioned capture interval");
    for (const auto value : sources.back())
      if (!std::isfinite(value.real()) || !std::isfinite(value.imag()))
        throw std::invalid_argument("Replay echo injection input must be finite");
  }
  if (!positioned_ || epoch_ != frame.epoch || nextSample_ != frame.first)
    for (auto& history : history_) history.clear();
  const double amplitude = std::pow(10.0, config_.amplitudeDb / 20.0);
  const double waveNumber = Tau * frequencyHz_ / Constants::c;
  const double targetX = std::cos(config_.targetAzimuthRad);
  const double targetY = std::sin(config_.targetAzimuthRad);
  const double directX = std::cos(directPathAzimuthRad_);
  const double directY = std::sin(directPathAzimuthRad_);
  for (size_t channel = 0; channel < channels.size(); ++channel) {
    auto* iq = channels[channel];
    const auto& source = sources[channel];
    auto& history = history_[channel];
    auto& destination = iq->resize_for_write(source.size());
    const auto& position = positions_[channel];
    const double relativePhase = -waveNumber *
      (position[0] * (targetX - directX) + position[1] * (targetY - directY));
    const double increment = config_.dopplerHz / sampleRate_;
    // The channelizer convention mixes down; injection mixes the echo up.
    // Preserve exact represented Doppler before division, even at large epochs.
    double cycles = channelizer::mixerPhaseCycles(frame.first, -config_.dopplerHz, sampleRate_);
    for (size_t sample = 0; sample < source.size(); ++sample) {
      if (inject && (sample >= delaySamples_ || delaySamples_ - sample <= history.size())) {
        const auto delayed = sample >= delaySamples_ ? source[sample - delaySamples_] :
          history[history.size() - (delaySamples_ - sample)];
        destination[sample] += amplitude * delayed * std::polar(1.0, Tau * cycles + relativePhase);
      }
      cycles = std::remainder(cycles + increment, 1.0);
    }
    // Retain raw input only: retaining destination would feed injected echoes
    // back into subsequent CPIs and synthesize an artificial echo train.
    if (delaySamples_) {
      const size_t keep = std::min<uint64_t>(delaySamples_, source.size());
      while (history.size() > delaySamples_ - keep) history.pop_front();
      history.insert(history.end(), source.end() - keep, source.end());
    }
  }
  positioned_ = true; epoch_ = frame.epoch; nextSample_ = frame.end;
}

}
