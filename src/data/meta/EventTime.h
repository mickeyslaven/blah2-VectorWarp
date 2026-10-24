#pragma once

#include <cstdint>
#include <limits>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <stdexcept>
#include <string>
#include <utility>

namespace blah2
{
// Heimdall V2 supplies a coherent sample sequence but no RF/device timestamp.
// Preserve that useful relative timing while explicitly refusing to describe the
// host-arrival-derived epoch as exact UTC.
struct EventTimeProvenance
{
  bool present = false;
  bool estimateAvailable = false;
  int64_t estimateNanoseconds = 0;
  int64_t epochEstimateNanoseconds = 0;
  uint64_t epoch = 0;
  uint64_t firstSample = 0;
  uint64_t endSample = 0;
  uint32_t sampleRate = 0;
  int64_t hostReceiptResidualNanoseconds = 0;
  int64_t hostReceiptMaximumResidualNanoseconds = 0;
  int64_t hostReceiptDriftPpm = 0;
  uint32_t hostReceiptAnchors = 0;
  std::string source = "kraken_heimdall_host_arrival_estimate";
  bool deviceTimeAvailable = false;
  int64_t deviceTimeEpochTick = 0;
  std::string clockSource, timeSource, motherboard, subdevices;
  std::string reason;
};

inline EventTimeProvenance kraken_event_time_unavailable(
  std::string reason = "Kraken sample-time epoch is unavailable")
{
  EventTimeProvenance value;
  value.present = true;
  value.reason = std::move(reason);
  return value;
}

inline EventTimeProvenance usrp_event_time_unavailable(
  std::string reason = "USRP device sample time is unavailable")
{
  EventTimeProvenance value;
  value.present = true;
  value.source = "usrp_uhd_device_time";
  value.reason = std::move(reason);
  return value;
}

inline EventTimeProvenance hackrf_event_time_unavailable(
  std::string reason = "HackRF paired sample epoch is unavailable")
{
  EventTimeProvenance value;
  value.present = true;
  value.source = "hackrf_shared_trigger_relative_sample_epoch";
  value.reason = std::move(reason);
  return value;
}

inline EventTimeProvenance rspduo_event_time_unavailable(
  std::string reason = "RSPduo callback sample time has no RF/UTC epoch")
{
  EventTimeProvenance value;
  value.present = true;
  value.source = "sdrplay_callback_sample_counter";
  value.reason = std::move(reason);
  return value;
}

inline EventTimeProvenance rspduo_cpi_sample_epoch(uint64_t epoch,
  uint64_t firstSample, uint64_t endSample, uint32_t sampleRate)
{
  auto value = rspduo_event_time_unavailable();
  value.epoch = epoch;
  value.firstSample = firstSample;
  value.endSample = endSample;
  value.sampleRate = sampleRate;
  if (endSample <= firstSample || !sampleRate) return value;
  value.reason = "SDRplay callback counters preserve relative sample order, but the API supplies no exact RF/UTC epoch";
  return value;
}

inline EventTimeProvenance hackrf_cpi_sample_epoch(uint64_t epoch,
  uint64_t firstSample, uint64_t endSample, uint32_t sampleRate)
{
  auto value = hackrf_event_time_unavailable();
  value.epoch = epoch;
  value.firstSample = firstSample;
  value.endSample = endSample;
  value.sampleRate = sampleRate;
  if (!epoch || endSample <= firstSample || !sampleRate) return value;
  value.reason = "Shared trigger preserves a paired relative sample epoch, but HackRF supplies no RF/UTC timestamp";
  return value;
}

inline EventTimeProvenance usrp_cpi_midpoint_time(int64_t epochTick,
  uint64_t firstSample, uint64_t endSample, uint32_t sampleRate,
  std::string clockSource, std::string timeSource,
  std::string motherboard, std::string subdevices)
{
  auto value = usrp_event_time_unavailable();
  value.epoch = 0;
  value.firstSample = firstSample;
  value.endSample = endSample;
  value.sampleRate = sampleRate;
  value.deviceTimeEpochTick = epochTick;
  value.clockSource = std::move(clockSource);
  value.timeSource = std::move(timeSource);
  value.motherboard = std::move(motherboard);
  value.subdevices = std::move(subdevices);
  if (epochTick < 0 || endSample <= firstSample || !sampleRate ||
      value.clockSource.empty() || value.timeSource.empty() ||
      value.motherboard.empty() || value.subdevices.empty())
    return value;
  value.deviceTimeAvailable = true;
  value.reason = "UHD device time is sample-contiguous but is not mapped to UTC";
  return value;
}

inline EventTimeProvenance kraken_cpi_midpoint_time(int64_t epochNanoseconds,
  uint64_t epoch, uint64_t firstSample, uint64_t endSample,
  uint32_t sampleRate, int64_t hostReceiptResidualNanoseconds = 0,
  int64_t hostReceiptMaximumResidualNanoseconds = 0,
  int64_t hostReceiptDriftPpm = 0, uint32_t hostReceiptAnchors = 0)
{
  auto value = kraken_event_time_unavailable();
  value.epochEstimateNanoseconds = epochNanoseconds;
  value.epoch = epoch;
  value.firstSample = firstSample;
  value.endSample = endSample;
  value.sampleRate = sampleRate;
  value.hostReceiptResidualNanoseconds = hostReceiptResidualNanoseconds;
  value.hostReceiptMaximumResidualNanoseconds = hostReceiptMaximumResidualNanoseconds;
  value.hostReceiptDriftPpm = hostReceiptDriftPpm;
  value.hostReceiptAnchors = hostReceiptAnchors;
  if (epochNanoseconds <= 0 || endSample <= firstSample || sampleRate == 0)
    return value;
  const __int128 sampleNumerator = static_cast<__int128>(firstSample) +
    static_cast<__int128>(endSample);
  const __int128 offset = sampleNumerator * 1000000000LL /
    (static_cast<__int128>(sampleRate) * 2);
  const __int128 estimate = static_cast<__int128>(epochNanoseconds) + offset;
  if (estimate <= 0 || estimate > std::numeric_limits<int64_t>::max())
    return value;
  value.estimateAvailable = true;
  value.estimateNanoseconds = static_cast<int64_t>(estimate);
  value.reason = "Heimdall V2 has no device timestamp; UTC is estimated from host arrival with unbounded transport latency";
  return value;
}

inline uint64_t event_time_milliseconds(const EventTimeProvenance& value,
  uint64_t fallback)
{
  return value.estimateAvailable ?
    static_cast<uint64_t>(value.estimateNanoseconds / 1000000LL) : fallback;
}

inline rapidjson::Value event_time_json(const EventTimeProvenance& value,
  rapidjson::Document::AllocatorType& allocator)
{
  rapidjson::Value result(rapidjson::kObjectType);
  result.AddMember("schema", "vectorwarp-event-time-v1", allocator);
  result.AddMember("source", rapidjson::Value(value.source.c_str(), allocator), allocator);
  result.AddMember("exact", false, allocator);
  result.AddMember("uncertaintyBounded", false, allocator);
  result.AddMember("uncertaintyNs", rapidjson::Value(rapidjson::kNullType), allocator);
  result.AddMember("estimateAvailable", value.estimateAvailable, allocator);
  const auto text = [&](int64_t number) {
    const auto serialized = std::to_string(number);
    return rapidjson::Value(serialized.c_str(), allocator);
  };
  const auto unsignedText = [&](uint64_t number) {
    const auto serialized = std::to_string(number);
    return rapidjson::Value(serialized.c_str(), allocator);
  };
  if (value.estimateAvailable)
    result.AddMember("estimateNs", text(value.estimateNanoseconds), allocator);
  else result.AddMember("estimateNs", rapidjson::Value(rapidjson::kNullType), allocator);
  if (value.epochEstimateNanoseconds > 0)
    result.AddMember("epochEstimateNs", text(value.epochEstimateNanoseconds), allocator);
  else result.AddMember("epochEstimateNs", rapidjson::Value(rapidjson::kNullType), allocator);
  result.AddMember("epoch", unsignedText(value.epoch), allocator);
  result.AddMember("firstSample", unsignedText(value.firstSample), allocator);
  result.AddMember("endSample", unsignedText(value.endSample), allocator);
  result.AddMember("sampleRateHz", value.sampleRate, allocator);
  if (value.source == "kraken_heimdall_host_arrival_estimate") {
    rapidjson::Value hostReceipt(rapidjson::kObjectType);
    hostReceipt.AddMember("anchors", value.hostReceiptAnchors, allocator);
    hostReceipt.AddMember("residualNs", text(value.hostReceiptResidualNanoseconds), allocator);
    hostReceipt.AddMember("maximumResidualNs", text(value.hostReceiptMaximumResidualNanoseconds), allocator);
    hostReceipt.AddMember("driftPpm", text(value.hostReceiptDriftPpm), allocator);
    hostReceipt.AddMember("boundedRfUtc", false, allocator);
    hostReceipt.AddMember("note", "Robust host-receipt/sample fit; USB/TCP latency remains unbounded", allocator);
    result.AddMember("hostReceipt", hostReceipt, allocator);
  }
  if (value.source == "usrp_uhd_device_time") {
    result.AddMember("deviceTimeAvailable", value.deviceTimeAvailable, allocator);
    if (value.deviceTimeAvailable) {
      result.AddMember("deviceTimeEpochTick", text(value.deviceTimeEpochTick), allocator);
      const __int128 midpointNumerator =
        static_cast<__int128>(value.deviceTimeEpochTick) * 2 +
        static_cast<__int128>(value.firstSample) +
        static_cast<__int128>(value.endSample);
      if (midpointNumerator >= std::numeric_limits<int64_t>::min() &&
          midpointNumerator <= std::numeric_limits<int64_t>::max())
        result.AddMember("deviceTimeMidpointTickNumerator",
          text(static_cast<int64_t>(midpointNumerator)), allocator);
      else result.AddMember("deviceTimeMidpointTickNumerator",
        rapidjson::Value(rapidjson::kNullType), allocator);
      result.AddMember("deviceTimeMidpointTickDenominator", 2, allocator);
    } else {
      result.AddMember("deviceTimeEpochTick", rapidjson::Value(rapidjson::kNullType), allocator);
      result.AddMember("deviceTimeMidpointTickNumerator", rapidjson::Value(rapidjson::kNullType), allocator);
      result.AddMember("deviceTimeMidpointTickDenominator", rapidjson::Value(rapidjson::kNullType), allocator);
    }
    result.AddMember("clockSource", rapidjson::Value(value.clockSource.c_str(), allocator), allocator);
    result.AddMember("timeSource", rapidjson::Value(value.timeSource.c_str(), allocator), allocator);
    result.AddMember("motherboard", rapidjson::Value(value.motherboard.c_str(), allocator), allocator);
    result.AddMember("subdevices", rapidjson::Value(value.subdevices.c_str(), allocator), allocator);
  }
  result.AddMember("reason", rapidjson::Value(value.reason.c_str(), allocator), allocator);
  return result;
}

inline std::string annotate_event_time(std::string json,
  const EventTimeProvenance& value)
{
  if (!value.present || json.empty()) return json;
  rapidjson::Document document;
  document.Parse(json.c_str());
  if (document.HasParseError() || !document.IsObject())
    throw std::runtime_error("Cannot attach event-time provenance to invalid JSON");
  if (document.HasMember("eventTime"))
    throw std::runtime_error("Published JSON already contains event-time provenance");
  document.AddMember("eventTime", event_time_json(value, document.GetAllocator()),
    document.GetAllocator());
  rapidjson::StringBuffer buffer;
  rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
  writer.SetMaxDecimalPlaces(2);
  document.Accept(writer);
  return {buffer.GetString(), buffer.GetSize()};
}
}
