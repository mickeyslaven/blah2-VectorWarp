#include "process/analysis/MultiIlluminatorRadar.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace blah2::multi_illuminator
{
namespace
{
constexpr double speedOfLight = 299792458.0;
}

struct MultiIlluminatorRadar::Context
{
  channelizer::IlluminatorDefinition definition;
  uint32_t captureRate = 0, samples = 0, decimation = 0;
  RadarConfig config;
  std::vector<std::deque<std::complex<double>>> pending;
  bool positioned = false;
  uint64_t pendingFirstInputSample = 0;
  IqData reference;
  std::vector<std::unique_ptr<IqData>> ownedChannels;
  std::vector<IqData*> channels;
  production_replay::RawIqStages stages;
  std::vector<std::unique_ptr<Ambiguity>> ownedAmbiguity;
  std::vector<Ambiguity*> ambiguity;
  Noncoherent fusion;
  CfarDetector1D detector;
  Centroid centroid;
  Interpolate interpolate{true, true};
  Tracker tracker;

  Context(uint32_t captureRateHz, std::size_t channelCount,
    channelizer::IlluminatorDefinition value, RadarConfig radar)
    : definition(std::move(value)), captureRate(captureRateHz),
      samples(uint32_t(std::llround(radar.cpiSeconds * definition.outputRateHz))),
      decimation(captureRateHz / definition.outputRateHz), config(std::move(radar)),
      pending(channelCount), reference(samples),
      stages({config.clutterDelayMin, config.clutterDelayMax, samples,
        channelCount, config.workers, false, config.reference}),
      detector(config.pfa, config.guardCells, config.trainingCells,
        config.minimumDelay, config.minimumDopplerHz),
      centroid(config.centroidRadius, config.centroidRadius,
        1.0 / config.cpiSeconds),
      tracker(config.trackerM, config.trackerN, config.trackerDelete,
        config.cpiSeconds, config.trackerMaximumAcceleration,
        speedOfLight / definition.outputRateHz,
        speedOfLight / definition.frequencyHz, config.trackerMinimumSnrDb)
  {
    if (!samples || samples < 2000 || captureRateHz % definition.outputRateHz)
      throw std::invalid_argument("Multi-illuminator CPI or sample-rate ratio is invalid");
    ownedChannels.reserve(channelCount);
    ownedAmbiguity.reserve(channelCount);
    for (std::size_t channel = 0; channel < channelCount; ++channel)
    {
      ownedChannels.push_back(std::make_unique<IqData>(samples));
      channels.push_back(ownedChannels.back().get());
      ownedAmbiguity.push_back(std::make_unique<Ambiguity>(config.delayMin,
        config.delayMax, config.dopplerMin, config.dopplerMax,
        definition.outputRateHz, samples, true));
      ambiguity.push_back(ownedAmbiguity.back().get());
    }
  }

  void clear()
  {
    for (auto& queue : pending) queue.clear();
    positioned = false;
    pendingFirstInputSample = 0;
    stages.resetReference();
    tracker = Tracker(config.trackerM, config.trackerN,
      config.trackerDelete, config.cpiSeconds,
      config.trackerMaximumAcceleration,
      speedOfLight / definition.outputRateHz,
      speedOfLight / definition.frequencyHz, config.trackerMinimumSnrDb);
  }

  void append(const channelizer::SubbandChannelizer::Output& input)
  {
    if (input.channels.size() != pending.size() ||
        input.inputSamplesPerOutput != decimation)
      throw std::invalid_argument("Channelized illuminator topology changed");
    if (input.channels.empty()) return;
    const std::size_t count = input.channels.front().size();
    for (const auto& channel : input.channels)
      if (channel.size() != count)
        throw std::invalid_argument("Channelized illuminator channels are unpaired");
    if (!count) return;
    if (!input.hasFirstInputSample)
      throw std::invalid_argument("Channelized illuminator has no sample position");
    if (positioned)
    {
      const uint64_t expected = pendingFirstInputSample +
        pending.front().size() * uint64_t(decimation);
      if (input.firstInputSample != expected)
        throw std::invalid_argument("Channelized illuminator input has a sample gap");
    }
    else
    {
      pendingFirstInputSample = input.firstInputSample;
      positioned = true;
    }
    for (std::size_t channel = 0; channel < pending.size(); ++channel)
      for (const auto value : input.channels[channel])
        pending[channel].emplace_back(value);
  }

  std::unique_ptr<RadarFrame> next()
  {
    if (!positioned || pending.front().size() < samples) return {};
    const uint64_t first = pendingFirstInputSample;
    for (std::size_t channel = 0; channel < pending.size(); ++channel)
    {
      auto& destination = channels[channel]->resize_for_write(samples);
      for (uint32_t sample = 0; sample < samples; ++sample)
      {
        destination[sample] = pending[channel].front();
        pending[channel].pop_front();
      }
    }
    pendingFirstInputSample += uint64_t(samples) * decimation;
    stages.reference().process_into(channels, reference);
    if (!stages.clutter(reference, channels))
      throw std::runtime_error("Multi-illuminator clutter reference was rejected");
    stages.ambiguity(reference, channels, ambiguity, false);
    std::vector<Map<std::complex<double>>*> maps;
    maps.reserve(ambiguity.size());
    for (const auto& processor : ownedAmbiguity) maps.push_back(processor->result());
    auto map = fusion.process(maps);
    map->set_metrics();
    auto firstPass = detector.process(map.get());
    auto centered = centroid.process(firstPass.get());
    auto detections = interpolate.process(centered.get(), map.get());
    const uint64_t end = first + uint64_t(samples - 1) * decimation;
    auto tracks = tracker.process(detections.get(),
      (end * 1000) / captureRate);
    auto result = std::make_unique<RadarFrame>();
    result->illuminatorId = definition.id;
    result->frequencyHz = definition.frequencyHz;
    result->firstInputSample = first;
    result->endInputSample = end;
    result->sampleRateHz = definition.outputRateHz;
    result->map = std::move(map);
    result->detections = std::move(detections);
    result->tracks = std::move(tracks);
    return result;
  }
};

MultiIlluminatorRadar::~MultiIlluminatorRadar() = default;

MultiIlluminatorRadar::MultiIlluminatorRadar(uint32_t captureRateHz,
  std::size_t channelCount,
  const std::vector<channelizer::IlluminatorDefinition>& illuminators,
  RadarConfig config)
  : captureRateHz_(captureRateHz), channelCount_(channelCount),
    config_(std::move(config))
{
  if (!captureRateHz_ || !channelCount_ || illuminators.empty() ||
      !std::isfinite(config_.cpiSeconds) || config_.cpiSeconds <= 0)
    throw std::invalid_argument("Multi-illuminator radar configuration is empty");
  contexts_.reserve(illuminators.size());
  for (const auto& illuminator : illuminators)
    contexts_.push_back(std::make_unique<Context>(captureRateHz_,
      channelCount_, illuminator, config_));
}

std::vector<RadarFrame> MultiIlluminatorRadar::process(
  std::vector<channelizer::IlluminatorOutput> input)
{
  if (input.size() != contexts_.size())
    throw std::invalid_argument("Multi-illuminator output set changed");
  std::unordered_map<std::string, channelizer::IlluminatorOutput*> byId;
  for (auto& illuminator : input)
    if (!byId.emplace(illuminator.id, &illuminator).second)
      throw std::invalid_argument("Multi-illuminator output IDs are duplicated");
  // Validate the complete identity set before advancing any adaptive context.
  for (const auto& context : contexts_)
  {
    const auto found = byId.find(context->definition.id);
    if (found == byId.end() ||
        found->second->frequencyHz != context->definition.frequencyHz)
      throw std::invalid_argument("Multi-illuminator output identity changed");
  }
  try
  {
    for (auto& context : contexts_)
      context->append(byId.at(context->definition.id)->samples);
    std::vector<RadarFrame> result;
    for (auto& context : contexts_)
      while (auto frame = context->next()) result.push_back(std::move(*frame));
    return result;
  }
  catch (...)
  {
    reset();
    throw;
  }
}

void MultiIlluminatorRadar::reset()
{
  for (auto& context : contexts_) context->clear();
}

} // namespace blah2::multi_illuminator
