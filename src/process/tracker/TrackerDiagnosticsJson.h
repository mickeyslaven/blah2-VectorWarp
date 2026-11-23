#pragma once

#include "process/tracker/Tracker.h"
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <stdexcept>

// Shared host tracker monitor for CPU, portable GPU and resident-front-end
// routes. Pair rejection counters are exclusive; occupancy is a state gauge.
// No evaluation truth enters these products or tracker decisions.
template<class Writer>
void writeTrackerFrameDiagnostics(Writer& writer, const TrackerFrameDiagnostics& d)
{
  writer.StartObject();
  writer.Key("schema"); writer.String("vectorwarp-tracker-frame-diagnostics-v2");
  writer.Key("truth_blind"); writer.Bool(true);
  writer.Key("scope"); writer.String("single-illuminator-single-frame");
  writer.Key("pair_semantics"); writer.String("exclusive-ordered-candidate-track-pair-rejections");
  writer.Key("no_op"); writer.Bool(d.noOp);
  for(const auto& field:trackerDiagnosticFields) {
    writer.Key(field.name); writer.Uint64(d.*field.member);
  }
  writer.EndObject();
}

// Attach to an internally serialized tracker object without parsing or rounding
// the scientific payload. Both standalone and channel envelopes use this owner.
inline std::string annotateTrackerFrameDiagnostics(std::string json,
    const TrackerFrameDiagnostics& diagnostics)
{
  if(json.empty()||json.back()!='}')throw std::invalid_argument("Tracker publication is not an object");
  rapidjson::StringBuffer buffer;
  rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
  writeTrackerFrameDiagnostics(writer,diagnostics);
  json.pop_back();if(json.size()>1)json+=',';
  json+="\"trackerDiagnostics\":";json+=buffer.GetString();json+='}';
  return json;
}
