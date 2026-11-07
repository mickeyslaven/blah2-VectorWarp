#include "process/conditioning/IlluminatorChannelizerBank.h"
#include "process/utility/FftwThreads.h"
#include "process/utility/ProcessingThreads.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstring>
#include <cstdlib>
#include <fftw3.h>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <thread>
#include <unordered_set>

namespace blah2::channelizer
{
namespace
{
constexpr double pi = 3.141592653589793238462643383279502884;

std::vector<double> design_filter(uint32_t inputRate, uint32_t outputRate,
  uint32_t occupied, uint32_t tapsPerPhase)
{
  const uint32_t decimation = inputRate / outputRate;
  std::size_t count = std::size_t(tapsPerPhase) * decimation + 1;
  if (!(count & 1)) ++count;
  const double normalized = (occupied / 2.0 + outputRate / 2.0) /
    2.0 / inputRate;
  const double middle = (count - 1) / 2.0;
  std::vector<double> taps(count);
  double sum = 0;
  for (std::size_t index = 0; index < count; ++index)
  {
    const double offset = double(index) - middle;
    const double sinc = offset == 0 ? 2 * normalized :
      std::sin(2 * pi * normalized * offset) / (pi * offset);
    const double window = .54 - .46 *
      std::cos(2 * pi * index / double(count - 1));
    sum += taps[index] = sinc * window;
  }
  for (auto& tap : taps) tap /= sum;
  return taps;
}

bool smooth(std::size_t value)
{
  for (const std::size_t factor : {2U, 3U, 5U, 7U})
    while (value % factor == 0) value /= factor;
  return value == 1;
}
}

struct IlluminatorChannelizerBank::SharedRasterState
{
  uint32_t inputRate = 0, outputRate = 0, decimation = 0;
  std::size_t channels = 0, lanes = 0, taps = 0;
  std::size_t transform = 0, foldedLength = 0, overlap = 0, hop = 0;
  std::vector<int64_t> bins;
  std::vector<double> offsets;
  std::vector<double> groupDelays;
  std::vector<std::complex<double>> kernels;
  std::vector<std::complex<double>> weightedKernels;
  std::vector<std::complex<double>> phase;
  std::size_t phaseFor = std::numeric_limits<std::size_t>::max();
  std::vector<std::vector<std::complex<double>>> history;
  fftw_complex* forwardWork = nullptr;
  fftw_complex* foldedWork = nullptr;
  fftw_plan forward = nullptr, inverse = nullptr;
  std::unique_ptr<utility::FixedExecutor> foldExecutor;
  bool started = false;
  uint64_t nextInputSample = 0;
  IlluminatorChannelizerBank::StageTiming lastTiming;

