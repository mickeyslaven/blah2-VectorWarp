#include "Detection.h"
#include "JsonArrayFile.h"
#include "data/meta/Constants.h"
#include <iostream>
#include <cstdlib>
#include <chrono>
#include <algorithm>
#include <cmath>
#include <stdexcept>

#include "rapidjson/document.h"
#include "rapidjson/writer.h"
#include "rapidjson/stringbuffer.h"

// constructor
Detection::Detection(std::vector<double> _delay, std::vector<double> _doppler, std::vector<double> _snr)
{
  delay = _delay;
  doppler = _doppler;
  snr = _snr;
}

Detection::Detection(double _delay, double _doppler, double _snr)
{
  delay.push_back(_delay);
  doppler.push_back(_doppler);
  snr.push_back(_snr);
}

std::vector<double> Detection::get_delay()
{
  return delay;
}

std::vector<double> Detection::get_doppler()
{
  return doppler;
}

std::vector<double> Detection::get_snr()
{
  return snr;
}

size_t Detection::get_nDetections()
{
  return delay.size();
}

std::string Detection::to_json(uint64_t timestamp)
{
  rapidjson::Document document;
  document.SetObject();
  rapidjson::Document::AllocatorType &allocator = document.GetAllocator();

  // store delay array
  rapidjson::Value arrayDelay(rapidjson::kArrayType);
  for (size_t i = 0; i < get_nDetections(); i++)
  {
    arrayDelay.PushBack(delay[i], allocator);
  }

  // store Doppler array
  rapidjson::Value arrayDoppler(rapidjson::kArrayType);
  for (size_t i = 0; i < get_nDetections(); i++)
  {
    arrayDoppler.PushBack(doppler[i], allocator);
  }

  // store snr array
  rapidjson::Value arraySnr(rapidjson::kArrayType);
  for (size_t i = 0; i < get_nDetections(); i++)
  {
    arraySnr.PushBack(snr[i], allocator);
  }

  document.AddMember("timestamp", timestamp, allocator);
  document.AddMember("delay", arrayDelay, allocator);
  document.AddMember("doppler", arrayDoppler, allocator);
  document.AddMember("snr", arraySnr, allocator);
  if (!evidence.empty()) {
    rapidjson::Value support(rapidjson::kArrayType);
    for (const auto& item : evidence) {
      rapidjson::Value value(rapidjson::kObjectType), sequences(rapidjson::kArrayType);
      value.AddMember("kind", "temporal_power", allocator);
      value.AddMember("epoch_exact", rapidjson::Value(std::to_string(item.epoch).c_str(), allocator), allocator);
      value.AddMember("newest_sequence_exact", rapidjson::Value(std::to_string(item.newestSequence).c_str(), allocator), allocator);
      value.AddMember("cpis", item.depth, allocator);
      value.AddMember("bank", item.bank, allocator);
      value.AddMember("delay_motion_bins", item.delayMotion, allocator);
      value.AddMember("doppler_motion_bins", item.dopplerMotion, allocator);
      for (uint8_t age = 0; age < item.depth; ++age)
        sequences.PushBack(rapidjson::Value(std::to_string(item.sequences[age]).c_str(), allocator), allocator);
      value.AddMember("support_sequences_exact", sequences, allocator);
      support.PushBack(value, allocator);
    }
    document.AddMember("evidence", support, allocator);
  }
  
  rapidjson::StringBuffer strbuf;
  rapidjson::Writer<rapidjson::StringBuffer> writer(strbuf);
  writer.SetMaxDecimalPlaces(2);
  document.Accept(writer);

  return strbuf.GetString();
}

std::string Detection::to_json_km(uint64_t timestamp, uint32_t fs)
{
  if (!evidence.empty()) return delay_bin_to_km(to_json(timestamp), fs);
  const size_t count = get_nDetections();
  if (doppler.size() < count || snr.size() < count)
    throw std::invalid_argument("Cannot convert invalid detections or zero sample rate");
  // Match the legacy parse/write round trip for large scientific notation;
  // ordinary two-decimal display fields need only one write.
  const auto ordinary = [](double value) {
    return std::isfinite(value) && std::abs(value) <= 1e9;
  };
  if (!fs || !std::all_of(delay.begin(), delay.end(), [fs](double value) {
        return std::isfinite(value) &&
          std::isfinite(1.0 * value * (Constants::c / (double)fs) / 1000);
      }) ||
      !std::all_of(doppler.begin(), doppler.begin() + count, ordinary) ||
      !std::all_of(snr.begin(), snr.begin() + count, ordinary))
    return delay_bin_to_km(to_json(timestamp), fs);

  rapidjson::StringBuffer buffer;
  rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
  writer.SetMaxDecimalPlaces(2);
  writer.StartObject();
  writer.Key("timestamp"); writer.Uint64(timestamp);
  writer.Key("delay"); writer.StartArray();
  for (const auto value : delay)
    writer.Double(1.0 * value * (Constants::c / (double)fs) / 1000);
  writer.EndArray();
  writer.Key("doppler"); writer.StartArray();
  for (size_t i = 0; i < count; ++i) writer.Double(doppler[i]);
  writer.EndArray();
  writer.Key("snr"); writer.StartArray();
  for (size_t i = 0; i < count; ++i) writer.Double(snr[i]);
  writer.EndArray();
  writer.EndObject();
  return std::string(buffer.GetString(), buffer.GetSize());
}

std::string Detection::delay_bin_to_km(std::string json, uint32_t fs)
{
  rapidjson::Document document;
  document.SetObject();
  rapidjson::Document::AllocatorType &allocator = document.GetAllocator();
  document.Parse(json.c_str());

  if (fs == 0 || document.HasParseError() || !document.IsObject() ||
      !document.HasMember("delay") || !document["delay"].IsArray())
    throw std::invalid_argument("Cannot convert invalid detections or zero sample rate");

  document["delay"].Clear();
  for (size_t i = 0; i < delay.size(); i++)
  {
    document["delay"].PushBack(1.0*delay[i]*(Constants::c/(double)fs)/1000, allocator);
  }

  rapidjson::StringBuffer strbuf;
  rapidjson::Writer<rapidjson::StringBuffer> writer(strbuf);
  writer.SetMaxDecimalPlaces(2);
  document.Accept(writer);

  return strbuf.GetString();
}

bool Detection::save(std::string _json, std::string filename)
{
  return blah2::data::appendJsonArray(_json, filename);
}
