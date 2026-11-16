#pragma once

#include "data/meta/EventTime.h"
#include "data/meta/StageExecution.h"
#include <cstdint>
#include <string>
#include <vector>

namespace blah2::analysis {

struct NativeProductIdentity {
  std::string illuminatorId;
  uint64_t frequencyHz = 0;
  uint32_t sampleRateHz = 0;
  uint64_t firstInputSample = 0, endInputSample = 0;
  uint32_t inputSamplesPerOutput = 1;
  bool captureSamplePositionAvailable = false;
  uint64_t captureEpoch = 0, captureFirst = 0, captureEnd = 0;
  std::vector<uint32_t> channelIds, referenceChannelIds;
  EventTimeProvenance eventTime;
  std::string productBackend, productDevice;
  bool temporalExecuted = false, detectorExecuted = false, trackerExecuted = false;
  bool bearingExecuted = false;
};

// Add identity without parsing/reformatting the scientific payload. Map and
// detection saves retain their legacy top-level schema and full native raster.
inline std::string annotate_native_product(std::string json,
    const NativeProductIdentity& identity) {
  if (json.size() < 2 || json.front() != '{' || json.back() != '}')
    throw std::invalid_argument("Native product must be a JSON object");
  rapidjson::StringBuffer buffer;
  rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
  writer.StartObject();
  writer.Key("illuminator_id"); writer.String(identity.illuminatorId.c_str());
  writer.Key("frequency_hz"); writer.Uint64(identity.frequencyHz);
  writer.Key("sample_rate_hz"); writer.Uint(identity.sampleRateHz);
  writer.Key("first_input_sample"); writer.Uint64(identity.firstInputSample);
  writer.Key("end_input_sample"); writer.Uint64(identity.endInputSample);
  // Preserve the legacy inclusive last output-sample position. The explicit
  // canonical interval below is half-open in original capture sample units.
  writer.Key("input_samples_per_output"); writer.Uint(identity.inputSamplesPerOutput);
  writer.Key("input_lineage");
  rapidjson::Document lineage;
  const bool validEnd = identity.inputSamplesPerOutput &&
    identity.endInputSample <= UINT64_MAX - identity.inputSamplesPerOutput;
  sample_interval_json(
    identity.captureSamplePositionAvailable && validEnd, identity.captureEpoch,
    identity.firstInputSample, validEnd ?
      identity.endInputSample + identity.inputSamplesPerOutput : 0,
    lineage.GetAllocator()).Accept(writer);
  if (!identity.productBackend.empty()) {
    writer.Key("product_stage_execution"); writer.StartObject();
    const auto stage = [&](const char* name, bool executed) {
      writer.Key(name); writer.StartObject();
      writer.Key("backend"); writer.String(identity.productBackend.c_str());
      writer.Key("device"); writer.String(identity.productDevice.c_str());
      writer.Key("execution");
      stage_execution_json(true, executed && identity.productBackend != "cpu",
        executed && identity.productBackend == "cpu",
        identity.captureSamplePositionAvailable && validEnd, identity.captureEpoch,
        identity.firstInputSample, validEnd ?
          identity.endInputSample + identity.inputSamplesPerOutput : 0,
        lineage.GetAllocator()).Accept(writer);
      writer.EndObject();
    };
    stage("fusion", true);
    stage("temporal", identity.temporalExecuted);
    stage("detector", identity.detectorExecuted);
    stage("tracker", identity.trackerExecuted);
    stage("bearing", identity.bearingExecuted);
    writer.EndObject();
  }
  writer.Key("capture_sample_position_available"); writer.Bool(identity.captureSamplePositionAvailable);
  writer.Key("capture_epoch"); writer.Uint64(identity.captureEpoch);
  writer.Key("capture_first_input_sample"); writer.Uint64(identity.captureFirst);
  writer.Key("capture_end_input_sample"); writer.Uint64(identity.captureEnd);
  if (!identity.channelIds.empty()) {
    writer.Key("channel_ids"); writer.StartArray();
    for (const auto channel : identity.channelIds) writer.Uint(channel);
    writer.EndArray();
  }
  if (!identity.referenceChannelIds.empty()) {
    writer.Key("reference_channel_ids"); writer.StartArray();
    for (const auto channel : identity.referenceChannelIds) writer.Uint(channel);
    writer.EndArray();
  }
  if (identity.eventTime.present) {
    rapidjson::Document metadata;
    writer.Key("eventTime");
    event_time_json(identity.eventTime, metadata.GetAllocator()).Accept(writer);
  }
  writer.EndObject();
  json.pop_back();
  if (json.back() != '{') json += ',';
  json.append(buffer.GetString() + 1, buffer.GetSize() - 1);
  return json;
}
}
