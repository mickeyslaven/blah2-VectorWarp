#include "DirectPathCoherentReplay.h"
#include "capture/Sha256.h"
#include "process/ambiguity/GpuResidentWire.h"
#include <rapidjson/document.h>
#include <fstream>
#include <limits>

namespace blah2::integration::coherent_replay {
namespace {
using resident::wire::require;
using Hash = resident_replay::Hash;
using Json = rapidjson::Value;
const Json& field(const Json& value, const char* key) {
  require(value.IsObject() && value.HasMember(key), "Authenticated IQ manifest field missing");
  return value[key];
}
std::string string(const Json& value) {
  require(value.IsString(), "Authenticated IQ manifest string invalid");
  return {value.GetString(), value.GetStringLength()};
}
uint64_t exact(const Json& value) {
  const auto text = string(value);
  require(!text.empty() && text.size() <= 20 && (text.size() == 1 || text[0] != '0'), "Invalid exact IQ integer");
  uint64_t result = 0;
  for (char digit : text) {
    require(digit >= '0' && digit <= '9' && result <= (UINT64_MAX - uint64_t(digit - '0')) / 10,
      "Inexact/overflowed IQ integer");
    result = result * 10 + uint64_t(digit - '0');
  }
  return result;
}
uint32_t number(const Json& value) {
  require(value.IsUint(), "IQ plan integer invalid"); return value.GetUint();
}
int signedNumber(const Json& value) {
  require(value.IsInt(), "IQ plan signed integer invalid"); return value.GetInt();
}
std::vector<uint8_t> unhex(const std::string& text, size_t length) {
  require(text.size() == 2 * length, "IQ manifest hash/sidecar length invalid");
  std::vector<uint8_t> result(length);
  const auto nibble = [](char c) {
    require((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'), "IQ manifest hex invalid");
    return uint8_t(c <= '9' ? c - '0' : c - 'a' + 10);
  };
  for (size_t i = 0; i < length; ++i) result[i] = uint8_t(16 * nibble(text[2*i]) + nibble(text[2*i+1]));
  return result;
}
Hash hashValue(const Json& value) {
  const auto bytes = unhex(string(value), 32); Hash result{};
  std::copy(bytes.begin(), bytes.end(), result.begin()); return result;
}
Hash digest(const std::vector<uint8_t>& bytes) {
  Sha256 hash; hash.update(bytes.data(), bytes.size()); return hash.finishBytes();
}
std::vector<uint8_t> read(const std::filesystem::path& path, size_t bound) {
  require(std::filesystem::is_regular_file(path) && !std::filesystem::is_symlink(path), "IQ input must be a regular file");
  std::ifstream input(path, std::ios::binary | std::ios::ate);
  require(bool(input), "Cannot open authenticated IQ input"); const auto size = input.tellg();
  require(size > 0 && uint64_t(size) <= bound, "Authenticated IQ input exceeds bound");
  std::vector<uint8_t> bytes(static_cast<size_t>(size)); input.seekg(0);
  require(bool(input.read(reinterpret_cast<char*>(bytes.data()), size)) && input.peek() == std::char_traits<char>::eof(),
    "Authenticated IQ input incomplete"); return bytes;
}
rapidjson::Document parse(const std::vector<uint8_t>& bytes) {
  rapidjson::Document value; value.Parse(reinterpret_cast<const char*>(bytes.data()), bytes.size());
  require(!value.HasParseError() && value.IsObject(), "Authenticated IQ manifest invalid"); return value;
}
}

AuthenticatedIqSegment loadAuthenticatedIqSegment(const std::filesystem::path& planPath,
    const std::filesystem::path& receiptPath, const std::filesystem::path& segmentPath,
    const Hash& planHash, const Hash& receiptHash) {
  const auto planBytes = read(planPath, 1u << 20), receiptBytes = read(receiptPath, 1u << 20);
  require(digest(planBytes) == planHash && digest(receiptBytes) == receiptHash, "IQ plan/receipt pin differs");
  const auto plan = parse(planBytes), receipt = parse(receiptBytes);
  require(string(field(plan, "schema")) == "vectorwarp-coherent-development-ab-plan-v1" &&
    string(field(receipt, "schema")) == "vectorwarp-authenticated-iq-segments-v1" &&
    hashValue(field(receipt, "plan_sha256")) == planHash &&
    field(receipt, "native_snapshot").IsFalse() && field(receipt, "promotion_eligible").IsFalse(),
    "IQ extraction provenance contract differs");
  const auto& segments = field(receipt, "segments"); const auto& sources = field(plan, "sources");
  require(segments.IsArray() && segments.Size() <= 40 && sources.IsArray() && sources.Size() <= 8,
    "IQ manifest population exceeds bound");
  require(std::filesystem::weakly_canonical(segmentPath.parent_path()) ==
    std::filesystem::weakly_canonical(receiptPath.parent_path()), "IQ segment outside pinned receipt directory");
  const Json* selected = nullptr; uint64_t sequence = 0;
  for (rapidjson::SizeType i = 0; i < segments.Size(); ++i) {
    const auto name = string(field(segments[i], "file"));
    require(std::filesystem::path(name).filename() == name, "IQ segment name is not a basename");
    if (name == segmentPath.filename().string()) {
      require(!selected, "Duplicate IQ segment identity"); selected = &segments[i]; sequence = i + 1;
    }
  }
  require(selected && string(field(*selected, "split")) == "calibration", "Only explicitly selected calibration IQ is admitted");
  const auto parentHash = hashValue(field(*selected, "parent_snapshot_sha256")); const Json* source = nullptr;
  for (const auto& item : sources.GetArray()) if (hashValue(field(item, "snapshot_sha256")) == parentHash) {
    require(!source, "Duplicate IQ parent identity"); source = &item;
  }
  require(source && string(field(*source, "split")) == "calibration", "IQ calibration parent unavailable");
  const auto& authenticated = field(receipt, "sources");
  require(authenticated.IsArray() && authenticated.Size() <= 8, "IQ receipt source population invalid");
  unsigned matches = 0;
  for (const auto& item : authenticated.GetArray()) if (hashValue(field(item, "snapshot_sha256")) == parentHash) {
    require(field(item, "envelope_valid").IsTrue() &&
      hashValue(field(item, "raw_sha256")) == hashValue(field(*source, "raw_sha256")), "IQ parent authentication incomplete");
    ++matches;
  }
  require(matches == 1, "IQ parent is not uniquely authenticated");
  const auto sidecar = unhex(string(field(*source, "sidecar_hex")), sidecarBytes);
  require(digest(sidecar) == hashValue(field(*source, "sidecar_sha256")), "IQ parent sidecar pin differs");
  AuthenticatedIqSegment result; result.parent = decodeBoundSidecar(sidecar.data(), sidecar.size(), parentHash);
  const auto& parent = result.parent;
  require(parent.firstSample == exact(field(*source, "first_sample")) && parent.endSample == exact(field(*source, "end_sample")) &&
    parent.identity.configHash == hashValue(field(*source, "config_sha256")) &&
    parent.identity.rawIqHash == hashValue(field(*source, "raw_sha256")) &&
    parent.sourceHash == hashValue(field(*source, "source_sha256")) && parent.rfHash == hashValue(field(*source, "rf_sha256")),
    "IQ parent identity differs from plan");
  result.sampleRate = number(field(plan, "sample_rate")); result.channelId = number(field(plan, "channel"));
  const auto samples = number(field(plan, "sample_count"));
  require(samples > 0 && samples <= 240000 && result.channelId == 0 && result.sampleRate == parent.sampleRateHz,
    "IQ extraction sample/channel/rate contract differs");
  result.sequence = sequence; result.firstSample = exact(field(*selected, "first_sample"));
  result.endSample = exact(field(*selected, "end_sample"));
  require(result.firstSample >= parent.firstSample && result.endSample <= parent.endSample &&
    result.endSample > result.firstSample && result.endSample - result.firstSample == samples &&
    (result.firstSample - parent.firstSample) % samples == 0 &&
    exact(field(*selected, "epoch")) == parent.captureEpoch && exact(field(*selected, "gain_epoch")) == parent.gainCalibrationEpoch &&
    exact(field(*selected, "tuning_hz")) == parent.tuningHz && field(*selected, "raw_reference_filtered_order").IsTrue(),
    "IQ sample range/epoch/channel order differs");
  const auto& ranges = field(*selected, "byte_ranges");
  require(ranges.IsArray() && ranges.Size() == 3, "IQ parent byte ranges missing");
  const auto& sourceBytes = field(*source, "bytes");
  const uint64_t parentSamples = parent.endSample - parent.firstSample;
  require(sourceBytes.IsUint64() && sourceBytes.GetUint64() <= (1u << 30) &&
    parentSamples <= (1u << 30) / (16 * parent.actualGainDb.size()), "IQ parent byte/sample bound invalid");
  uint64_t previousBlockEnd = 0;
  const char* kinds[] = {"14", "15", "16"};
  for (unsigned i = 0; i < 3; ++i) {
    const auto& block = field(field(*source, "blocks"), kinds[i]);
    require(block.IsArray() && block.Size() == 2 && block[0].IsUint64() && block[1].IsUint64() &&
      ranges[i].IsArray() && ranges[i].Size() == 2, "IQ block/range contract invalid");
    const uint64_t begin = exact(ranges[i][0]), end = exact(ranges[i][1]);
    const auto offset = block[0].GetUint64(), length = block[1].GetUint64();
    require(offset >= previousBlockEnd && offset <= sourceBytes.GetUint64() &&
      length <= sourceBytes.GetUint64() - offset &&
      length == parentSamples * 16 * (i == 1 ? 1 : parent.actualGainDb.size()) &&
      result.firstSample - parent.firstSample <= length / 16 && begin >= offset && end > begin &&
      end - offset <= length && end - begin == uint64_t(samples) * 16 &&
      begin - offset == (result.firstSample - parent.firstSample) * 16,
      "IQ exact parent byte/sample mapping differs");
    previousBlockEnd = offset + length;
    result.lineage.parentByteRanges[i] = {begin, end};
  }
  result.delayMin = signedNumber(field(plan, "delay_min")); result.delayMax = signedNumber(field(plan, "delay_max"));
  result.dopplerMin = signedNumber(field(plan, "doppler_min")); result.dopplerMax = signedNumber(field(plan, "doppler_max"));
  require(result.delayMin >= -4096 && result.delayMax <= 4096 && result.delayMin < result.delayMax &&
    int64_t(result.delayMax) - result.delayMin < 4096 && result.dopplerMin < result.dopplerMax &&
    std::abs(int64_t(result.dopplerMin)) <= result.sampleRate / 2 &&
    std::abs(int64_t(result.dopplerMax)) <= result.sampleRate / 2, "IQ derived CAF axes exceed bounds");
  const auto payload = read(segmentPath, size_t(samples) * 16 * 3);
  result.lineage.payloadHash = hashValue(field(*selected, "sha256"));
  require(payload.size() == size_t(samples) * 16 * 3 && digest(payload) == result.lineage.payloadHash,
    "IQ segment payload seal differs");
  result.lineage.planHash = planHash; result.lineage.receiptHash = receiptHash;
  result.lineage.planJson.assign(planBytes.begin(), planBytes.end()); result.lineage.receiptJson.assign(receiptBytes.begin(), receiptBytes.end());
  resident::wire::Reader input{{payload.data(), payload.size()}};
  std::vector<std::complex<double>>* arrays[] = {&result.raw, &result.reference, &result.filtered};
  for (unsigned channel = 0; channel < 3; ++channel) {
    arrays[channel]->reserve(samples);
    for (uint32_t n = 0; n < samples; ++n) {
      const size_t at = (size_t(channel) * samples + n) * 16;
      const double re = input.f64(at), im = input.f64(at + 8);
      require(std::isfinite(re) && std::isfinite(im), "IQ segment has nonfinite samples");
      arrays[channel]->emplace_back(re, im);
    }
  }
  return result;
}
} // namespace blah2::integration::coherent_replay
