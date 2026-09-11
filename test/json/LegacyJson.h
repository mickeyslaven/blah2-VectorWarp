#pragma once
// Frozen from 2a9bfdf. Keep this DOM/two-pass oracle independent of production
// serializers: byte equality includes RapidJSON's existing decimal behavior.
#include "data/Map.h"
#include "data/meta/Constants.h"
#include "rapidjson/document.h"
#include "rapidjson/stringbuffer.h"
#include "rapidjson/writer.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace legacy_json {
inline std::string stringify(rapidjson::Document& document) {
  rapidjson::StringBuffer buffer;
  rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
  writer.SetMaxDecimalPlaces(2);
  document.Accept(writer);
  return buffer.GetString();
}
template<class T> std::string map(Map<T>& source, uint64_t timestamp) {
  rapidjson::Document document;
  document.SetObject();
  auto& allocator = document.GetAllocator();
  rapidjson::Value data(rapidjson::kArrayType);
  for (const auto& row : source.data) {
    rapidjson::Value values(rapidjson::kArrayType);
    for (const auto value : row) {
      const double magnitude = std::abs(value);
      if (!std::isfinite(magnitude))
        throw std::runtime_error("Radar map contains a non-finite sample");
      values.PushBack(10 * std::log10(std::max(1e-30, magnitude)) - source.noisePower, allocator);
    }
    data.PushBack(values, allocator);
  }
  rapidjson::Value delay(rapidjson::kArrayType), doppler(rapidjson::kArrayType);
  for (const auto value : source.delay) delay.PushBack(value, allocator);
  for (uint32_t i = 0; i < source.get_nRows(); ++i) doppler.PushBack(source.doppler[i], allocator);
  document.AddMember("timestamp", timestamp, allocator);
  document.AddMember("nRows", source.get_nRows(), allocator);
  document.AddMember("nCols", source.get_nCols(), allocator);
  document.AddMember("noisePower", source.noisePower, allocator);
  document.AddMember("maxPower", source.maxPower, allocator);
  document.AddMember("delay", delay, allocator);
  document.AddMember("doppler", doppler, allocator);
  document.AddMember("data", data, allocator);
  return stringify(document);
}
inline std::string detection(const std::vector<double>& delays,
    const std::vector<double>& dopplers, const std::vector<double>& snrs, uint64_t timestamp) {
  rapidjson::Document document;
  document.SetObject();
  auto& allocator = document.GetAllocator();
  rapidjson::Value delay(rapidjson::kArrayType), doppler(rapidjson::kArrayType), snr(rapidjson::kArrayType);
  for (size_t i = 0; i < delays.size(); ++i) delay.PushBack(delays[i], allocator);
  for (size_t i = 0; i < delays.size(); ++i) doppler.PushBack(dopplers[i], allocator);
  for (size_t i = 0; i < delays.size(); ++i) snr.PushBack(snrs[i], allocator);
  document.AddMember("timestamp", timestamp, allocator);
  document.AddMember("delay", delay, allocator);
  document.AddMember("doppler", doppler, allocator);
  document.AddMember("snr", snr, allocator);
  return stringify(document);
}
template<class Range> std::string kilometres(std::string json, const Range& delay,
    uint32_t fs, const char* invalid) {
  rapidjson::Document document;
  document.SetObject();
  auto& allocator = document.GetAllocator();
  document.Parse(json.c_str());
  if (!fs || document.HasParseError() || !document.IsObject() ||
      !document.HasMember("delay") || !document["delay"].IsArray())
    throw std::invalid_argument(invalid);
  document["delay"].Clear();
  for (const auto value : delay)
    document["delay"].PushBack(1.0 * value * (Constants::c / (double)fs) / 1000, allocator);
  return stringify(document);
}
template<class T> std::string mapKm(Map<T>& source, uint64_t timestamp, uint32_t fs) {
  return kilometres(map(source, timestamp), source.delay, fs,
    "Cannot convert an invalid radar map or zero sample rate");
}
}
