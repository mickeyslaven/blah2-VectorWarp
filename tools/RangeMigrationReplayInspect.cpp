// Offline full-CAF migration experiment. Its only ingress is the canonical
// sealed snapshot/sidecar reader; no capture, GPU worker, detector or publisher.
#include "process/integration/RangeMigrationReplay.h"
#include "WienerHopfReplayContract.h"
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <fstream>
#include <iostream>
#include <sstream>

namespace replay = blah2::integration::coherent_replay;
namespace lineage = blah2::integration::cancellation_replay;
using Migration = blah2::integration::RangeMigrationReplay;
namespace {
std::string hex(const blah2::resident_replay::Hash& hash) {
  const char* digits = "0123456789abcdef";
  std::string text;
  for (auto byte : hash) { text += digits[byte >> 4]; text += digits[byte & 15]; }
  return text;
}
std::string fileHash(const std::filesystem::path& path) {
  if (std::filesystem::file_size(path) > (128u << 20))
    throw std::invalid_argument("Migration input/executable exceeds 128 MiB bound");
  std::ifstream input(path, std::ios::binary);
  if (!input) throw std::runtime_error("Cannot hash migration lineage file");
  blah2::Sha256 hash; std::array<char, 65536> bytes;
  while (input.read(bytes.data(), bytes.size()) || input.gcount())
    hash.update(bytes.data(), size_t(input.gcount()));
  if (!input.eof()) throw std::runtime_error("Migration lineage hash read failed");
  return hash.finish();
}
}

int main(int argc, char** argv) try {
  if (argc != 6 || std::string(argv[1]) != "--channel" ||
      std::string(argv[3]) != "--rates-bins-per-second")
    throw std::invalid_argument("Usage: ./rangeMigrationReplayInspect --channel ID --rates-bins-per-second 0,RATE,... FRAME.vwrps");
  const std::string channelText = argv[2];
  if (channelText.empty() || channelText.find_first_not_of("0123456789") != std::string::npos)
    throw std::invalid_argument("Migration requires a physical surveillance channel ID");
  const auto channelId = std::stoull(channelText);
  std::vector<double> rates; std::string item; std::stringstream stream(argv[4]);
  while (std::getline(stream, item, ',')) {
    size_t used = 0; const double value = std::stod(item, &used);
    if (used != item.size()) throw std::invalid_argument("Invalid migration rate list");
    rates.push_back(value);
    if (rates.size() > Migration::maximumTrials) throw std::invalid_argument("Too many migration trials");
  }
  if (std::string(argv[4]).empty() || std::string(argv[4]).back() == ',')
    throw std::invalid_argument("Invalid migration rate list");
  const char* searchPath = std::getenv("PATH");
  const auto executableHash = fileHash(lineage::runningExecutablePath(argv[0], searchPath ? searchPath : ""));
  const auto snapshotHash = fileHash(argv[5]);
  const auto frame = replay::loadValidatedSnapshot(argv[5]);
  const auto& config = frame.config; const auto& sidecar = frame.sidecar;
  if (snapshotHash != hex(sidecar.snapshotHash))
    throw std::invalid_argument("Migration snapshot changed while loading");
  const auto channel = std::find(config.surveillanceChannels.begin(),
    config.surveillanceChannels.end(), channelId);
  if (channel == config.surveillanceChannels.end())
    throw std::invalid_argument("Migration channel absent from sealed configuration");
  // Reuse the canonical single-channel decoder, including native FFT ordering.
  const auto caf = replay::experiment_detail::channelMap(frame.snapshot, config,
    uint32_t(channel - config.surveillanceChannels.begin()));
  const auto result = Migration::process(caf, sidecar.sampleRateHz, config.correlation, rates);
  rapidjson::StringBuffer buffer; rapidjson::Writer<rapidjson::StringBuffer> w(buffer);
  const auto text = [&](const char* key, const std::string& value) {
    w.Key(key); w.String(value.c_str(), rapidjson::SizeType(value.size()));
  };
  const auto integer = [&](const char* key, uint64_t value) { text(key, std::to_string(value)); };
  w.StartObject(); text("schema", "vectorwarp-range-migration-replay-v1");
  w.Key("offline_only"); w.Bool(true); w.Key("null_calibrated"); w.Bool(false);
  text("executable_sha256", executableHash); text("snapshot_sha256", snapshotHash);
  text("snapshot_and_lineage_sha256", hex(lineage::frameIdentity(sidecar)));
  text("source_sha256", hex(sidecar.sourceHash));
  text("rf_sha256", hex(sidecar.rfHash)); text("config_sha256", hex(sidecar.identity.configHash));
  text("module_sha256", hex(sidecar.identity.moduleHash)); text("raw_iq_sha256", hex(sidecar.identity.rawIqHash));
  integer("capture_epoch", sidecar.captureEpoch); integer("logical_frame", sidecar.identity.logicalFrame);
  integer("first_sample", sidecar.firstSample); integer("end_sample", sidecar.endSample);
  integer("sample_rate_hz", sidecar.sampleRateHz); integer("correlation_samples", config.correlation);
  integer("channel_id", channelId); integer("complex_mac_bound", result.complexMacBound);
  integer("per_cell_trials", result.banks.size()); integer("evaluated_cells", result.evaluatedCells);
  w.Key("delay_axis_samples_at_block_midpoint"); w.StartArray();
  for (uint32_t col = 0; col < result.columns; ++col) w.Int(caf.delay[result.firstColumn + col]);
  w.EndArray(); w.Key("doppler_axis_hz"); w.StartArray();
  for (auto frequency : caf.doppler) w.Double(frequency);
  w.EndArray(); w.Key("banks"); w.StartArray();
  for (const auto& bank : result.banks) {
    w.StartObject(); w.Key("delay_rate_bins_per_second"); w.Double(bank.delayRateBinsPerSecond);
    w.Key("white_noise_gain"); w.Double(bank.whiteNoiseGain);
    w.Key("complex_cells_doppler_major"); w.StartArray();
    for (auto value : bank.cells) { w.StartArray(); w.Double(value.real()); w.Double(value.imag()); w.EndArray(); }
    w.EndArray(); w.EndObject();
  }
  w.EndArray(); w.EndObject(); std::cout << buffer.GetString() << '\n';
  return 0;
} catch (const std::exception& error) {
  std::cerr << "rangeMigrationReplayInspect: " << error.what() << '\n'; return 1;
}
