#include "ReplayEchoInjector.h"
#include "data/meta/Constants.h"
#include <cmath>
#include <complex>
#include <deque>
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
  if (!sampleRate_ || !(frequencyHz_ > 0) || positions_.empty() ||
      !std::isfinite(config_.amplitudeDb) || config_.amplitudeDb > 0 ||
      config_.amplitudeDb < -160 || !std::isfinite(config_.delayKm) ||
      config_.delayKm < 0 || !std::isfinite(config_.dopplerHz) ||
      !std::isfinite(config_.targetAzimuthRad) ||
      !std::isfinite(directPathAzimuthRad_))
    throw std::invalid_argument("Invalid replay echo-injection configuration");
}

void ReplayEchoInjector::apply(uint64_t firstSample,
  const std::vector<IqData*>& channels) const
{
  if (!config_.enabled) return;
  if (channels.size() != positions_.size() || channels.empty())
    throw std::invalid_argument("Echo injection channel geometry changed");
  const uint64_t delaySamples = uint64_t(std::llround(
    config_.delayKm * 1000.0 * sampleRate_ / Constants::c));
  const double amplitude = std::pow(10.0, config_.amplitudeDb / 20.0);
  const double waveNumber = Tau * frequencyHz_ / Constants::c;
  const double targetX = std::cos(config_.targetAzimuthRad);
  const double targetY = std::sin(config_.targetAzimuthRad);
  const double directX = std::cos(directPathAzimuthRad_);
  const double directY = std::sin(directPathAzimuthRad_);
  for (size_t channel = 0; channel < channels.size(); ++channel) {
    auto* iq = channels[channel];
    if (!iq) throw std::invalid_argument("Echo injection channel is null");
    const auto source = iq->get_data();
    auto& destination = iq->resize_for_write(source.size());
    const auto& position = positions_[channel];
    const double relativePhase = -waveNumber *
      (position[0] * (targetX - directX) + position[1] * (targetY - directY));
    for (size_t sample = size_t(std::min<uint64_t>(delaySamples, source.size()));
         sample < source.size(); ++sample) {
      const double dopplerPhase = Tau * config_.dopplerHz *
        double(firstSample + sample) / sampleRate_;
      destination[sample] += amplitude * source[sample - delaySamples] *
        std::polar(1.0, dopplerPhase + relativePhase);
    }
  }
}

}
