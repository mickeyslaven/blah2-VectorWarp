#include "process/conditioning/IlluminatorChannelizerBank.h"

#include <limits>
#include <stdexcept>
#include <unordered_set>

namespace blah2::channelizer
{

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
  channelizers_.reserve(definitions_.size());
  for (const auto& illuminator : definitions_)
  {
    if (illuminator.id.empty() || !ids.insert(illuminator.id).second ||
        !illuminator.frequencyHz ||
        illuminator.frequencyHz > uint64_t(std::numeric_limits<int64_t>::max()) ||
        captureCenterHz_ > uint64_t(std::numeric_limits<int64_t>::max()))
      throw std::invalid_argument("Illuminator identity is invalid or duplicated");
    const int64_t offset = int64_t(illuminator.frequencyHz) -
      int64_t(captureCenterHz_);
    channelizers_.push_back(std::make_unique<SubbandChannelizer>(
      SubbandChannelizer::Config{captureRateHz_, illuminator.outputRateHz,
        double(offset), double(illuminator.occupiedBandwidthHz),
        illuminator.tapsPerPhase}, channelCount_));
  }
}

std::vector<IlluminatorOutput> IlluminatorChannelizerBank::process(
  const std::vector<std::vector<std::complex<float>>>& input,
  uint64_t firstInputSample)
{
  std::vector<IlluminatorOutput> output;
  output.reserve(channelizers_.size());
  for (std::size_t index = 0; index < channelizers_.size(); ++index)
    output.push_back({definitions_[index].id, definitions_[index].frequencyHz,
      channelizers_[index]->process(input, firstInputSample)});
  return output;
}

void IlluminatorChannelizerBank::reset()
{
  for (auto& channelizer : channelizers_) channelizer->reset();
}

} // namespace blah2::channelizer
