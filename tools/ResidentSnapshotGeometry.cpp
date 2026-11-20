// Offline metadata only: share the scorer's sealed decoder and lineage identity.
#include "ResidentWeakEvidenceScore.h"
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <iostream>
#include <set>

namespace score = blah2::resident_weak_score;
namespace replay = blah2::integration::coherent_replay;
namespace contract = blah2::integration::cancellation_replay;
namespace {
using Writer = rapidjson::Writer<rapidjson::StringBuffer>;
std::string hex(const blah2::resident_replay::Hash& value) {
  static constexpr char digits[] = "0123456789abcdef";
  std::string out;
  for (auto byte : value) { out += digits[byte >> 4]; out += digits[byte & 15]; }
  return out;
}
void text(Writer& w, const char* name, const std::string& value) {
  w.Key(name); w.String(value.c_str(), rapidjson::SizeType(value.size()));
}
void integer(Writer& w, const char* name, uint64_t value) {
  // Preserve all source sample/epoch bits through JSON consumers.
  text(w, name, std::to_string(value));
}
}

int main(int argc, char** argv) try {
  if (argc < 2 || unsigned(argc - 1) > score::maximumFrames)
    throw std::invalid_argument("Supply 1..20 ordered .vwrps snapshots");
  uint64_t total = 0;
  for (int i = 1; i < argc; ++i) {
    const auto bytes = std::filesystem::file_size(argv[i]);
    if (bytes > score::maximumInputBytes - total)
      throw std::invalid_argument("Snapshot cohort exceeds scorer input byte bound");
    total += bytes;
  }
  rapidjson::StringBuffer buffer; Writer w(buffer);
  w.StartObject(); text(w, "schema", "vectorwarp-resident-snapshot-geometry-v1");
  w.Key("evaluation_only"); w.Bool(true);
  w.Key("frames"); w.StartArray();
  contract::Cohort cohort; std::set<std::string> identities;
  for (int i = 1; i < argc; ++i) {
    const auto frame = replay::loadValidatedSnapshot(argv[i]);
    const auto& s = frame.sidecar; const auto& c = frame.config;
    if (uint64_t(c.geometry.doppler) * c.geometry.delays > score::maximumCells ||
        c.geometry.clutterSamples > 2000000)
      throw std::invalid_argument("Snapshot geometry exceeds scorer bounds");
    cohort.append(s, c.geometry.clutterSamples);
    const auto identity = hex(contract::frameIdentity(s));
    if (!identities.insert(identity).second) throw std::invalid_argument("Duplicate snapshot identity");
    w.StartObject(); text(w, "snapshot", std::filesystem::canonical(argv[i]).string());
    text(w, "snapshot_sha256", hex(s.snapshotHash));
    text(w, "snapshot_and_lineage_sha256", identity);
    text(w, "source_sha256", hex(s.sourceHash)); text(w, "rf_sha256", hex(s.rfHash));
    integer(w, "logical_frame", s.identity.logicalFrame);
    integer(w, "consumed_sequence", s.identity.consumedSequence);
    integer(w, "signal_timestamp_ms", s.identity.signalTimestampMs);
    integer(w, "capture_epoch", s.captureEpoch);
    integer(w, "first_sample", s.firstSample); integer(w, "end_sample", s.endSample);
    integer(w, "sample_rate_hz", s.sampleRateHz); integer(w, "frequency_hz", s.tuningHz);
    integer(w, "clutter_samples", c.geometry.clutterSamples);
    w.Key("delay_axis_samples"); w.StartArray(); for (auto value : c.delayAxis) w.Int(value); w.EndArray();
    w.Key("doppler_axis_hz"); w.StartArray(); for (auto value : c.dopplerAxis) w.Double(value); w.EndArray();
    w.EndObject();
  }
  w.EndArray(); w.EndObject();
  // Never publish a partial geometry document from a failed cohort.
  std::cout << buffer.GetString() << '\n'; return 0;
} catch (const std::exception& error) {
  std::cerr << "residentSnapshotGeometry: " << error.what() << '\n'; return 1;
}
