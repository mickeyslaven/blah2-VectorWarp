#include "ResidentWeakEvidenceScore.h"
#include "process/ambiguity/GpuResidentWire.h"
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <fstream>
#include <iostream>
#include <map>
#include <set>

namespace score = blah2::resident_weak_score;
namespace replay = blah2::integration::coherent_replay;
namespace contract = blah2::integration::cancellation_replay;
using Writer = rapidjson::Writer<rapidjson::StringBuffer>;
using Hash = blah2::resident_replay::Hash;
namespace {
std::string hex(const Hash& hash) {
  const char* digits = "0123456789abcdef"; std::string result;
  for (auto b : hash) { result += digits[b >> 4]; result += digits[b & 15]; }
  return result;
}
void require(bool ok, const char* error) { if (!ok) throw std::invalid_argument(error); }
void text(Writer& w, const char* key, const std::string& value) {
  w.Key(key); w.String(value.c_str(), rapidjson::SizeType(value.size()));
}
void number(Writer& w, const char* key, double value) {
  w.Key(key); if (std::isfinite(value)) w.Double(value); else w.Null();
}
void count(Writer& w, const char* key, uint64_t value) { w.Key(key); w.Uint64(value); }
void flag(Writer& w, const char* key, bool value) { w.Key(key); w.Bool(value); }
std::string executableHash(const char* argument) {
  const char* path = std::getenv("PATH");
  std::ifstream input(contract::runningExecutablePath(argument, path ? path : ""), std::ios::binary);
  require(bool(input), "Cannot bind running scorer executable");
  blah2::Sha256 hash; std::array<char, 65536> bytes{};
  while (input.read(bytes.data(), bytes.size()) || input.gcount()) hash.update(bytes.data(), size_t(input.gcount()));
  require(input.eof(), "Cannot read running scorer executable"); return hex(hash.finishBytes());
}
std::string field(const rapidjson::Value& v, const char* key) {
  require(v.IsObject() && v.HasMember(key) && v[key].IsString(), "Evaluation string field missing");
  require(v[key].GetStringLength() > 0 && v[key].GetStringLength() <= 256,
    "Evaluation string length invalid");
  return {v[key].GetString(), v[key].GetStringLength()};
}
struct Point { std::string kind, id, evidence; uint32_t row, column; };
struct Labels { std::map<std::string, std::vector<Point>> frames; std::string hash; };
Labels labels(const std::string& path) {
  Labels out; if (path.empty()) return out;
  const auto size = std::filesystem::file_size(path);
  require(size > 0 && size <= (1u << 20), "Evaluation file exceeds 1 MiB");
  std::ifstream input(path, std::ios::binary); std::string bytes(size, '\0');
  require(bool(input.read(bytes.data(), std::streamsize(size))) && input.peek() == EOF,
    "Evaluation file read incomplete");
  blah2::Sha256 digest; digest.update(bytes.data(), bytes.size()); out.hash = hex(digest.finishBytes());
  rapidjson::Document document; document.Parse(bytes.data(), bytes.size());
  require(!document.HasParseError() && document.IsObject() && document.HasMember("version") &&
    document["version"].IsUint() && document["version"].GetUint() == 1 &&
    document.HasMember("frames") && document["frames"].IsArray() &&
    document["frames"].Size() <= score::maximumFrames, "Evaluation manifest schema invalid");
  for (const auto& frame : document["frames"].GetArray()) {
    const auto hash = field(frame, "snapshot_and_lineage_sha256");
    require(hash.size() == 64 && hash.find_first_not_of("0123456789abcdef") == std::string::npos,
      "Evaluation identity must be a canonical SHA256");
    require(frame.HasMember("points") && frame["points"].IsArray() && frame["points"].Size() <= 256,
      "Evaluation point count exceeds 256");
    require(out.frames.emplace(hash, std::vector<Point>{}).second, "Duplicate evaluation frame");
    std::set<std::pair<unsigned, unsigned>> cells;
    for (const auto& p : frame["points"].GetArray()) {
      Point point{field(p, "kind"), field(p, "id"), field(p, "evidence"), 0, 0};
      require(point.kind == "target" || point.kind == "control", "Unknown evaluation point kind");
      require(p.HasMember("row") && p["row"].IsUint() && p.HasMember("column") && p["column"].IsUint(),
        "Evaluation cell indices must be unsigned integers");
      point.row = p["row"].GetUint(); point.column = p["column"].GetUint();
      require(cells.emplace(point.row, point.column).second, "Duplicate or conflicting evaluation cell");
      out.frames.at(hash).push_back(std::move(point));
    }
  }
  return out;
}
void limits(Writer& w) {
  for (const auto* key : {"pfa", "false_track_delta", "whole_pass_detection", "tracking_gain"}) {
    w.Key(key); w.Null();
  }
  for (const auto* key : {"null_calibrated", "promotion_authorized", "detector_mutation", "gpu_execution"})
    flag(w, key, false);
}
}

