#include "process/analysis/MultiIlluminatorRadar.h"
#include "process/utility/ProcessingThreads.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <thread>
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
  std::vector<std::vector<std::complex<double>>> pending;
  std::size_t pendingCount = 0;
  bool positioned = false;
  uint64_t pendingFirstInputSample = 0;
  IqData reference;
  std::vector<std::unique_ptr<IqData>> ownedChannels;
  std::vector<IqData*> channels;
  std::vector<IqData*> referenceChannels;
  production_replay::RawIqStages stages;
  std::vector<std::unique_ptr<Ambiguity>> ownedAmbiguity;
  std::vector<Ambiguity*> ambiguity;
  Noncoherent fusion;
  std::shared_ptr<Map<std::complex<double>>> fusedMap;
  CfarDetector1D detector;
  Centroid centroid;
  Interpolate interpolate{true, true};
  Tracker tracker;
  bool directFloatBorrowed = false;

  Context(uint32_t captureRateHz, std::size_t channelCount,
    channelizer::IlluminatorDefinition value, RadarConfig radar)
    : definition(std::move(value)), captureRate(captureRateHz),
      samples(uint32_t(std::llround(radar.cpiSeconds * definition.outputRateHz))),
      decimation(captureRateHz / definition.outputRateHz), config(std::move(radar)),
      pending(channelCount),
      reference(samples),
      stages({config.clutterDelayMin, config.clutterDelayMax, samples,
        config.surveillanceChannels.size(), config.workers,
        config.borrowedClutter.value_or(!config.arrayReference &&
          config.surveillanceChannels.size() == 1),
        config.reference, true}),
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
    if (config.surveillanceChannels.empty())
      throw std::invalid_argument("Multi-illuminator surveillance topology is empty");
    std::vector<bool> surveillanceSeen(channelCount), referenceSeen(channelCount);
    for (const uint32_t channel : config.surveillanceChannels)
    {
      if (channel >= channelCount || surveillanceSeen[channel])
        throw std::invalid_argument("Multi-illuminator surveillance topology is invalid");
      surveillanceSeen[channel] = true;
    }
    if (config.arrayReference)
    {
      if (config.referenceChannels.size() < 2)
        throw std::invalid_argument("Multi-illuminator array reference needs multiple channels");
      for (const uint32_t channel : config.referenceChannels)
      {
        if (channel >= channelCount || referenceSeen[channel])
          throw std::invalid_argument("Multi-illuminator reference topology is invalid");
        referenceSeen[channel] = true;
      }
    }
    else if (config.referenceChannel >= channelCount ||
        surveillanceSeen[config.referenceChannel])
      throw std::invalid_argument("Multi-illuminator dedicated reference topology is invalid");
    directFloatBorrowed = !config.arrayReference &&
      config.surveillanceChannels.size() == 1 &&
      config.borrowedClutter.value_or(true);

    ownedChannels.reserve(channelCount);
    for (std::size_t channel = 0; channel < channelCount; ++channel)
      ownedChannels.push_back(std::make_unique<IqData>(samples));
    channels.reserve(config.surveillanceChannels.size());
    for (const uint32_t channel : config.surveillanceChannels)
      channels.push_back(ownedChannels[channel].get());
    if (config.arrayReference)
      for (const uint32_t channel : config.referenceChannels)
        referenceChannels.push_back(ownedChannels[channel].get());

    ownedAmbiguity.reserve(channels.size());
    for (std::size_t channel = 0; channel < channels.size(); ++channel)
    {
      ownedAmbiguity.push_back(std::make_unique<Ambiguity>(config.delayMin,
        config.delayMax, config.dopplerMin, config.dopplerMax,
        definition.outputRateHz, samples, true));
      ambiguity.push_back(ownedAmbiguity.back().get());
    }
  }

  void clear()
  {
    pendingCount = 0;
    positioned = false;
    pendingFirstInputSample = 0;
    stages.resetReference();
    tracker = Tracker(config.trackerM, config.trackerN,
      config.trackerDelete, config.cpiSeconds,
      config.trackerMaximumAcceleration,
      speedOfLight / definition.outputRateHz,
      speedOfLight / definition.frequencyHz, config.trackerMinimumSnrDb);
  }

  std::vector<RadarFrame> consume(
    const channelizer::SubbandChannelizer::Output& input)
  {
    if (input.channels.size() != pending.size() ||
        input.inputSamplesPerOutput != decimation)
      throw std::invalid_argument("Channelized illuminator topology changed");
    if (input.channels.empty()) return {};
    const std::size_t count = input.channels.front().size();
    for (const auto& channel : input.channels)
      if (channel.size() != count)
        throw std::invalid_argument("Channelized illuminator channels are unpaired");
    if (!count) return {};
    if (!input.hasFirstInputSample)
      throw std::invalid_argument("Channelized illuminator has no sample position");
    if (positioned)
    {
      if (pendingCount > (std::numeric_limits<uint64_t>::max() -
          pendingFirstInputSample) / uint64_t(decimation))
        throw std::overflow_error("Multi-illuminator sample position overflow");
      const uint64_t expected = pendingFirstInputSample +
        pendingCount * uint64_t(decimation);
      if (input.firstInputSample != expected)
      {
        // The channelizer has already discarded its pre-gap FIR history.  Do
        // the same for clutter, tracker, and partial-CPI state so no adaptive
        // evidence crosses a missing-sample boundary.
        clear();
        pendingFirstInputSample = input.firstInputSample;
        positioned = true;
      }
    }
    else
    {
      pendingFirstInputSample = input.firstInputSample;
      positioned = true;
    }
    std::vector<RadarFrame> result;
    std::size_t offset = 0;
    while (offset < count)
    {
      // Normal live capture produces an exact CPI per lane. Load that block
      // straight into the retained DSP buffers instead of first widening it
      // into the partial-CPI accumulator and then copying it a second time.
      if (pendingCount == 0 && count - offset >= samples)
      {
        if (directFloatBorrowed)
        {
          result.push_back(process_float_cpi(input, offset,
            pendingFirstInputSample));
          pendingFirstInputSample += uint64_t(samples) * decimation;
          offset += samples;
          continue;
        }
        for (std::size_t channel = 0; channel < pending.size(); ++channel)
        {
          auto& destination = ownedChannels[channel]->resize_for_write(samples);
          std::transform(input.channels[channel].begin() + offset,
            input.channels[channel].begin() + offset + samples,
            destination.begin(), [](const std::complex<float> value) {
              return std::complex<double>(value);
            });
        }
        result.push_back(process_loaded_cpi(pendingFirstInputSample));
        pendingFirstInputSample += uint64_t(samples) * decimation;
        offset += samples;
        continue;
      }
      const std::size_t copied = std::min<std::size_t>(
        samples - pendingCount, count - offset);
      for (std::size_t channel = 0; channel < pending.size(); ++channel)
      {
        if (pending[channel].empty()) pending[channel].resize(samples);
        std::transform(input.channels[channel].begin() + offset,
          input.channels[channel].begin() + offset + copied,
          pending[channel].begin() + pendingCount,
          [](const std::complex<float> value) {
            return std::complex<double>(value);
          });
      }
      pendingCount += copied;
      offset += copied;
      if (pendingCount == samples)
      {
        for (std::size_t channel = 0; channel < pending.size(); ++channel)
          ownedChannels[channel]->assign_complex(pending[channel].data(), samples);
        result.push_back(process_loaded_cpi(pendingFirstInputSample));
        pendingFirstInputSample += uint64_t(samples) * decimation;
        pendingCount = 0;
      }
    }
    return result;
  }

  RadarFrame process_loaded_cpi(uint64_t first)
  {
    IqData* selectedReference = &reference;
    if (config.arrayReference)
      stages.reference().process_into(referenceChannels, reference);
    else
      selectedReference = ownedChannels[config.referenceChannel].get();
    if (!stages.clutter(*selectedReference, channels))
      throw std::runtime_error("Multi-illuminator clutter reference was rejected");
    stages.ambiguity(*selectedReference, channels, ambiguity, false);
    return finish_processed_cpi(first);
  }

  RadarFrame process_float_cpi(
    const channelizer::SubbandChannelizer::Output& input,
    std::size_t offset, uint64_t first)
  {
    std::vector<const std::complex<float>*> surveillance;
    surveillance.reserve(config.surveillanceChannels.size());
    for (const uint32_t channel : config.surveillanceChannels)
      surveillance.push_back(input.channels[channel].data() + offset);
    if (!stages.clutter_float(
          input.channels[config.referenceChannel].data() + offset,
          surveillance, samples))
      throw std::runtime_error("Multi-illuminator clutter reference was rejected");
    stages.ambiguity_borrowed(ambiguity);
    return finish_processed_cpi(first);
  }

  RadarFrame finish_processed_cpi(uint64_t first)
  {
    std::vector<Map<std::complex<double>>*> maps;
    maps.reserve(ambiguity.size());
    for (const auto& processor : ownedAmbiguity) maps.push_back(processor->result());
    if (!fusedMap || fusedMap.use_count() != 1)
      fusedMap = std::make_shared<Map<std::complex<double>>>(
        maps.front()->get_nRows(), maps.front()->get_nCols());
    fusion.process_into(maps, *fusedMap);
    auto map = fusedMap;
    map->set_metrics();
    auto firstPass = detector.process(map.get());
    auto centered = centroid.process(firstPass.get());
    auto detections = interpolate.process(centered.get(), map.get());
    const uint64_t end = first + uint64_t(samples - 1) * decimation;
    auto tracks = tracker.process(detections.get(),
      (end * 1000) / captureRate);
    RadarFrame result;
    result.illuminatorId = definition.id;
    result.frequencyHz = definition.frequencyHz;
    result.firstInputSample = first;
    result.endInputSample = end;
    result.sampleRateHz = definition.outputRateHz;
    result.map = std::move(map);
    result.detections = std::move(detections);
    result.tracks = std::move(tracks);
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
  const std::size_t available = blah2::available_cpu_threads();
  const std::size_t outerMaximum = std::min(
    std::max<std::size_t>(1, available - 1), illuminators.size());
  const std::size_t outerWorkers = config_.laneWorkers ?
    std::min(config_.laneWorkers, outerMaximum) : outerMaximum;
  config_.workers = std::max<std::size_t>(1,
    std::min(config_.workers, available / outerWorkers));
  contexts_.reserve(illuminators.size());
  for (const auto& illuminator : illuminators)
    contexts_.push_back(std::make_unique<Context>(captureRateHz_,
      channelCount_, illuminator, config_));
  executor_ = std::make_unique<utility::FixedExecutor>(outerWorkers);
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
    std::vector<std::vector<RadarFrame>> perContext(contexts_.size());
    executor_->parallel_for(contexts_.size(), [&](std::size_t index) {
      perContext[index] = contexts_[index]->consume(
        byId.at(contexts_[index]->definition.id)->samples);
    });
    std::vector<RadarFrame> result;
    for (auto& frames : perContext)
      for (auto& frame : frames) result.push_back(std::move(frame));
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
