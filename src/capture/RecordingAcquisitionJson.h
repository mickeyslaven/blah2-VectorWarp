#pragma once
#include "RecordingAcquisition.h"
#include "Sha256.h"
#include "RecordingFileCache.h"
#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <fcntl.h>
#include <filesystem>
#include <sys/stat.h>
#include <unistd.h>
#include <set>
#include <stdexcept>
#include <utility>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

namespace blah2::acquisition {
constexpr size_t maximumJsonBytes = 1U << 20;
constexpr size_t maximumFrames = 4096;
// Keep callback granularity without retaining one allocation per callback.
// The caller owns epoch/settings continuity; gaps and sequence loss fail closed.
inline bool appendFrame(std::vector<RecordingFrameLineage>& frames, RecordingFrameLineage frame) {
  if (!frame.ingressSequence || frame.repeat != 1 || !frame.count || !frame.sourceCount ||
      frame.sourceFirst > UINT64_MAX - frame.sourceCount || frame.sliceOffset > frame.sourceCount ||
      frame.count > frame.sourceCount - frame.sliceOffset) return false;
  if (!frames.empty()) {
    auto& last = frames.back();
    if (last.ingressSequence > UINT64_MAX - last.repeat ||
        frame.ingressSequence != last.ingressSequence + last.repeat ||
        last.repeat > (UINT64_MAX - last.sourceFirst - last.sliceOffset) / last.count ||
        frame.sourceFirst + frame.sliceOffset != last.sourceFirst + last.sliceOffset + last.count * last.repeat)
      return false;
    if (!last.sliceOffset && !frame.sliceOffset && last.count == last.sourceCount &&
        frame.count == frame.sourceCount && last.count == frame.count &&
        last.originalMeanIq.empty() && frame.originalMeanIq.empty()) {
      ++last.repeat; return true;
    }
  }
  if (frames.size() >= maximumFrames) return false;
  frames.push_back(std::move(frame)); return true;
}
inline bool sha256(const std::string& value) {
  return value.size() == 64 && value.find_first_not_of("0123456789abcdef") == std::string::npos;
}
inline std::string digest(const std::string& value) {
  Sha256 hash; hash.update(value.data(), value.size()); return hash.finish();
}
inline std::string encode(const rapidjson::Value& value) {
  rapidjson::StringBuffer buffer; rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
  if (!value.Accept(writer)) throw std::invalid_argument("Invalid acquisition JSON value");
  return {buffer.GetString(), buffer.GetSize()};
}
inline bool validGeographicSites(const rapidjson::Value& sites) {
  const auto unique = [](const rapidjson::Value& value) {
    if (!value.IsObject()) return false;
    std::set<std::string> keys;
    for (auto it = value.MemberBegin(); it != value.MemberEnd(); ++it)
      if (!keys.emplace(it->name.GetString(), it->name.GetStringLength()).second) return false;
    return true;
  };
  const auto site = [&](const rapidjson::Value& value) {
    if (!unique(value)) return false;
    for (const char* key : {"latitude", "longitude", "altitude"})
      if (!value.HasMember(key) || !value[key].IsNumber() || !std::isfinite(value[key].GetDouble())) return false;
    if (std::abs(value["latitude"].GetDouble()) > 90 || std::abs(value["longitude"].GetDouble()) > 180) return false;
    for (const char* key : {"site_id", "name"})
      if (value.HasMember(key) && (!value[key].IsString() || value[key].GetStringLength() > 128)) return false;
    return true;
  };
  if (!unique(sites) || !sites.HasMember("schema") || !sites["schema"].IsString() ||
      std::string(sites["schema"].GetString()) != "vectorwarp-recorded-geographic-sites-v1" ||
      !sites.HasMember("provenance") || !sites["provenance"].IsString() ||
      std::string(sites["provenance"].GetString()) != "operator-configured-at-acquisition" ||
      !sites.HasMember("rx") || !site(sites["rx"]) || !sites.HasMember("tx") ||
      (!sites["tx"].IsNull() && !site(sites["tx"])) ||
      !sites.HasMember("illuminators") || !unique(sites["illuminators"]) ||
      sites["illuminators"].MemberCount() > 16) return false;
  std::set<std::string> ids;
  for (auto it = sites["illuminators"].MemberBegin(); it != sites["illuminators"].MemberEnd(); ++it) {
    const std::string id(it->name.GetString(), it->name.GetStringLength());
    const auto& value = it->value;
    if (id.empty() || id.size() > 64 || id.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789._-") != std::string::npos ||
        !ids.insert(id).second || !unique(value) || !value.HasMember("frequencyHz") ||
        !value["frequencyHz"].IsUint() || !value["frequencyHz"].GetUint() ||
        !value.HasMember("transmitter") || !site(value["transmitter"])) return false;
  }
  return !sites["tx"].IsNull() || !sites["illuminators"].ObjectEmpty();
}
inline std::string serialize(const RecordingBlockMetadata* metadata,
    const std::string& receiver, uint32_t channels, uint32_t rate, uint32_t frequency,
    uint64_t epoch, uint64_t first, uint64_t end, bool positions,
    const std::vector<RecordingFrameLineage>& frames) {
  if (frames.empty() || frames.size() > maximumFrames)
    throw std::invalid_argument("Recording acquisition frame count exceeds its bound");
  rapidjson::Document doc(rapidjson::kObjectType); auto& a = doc.GetAllocator();
  const auto string = [&](rapidjson::Value& object, const char* key, const std::string& value) {
    object.AddMember(rapidjson::Value(key, a), rapidjson::Value(value.c_str(), value.size(), a), a);
  };
  const bool runs = std::any_of(frames.begin(), frames.end(),
    [](const auto& frame) { return frame.repeat != 1; });
  string(doc, "schema", runs ? "vectorwarp-recording-acquisition-v2" : "vectorwarp-recording-acquisition-v1");
  string(doc, "receiver", receiver);
  doc.AddMember("channels", channels, a); doc.AddMember("sampleRateHz", rate, a);
  doc.AddMember("frequencyHz", frequency, a);
  string(doc, "epoch", std::to_string(epoch)); string(doc, "firstSample", std::to_string(first));
  string(doc, "endSample", std::to_string(end));
  doc.AddMember("sourcePositionAvailable", positions, a);
  string(doc, "frameSequenceSource", "recorder-ingress");
  doc.AddMember("metadataAvailable", metadata != nullptr, a);
  rapidjson::Value frameValues(rapidjson::kArrayType);
  for (const auto& frame : frames) {
    rapidjson::Value value(rapidjson::kObjectType);
    string(value, "sequence", std::to_string(frame.ingressSequence));
    string(value, "sourceFirst", std::to_string(frame.sourceFirst));
    string(value, "sourceCount", std::to_string(frame.sourceCount));
    string(value, "sliceOffset", std::to_string(frame.sliceOffset));
    string(value, "count", std::to_string(frame.count));
    if (frame.repeat != 1) string(value, "repeat", std::to_string(frame.repeat));
    if (!frame.originalMeanIq.empty()) {
      if (frame.originalMeanIq.size() != channels * 2)
        throw std::invalid_argument("Original frame conditioning has invalid dimensions");
      rapidjson::Value means(rapidjson::kArrayType);
      for (double mean : frame.originalMeanIq) {
        if (!std::isfinite(mean) || mean < 0 || mean > 255)
          throw std::invalid_argument("Original frame conditioning is invalid");
        means.PushBack(mean, a);
      }
      value.AddMember("originalMeanIq", means, a);
    }
    frameValues.PushBack(value, a);
  }
  doc.AddMember("frames", frameValues, a);
  if (metadata) {
    const auto& context = metadata->context;
    string(doc, "gainProvenance", metadata->gainProvenance);
    rapidjson::Value gains(rapidjson::kArrayType);
    for (double gain : metadata->perChannelGainDb) {
      if (!std::isfinite(gain)) throw std::invalid_argument("Nonfinite applied recording gain");
      gains.PushBack(gain, a);
    }
    doc.AddMember("perChannelGainDb", gains, a);
    rapidjson::Value requested(rapidjson::kArrayType);
    if (!context.requestedChannelGainDb.empty() && context.requestedChannelGainDb.size() != channels)
      throw std::invalid_argument("Requested recording gains have invalid dimensions");
    for (double gain : context.requestedChannelGainDb) {
      if (!std::isfinite(gain)) throw std::invalid_argument("Nonfinite requested recording gain");
      requested.PushBack(gain, a);
    }
    doc.AddMember("requestedChannelGainDb", requested, a);
    rapidjson::Document details;
    if (metadata->receiverDetailsJson.size() > 65536)
      throw std::invalid_argument("Receiver recording details exceed bound");
    details.Parse<rapidjson::kParseFullPrecisionFlag>(metadata->receiverDetailsJson.c_str());
    if (details.HasParseError() || !details.IsObject())
      throw std::invalid_argument("Invalid receiver recording details");
    rapidjson::Value copied; copied.CopyFrom(details, a); doc.AddMember("receiverDetails", copied, a);
    rapidjson::Value topology(rapidjson::kObjectType);
    string(topology, "referenceMode", context.referenceMode);
    topology.AddMember("coherent", context.coherent, a);
    const auto ids = [&](const char* name, const std::vector<uint32_t>& values) {
      rapidjson::Value array(rapidjson::kArrayType);
      for (auto value : values) array.PushBack(value, a);
      topology.AddMember(rapidjson::Value(name, a), array, a);
    };
    ids("referenceChannels", context.referenceChannels);
    ids("surveillanceChannels", context.surveillanceChannels);
    doc.AddMember("topology", topology, a);
    rapidjson::Document geometry;
    if (context.geometryJson.size() > 65536) throw std::invalid_argument("Recording geometry exceeds bound");
    geometry.Parse<rapidjson::kParseFullPrecisionFlag>(context.geometryJson.c_str());
    if (geometry.HasParseError() || (!geometry.IsObject() && !geometry.IsNull()))
      throw std::invalid_argument("Invalid recording geometry");
    copied.CopyFrom(geometry, a); doc.AddMember("geometry", copied, a);
    doc.AddMember("geometryConfirmed", context.geometryConfirmed, a);
    string(doc, "geometrySha256", digest(encode(geometry)));
    rapidjson::Document sites;
    if (context.geographicSitesJson.size() > 16384) throw std::invalid_argument("Recording geographic sites exceed bound");
    sites.Parse<rapidjson::kParseFullPrecisionFlag>(context.geographicSitesJson.c_str());
    if (sites.HasParseError() || (!sites.IsNull() && !validGeographicSites(sites)))
      throw std::invalid_argument("Invalid recording geographic sites");
    copied.CopyFrom(sites, a); doc.AddMember("geographicSites", copied, a);
    string(doc, "geographicSitesSha256", digest(encode(sites)));
    rapidjson::Value manifolds(rapidjson::kObjectType);
    if (context.manifoldSha256.size() > 32) throw std::invalid_argument("Too many recorded manifold identities");
    for (const auto& entry : context.manifoldSha256) {
      if (entry.first.empty() || entry.first.size() > 128 || !sha256(entry.second))
        throw std::invalid_argument("Invalid recorded manifold identity");
      string(manifolds, entry.first.c_str(), entry.second);
    }
    doc.AddMember("manifoldSha256", manifolds, a);
    string(doc, "configurationSha256", context.configurationSha256);
    string(doc, "codeSha256", context.codeSha256);
  }
  auto result = encode(doc);
  if (result.size() > maximumJsonBytes) throw std::invalid_argument("Recording acquisition metadata exceeds bound");
  return result;
}
template<class Io = RecordingFileCacheIo>
inline std::pair<uint64_t, std::string> fileDigest(int fd) {
  struct stat before{}, after{};
  if (::fstat(fd, &before) || !S_ISREG(before.st_mode) || before.st_size < 0)
    throw std::runtime_error("Acquisition recording must be a regular file");
  Sha256 hash; std::array<uint8_t,65536> bytes{}; uint64_t total = 0;
  Io::prepare(fd);
  RecordingFileCache<Io> cache;
  // Bound work to the original length and leave the caller's replay cursor
  // untouched. A growing or truncated file cannot extend this verification.
  while (total < static_cast<uint64_t>(before.st_size)) {
    const auto requested = static_cast<size_t>(std::min<uint64_t>(bytes.size(), before.st_size - total));
    const auto count = ::pread(fd, bytes.data(), requested, static_cast<off_t>(total));
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) throw std::runtime_error("Cannot hash finalized recording");
    hash.update(bytes.data(), count); total += count;
    cache.consumed(fd, total);
  }
  cache.consumed(fd, total, true);
  // Size alone misses overwrites of bytes already hashed. Check modification
  // and change times (not access time) before accepting the complete digest.
  const bool changed = ::fstat(fd, &after) || before.st_size != after.st_size ||
#if defined(__APPLE__)
    before.st_mtimespec.tv_sec != after.st_mtimespec.tv_sec ||
    before.st_mtimespec.tv_nsec != after.st_mtimespec.tv_nsec ||
    before.st_ctimespec.tv_sec != after.st_ctimespec.tv_sec ||
    before.st_ctimespec.tv_nsec != after.st_ctimespec.tv_nsec;
#else
    before.st_mtim.tv_sec != after.st_mtim.tv_sec || before.st_mtim.tv_nsec != after.st_mtim.tv_nsec ||
    before.st_ctim.tv_sec != after.st_ctim.tv_sec || before.st_ctim.tv_nsec != after.st_ctim.tv_nsec;
#endif
  if (changed) throw std::runtime_error("Recording changed during acquisition finalization");
  return {total, hash.finish()};
}
template<class Io = RecordingFileCacheIo>
inline std::string fileDigest(const std::string& path, uint64_t* size = nullptr) {
  const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (fd < 0) throw std::runtime_error("Cannot open finalized recording for acquisition digest");
  std::pair<uint64_t, std::string> result;
  try {
    result = fileDigest<Io>(fd);
    struct stat opened{}, named{};
    if (::fstat(fd, &opened) || ::lstat(path.c_str(), &named) ||
        opened.st_dev != named.st_dev || opened.st_ino != named.st_ino)
      throw std::runtime_error("Recording path changed during acquisition finalization");
    if (size) *size = result.first;
  } catch (...) { ::close(fd); throw; }
  ::close(fd);
  return std::move(result.second);
}

