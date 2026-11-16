#pragma once
#include "data/meta/StageExecution.h"
#include "process/ambiguity/AccelerationStatus.h"

namespace blah2 {
inline rapidjson::Value backend_members_json(const AccelerationStatus& status,
    bool known, bool positioned, uint64_t epoch,
    rapidjson::Document::AllocatorType& allocator) {
  rapidjson::Value members(rapidjson::kArrayType);
  for (const auto& member : status.members) {
    rapidjson::Value item(rapidjson::kObjectType);
    const auto text = [&](const char* key, const std::string& value) {
      item.AddMember(rapidjson::Value(key, allocator),
        rapidjson::Value(value.c_str(), allocator), allocator);
    };
    text("laneId", member.laneId); text("backend", member.backend);
    text("executedBackend", known && member.gpuExecuted ? member.executedBackend : "");
    text("device", member.device); text("state", member.state); text("reason", member.reason);
    item.AddMember("execution", stage_execution_json(known,
      member.gpuExecuted, member.cpuExecuted, positioned && member.positioned,
      epoch, member.firstInputSample, member.endInputSample, allocator), allocator);
    members.PushBack(item, allocator);
  }
  return members;
}
}
