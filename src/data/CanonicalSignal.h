#pragma once

#include "data/IqSampleView.h"
#include <algorithm>
#include <array>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <variant>
#include <vector>

namespace blah2::iq {

// Immutable canonical FP32 storage view. Ownership belongs to BorrowedSamples;
// consumers may retain that owner, never an unowned pointer past consumption.
struct SampleView
{
  const std::complex<float>* values = nullptr;
  std::size_t count = 0;
  std::size_t size() const noexcept { return count; }
  const std::complex<float>* data() const noexcept { return values; }
  const std::complex<float>* begin() const noexcept { return values; }
  const std::complex<float>* end() const noexcept { return count ? values + count : values; }
  const std::complex<float>& operator[](std::size_t index) const { return values[index]; }
  const std::complex<float>& at(std::size_t index) const {
    if(index>=count)throw std::out_of_range("Canonical sample view index");
    return values[index];
  }
};

struct BorrowedSamples
{
  // The owner must retain every channel allocation, including deque objects.
  // All aliases must remain immutable until synchronous consumers finish or
  // the last asynchronous consumer releases its retained owner.
  std::shared_ptr<const void> owner;
  using FloatChannels = std::vector<SampleView>;
  using DoubleChannels = std::vector<IqSampleView>;
  std::variant<FloatChannels, DoubleChannels> channels;
};

// Canonical signal storage and physical metadata shared by capture/replay and
// channelization. No receiver or operating-system identity enters processing.
struct CoherentSamples
{
  std::vector<std::vector<std::complex<float>>> channels;
  // Input index at which the first output was produced. The corresponding
  // RF time is earlier by groupDelayInputSamples.
  uint64_t firstInputSample = 0;
  bool hasFirstInputSample = false;
  uint32_t inputSamplesPerOutput = 0;
  double groupDelayInputSamples = 0;
  // Trailing optional representation preserves existing owned aggregate
  // initializers. Exactly one representation may be populated.
  std::optional<BorrowedSamples> borrowed;

  bool isDouble() const noexcept {
    return borrowed && std::holds_alternative<BorrowedSamples::DoubleChannels>(borrowed->channels);
  }
  std::size_t channelCount() const noexcept {
    return borrowed ? std::visit([](const auto& views) { return views.size(); },
      borrowed->channels) : channels.size();
  }
  // Precision-specific access refuses the wrong representation; no narrowing.
  SampleView channel(std::size_t index) const {
    if(borrowed)return std::get<BorrowedSamples::FloatChannels>(borrowed->channels).at(index);
    const auto& owned=channels.at(index);return {owned.data(),owned.size()};
  }
  template<class Consumer> decltype(auto) visitChannel(std::size_t index,
      Consumer&& consume) const {
    if(borrowed)return std::visit([&](const auto& views) -> decltype(auto) {
      return consume(views.at(index));
    }, borrowed->channels);
    return consume(channel(index));
  }
  void copyChannelTo(std::size_t index, std::complex<double>* destination,
      std::size_t count, std::size_t first = 0) const {
    visitChannel(index, [&](const auto& view) {
      if(first>view.size()||count>view.size()-first)
        throw std::out_of_range("Canonical sample copy range");
      if(count&&!destination)throw std::invalid_argument("Canonical sample destination is null");
      if(count)std::copy_n(view.begin()+first,count,destination);
    });
  }
  void validateStorage() const {
    if(borrowed&&(!channels.empty()||!borrowed->owner))
      throw std::invalid_argument("Canonical sample storage ownership is ambiguous");
    if(borrowed)for(std::size_t channel=0;channel<channelCount();++channel)
      visitChannel(channel, [](const auto& view) {
        if(view.size()&&!view.data()&&std::is_same_v<std::decay_t<decltype(view)>,SampleView>)
          throw std::invalid_argument("Canonical sample view is null");
        if(view.size()>std::size_t(std::numeric_limits<std::ptrdiff_t>::max())/sizeof(*view.begin()))
          throw std::overflow_error("Canonical sample view is too large");
      });
  }
};


struct SignalDefinition
{
  std::string id;
  uint64_t frequencyHz = 0;
  uint32_t occupiedBandwidthHz = 0;
  uint32_t outputRateHz = 0;
  uint32_t tapsPerPhase = 24;
  // Optional broadcast-system label used by reference conditioning. Keep it
  // before new optional topology fields so existing aggregate initializers
  // remain source compatible.
  std::string standard;
  std::string atscReferenceMode = "raw";
  bool hasInputRoles = false;
  uint32_t referenceChannel = 0;
  std::vector<uint32_t> surveillanceChannels;
  // Explicit physical site, never inferred from frequency or receiver name.
  std::optional<std::array<double, 3>> transmitterLocation;
  std::string detectionManifoldFile, bearingManifoldFile;
};

struct SignalBlock
{
  std::string id;
  uint64_t frequencyHz = 0;
  CoherentSamples samples;
};


} // namespace blah2::iq
