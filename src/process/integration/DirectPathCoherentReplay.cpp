#include "DirectPathCoherentReplay.h"
#include "process/ambiguity/GpuResidentHash.h"
#include "process/ambiguity/GpuResidentWire.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <limits>
#include <unistd.h>

namespace blah2::integration::coherent_replay {
namespace {
using resident::wire::Reader;
using resident::wire::Writer;
using resident::wire::require;
using resident_replay::Hash;

constexpr uint32_t magic = 0x4c525744; // DWRL, little-endian
constexpr size_t hashOffset = 400, digestOffset = 432;
bool nonzero(const Hash& value) { return std::any_of(value.begin(), value.end(), [](uint8_t b) { return b != 0; }); }
Hash digest(const uint8_t* data, size_t size) { resident::Sha256 h; h.add(data, size); return h.finish(); }
bool same(const Sidecar& a, const Sidecar& b) {
  return a.captureEpoch == b.captureEpoch && a.identity.epoch == b.identity.epoch &&
    a.identity.configHash == b.identity.configHash &&
    a.identity.moduleHash == b.identity.moduleHash &&
    a.identity.deviceHash == b.identity.deviceHash &&
    a.sourceHash == b.sourceHash && a.rfHash == b.rfHash &&
    a.tuningHz == b.tuningHz &&
    a.gainCalibrationEpoch == b.gainCalibrationEpoch &&
    a.sampleRateHz == b.sampleRateHz && a.actualGainDb == b.actualGainDb;
}
resident::ConstBytes block(const Reader& r, uint32_t kind) {
  const size_t descriptor = 512 + 24 * (kind - 1);
  require(r.u32(descriptor) == kind && !r.u32(descriptor + 4), "Replay snapshot block descriptor invalid");
  const auto offset = r.u64(descriptor + 8), length = r.u64(descriptor + 16);
  require(offset >= resident_replay::headerBytes && offset <= r.b.size && length <= r.b.size - offset,
    "Replay snapshot block outside input");
  return {r.b.data + size_t(offset), size_t(length)};
}
resident::Config snapshotConfig(const std::vector<uint8_t>& bytes) {
  const Reader r{{bytes.data(), bytes.size()}};
  require(r.u32(0) == 0x52505331 && r.u32(4) == resident_replay::version &&
    r.u32(8) == resident_replay::headerBytes && r.u32(12) == 20 && r.u64(16) == bytes.size(),
    "Replay snapshot envelope invalid");
  const auto metadata = block(r, uint32_t(resident_replay::BlockKind::postprocessMetadata));
  const Reader m{metadata};
  const auto rows = r.u32(76), columns = r.u32(80);
  require(rows && columns && rows <= 65535 && columns <= 65535, "Replay snapshot geometry invalid");
  const size_t doppler = 96 * 4, delays = doppler + size_t(rows) * 8;
  m.range(doppler, size_t(rows) * 8); m.range(delays, size_t(columns) * 4);
  return resident::decodeConfig(block(r, uint32_t(resident_replay::BlockKind::configuration)),
    {metadata.data + delays, size_t(columns) * 4}, {metadata.data + doppler, size_t(rows) * 8});
}
DirectPathCoherentShadow::ComplexMap channelMap(const resident_replay::ResidentReplaySnapshot& snapshot,
  const resident::Config& config, uint32_t channel) {
  const uint32_t rows = config.geometry.doppler, columns = config.geometry.delays;
  const uint64_t cells = uint64_t(rows) * columns;
  require(channel < config.geometry.channels && cells <= (1u << 20), "Replay channel map bounds invalid");
  const auto& bytes = snapshot.blocks.at(uint32_t(resident_replay::BlockKind::channelMaps) - 1).bytes;
  const Reader r{{bytes.data(), bytes.size()}};
  DirectPathCoherentShadow::ComplexMap result(rows, columns);
  result.delay.assign(config.delayAxis.begin(), config.delayAxis.end());
  result.doppler.assign(config.dopplerAxis.begin(), config.dopplerAxis.end());
  for (uint32_t row = 0; row < rows; ++row) for (uint32_t column = 0; column < columns; ++column) {
    const uint64_t index = uint64_t(channel) * cells + uint64_t(column) * rows + ((row + rows / 2 + 1) % rows);
    const auto real = r.f32(size_t(index) * 8), imag = r.f32(size_t(index) * 8 + 4);
    require(std::isfinite(real) && std::isfinite(imag), "Replay complex channel map is nonfinite");
    result.data[row][column] = {real, imag};
  }
  return result;
}
}

std::filesystem::path sidecarPath(const std::filesystem::path& snapshot) {
  require(snapshot.extension() == ".vwrps", "Snapshot filename must end in .vwrps");
  return snapshot.string() + ".vwrl";
}

std::vector<uint8_t> encodeSidecar(const Sidecar& sidecar, const uint8_t* snapshot, size_t snapshotSize) {
  require(snapshot && snapshotSize >= resident_replay::headerBytes && snapshotSize <= resident_replay::byteLimit,
    "Snapshot binding outside bound");
  const auto& id = sidecar.identity;
  require(id.epoch && id.logicalFrame && id.generation && id.consumedSequence && nonzero(id.configHash) &&
    nonzero(id.moduleHash) && nonzero(id.rawIqHash) && nonzero(id.deviceHash) && nonzero(sidecar.sourceHash) &&
    nonzero(sidecar.rfHash) && sidecar.firstSample < sidecar.endSample &&
    sidecar.tuningHz && sidecar.gainCalibrationEpoch && sidecar.sampleRateHz &&
    !sidecar.actualGainDb.empty() && sidecar.actualGainDb.size() <= 8, "Replay sidecar lineage incomplete");
  for (double gain : sidecar.actualGainDb) require(std::isfinite(gain), "Replay sidecar gain invalid");
  std::vector<uint8_t> result(sidecarBytes, 0); Writer w{{result.data(), result.size()}};
  w.u32(0, magic); w.u32(4, sidecarVersion); w.u32(8, uint32_t(sidecarBytes)); w.u32(12, uint32_t(sidecar.actualGainDb.size()));
  const uint64_t values[] = {id.epoch,id.logicalFrame,id.generation,id.consumedSequence,id.signalTimestampMs,id.trackerTransaction,
    sidecar.captureEpoch,sidecar.firstSample,sidecar.endSample,sidecar.tuningHz,sidecar.gainCalibrationEpoch};
  for (size_t i = 0; i < std::size(values); ++i) w.u64(16 + 8 * i, values[i]);
  w.u32(104, sidecar.sampleRateHz);
  w.copy(112, id.configHash.data(), 32); w.copy(144, id.moduleHash.data(), 32); w.copy(176, id.rawIqHash.data(), 32); w.copy(208, id.deviceHash.data(), 32);
  w.copy(240, sidecar.sourceHash.data(), 32); w.copy(272, sidecar.rfHash.data(), 32); const auto snapshotDigest = digest(snapshot, snapshotSize); w.copy(hashOffset, snapshotDigest.data(), 32);
  for (size_t i = 0; i < sidecar.actualGainDb.size(); ++i) w.f64(304 + 8 * i, sidecar.actualGainDb[i]);
  const auto recordDigest = digest(result.data(), digestOffset); w.copy(digestOffset, recordDigest.data(), 32); return result;
}

Sidecar decodeSidecar(const uint8_t* data, size_t bytes, const uint8_t* snapshot, size_t snapshotSize) {
  require(data && bytes == sidecarBytes && snapshot && snapshotSize >= resident_replay::headerBytes && snapshotSize <= resident_replay::byteLimit,
    "Replay sidecar input bound differs");
  const Reader r{{data, bytes}}; require(r.u32(0) == magic && r.u32(4) == sidecarVersion && r.u32(8) == sidecarBytes && r.u32(12) > 0 && r.u32(12) <= 8,
    "Replay sidecar envelope invalid");
  Hash expected; std::memcpy(expected.data(), data + digestOffset, expected.size());
  require(expected == digest(data, digestOffset), "Replay sidecar digest differs");
  Sidecar result;
  uint64_t values[11]{}; for (size_t i = 0; i < std::size(values); ++i) values[i] = r.u64(16 + 8 * i);
  result.identity = {values[0],values[1],values[2],values[3],values[4],values[5]};
  result.captureEpoch=values[6]; result.firstSample=values[7]; result.endSample=values[8]; result.tuningHz=values[9]; result.gainCalibrationEpoch=values[10]; result.sampleRateHz=r.u32(104);
  std::memcpy(result.identity.configHash.data(), data + 112, 32); std::memcpy(result.identity.moduleHash.data(), data + 144, 32);
  std::memcpy(result.identity.rawIqHash.data(), data + 176, 32); std::memcpy(result.identity.deviceHash.data(), data + 208, 32);
  std::memcpy(result.sourceHash.data(), data + 240, 32); std::memcpy(result.rfHash.data(), data + 272, 32); std::memcpy(result.snapshotHash.data(), data + hashOffset, 32);
  for (uint32_t i = 0; i < r.u32(12); ++i) result.actualGainDb.push_back(r.f64(304 + 8 * i));
  require(result.snapshotHash == digest(snapshot, snapshotSize), "Replay sidecar snapshot binding differs");
  (void)encodeSidecar(result, snapshot, snapshotSize); return result;
}

void writeSidecarExclusive(const std::filesystem::path& snapshot, const Sidecar& sidecar) {
  std::ifstream input(snapshot, std::ios::binary | std::ios::ate); require(bool(input), "Cannot open replay snapshot for sidecar");
  const auto size = input.tellg(); require(size >= std::streamoff(resident_replay::headerBytes) && size <= std::streamoff(resident_replay::byteLimit), "Replay snapshot size invalid");
  std::vector<uint8_t> bytes(static_cast<size_t>(size)); input.seekg(0); require(bool(input.read(reinterpret_cast<char*>(bytes.data()), size)), "Cannot read replay snapshot for sidecar");
  const auto encoded = encodeSidecar(sidecar, bytes.data(), bytes.size()); const auto destination = sidecarPath(snapshot);
  const int fd = open(destination.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600); if (fd < 0) throw std::runtime_error("Cannot exclusively create replay lineage sidecar: " + destination.string());
  try { size_t done = 0; while (done < encoded.size()) { const auto count = write(fd, encoded.data() + done, encoded.size() - done); if (count <= 0) throw std::runtime_error("Cannot write replay lineage sidecar"); done += size_t(count); }
    if (fsync(fd) || close(fd)) throw std::runtime_error("Cannot sync replay lineage sidecar");
  } catch (...) { close(fd); unlink(destination.c_str()); throw; }
}

std::vector<uint8_t> readSidecar(const std::filesystem::path& snapshot) {
  const auto path = sidecarPath(snapshot); std::ifstream input(path, std::ios::binary | std::ios::ate); require(bool(input), "Replay lineage sidecar missing");
  require(input.tellg() == std::streamoff(sidecarBytes), "Replay lineage sidecar size differs"); std::vector<uint8_t> result(sidecarBytes); input.seekg(0);
  require(bool(input.read(reinterpret_cast<char*>(result.data()), result.size())) && input.peek() == std::char_traits<char>::eof(), "Replay lineage sidecar read incomplete"); return result;
}

ValidatedSnapshot loadValidatedSnapshot(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary | std::ios::ate);
  require(bool(input), "Cannot open replay snapshot");
  const auto end = input.tellg();
  require(end >= std::streamoff(resident_replay::headerBytes) &&
    end <= std::streamoff(resident_replay::byteLimit), "Replay snapshot size invalid");
  std::vector<uint8_t> bytes(static_cast<size_t>(end)); input.seekg(0);
  require(bool(input.read(reinterpret_cast<char*>(bytes.data()), end)) &&
    input.peek() == std::char_traits<char>::eof(), "Replay snapshot read incomplete");
  const auto sidecarBytesOnDisk = readSidecar(path);
  const auto sidecar = decodeSidecar(sidecarBytesOnDisk.data(), sidecarBytes,
    bytes.data(), bytes.size());
  const auto config = snapshotConfig(bytes);
  require(config.replay.sourceHash == sidecar.sourceHash &&
    config.replay.rfHash == sidecar.rfHash &&
    config.replay.actualGainDb == sidecar.actualGainDb &&
    uint64_t(config.sampleRate) == sidecar.sampleRateHz &&
    uint64_t(config.replay.frequencyHz) == sidecar.tuningHz,
    "Replay sidecar RF/config identity differs");
  return {sidecar, config, resident_replay::decode(bytes.data(), bytes.size(),
    config, sidecar.identity)};
}

