#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace blah2::capture_topology {

// Receiver identity is interpreted only at the adapter boundary. Downstream
// algorithms consume these facts and an explicit physical-channel mapping.
struct Adapter {
  bool coherent = false, variableChannels = false, appliedGains = false;
};
inline Adapter adapter(const std::string& type) {
  if (type == "Kraken") return {true, true, true};
  if (type == "Usrp" || type == "RspDuo") return {true, false, false};
  if (type == "HackRF") return {};
  throw std::invalid_argument("Unknown capture adapter topology");
}

struct Request {
  uint32_t channelCount = 2;
  std::string referenceMode = "dedicated";
  std::optional<uint32_t> referenceChannel;
  std::optional<std::vector<uint32_t>> referenceChannels, surveillanceChannels;
  bool coherent = false;
};
struct Topology {
  uint32_t channelCount = 0, referenceChannel = 0;
  bool arrayReference = false, coherent = false;
  std::vector<uint32_t> referenceChannels, surveillanceChannels;
};

inline Topology resolve(const Request& request) {
  if (request.channelCount < 2 || request.channelCount > 8 ||
      (request.referenceMode != "dedicated" &&
       request.referenceMode != "array_eigenbeam"))
    throw std::invalid_argument("Invalid capture channel configuration");
  Topology result;
  result.channelCount = request.channelCount;
  result.arrayReference = request.referenceMode == "array_eigenbeam";
  result.coherent = request.coherent;
  result.referenceChannel = request.referenceChannel.value_or(
    !result.arrayReference && request.referenceChannels &&
      request.referenceChannels->size() == 1 ?
      request.referenceChannels->front() : 0);
  if (result.referenceChannel >= result.channelCount)
    throw std::invalid_argument("Invalid dedicated reference channel");
  if (request.referenceChannels) result.referenceChannels = *request.referenceChannels;
  else if (result.arrayReference)
    for (uint32_t channel = 0; channel < result.channelCount; ++channel)
      result.referenceChannels.push_back(channel);
  else result.referenceChannels = {result.referenceChannel};
  if (request.surveillanceChannels) result.surveillanceChannels = *request.surveillanceChannels;
  else for (uint32_t channel = 0; channel < result.channelCount; ++channel)
    if (result.arrayReference || channel != result.referenceChannel)
      result.surveillanceChannels.push_back(channel);
  const auto validate = [&](std::vector<uint32_t>& channels) {
    std::sort(channels.begin(), channels.end());
    if (channels.empty() || channels.back() >= result.channelCount ||
        std::adjacent_find(channels.begin(), channels.end()) != channels.end())
      throw std::invalid_argument("Capture roles require unique in-range channels");
  };
  validate(result.referenceChannels);
  validate(result.surveillanceChannels);
  if (result.arrayReference) {
    if (!result.coherent || result.referenceChannels.size() < 2)
      throw std::invalid_argument("Array reference requires coherent multi-channel input");
  } else if (result.referenceChannels.size() != 1 ||
      result.referenceChannels.front() != result.referenceChannel ||
      std::find(result.surveillanceChannels.begin(), result.surveillanceChannels.end(),
        result.referenceChannel) != result.surveillanceChannels.end())
    throw std::invalid_argument("Dedicated reference channel declarations disagree");
  return result;
}
}
