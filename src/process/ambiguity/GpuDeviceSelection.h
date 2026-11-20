#pragma once

#include "rapidjson/document.h"
#include <algorithm>
#include <stdexcept>
#include <string>
#include <vector>

namespace blah2 {
// Enumeration establishes feature eligibility, not arithmetic qualification.
// Keep these rules aligned with api/native-processing-capabilities.js: exact
// selectors are never silently replaced and automatic selection is ID-sorted.
inline std::string selectResidentGpuDevice(const std::string& json,
    const std::string& requested = "auto") {
  const auto invalid = []() -> void {
    throw std::runtime_error("Invalid GPU driver status; resident route stopped");
  };
  if (json.empty() || json.size() > 32768) invalid();
  rapidjson::Document report;
  report.Parse(json.data(), json.size());
  if (report.HasParseError() || !report.IsObject() ||
      !report.HasMember("version") || !report["version"].IsInt() || report["version"].GetInt() != 1 ||
      !report.HasMember("available") || !report["available"].IsBool() ||
      !report.HasMember("qualification") || !report["qualification"].IsString() ||
      std::string(report["qualification"].GetString()) != "not-run" ||
      !report.HasMember("devices") || !report["devices"].IsArray() ||
      report["devices"].Size() > 16) invalid();
  if (!report["available"].GetBool())
    throw std::runtime_error("GPU driver unavailable; resident route stopped");
  std::vector<std::string> eligible, seen;
  bool explicitFp32 = false;
  for (const auto& device : report["devices"].GetArray()) {
    if (!device.IsObject() || !device.HasMember("id") || !device["id"].IsString() ||
        device["id"].GetStringLength() == 0 || device["id"].GetStringLength() >= 96 ||
        !device.HasMember("name") || !device["name"].IsString() ||
        device["name"].GetStringLength() == 0 || device["name"].GetStringLength() > 256 ||
        !device.HasMember("shaderFloat64") || !device["shaderFloat64"].IsBool()) invalid();
    const std::string id(device["id"].GetString(), device["id"].GetStringLength());
    if (id.find('\0') != std::string::npos ||
        std::find(seen.begin(), seen.end(), id) != seen.end()) invalid();
    seen.push_back(id);
    if (device["shaderFloat64"].GetBool()) eligible.push_back(id);
    else if (id == requested) explicitFp32 = true;
  }
  std::sort(eligible.begin(), eligible.end());
  if (!requested.empty() && requested != "auto") {
    if (explicitFp32)
      throw std::runtime_error("GPU_NATIVE_UNSUPPORTED shaderFloat64=0 for selected device " + requested);
    if (std::find(eligible.begin(), eligible.end(), requested) == eligible.end())
      throw std::runtime_error("Selected resident GPU device is unavailable: " + requested);
    return requested;
  }
  if (eligible.empty())
    throw std::runtime_error(seen.empty() ? "No GPU devices available; resident route stopped" :
      "GPU_NATIVE_UNSUPPORTED shaderFloat64=0 on every discovered device");
  return eligible.front();
}
}