ShadowReader::ShadowReader(DirectPathCoherentShadow::Config config)
  : shadow_(config, true), anchorChannel_(config.anchorChannelId) {}
DirectPathCoherentShadow::Result ShadowReader::append(const std::filesystem::path& path) {
  return append(loadValidatedSnapshot(path));
}
std::vector<DirectPathCoherentShadow::ComplexMap> channelMaps(const ValidatedSnapshot& frame) {
  std::vector<DirectPathCoherentShadow::ComplexMap> result;
  result.reserve(frame.config.geometry.channels);
  for (uint32_t channel = 0; channel < frame.config.geometry.channels; ++channel)
    result.push_back(channelMap(frame.snapshot, frame.config, channel));
  return result;
}
DirectPathCoherentShadow::Result ShadowReader::append(const ValidatedSnapshot& validated) {
  const auto& sidecar = validated.sidecar; const auto& config = validated.config;
  const auto& snapshot = validated.snapshot;
  if (previous_) { require(same(*previous_, sidecar), "Replay coherent lineage identity changed"); require(sidecar.identity.logicalFrame == previous_->identity.logicalFrame + 1 && sidecar.identity.generation == previous_->identity.generation + 1 && sidecar.identity.consumedSequence > previous_->identity.consumedSequence && sidecar.firstSample > previous_->firstSample, "Replay coherent sidecars reordered or discontinuous"); }
  auto maps = channelMaps(validated); std::vector<DirectPathCoherentShadow::ComplexMap*> mapPointers;
  mapPointers.reserve(maps.size()); for (auto& map : maps) mapPointers.push_back(&map);
  fallback_ = std::make_unique<DirectPathCoherentShadow::ComplexMap>(config.geometry.doppler, config.geometry.delays);
  fallback_->delay.assign(config.delayAxis.begin(), config.delayAxis.end());
  fallback_->doppler.assign(config.dopplerAxis.begin(), config.dopplerAxis.end());
  const auto& currentPower = snapshot.blocks.at(
    uint32_t(resident_replay::BlockKind::currentPower) - 1).bytes;
  const Reader powerReader{{currentPower.data(), currentPower.size()}};
  for (uint32_t row = 0; row < config.geometry.doppler; ++row)
    for (uint32_t column = 0; column < config.geometry.delays; ++column)
      fallback_->data[row][column] = DirectPathCoherentShadow::Complex(
        powerReader.f64((size_t(row) * config.geometry.delays + column) * 8), 0);
  const DirectPathCoherentShadow::Frame frame{sidecar.captureEpoch,sidecar.identity.logicalFrame,sidecar.firstSample,sidecar.endSample,sidecar.tuningHz,sidecar.gainCalibrationEpoch,sidecar.sampleRateHz,true,true};
  const auto anchor = std::find(config.surveillanceChannels.begin(), config.surveillanceChannels.end(), anchorChannel_);
  require(anchor != config.surveillanceChannels.end(), "Replay shadow anchor channel unavailable");
  const auto anchorIndex = size_t(std::distance(config.surveillanceChannels.begin(), anchor));
  const auto& raw = snapshot.blocks.at(uint32_t(resident_replay::BlockKind::rawIq) - 1).bytes;
  const auto& reference = snapshot.blocks.at(uint32_t(resident_replay::BlockKind::referenceIq) - 1).bytes;
  const auto samples = config.geometry.clutterSamples; require(raw.size() == size_t(samples) * config.geometry.channels * 16 && reference.size() == size_t(samples) * 16, "Replay IQ pilot layout differs");
  const Reader rawReader{{raw.data(), raw.size()}}, referenceReader{{reference.data(), reference.size()}}; std::vector<DirectPathCoherentShadow::Complex> pilotReference(samples), pilotAnchor(samples);
  for (uint32_t sample = 0; sample < samples; ++sample) { pilotReference[sample] = {referenceReader.f64(size_t(sample) * 16), referenceReader.f64(size_t(sample) * 16 + 8)}; const size_t offset = (anchorIndex * size_t(samples) + sample) * 16; pilotAnchor[sample] = {rawReader.f64(offset), rawReader.f64(offset + 8)}; }
  previous_ = sidecar; return shadow_.process(mapPointers, config.surveillanceChannels, frame,
    DirectPathCoherentShadow::estimatePilot(pilotReference.data(), pilotAnchor.data(), samples, frame, anchorChannel_), *fallback_);
}
} // namespace blah2::integration::coherent_replay
