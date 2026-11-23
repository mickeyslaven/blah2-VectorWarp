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

bool Detection::valid_evidence(const Evidence& value)
{
  if (!value.newestSequence || (value.depth != 1 && value.depth != 2 &&
      value.depth != 4 && value.depth != 8) || value.newestSequence != value.sequences[0]) return false;
  for (uint8_t i = 1; i < value.depth; ++i)
    if (!value.sequences[i] || value.sequences[i]>=value.sequences[i-1] ||
        value.sequences[i-1] - value.sequences[i] != 1) return false;
  return true;
}

rapidjson::Value Detection::identity_json(uint64_t epoch, uint64_t sequence,
    rapidjson::Document::AllocatorType& allocator)
{
  rapidjson::Value value(rapidjson::kObjectType);
  value.AddMember("epoch_exact", rapidjson::Value(std::to_string(epoch).c_str(), allocator), allocator);
  value.AddMember("newest_sequence_exact", rapidjson::Value(std::to_string(sequence).c_str(), allocator), allocator);
  return value;
}

rapidjson::Value Detection::evidence_json(const Evidence& item,
    rapidjson::Document::AllocatorType& allocator, bool compact)
{
  if (!valid_evidence(item)) return rapidjson::Value();
  rapidjson::Value value(rapidjson::kObjectType), sequences(rapidjson::kArrayType);
  if (!compact) value.AddMember("kind", "temporal_power", allocator);
  value.AddMember("epoch_exact", rapidjson::Value(std::to_string(item.epoch).c_str(), allocator), allocator);
  value.AddMember("newest_sequence_exact", rapidjson::Value(std::to_string(item.newestSequence).c_str(), allocator), allocator);
  value.AddMember("cpis", item.depth, allocator);
  if (!compact) {
    value.AddMember("bank", item.bank, allocator);
    value.AddMember("delay_motion_bins", item.delayMotion, allocator);
    value.AddMember("doppler_motion_bins", item.dopplerMotion, allocator);
  }
  for (uint8_t age = 0; age < item.depth; ++age)
    sequences.PushBack(rapidjson::Value(std::to_string(item.sequences[age]).c_str(), allocator), allocator);
  value.AddMember("support_sequences_exact", sequences, allocator);
  return value;
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
  if (evidenceSequence) document.AddMember("evidence_identity",
    identity_json(evidenceEpoch,evidenceSequence,allocator),allocator);
  if (!evidence.empty()) {
    rapidjson::Value support(rapidjson::kArrayType);
    for (const auto& item : evidence) support.PushBack(evidence_json(item, allocator), allocator);
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
  if (!evidence.empty() || evidenceSequence) return delay_bin_to_km(to_json(timestamp), fs);
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

std::string Detection::to_calibration_evidence_json(uint64_t timestamp) const
{
  if (doppler.size() != delay.size() || snr.size() != delay.size() ||
      (!evidence.empty() && evidence.size() != delay.size()))
    throw std::invalid_argument("Calibration evidence dimensions differ");
  rapidjson::StringBuffer buffer;
  rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
  writer.StartObject();
  writer.Key("record_kind"); writer.String("canonical-temporal-score-evidence-v1");
  writer.Key("timestamp_exact"); writer.String(std::to_string(timestamp).c_str());
  writer.Key("candidates"); writer.StartArray();
  for (size_t index = 0; index < delay.size(); ++index) {
    if (!std::isfinite(delay[index]) || !std::isfinite(doppler[index]) || !std::isfinite(snr[index]))
      throw std::invalid_argument("Calibration evidence is non-finite");
    writer.StartObject();
    writer.Key("candidate_index"); writer.Uint64(index);
    writer.Key("delay_bins"); writer.Double(delay[index]);
    writer.Key("doppler_hz"); writer.Double(doppler[index]);
    writer.Key("score"); writer.Double(snr[index]);
    if (!evidence.empty()) {
      const auto& item = evidence[index];
      if (!valid_evidence(item))
        throw std::invalid_argument("Calibration evidence lineage is invalid");
      writer.Key("score_family"); writer.String("temporal_depth");
      writer.Key("depth"); writer.Uint(item.depth);
      writer.Key("bank"); writer.Uint(item.bank);
      writer.Key("delay_motion_bins"); writer.Int(item.delayMotion);
      writer.Key("doppler_motion_bins"); writer.Int(item.dopplerMotion);
      writer.Key("epoch_exact"); writer.String(std::to_string(item.epoch).c_str());
      writer.Key("newest_sequence_exact"); writer.String(std::to_string(item.newestSequence).c_str());
      writer.Key("support_sequences_exact"); writer.StartArray();
      for (uint8_t age = 0; age < item.depth; ++age)
        writer.String(std::to_string(item.sequences[age]).c_str());
      writer.EndArray();
    } else {
      writer.Key("score_family"); writer.String("ordinary");
      writer.Key("depth"); writer.Uint(1);
    }
    writer.EndObject();
  }
  writer.EndArray(); writer.EndObject();
  return {buffer.GetString(), buffer.GetSize()};
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