  SharedRasterState(uint64_t center, uint32_t rate, std::size_t channelCount,
    const std::vector<IlluminatorDefinition>& definitions)
    : inputRate(rate), outputRate(definitions.front().outputRateHz),
      decimation(rate / outputRate), channels(channelCount),
      lanes(definitions.size())
  {
    if (!decimation || rate % outputRate) throw std::invalid_argument("shared raster rate");
    std::vector<std::vector<double>> laneTaps;
    for (const auto& definition : definitions)
    {
      if (definition.outputRateHz != outputRate)
        throw std::invalid_argument("shared raster rates differ");
      laneTaps.push_back(design_filter(rate, outputRate,
        definition.occupiedBandwidthHz, definition.tapsPerPhase));
      if (!taps) taps = laneTaps.back().size();
      if (laneTaps.back().size() != taps)
        throw std::invalid_argument("shared raster tap lengths differ");
      offsets.push_back(double(int64_t(definition.frequencyHz) - int64_t(center)));
      groupDelays.push_back((taps - 1) / 2.0);
    }
    overlap = taps - 1;
    const std::size_t minimumFolded = (2 * taps + decimation - 1) / decimation;
    double bestCost = std::numeric_limits<double>::infinity();
    for (std::size_t candidate = minimumFolded;
         candidate <= minimumFolded * 16; ++candidate)
    {
      if (!smooth(candidate)) continue;
      bool aligned = true;
      for (const double offset : offsets)
      {
        const double bin = offset * candidate / outputRate;
        if (std::abs(bin - std::round(bin)) > 1e-9) aligned = false;
      }
      const std::size_t candidateTransform = candidate * decimation;
      const std::size_t candidateHop = candidateTransform - overlap;
      if (!aligned || !candidateHop || candidateHop % decimation) continue;
      // Amortize the shared forward and all folded inverses per accepted
      // input sample. This selects a practical FFT size rather than merely
      // the first legal raster, which can spend most work recomputing overlap.
      const double cost = (candidateTransform *
        std::log2(double(candidateTransform)) + lanes * candidate *
        std::log2(double(candidate))) / candidateHop;
      if (cost < bestCost)
      {
        bestCost = cost;
        foldedLength = candidate;
      }
    }
    if (!foldedLength) throw std::invalid_argument("shared raster is not aligned");
    transform = foldedLength * decimation;
    hop = transform - overlap;
    if (!hop || hop % decimation)
      throw std::invalid_argument("shared raster hop is not aligned");
    for (const double offset : offsets)
      bins.push_back(int64_t(std::llround(offset * transform / inputRate)));

    forwardWork = fftw_alloc_complex(transform * channels);
    foldedWork = fftw_alloc_complex(foldedLength * channels * lanes);
    kernels.resize(transform * lanes);
    weightedKernels.resize(transform * lanes);
    phase.resize(transform);
    history.assign(channels, std::vector<std::complex<double>>(overlap));
    if (!forwardWork || !foldedWork) throw std::bad_alloc();
    std::memset(forwardWork, 0, sizeof(fftw_complex) * transform * channels);
    std::memset(foldedWork, 0,
      sizeof(fftw_complex) * foldedLength * channels * lanes);
    const unsigned flags = std::getenv("VECTORWARP_FFTW_PLAN") &&
      std::string(std::getenv("VECTORWARP_FFTW_PLAN")) == "estimate" ?
      FFTW_ESTIMATE : FFTW_MEASURE;
    int full = int(transform), shortLength = int(foldedLength);
    forward = fftw_plan_many_dft(1, &full, int(channels), forwardWork,
      nullptr, 1, full, forwardWork, nullptr, 1, full, FFTW_FORWARD, flags);
    inverse = fftw_plan_many_dft(1, &shortLength, int(channels * lanes),
      foldedWork, nullptr, 1, shortLength, foldedWork, nullptr, 1,
      shortLength, FFTW_BACKWARD, flags);
    if (!forward || !inverse)
      throw std::runtime_error("shared raster FFT plan failed");
    // Four fold workers was the best sustained cache/memory-bandwidth point
    // in the same 8/16-core width sweeps used by the radar executor. More
    // workers increase contention without overlapping either FFT plan.
    const std::size_t cpuBudget = std::max<std::size_t>(1,
      blah2::available_cpu_threads());
    foldExecutor = std::make_unique<utility::FixedExecutor>(std::min(
      {lanes * channels, cpuBudget, std::size_t(4)}));
    std::vector<std::complex<double>> filter(transform);
    auto* raw = reinterpret_cast<fftw_complex*>(filter.data());
    fftw_plan filterPlan = fftw_plan_dft_1d(int(transform), raw, raw,
      FFTW_FORWARD, flags);
    if (!filterPlan) throw std::runtime_error("shared raster filter plan failed");
    for (std::size_t lane = 0; lane < lanes; ++lane)
    {
      std::fill(filter.begin(), filter.end(), std::complex<double>{});
      std::copy(laneTaps[lane].begin(), laneTaps[lane].end(), filter.begin());
      fftw_execute(filterPlan);
      std::copy(filter.begin(), filter.end(),
        kernels.begin() + lane * transform);
    }
    fftw_destroy_plan(filterPlan);
  }

  ~SharedRasterState()
  {
    if (forward) fftw_destroy_plan(forward);
    if (inverse) fftw_destroy_plan(inverse);
    if (forwardWork) fftw_free(forwardWork);
    if (foldedWork) fftw_free(foldedWork);
  }

  void reset()
  {
    for (auto& values : history)
      std::fill(values.begin(), values.end(), std::complex<double>{});
    started = false;
    nextInputSample = 0;
  }