// Both identities must be pinned independently by an exact-acquisition corpus.
// This manifest binds data and declared metadata, not a hardware signature.
inline void writeManifest(const std::string& recordingPath, const std::string& json,
    const std::string& dataSha256 = {}, const std::string& unavailableReason = {}) {
  const auto dataHash = dataSha256.empty() ? fileDigest(recordingPath) : dataSha256;
  if (!sha256(dataHash) || json.size() > maximumJsonBytes)
    throw std::invalid_argument("Invalid bounded acquisition manifest");
  rapidjson::Document doc(rapidjson::kObjectType); auto& a = doc.GetAllocator();
  doc.AddMember("schema", "vectorwarp-acquisition-manifest-v1", a);
  doc.AddMember("recordingSha256", rapidjson::Value(dataHash.c_str(), a), a);
  doc.AddMember("complete", !json.empty(), a);
  doc.AddMember("reason", rapidjson::Value(unavailableReason.c_str(), a), a);
  if (!json.empty()) {
    rapidjson::Document value; value.Parse<rapidjson::kParseFullPrecisionFlag>(json.c_str());
    if (value.HasParseError() || !value.IsObject()) throw std::invalid_argument("Invalid acquisition manifest JSON");
    const auto canonical = encode(value), hash = digest(canonical);
    doc.AddMember("acquisitionSha256", rapidjson::Value(hash.c_str(), a), a);
    rapidjson::Value copied; copied.CopyFrom(value, a); doc.AddMember("acquisition", copied, a);
  }
  const auto output = encode(doc);
  const auto target = recordingPath + ".acquisition.json", temporary = target + ".partial";
  const int fd = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0640);
  if (fd < 0) throw std::runtime_error("Cannot exclusively create acquisition manifest");
  bool closed = false;
  try {
    size_t written = 0;
    while (written < output.size()) {
      const auto count = ::write(fd, output.data() + written, output.size() - written);
      if (count < 0 && errno == EINTR) continue;
      if (count <= 0) throw std::runtime_error("Cannot write acquisition manifest");
      written += count;
    }
    if (::fsync(fd)) throw std::runtime_error("Cannot flush acquisition manifest");
    ::close(fd); closed = true;
    // Atomic publication without replacing an existing artifact.
    if (::link(temporary.c_str(), target.c_str())) throw std::runtime_error("Cannot publish acquisition manifest exclusively");
    ::unlink(temporary.c_str());
    const auto directory = std::filesystem::path(target).parent_path();
    const int directoryFd = ::open(directory.c_str(), O_RDONLY | O_CLOEXEC);
    if (directoryFd >= 0) { (void)::fsync(directoryFd); ::close(directoryFd); }
  } catch (...) { if (!closed) ::close(fd); ::unlink(temporary.c_str()); throw; }
}
} // namespace blah2::acquisition