int main(int argc, char** argv) try {
  std::vector<std::string> paths; std::string evaluationPath; uint32_t anchor = 0;
  for (int arg = 1; arg < argc; ++arg) {
    const std::string value = argv[arg];
    if (value == "--evaluation" || value == "--anchor") {
      require(arg + 1 < argc, "Missing option value"); const std::string argument = argv[++arg];
      if (value == "--evaluation") evaluationPath = argument;
      else {
        require(!argument.empty() && argument.size() <= 2 &&
          argument.find_first_not_of("0123456789") == std::string::npos, "Invalid anchor channel");
        anchor = unsigned(std::stoul(argument)); require(anchor < 8, "Anchor channel exceeds seven");
      }
    } else if (value == "--help") {
      std::cout << "Usage: residentWeakEvidenceScore [--anchor 0..7] [--evaluation labels.json] FRAME.vwrps...\n"
        << "1..20 ordered snapshots with .vwrl sidecars; JSONL diagnostics on stdout.\n"; return 0;
    } else {
      require(value.rfind("--", 0) != 0, "Unknown option"); paths.push_back(value);
    }
  }
  require(!paths.empty() && paths.size() <= score::maximumFrames, "Supply 1..20 ordered .vwrps snapshots");
  uint64_t totalBytes = 0;
  for (const auto& path : paths) {
    const auto bytes = std::filesystem::file_size(path);
    require(bytes <= score::maximumInputBytes - totalBytes, "Cohort input exceeds 512 MiB"); totalBytes += bytes;
  }
  // Freeze all payload + lineage identities before numerical work. Never retain
  // all snapshot buffers, and reject any TOCTOU change on the processing pass.
  const auto evaluation = labels(evaluationPath);
  std::vector<Hash> frozen; contract::Cohort cohort; std::set<std::string> identities;
  for (const auto& path : paths) {
    const auto frame = replay::loadValidatedSnapshot(path);
    const auto& g = frame.config.geometry;
    require(uint64_t(g.doppler) * g.delays <= score::maximumCells && g.clutterSamples <= 2000000,
      "Scorer exceeds 65536 cells or 2M pilot samples");
    cohort.append(frame.sidecar, g.clutterSamples); frozen.push_back(contract::frameIdentity(frame.sidecar));
    const auto identity = hex(frozen.back()); require(identities.insert(identity).second, "Duplicate snapshot");
    if (const auto found = evaluation.frames.find(identity); found != evaluation.frames.end())
      for (const auto& point : found->second)
        require(point.row < g.doppler && point.column < g.delays, "Evaluation point outside snapshot");
  }
  for (const auto& item : evaluation.frames) require(identities.count(item.first), "Evaluation identity absent from cohort");
  const auto scorerHash = executableHash(argv[0]);
  score::Scorer scorer(anchor);
  for (size_t index = 0; index < paths.size(); ++index) {
    const auto frame = replay::loadValidatedSnapshot(paths[index]); contract::requireFrozen(frame.sidecar, frozen[index]);
    auto surfaces = scorer.append(frame); const auto identity = hex(frozen[index]);
    rapidjson::StringBuffer buffer; Writer w(buffer); w.StartObject();
    text(w, "record", "frame"); text(w, "snapshot", paths[index]);
    text(w, "snapshot_sha256", hex(frame.sidecar.snapshotHash)); text(w, "snapshot_and_lineage_sha256", identity);
    text(w, "scorer_executable_sha256", scorerHash);
    text(w, "evaluation_sha256", evaluation.hash); text(w, "logical_frame", std::to_string(frame.sidecar.identity.logicalFrame));
    text(w, "first_sample", std::to_string(frame.sidecar.firstSample)); text(w, "end_sample", std::to_string(frame.sidecar.endSample));
    text(w, "comparison", "paired endpoint CUT and identical valid local training cells; single-CPI baseline");
    text(w, "coherent_policy", "equal unit-norm channel weights; fixed zero-bin-motion; phase +1; pilot>=0.25; target coherence>=0.6");
    text(w, "coherent_reason", scorer.coherentReason); count(w, "anchor_channel", anchor);
    count(w, "guard_cells", score::guard); count(w, "training_cells_per_side", score::training);
    number(w, "bootstrap_shrinkage", .75); number(w, "bootstrap_diagonal_loading", .001);
    count(w, "declared_coherent_trials_per_cell", 3);
    number(w, "diagnostic_contrast_threshold_db", score::contrastThresholdDb);
    text(w, "burden_definition", "local-contrast cell crossings; no NMS, CFAR calibration, or candidate/track inference");
    w.Key("recorded_native_counts");
    if (!frame.config.detector.enabled) w.Null();
    else {
      const auto& bytes = frame.snapshot.blocks.at(unsigned(blah2::resident_replay::BlockKind::decisionFlags) - 1).bytes;
      blah2::resident::wire::Reader reader{{bytes.data(), bytes.size()}};
      uint64_t cfar = 0, centroid = 0, interpolated = 0;
      for (size_t offset = 0; offset < bytes.size(); offset += 4) {
        const auto bits = reader.u32(offset); cfar += (bits & 1) != 0; centroid += (bits & 2) != 0; interpolated += (bits & 4) != 0;
      }
      w.StartObject(); count(w, "cfar_cells", cfar); count(w, "centroid_cells", centroid);
      count(w, "interpolated_cells", interpolated); w.EndObject();
    }
    w.Key("arms"); w.StartArray();
    for (const auto& arm : surfaces) {
      const auto summary = score::summarize(surfaces.front(), arm);
      w.StartObject(); text(w, "arm", arm.name); flag(w, "ready", arm.ready);
      count(w, "valid_cells", summary.validCells); count(w, "paired_cells", summary.pairedCells);
      count(w, "dimensionless_diagonal_fallback_cells", arm.fallbackCells);
      count(w, "diagnostic_selected_cells", summary.selectedCells);
      count(w, "paired_baseline_selected_cells", summary.baselineSelectedCells);
      w.Key("evaluation_points");
      const auto found = evaluation.frames.find(identity);
      if (found == evaluation.frames.end()) w.Null();
      else {
        w.StartArray();
        for (const auto& point : found->second) {
          const auto measured = score::measure(surfaces.front(), arm, uint64_t(point.row) * arm.columns + point.column);
          w.StartObject(); text(w, "kind", point.kind); text(w, "id", point.id); text(w, "evidence", point.evidence);
          count(w, "row", point.row); count(w, "column", point.column); flag(w, "available", measured.available);
          count(w, "matched_training_cells", measured.trainingCells);
          number(w, "power", measured.available ? measured.power : NAN);
          number(w, "baseline_power", measured.available ? measured.baselinePower : NAN);
          number(w, "local_mean", measured.available ? measured.localMean : NAN);
          number(w, "baseline_local_mean", measured.available ? measured.baselineLocalMean : NAN);
          number(w, "contrast_db", measured.contrastDb); number(w, "baseline_contrast_db", measured.baselineContrastDb);
          number(w, "contrast_delta_db", measured.contrastDb - measured.baselineContrastDb);
          w.Key("diagnostic_selected"); if (measured.available) w.Bool(measured.contrastDb >= score::contrastThresholdDb); else w.Null();
          w.Key("paired_baseline_selected"); if (measured.available) w.Bool(measured.baselineContrastDb >= score::contrastThresholdDb); else w.Null();
          w.EndObject();
        }
        w.EndArray();
      }
      w.EndObject();
    }
    w.EndArray(); limits(w); w.EndObject(); std::cout << buffer.GetString() << '\n';
  }
  rapidjson::StringBuffer buffer; Writer w(buffer); w.StartObject(); text(w, "record", "complete");
  count(w, "frames", paths.size()); text(w, "status", "map_statistic_diagnostics_only"); limits(w);
  w.EndObject(); std::cout << buffer.GetString() << '\n'; return 0;
} catch (const std::exception& error) {
  rapidjson::StringBuffer buffer; Writer w(buffer); w.StartObject(); text(w, "record", "failure");
  text(w, "reason", error.what()); limits(w); w.EndObject(); std::cout << buffer.GetString() << '\n'; return 1;
}
