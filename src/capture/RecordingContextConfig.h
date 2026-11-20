#pragma once
#include "RecordingAcquisitionJson.h"
#include "Source.h"
#include "data/Yaml.h"

namespace blah2::acquisition {
inline std::vector<double> requestedGainsFromDevice(c4::yml::ConstNodeRef device) {
  uint32_t channels = 2;
  if (device.has_child("channel_count")) device["channel_count"] >> channels;
  const auto parse = [&](c4::yml::ConstNodeRef node) {
    std::vector<double> result;
    const auto number = [](c4::yml::ConstNodeRef value) {
      std::string text; value >> text; size_t consumed = 0;
      const double gain = std::stod(text, &consumed);
      if (consumed != text.size() || !std::isfinite(gain))
        throw std::invalid_argument("Invalid declared recording gain");
      return gain;
    };
    if (node.is_seq()) for (const auto value : node.children()) result.push_back(number(value));
    else {
      std::string text; node >> text;
      if (text == "keep") return result;
      result.assign(channels, number(node));
    }
    if (result.size() != channels) throw std::invalid_argument("Requested recording gains have invalid dimensions");
    return result;
  };
  if (device.has_child("gain")) return parse(device["gain"]);
  if (device.has_child("heimdall")) {
    const auto settings = device["heimdall"];
    if (settings.has_child("channel_gains")) return parse(settings["channel_gains"]);
    if (settings.has_child("gain")) return parse(settings["gain"]);
  }
  return {};
}

inline RecordingContext contextFromConfig(c4::yml::NodeRef root,
    const std::string& configuration, const std::string& referenceMode,
    const std::vector<uint32_t>& referenceChannels,
    const std::vector<uint32_t>& surveillanceChannels, bool coherent) {
  RecordingContext result;
  result.referenceMode = referenceMode; result.referenceChannels = referenceChannels;
  result.surveillanceChannels = surveillanceChannels; result.coherent = coherent;
  result.configurationSha256 = digest(configuration);
  result.codeSha256 = Source::recording_code_sha256();
  {
    rapidjson::Document sites(rapidjson::kObjectType); auto& a = sites.GetAllocator();
    sites.AddMember("schema", "vectorwarp-recorded-geographic-sites-v1", a);
    sites.AddMember("provenance", "operator-configured-at-acquisition", a);
    const auto site = [&](c4::yml::ConstNodeRef node) {
      rapidjson::Value value(rapidjson::kObjectType);
      for (const char* key : {"latitude", "longitude", "altitude"}) {
        const auto name = ryml::to_csubstr(key);
        if (!node.has_child(name)) return rapidjson::Value(rapidjson::kNullType);
        double number = 0; node[name] >> number;
        value.AddMember(rapidjson::Value(key, a), rapidjson::Value(number), a);
      }
      for (const char* key : {"site_id", "name"}) if (node.has_child(ryml::to_csubstr(key))) {
        std::string text; node[ryml::to_csubstr(key)] >> text;
        value.AddMember(rapidjson::Value(key, a), rapidjson::Value(text.c_str(), a), a);
      }
      return value;
    };
    rapidjson::Value rx, tx, lanes(rapidjson::kObjectType);
    if (root.has_child("location")) {
      const auto location = root["location"];
      if (location.has_child("rx")) rx = site(location["rx"]);
      if (location.has_child("tx")) tx = site(location["tx"]);
    }
    const auto process = root["process"];
    if (process.has_child("multi_illuminator")) {
      const auto multi = process["multi_illuminator"];
      bool active = false; if (multi.has_child("enabled")) multi["enabled"] >> active;
      if (active && multi.has_child("illuminators")) for (const auto lane : multi["illuminators"].children()) {
        bool enabled = false; if (lane.has_child("enabled")) lane["enabled"] >> enabled;
        if (!enabled) continue;
        rapidjson::Value transmitter;
        if (lane.has_child("transmitter")) transmitter = site(lane["transmitter"]);
        if (lane.has_child("transmitter_location")) {
          auto native = site(lane["transmitter_location"]);
          if (!native.IsNull() && !transmitter.IsNull())
            for (const char* key : {"latitude", "longitude", "altitude"})
              if (native[key].GetDouble() != transmitter[key].GetDouble())
                throw std::invalid_argument("Recording lane transmitter and transmitter_location disagree");
          if (transmitter.IsNull()) transmitter = std::move(native);
        }
        if (transmitter.IsNull()) continue;
        std::string id; uint64_t frequency = 0; lane["id"] >> id; lane["frequency_hz"] >> frequency;
        rapidjson::Value value(rapidjson::kObjectType);
        value.AddMember("frequencyHz", frequency, a); value.AddMember("transmitter", transmitter, a);
        lanes.AddMember(rapidjson::Value(id.c_str(), a), value, a);
      }
    }
    sites.AddMember("rx", rx, a); sites.AddMember("tx", tx, a); sites.AddMember("illuminators", lanes, a);
    if (validGeographicSites(sites)) result.geographicSitesJson = encode(sites);
  }
  const auto device = root["capture"]["device"];
  // Normalize declared total dB gains at ingress. Stage gains/reductions are
  // not interchangeable with this quantity and remain in receiver details.
  result.requestedChannelGainDb = requestedGainsFromDevice(device);
  if (device.has_child("array_geometry")) {
    const auto geometry = device["array_geometry"];
    bool mapping = false, confirmed = false, orientation = false;
    if (geometry.has_child("mapping_confirmed")) geometry["mapping_confirmed"] >> mapping;
    if (geometry.has_child("geometry_confirmed")) geometry["geometry_confirmed"] >> confirmed;
    if (geometry.has_child("orientation_confirmed")) geometry["orientation_confirmed"] >> orientation;
    result.geometryConfirmed = mapping && confirmed && orientation;
    rapidjson::Document doc(rapidjson::kObjectType); auto& a = doc.GetAllocator();
    for (const char* key : {"units", "coordinate_frame", "shape"})
      if (geometry.has_child(ryml::to_csubstr(key))) {
      std::string value; geometry[ryml::to_csubstr(key)] >> value;
      doc.AddMember(rapidjson::Value(key, a), rapidjson::Value(value.c_str(), a), a);
    }
    if (geometry.has_child("x_axis_bearing_deg_true")) {
      double bearing = 0; geometry["x_axis_bearing_deg_true"] >> bearing;
      doc.AddMember("x_axis_bearing_deg_true", bearing, a);
    }
    rapidjson::Value elements(rapidjson::kArrayType);
    if (geometry.has_child("elements")) for (const auto element : geometry["elements"].children()) {
      uint32_t id = UINT32_MAX; element["daq_channel"] >> id;
      rapidjson::Value value(rapidjson::kObjectType), position(rapidjson::kArrayType);
      value.AddMember("daq_channel", id, a);
      for (const auto coordinate : element["position"].children()) {
        double number = 0; coordinate >> number; position.PushBack(number, a);
      }
      value.AddMember("position", position, a); elements.PushBack(value, a);
    }
    doc.AddMember("elements", elements, a); result.geometryJson = encode(doc);
  }
  const auto remember = [&](const std::string& name, c4::yml::ConstNodeRef node, const char* key) {
    const auto keyName = ryml::to_csubstr(key);
    if (!node.has_child(keyName)) return;
    std::string path; node[keyName] >> path;
    if (!path.empty()) result.manifoldSha256.emplace(name, fileDigest(path));
  };
  const auto process = root["process"];
  bool bearingEnabled = false, multiEnabled = false;
  if (process.has_child("bearing") && process["bearing"].has_child("enable"))
    process["bearing"]["enable"] >> bearingEnabled;
  if (process.has_child("multi_illuminator") && process["multi_illuminator"].has_child("enabled"))
    process["multi_illuminator"]["enabled"] >> multiEnabled;
  if (bearingEnabled) remember("bearing", process["bearing"], "manifold_file");
  if (process.has_child("surveillance_fusion")) remember("detection", process["surveillance_fusion"], "manifold_file");
  if (multiEnabled && process["multi_illuminator"].has_child("illuminators")) {
    for (const auto lane : process["multi_illuminator"]["illuminators"].children()) {
      bool enabled = false; if (lane.has_child("enabled")) lane["enabled"] >> enabled;
      if (!enabled) continue;
      std::string id; lane["id"] >> id;
      if (bearingEnabled) remember(id + "/bearing", lane, "bearing_manifold_file");
      remember(id + "/detection", lane, "detection_manifold_file");
    }
  }
  return result;
}
} // namespace blah2::acquisition
