#pragma once

#include <cstdint>
#include <string>
#include "rapidjson/document.h"

namespace blah2 {

// Exact strings preserve lineage beyond JavaScript's integer precision.
inline rapidjson::Value sample_interval_json(bool positioned,
    uint64_t epoch, uint64_t first, uint64_t end,
    rapidjson::Document::AllocatorType& allocator) {
  rapidjson::Value receipt(rapidjson::kObjectType);
  // Availability is explicit. Epoch zero is a valid source epoch, including
  // preserved recording lineage; it must not be treated as an unset sentinel.
  const bool bound = positioned && end > first;
  receipt.AddMember("available", bound, allocator);
  receipt.AddMember("sampleInterval", "half-open", allocator);
  if (bound) {
    const auto exact = [&](const char* key, uint64_t value) {
      const auto text = std::to_string(value);
      receipt.AddMember(rapidjson::Value(key, allocator),
        rapidjson::Value(text.c_str(), allocator), allocator);
    };
    exact("captureEpochExact", epoch);
    exact("firstInputSampleExact", first);
    exact("endInputSampleExact", end);
  }
  return receipt;
}

// Host-side receipt of completed stage work, not a device timestamp or a
// build-capability claim. No stale execution claim survives a frame reset.
inline rapidjson::Value stage_execution_json(bool known, bool gpu, bool cpu,
    bool positioned, uint64_t epoch, uint64_t first, uint64_t end,
    rapidjson::Document::AllocatorType& allocator) {
  auto receipt = sample_interval_json(known && positioned, epoch, first, end, allocator);
  const bool bound = receipt["available"].GetBool();
  receipt.AddMember("known", known, allocator);
  receipt.AddMember("gpuExecuted", known && gpu, allocator);
  receipt.AddMember("cpuExecuted", known && cpu, allocator);
  receipt.AddMember("provenance", "processor_stage_receipt", allocator);
  receipt.AddMember("frameBound", bound, allocator);
  return receipt;
}
}