  template <typename SampleAt>
  std::vector<IlluminatorOutput> process(std::size_t samples, uint64_t first,
    const std::vector<IlluminatorDefinition>& definitions, SampleAt sampleAt)
  {
    using Clock = std::chrono::steady_clock;
    const bool measureStages = std::getenv(
      "VECTORWARP_CHANNELIZER_STAGE_TIMING") != nullptr;
    lastTiming = {};
    if (started && first != nextInputSample) reset();
    if (samples > std::numeric_limits<uint64_t>::max() - first)
      throw std::overflow_error("shared raster sample position overflow");
    started = true;
    std::vector<IlluminatorOutput> result(lanes);
    for (std::size_t lane = 0; lane < lanes; ++lane)
    {
      result[lane].id = definitions[lane].id;
      result[lane].frequencyHz = definitions[lane].frequencyHz;
      auto& output = result[lane].samples;
      output.channels.resize(channels);
      output.inputSamplesPerOutput = decimation;
      output.groupDelayInputSamples = groupDelays[lane];
      for (auto& channel : output.channels)
        channel.reserve(samples / decimation + 1);
    }
    // hop is a multiple of the decimation, so one call has one decimation
    // phase even when it spans thousands of overlap-save blocks. Computing
    // these phasors once avoids a transcendental O(input samples) hot path.
    const uint32_t callRemainder = uint32_t(first % decimation);
    const std::size_t callFirstOutput = callRemainder ?
      decimation - callRemainder : 0;
    const std::size_t callSpectrumPhase =
      (overlap + callFirstOutput) % decimation;
    if (phaseFor != callSpectrumPhase && callSpectrumPhase)
      for (std::size_t index = 0; index < transform; ++index)
        phase[index] = std::polar(1.0, 2 * pi * double(index) *
          callSpectrumPhase / double(transform));
    else if (phaseFor != callSpectrumPhase)
      std::fill(phase.begin(), phase.end(), std::complex<double>{1, 0});
    if (phaseFor != callSpectrumPhase)
      for (std::size_t lane = 0; lane < lanes; ++lane)
        for (std::size_t bin = 0; bin < transform; ++bin)
          weightedKernels[lane * transform + bin] =
            kernels[lane * transform + bin] * phase[bin];
    phaseFor = callSpectrumPhase;
    for (std::size_t offset = 0; offset < samples; offset += hop)
    {
      const std::size_t count = std::min(hop, samples - offset);
      for (std::size_t channel = 0; channel < channels; ++channel)
      {
        const std::size_t base = channel * transform;
        for (std::size_t index = 0; index < overlap; ++index)
        {
          forwardWork[base + index][0] = history[channel][index].real();
          forwardWork[base + index][1] = history[channel][index].imag();
        }
        for (std::size_t index = 0; index < count; ++index)
        {
          const auto value = sampleAt(channel, offset + index);
          forwardWork[base + overlap + index][0] = value.real();
          forwardWork[base + overlap + index][1] = value.imag();
        }
        for (std::size_t index = overlap + count; index < transform; ++index)
          forwardWork[base + index][0] = forwardWork[base + index][1] = 0;
        if (count >= overlap)
          for (std::size_t index = 0; index < overlap; ++index)
            history[channel][index] = sampleAt(channel,
              offset + count - overlap + index);
        else
        {
          std::move(history[channel].begin() + count, history[channel].end(),
            history[channel].begin());
          for (std::size_t index = 0; index < count; ++index)
            history[channel][overlap - count + index] =
              sampleAt(channel, offset + index);
        }
      }
      Clock::time_point stageStart;
      if (measureStages) stageStart = Clock::now();
      fftw_execute(forward);
      if (measureStages)
        lastTiming.forwardMs += std::chrono::duration<double, std::milli>(
          Clock::now() - stageStart).count();
      const uint64_t blockFirst = first + offset;
      const uint32_t remainder = uint32_t(blockFirst % decimation);
      const std::size_t firstOutput = remainder ? decimation - remainder : 0;
      const std::size_t spectrumPhase = (overlap + firstOutput) % decimation;
      if (measureStages) stageStart = Clock::now();
      foldExecutor->parallel_for(lanes * channels, [&](std::size_t task)
        {
          const std::size_t lane = task / channels;
          const std::size_t channel = task % channels;
          const std::size_t foldedBase =
            (lane * channels + channel) * foldedLength;
          const std::size_t inputBase = channel * transform;
          std::memset(foldedWork + foldedBase, 0,
            sizeof(fftw_complex) * foldedLength);
          const std::size_t shifted = std::size_t(
            (bins[lane] % int64_t(transform) + int64_t(transform)) %
            int64_t(transform));
          // Stream each D-sized spectral partition contiguously. The former
          // k-outer loop touched forward/kernel with an M stride, defeating
          // prefetch and reloading the same hot folded row for every bin.
          for (std::size_t fold = 0; fold < decimation; ++fold)
          {
            const std::size_t binBase = fold * foldedLength;
            std::size_t source = binBase + shifted;
            if (source >= transform) source -= transform;
            for (std::size_t shortBin = 0; shortBin < foldedLength;
                 ++shortBin)
            {
              const std::size_t bin = binBase + shortBin;
              const auto& kernel = weightedKernels[lane * transform + bin];
              const double real = forwardWork[inputBase + source][0];
              const double imaginary = forwardWork[inputBase + source][1];
              foldedWork[foldedBase + shortBin][0] +=
                real * kernel.real() - imaginary * kernel.imag();
              foldedWork[foldedBase + shortBin][1] +=
                real * kernel.imag() + imaginary * kernel.real();
              if (++source == transform) source = 0;
            }
          }
        });
      if (measureStages)
        lastTiming.foldMs += std::chrono::duration<double, std::milli>(
          Clock::now() - stageStart).count();
      if (measureStages) stageStart = Clock::now();
      fftw_execute(inverse);
      if (measureStages)
        lastTiming.inverseMs += std::chrono::duration<double, std::milli>(
          Clock::now() - stageStart).count();
      for (std::size_t lane = 0; lane < lanes; ++lane)
      {
        auto& output = result[lane].samples;
        if (firstOutput < count && !output.hasFirstInputSample)
        {
          output.firstInputSample = blockFirst + firstOutput;
          output.hasFirstInputSample = true;
        }
        const int64_t inputStart = int64_t(blockFirst) - int64_t(overlap);
        const auto basePhase = std::polar(1.0, -2 * pi * offsets[lane] *
          double(inputStart) / inputRate);
        for (std::size_t channel = 0; channel < channels; ++channel)
        {
          const std::size_t foldedBase =
            (lane * channels + channel) * foldedLength;
          for (std::size_t index = firstOutput; index < count;
               index += decimation)
          {
            const std::size_t fullIndex = overlap + index;
            const std::size_t shortIndex =
              ((fullIndex - spectrumPhase) / decimation) % foldedLength;
            output.channels[channel].emplace_back(basePhase *
              std::complex<double>(foldedWork[foldedBase + shortIndex][0],
                foldedWork[foldedBase + shortIndex][1]) / double(transform));
          }
        }
      }
    }
    nextInputSample = first + samples;
    return result;
  }
};

IlluminatorChannelizerBank::~IlluminatorChannelizerBank() = default;

std::size_t IlluminatorChannelizerBank::worker_count() const
{
  return shared_ ? shared_->foldExecutor->size() : executor_->size();
}

IlluminatorChannelizerBank::StageTiming
IlluminatorChannelizerBank::last_stage_timing() const
{
  return shared_ ? shared_->lastTiming : StageTiming{};
}

IlluminatorChannelizerBank::IlluminatorChannelizerBank(
  uint64_t captureCenterHz, uint32_t captureRateHz,
  std::size_t channelCount,
  std::vector<IlluminatorDefinition> illuminators)
  : captureCenterHz_(captureCenterHz), captureRateHz_(captureRateHz),
    channelCount_(channelCount), definitions_(std::move(illuminators))
{
  if (!captureCenterHz_ || !captureRateHz_ || !channelCount_ ||
      definitions_.empty())
    throw std::invalid_argument("Illuminator channelizer bank is empty");
  std::unordered_set<std::string> ids;
  for (const auto& illuminator : definitions_)
  {
    if (illuminator.id.empty() || !ids.insert(illuminator.id).second ||
        !illuminator.frequencyHz ||
        !illuminator.outputRateHz || captureRateHz_ % illuminator.outputRateHz ||
        !illuminator.occupiedBandwidthHz ||
        illuminator.occupiedBandwidthHz >= illuminator.outputRateHz ||
        illuminator.tapsPerPhase < 4 || illuminator.tapsPerPhase > 128 ||
        illuminator.frequencyHz > uint64_t(std::numeric_limits<int64_t>::max()) ||
        captureCenterHz_ > uint64_t(std::numeric_limits<int64_t>::max()))
      throw std::invalid_argument("Illuminator identity is invalid or duplicated");
    const int64_t offset = int64_t(illuminator.frequencyHz) -
      int64_t(captureCenterHz_);
    if (std::abs(double(offset)) + illuminator.occupiedBandwidthHz / 2.0 >=
        captureRateHz_ / 2.0)
      throw std::invalid_argument("Illuminator lies outside the capture passband");
  }
  try
  {
    shared_ = std::make_unique<SharedRasterState>(captureCenterHz_,
      captureRateHz_, channelCount_, definitions_);
  }
  catch (const std::invalid_argument&)
  {
    shared_.reset();
  }
  if (shared_) return;
  channelizers_.reserve(definitions_.size());
  for (const auto& illuminator : definitions_)
  {
    const int64_t offset = int64_t(illuminator.frequencyHz) -
      int64_t(captureCenterHz_);
    channelizers_.push_back(std::make_unique<SubbandChannelizer>(
      SubbandChannelizer::Config{captureRateHz_, illuminator.outputRateHz,
        double(offset), double(illuminator.occupiedBandwidthHz),
        illuminator.tapsPerPhase}, channelCount_));
  }
  const std::size_t available = blah2::available_cpu_threads();
  const std::size_t fftThreads = std::max(1, blah2::fftw_planner_threads());
  const std::size_t laneBudget = std::max<std::size_t>(1,
    (std::max<std::size_t>(1, available) - 1) / std::size_t(fftThreads));
  executor_ = std::make_unique<utility::FixedExecutor>(
    std::min(laneBudget, channelizers_.size()));
}

std::vector<IlluminatorOutput> IlluminatorChannelizerBank::process(
  const std::vector<std::vector<std::complex<float>>>& input,
  uint64_t firstInputSample)
{
  if (shared_)
  {
    if (input.size() != channelCount_)
      throw std::invalid_argument("Illuminator channelizer input count changed");
    const std::size_t samples = input.empty() ? 0 : input.front().size();
    for (const auto& channel : input)
      if (channel.size() != samples)
        throw std::invalid_argument("Illuminator channelizer inputs are unpaired");
    return shared_->process(samples, firstInputSample, definitions_,
      [&input](std::size_t channel, std::size_t sample) {
        return std::complex<double>(input[channel][sample]);
      });
  }
  std::vector<IlluminatorOutput> output(channelizers_.size());
  executor_->parallel_for(channelizers_.size(), [&](std::size_t index) {
    output[index] = {definitions_[index].id, definitions_[index].frequencyHz,
      channelizers_[index]->process(input, firstInputSample)};
  });
  return output;
}

std::vector<IlluminatorOutput> IlluminatorChannelizerBank::process(
  const std::vector<const std::deque<std::complex<double>>*>& input,
  uint64_t firstInputSample)
{
  if (shared_)
  {
    if (input.size() != channelCount_ || std::any_of(input.begin(), input.end(),
          [](const auto* channel) { return channel == nullptr; }))
      throw std::invalid_argument("Illuminator channelizer input count changed");
    const std::size_t samples = input.empty() ? 0 : input.front()->size();
    for (const auto* channel : input)
      if (channel->size() != samples)
        throw std::invalid_argument("Illuminator channelizer inputs are unpaired");
    return shared_->process(samples, firstInputSample, definitions_,
      [&input](std::size_t channel, std::size_t sample) {
        return (*input[channel])[sample];
      });
  }
  std::vector<IlluminatorOutput> output(channelizers_.size());
  executor_->parallel_for(channelizers_.size(), [&](std::size_t index) {
    output[index] = {definitions_[index].id, definitions_[index].frequencyHz,
      channelizers_[index]->process(input, firstInputSample)};
  });
  return output;
}

void IlluminatorChannelizerBank::reset()
{
  if (shared_) { shared_->reset(); return; }
  for (auto& channelizer : channelizers_) channelizer->reset();
}

} // namespace blah2::channelizer
