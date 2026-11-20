// Offline metadata admission for the replay ledger. Reuses the production
// reader/canonical JSON implementation; never opens a receiver or GPU.
#include "capture/Recording.h"
#include <iostream>
#include <limits>
#include <stdexcept>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

int main(int argc, char** argv) try {
  if (argc != 9) throw std::invalid_argument(
    "usage: inspectRecordingProvenance file format channels rate frequency iq-sha256 acquisition-sha256 exact");
  const auto number = [](const char* value) {
    std::size_t end = 0; const std::string text(value);
    if (text.empty() || text.find_first_not_of("0123456789") != std::string::npos)
      throw std::invalid_argument("Invalid recording dimensions");
    const auto parsed = std::stoull(text, &end);
    if (end != text.size() || !parsed || parsed > UINT32_MAX)
      throw std::invalid_argument("Invalid recording dimensions");
    return static_cast<uint32_t>(parsed);
  };
  blah2::ReplayOptions options;
  options.format = argv[2]; options.channels = number(argv[3]);
  options.sampleRate = number(argv[4]); options.frequency = number(argv[5]);
  options.expectedRecordingSha256 = argv[6]; options.expectedAcquisitionSha256 = argv[7];
  if (std::string(argv[8]) != "0" && std::string(argv[8]) != "1")
    throw std::invalid_argument("Invalid exact acquisition selection");
  options.requireExactAcquisition = std::string(argv[8]) == "1";
  blah2::RecordingReader reader(argv[1], options);
  const auto& metadata = reader.metadata(); const auto& provenance = reader.provenance();
  rapidjson::Document output(rapidjson::kObjectType); auto& a = output.GetAllocator();
  const auto text = [&](const char* name, const std::string& value) {
    output.AddMember(rapidjson::Value(name, a), rapidjson::Value(value.c_str(), value.size(), a), a);
  };
  output.AddMember("schema", "vectorwarp-recording-inspection-v1", a);
  text("format", metadata.format); output.AddMember("channels", metadata.channels, a);
  output.AddMember("sample_rate_hz", metadata.sampleRate, a);
  output.AddMember("center_frequency_hz", metadata.frequency, a);
  text("sample_count", std::to_string(metadata.samples));
  text("epoch", std::to_string(provenance.epoch));
  text("source_first_sample", std::to_string(provenance.sourceFirstSample));
  text("recording_sha256", provenance.recordingSha256);
  text("acquisition_sha256", provenance.acquisitionSha256);
  text("gain_provenance", provenance.gainProvenance);
  text("acquisition_reason", provenance.acquisitionReason);
  output.AddMember("acquisition_integrity_valid", provenance.acquisitionIntegrityValid, a);
  output.AddMember("acquisition_hash_trusted", provenance.acquisitionHashTrusted, a);
  output.AddMember("acquisition_capable", provenance.exactAcquisition, a);
  output.AddMember("exact_acquisition", provenance.exactAcquisition && provenance.acquisitionHashTrusted, a);
  output.AddMember("exact_utc", provenance.exactTime, a);
  output.AddMember("geographic_sites_available", provenance.geographicSitesAvailable, a);
  text("geographic_sites_reason", provenance.geographicSitesReason);
  output.AddMember("frame_count", static_cast<uint64_t>(provenance.frames.size()), a);
  rapidjson::StringBuffer bytes; rapidjson::Writer<rapidjson::StringBuffer> writer(bytes);
  if (!output.Accept(writer)) throw std::runtime_error("Cannot serialize recording inspection");
  std::cout << bytes.GetString() << '\n'; return 0;
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n'; return 1;
}
