#include "Recording.h"
#include "RecordingAcquisitionJson.h"
#include "RecordingFileCache.h"
#include "kraken/HeimdallFrame.h"
#include <algorithm>
#include <array>
#include <condition_variable>
#include <cerrno>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <deque>
#include <limits>
#include <mutex>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <stdexcept>
#include <thread>
#include <sys/stat.h>
#include <unistd.h>
#if __has_include("ReceiverCohort.h")
#include "ReceiverCohort.h"
#else
#define BLAH2_RECEIVER_COHORT "standalone-unqualified-build"
#define BLAH2_RECEIVER_CODE_FINGERPRINT "standalone-unqualified-build"
#endif
#ifndef BLAH2_RECEIVER_CODE_FINGERPRINT
#define BLAH2_RECEIVER_CODE_FINGERPRINT BLAH2_RECEIVER_COHORT
#endif

namespace blah2 {
namespace {
constexpr uint32_t maxBlock = 262144;
// Enough elasticity for ordinary filesystem jitter without allowing a slow or
// failed disk to consume unbounded RAM. Acquisition never waits for capacity:
// a full queue fails recording explicitly and leaves radar processing alive.
constexpr std::size_t maxQueuedRecordingBytes = 64ULL << 20;
constexpr std::size_t recordingBatchBytes = 1ULL << 20;
constexpr std::array<uint8_t,8> magic{'B','L','A','H','2','I','Q',0};
uint32_t u32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1])<<8 | uint32_t(p[2])<<16 | uint32_t(p[3])<<24; }
uint64_t u64(const uint8_t* p) { return u32(p) | uint64_t(u32(p+4))<<32; }
void put32(uint8_t* p,uint32_t v) { for(unsigned i=0;i<4;++i) p[i]=v>>(8*i); }
void put64(uint8_t* p,uint64_t v) { put32(p,v); put32(p+4,v>>32); }
float floating(const uint8_t* p) { const auto bits=u32(p); float value; std::memcpy(&value,&bits,4); return value; }
void putFloat(uint8_t* p,float value) { uint32_t bits; std::memcpy(&bits,&value,4); put32(p,bits); }
void fail(const std::string& message) { throw std::runtime_error(message); }
std::string fnv1a64(const std::string& value) {
  uint64_t hash=1469598103934665603ULL;
  for(unsigned char byte:value) { hash^=byte; hash*=1099511628211ULL; }
  char output[25]{};
  std::snprintf(output,sizeof(output),"fnv1a64:%016llx",
    static_cast<unsigned long long>(hash));
  return output;
}
struct File {
  int fd=-1;
  explicit File(const std::string& path,bool writing=false) {
    fd=::open(path.c_str(), writing ? O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC : O_RDONLY|O_NONBLOCK|O_CLOEXEC,0600);
    if(fd<0) fail(std::string(writing ? "Cannot create recording: " : "Cannot open replay file: ")+std::strerror(errno));
    struct stat info{};
    if(fstat(fd,&info) || !S_ISREG(info.st_mode)) { ::close(fd); fd=-1; fail("Recording path must be a regular file"); }
  }
  ~File() { if(fd>=0) ::close(fd); }
  uint64_t size() const { struct stat info{}; if(fstat(fd,&info)) fail("Cannot inspect recording size"); return info.st_size; }
  uint64_t position() const { const auto p=lseek(fd,0,SEEK_CUR); if(p<0) fail("Cannot read recording position"); return p; }
  void seek(uint64_t p) { if(p>uint64_t(INT64_MAX) || lseek(fd,p,SEEK_SET)<0) fail("Cannot seek recording"); }
  void read(void* destination,size_t count) {
    auto* p=static_cast<uint8_t*>(destination);
    while(count) {
      const auto n=::read(fd,p,count);
      if(n<0 && errno==EINTR) continue;
      if(n<=0) fail(n==0 ? "Recording is truncated" : "Cannot read recording");
      p+=n; count-=n;
    }
  }
  void write(const void* source,size_t count) {
    const auto* p=static_cast<const uint8_t*>(source);
    while(count) {
      const auto n=::write(fd,p,count);
      if(n<0 && errno==EINTR) continue;
      if(n<=0) fail(std::string("Recording write failed: ")+std::strerror(errno));
      p+=n; count-=n;
    }
  }
};
void valid(const RecordingMetadata& m) {
  if(m.channels<2 || m.channels>8 || !m.sampleRate || !m.frequency) fail("Invalid recording channel count, sample rate or frequency");
}
void finite(const IqBlock& block) {
  for(const auto& channel:block) for(auto value:channel)
    if(!std::isfinite(value.real()) || !std::isfinite(value.imag())) fail("Recording contains invalid I/Q samples");
}
void finite_slice(const IqBlock& block,size_t offset,size_t count) {
  for(const auto& channel:block)
    for(size_t i=offset;i<offset+count;++i) {
      const auto value=channel[i];
      if(!std::isfinite(value.real()) || !std::isfinite(value.imag()))
        fail("Recording contains invalid I/Q samples");
    }
}
bool native_float_iq_is_little_endian() {
  static_assert(sizeof(float) == 4 && std::numeric_limits<float>::is_iec559,
    "Recording format requires IEEE-754 binary32 samples");
  static_assert(sizeof(std::complex<float>) == 2*sizeof(float),
    "Recording fast path requires the standard complex-float representation");
  const float value=1.0f;
  constexpr std::array<uint8_t,4> little{{0x00,0x00,0x80,0x3f}};
  return std::memcmp(&value,little.data(),little.size())==0;
}
std::string json_string(const rapidjson::Value& object,const char* name) {
  if(!object.HasMember(name) || !object[name].IsString() || !object[name].GetStringLength())
    fail(std::string("Recording provenance is missing ")+name);
  return {object[name].GetString(),object[name].GetStringLength()};
}
uint64_t decimal(const rapidjson::Value& object,const char* name) {
  if(!object.HasMember(name) || !object[name].IsString() || !object[name].GetStringLength())
    fail(std::string("Recording provenance is missing ")+name);
  const std::string value(object[name].GetString(),object[name].GetStringLength());
  if(value.size()>20 || value.find_first_not_of("0123456789")!=std::string::npos)
    fail(std::string("Recording provenance has invalid ")+name);
  try {
    size_t used=0; const auto result=std::stoull(value,&used);
    if(used!=value.size()) fail(std::string("Recording provenance has invalid ")+name);
    return result;
  } catch(const std::exception&) { fail(std::string("Recording provenance has invalid ")+name); }
  return 0;
}
RecordingProvenance read_provenance(const std::string& path,
    const ReplayOptions& options,const RecordingMetadata& metadata) {
  RecordingProvenance result;
  result.receiver=options.receiver;
  result.reason="Recording has no replay-qualified v2 provenance sidecar";
  const std::filesystem::path sidecar(path+".provenance.json");
  if(!std::filesystem::exists(sidecar)) return result;
  std::error_code sizeError;
  const auto size=std::filesystem::file_size(sidecar,sizeError);
  if(sizeError || !size || size>1048576) fail("Recording provenance sidecar is empty or too large");
  std::ifstream input(sidecar,std::ios::binary);
  result.json.assign(std::istreambuf_iterator<char>(input),{});
  if(input.bad()) fail("Cannot read recording provenance sidecar");
  rapidjson::Document document; document.Parse(result.json.c_str());
  if(document.HasParseError() || !document.IsObject()) fail("Recording provenance sidecar is invalid JSON");
  if(!document.HasMember("schema") || !document["schema"].IsString())
    fail("Recording provenance sidecar has no schema");
  result.present=true;
  result.schema={document["schema"].GetString(),document["schema"].GetStringLength()};
  if(result.schema!="vectorwarp-recording-provenance-v2") {
    result.reason="Legacy provenance schema is not replay-qualified";
    return result;
  }
  result.receiver=json_string(document,"receiver");
  result.backend=json_string(document,"backend");
  // Receiver provenance describes the radio that produced the samples. Replay
  // never opens that radio, so the currently selected live receiver is not a
  // compatibility constraint. Channel/rate/frequency checks below still guard
  // the processing contract.
  const auto required_uint=[&](const char* name,uint32_t expected) {
    if(!document.HasMember(name) || !document[name].IsUint() || document[name].GetUint()!=expected)
      fail(std::string("Recording provenance ")+name+" differs from the recording header");
  };
  required_uint("channels",metadata.channels);
  required_uint("sampleRateHz",metadata.sampleRate);
  required_uint("frequencyHz",metadata.frequency);
  if(!document.HasMember("channelSettings") || !document["channelSettings"].IsArray() ||
      document["channelSettings"].Size()!=metadata.channels)
    fail("Recording provenance channel settings are incomplete");
  if(!document.HasMember("receiverDetails") || !document["receiverDetails"].IsObject() ||
      !document.HasMember("device") || !document["device"].IsObject() ||
      !document.HasMember("topology") || !document["topology"].IsObject())
    fail("Recording provenance receiver/device/topology contract is incomplete");
  if(!document.HasMember("recording") || !document["recording"].IsObject())
    fail("Recording provenance recording contract is missing");
  const auto& recording=document["recording"];
  if(!recording.HasMember("complete") || !recording["complete"].IsBool() ||
      !recording["complete"].GetBool() || decimal(recording,"samples")!=metadata.samples)
    fail("Recording provenance does not describe this completed recording");
  if(!document.HasMember("timing") || !document["timing"].IsObject())
    fail("Recording provenance timing contract is missing");
  const auto& timing=document["timing"];
  result.timeSource=json_string(timing,"source");
  if(!timing.HasMember("exact") || !timing["exact"].IsBool() ||
      !timing.HasMember("uncertaintyBounded") || !timing["uncertaintyBounded"].IsBool() ||
      !timing.HasMember("continuous") || !timing["continuous"].IsBool() ||
      !timing.HasMember("gaps") || !timing["gaps"].IsArray() ||
      !timing.HasMember("discontinuities") || !timing["discontinuities"].IsArray())
    fail("Recording provenance timing fields are incomplete");
  result.exactTime=timing["exact"].GetBool();
  result.uncertaintyBounded=timing["uncertaintyBounded"].GetBool();
  result.continuous=timing["continuous"].GetBool();
  result.firstSample=decimal(timing,"firstSample");
  result.endSample=decimal(timing,"endSample");
  if(result.firstSample!=0 || result.endSample!=metadata.samples ||
      !result.continuous || !timing["gaps"].Empty() || !timing["discontinuities"].Empty())
    fail("Recording provenance sample intervals do not match a continuous recording");
  if(timing.HasMember("epoch") && timing["epoch"].IsString()) result.epoch=decimal(timing,"epoch");
  if(timing.HasMember("sourceFirstSample") && timing["sourceFirstSample"].IsString()) {
    result.sourceFirstSample=decimal(timing,"sourceFirstSample");
    result.sourceSampleAvailable=true;
  }
  if(timing.HasMember("deviceTimeEpochTick") && timing["deviceTimeEpochTick"].IsString()) {
    const std::string tick(timing["deviceTimeEpochTick"].GetString(),timing["deviceTimeEpochTick"].GetStringLength());
    try { size_t used=0; result.deviceTimeEpochTick=std::stoll(tick,&used);
      if(used!=tick.size()) fail("Recording provenance has invalid deviceTimeEpochTick");
    } catch(const std::exception&) { fail("Recording provenance has invalid deviceTimeEpochTick"); }
    result.deviceTimeAvailable=true;
  }
  const auto& topology=document["topology"];
  const auto optional_string=[&](const char* name) {
    return topology.HasMember(name) && topology[name].IsString() ?
      std::string(topology[name].GetString(),topology[name].GetStringLength()) : std::string{};
  };
  result.clockSource=optional_string("clockSource");
  result.deviceTimeSource=optional_string("timeSource");
  result.motherboard=optional_string("motherboard");
  result.subdevices=optional_string("subdevices");
  if(!document.HasMember("fingerprints") || !document["fingerprints"].IsObject())
    fail("Recording provenance fingerprints are missing");
  const auto& fingerprints=document["fingerprints"];
  result.configurationFingerprint=json_string(fingerprints,"configuration");
  result.codeFingerprint=json_string(fingerprints,"code");
  rapidjson::StringBuffer detailsBuffer;
  rapidjson::Writer<rapidjson::StringBuffer> detailsWriter(detailsBuffer);
  document["receiverDetails"].Accept(detailsWriter);
  const auto expectedConfiguration=fnv1a64(std::string(detailsBuffer.GetString(),detailsBuffer.GetSize())+
    "|"+std::to_string(metadata.channels)+"|"+std::to_string(metadata.sampleRate)+"|"+
    std::to_string(metadata.frequency));
  if(result.configurationFingerprint!=expectedConfiguration)
    fail("Recording provenance configuration fingerprint is invalid");
  result.configurationFingerprintValid=true;
  result.codeFingerprintMatches=result.codeFingerprint==
    std::string("sha256:")+BLAH2_RECEIVER_CODE_FINGERPRINT+
      ";receiver-cohort:"+BLAH2_RECEIVER_COHORT;
  result.contractValid=true; result.legacy=false;
  result.decisionGrade=result.exactTime && result.uncertaintyBounded &&
    result.codeFingerprintMatches;
  result.reason=result.decisionGrade ? "Replay provenance is decision-grade" :
    !result.codeFingerprintMatches ?
      "Replay provenance is valid but was produced by a different receiver code cohort" :
      "Replay provenance is valid but RF time is not exact and bounded";
  return result;
}
void read_native_mchq_sidecar(const std::string& path,
    const RecordingMetadata& metadata,uint64_t fileSize,
    RecordingProvenance& result) {
  const std::filesystem::path sidecar(path+".json");
  if(!std::filesystem::exists(sidecar))return;
  std::error_code sizeError;
  const auto size=std::filesystem::file_size(sidecar,sizeError);
  if(sizeError||!size||size>1048576)fail("Kraken clip sidecar is empty or too large");
  std::ifstream input(sidecar,std::ios::binary);
  const std::string json(std::istreambuf_iterator<char>(input),{});
  if(input.bad())fail("Cannot read Kraken clip sidecar");
  rapidjson::Document document;document.Parse(json.c_str());
  const auto unsigned_value=[&](const char* name)->uint64_t{
    if(!document.HasMember(name)||!document[name].IsUint64())
      fail(std::string("Kraken clip sidecar has invalid ")+name);
    return document[name].GetUint64();
  };
  if(document.HasParseError()||!document.IsObject()||
      !document.HasMember("schema")||!document["schema"].IsString()||
      std::string(document["schema"].GetString(),document["schema"].GetStringLength())!=
        "vectorwarp-kraken-iq-clip-v1"||
      !document.HasMember("format")||!document["format"].IsString()||
      std::string(document["format"].GetString(),document["format"].GetStringLength())!="mchq"||
      unsigned_value("channels")!=metadata.channels||
      unsigned_value("sampleRate")!=metadata.sampleRate||
      unsigned_value("frequency")!=metadata.frequency||
      unsigned_value("samplesPerChannel")!=metadata.samples||
      unsigned_value("dataBytes")!=fileSize)
    fail("Kraken clip sidecar differs from recording metadata");
  const uint64_t first=unsigned_value("firstEpochSample");
  const uint64_t end=unsigned_value("endEpochSample");
  if(end<=first||end-first!=metadata.samples)
    fail("Kraken clip sidecar sample interval differs from recording");
  result.epoch=unsigned_value("epoch");
  result.sourceFirstSample=first;
  result.sourceSampleAvailable=true;
}
void read_portable_sidecar(const std::string& path,
    const RecordingMetadata& metadata,uint64_t fileSize,File& file,
    RecordingProvenance& result) {
  const std::filesystem::path sidecar(path+".json");
  if(!std::filesystem::exists(sidecar))return;
  std::error_code sizeError;
  const auto size=std::filesystem::file_size(sidecar,sizeError);
  if(sizeError||!size||size>1048576)fail("Portable clip sidecar is empty or too large");
  std::ifstream input(sidecar,std::ios::binary);
  const std::string json(std::istreambuf_iterator<char>(input),{});
  if(input.bad())fail("Cannot read portable clip sidecar");
  rapidjson::Document document;document.Parse<rapidjson::kParseFullPrecisionFlag>(json.c_str());
  if(document.HasParseError()||!document.IsObject())
    fail("Portable clip sidecar is invalid JSON");
  const auto unsigned_value=[&](const char* name)->uint64_t{
    if(!document.HasMember(name)||!document[name].IsUint64())
      fail(std::string("Portable clip sidecar has invalid ")+name);
    return document[name].GetUint64();
  };
  if(json_string(document,"schema")!="vectorwarp-portable-iq-clip-v1"||
      json_string(document,"format")!="blah2iq"||
      json_string(document,"encoding")!="float32-complex-channel-major-blocks"||
      unsigned_value("channels")!=metadata.channels||
      unsigned_value("sampleRate")!=metadata.sampleRate||
      unsigned_value("frequency")!=metadata.frequency||
      unsigned_value("samplesPerChannel")!=metadata.samples||
      unsigned_value("dataBytes")!=fileSize)
    fail("Portable clip sidecar differs from recording metadata");
  const uint64_t epoch=unsigned_value("epoch"),first=unsigned_value("firstEpochSample");
  const uint64_t end=unsigned_value("endEpochSample");
  if(end<=first||end-first!=metadata.samples)
    fail("Portable clip sidecar sample interval differs from recording");
  if(result.sourceSampleAvailable&&
      (result.epoch!=epoch||result.sourceFirstSample!=first))
    fail("Portable clip sidecar conflicts with recording provenance");
  const auto expected=json_string(document,"dataSha256");
  if(expected.size()!=64||expected.find_first_not_of("0123456789abcdef")!=std::string::npos)
    fail("Portable clip sidecar has invalid dataSha256");
  // Hash the already-open recording, including its completed header, rather
  // than reopening a pathname that could now identify a different file.
  const auto integrity = acquisition::fileDigest(file.fd);
  if(integrity.first!=fileSize||integrity.second!=expected)
    fail("Portable clip sidecar SHA-256 differs from recording");
  result.recordingSha256 = integrity.second;
  // Applied gains are optional, frame-wide source metadata, never a requested
  // receiver setting. Only admit them after this sidecar is bound to the exact
  // completed IQ file. Older clips remain explicitly uncalibrated.
  if(document.HasMember("perChannelGainDb")) {
    const auto& values=document["perChannelGainDb"];
    if(!values.IsArray()||values.Size()!=metadata.channels)
      fail("Portable clip sidecar perChannelGainDb has invalid channel count");
    std::vector<double> gains;
    for(const auto& value:values.GetArray()) {
      if(!value.IsNumber()||!std::isfinite(value.GetDouble()))
        fail("Portable clip sidecar perChannelGainDb must contain finite numbers");
      gains.push_back(value.GetDouble());
    }
    if(!result.perChannelGainDb.empty()&&result.perChannelGainDb!=gains)
      fail("Portable clip gains conflict with recording provenance");
    result.perChannelGainDb=std::move(gains);
  }
  // This authenticates relative sample identity only. Keep all exact-time,
  // decision-grade and v2-contract qualification flags unchanged.
  result.epoch=epoch;
  result.sourceFirstSample=first;
  result.sourceSampleAvailable=true;
}

std::string read_acquisition_manifest(const std::string& path, RecordingProvenance& result,
    std::string& expectedDataHash) {
  const auto name = path + ".acquisition.json";
  if (!std::filesystem::exists(name)) return {};
  File manifest(name); const auto size = manifest.size();
  if (!size || size > acquisition::maximumJsonBytes + 2048)
    fail("Acquisition manifest exceeds bounded size");
  std::string json(size, '\0'); manifest.read(json.data(), json.size());
  if (manifest.size() != size) fail("Acquisition manifest changed during read");
  rapidjson::Document doc; doc.Parse<rapidjson::kParseFullPrecisionFlag>(json.c_str());
  if (doc.HasParseError() || !doc.IsObject() ||
      json_string(doc, "schema") != "vectorwarp-acquisition-manifest-v1" ||
      !doc.HasMember("complete") || !doc["complete"].IsBool())
    fail("Invalid acquisition manifest contract");
  expectedDataHash = json_string(doc, "recordingSha256");
  if (!acquisition::sha256(expectedDataHash)) fail("Invalid acquisition manifest recording hash");
  if (!doc["complete"].GetBool()) {
    result.acquisitionReason = json_string(doc, "reason"); return {};
  }
  if (!doc.HasMember("acquisition") || !doc["acquisition"].IsObject()) fail("Acquisition manifest metadata missing");
  const auto acquisitionJson = acquisition::encode(doc["acquisition"]);
  result.acquisitionSha256 = json_string(doc, "acquisitionSha256");
  if (result.acquisitionSha256 != acquisition::digest(acquisitionJson))
    fail("Acquisition manifest metadata digest mismatch");
  return acquisitionJson;
}

bool read_acquisition(const RecordingMetadata& metadata, RecordingProvenance& result) {
  if (metadata.acquisitionJson.empty()) return false;
  rapidjson::Document doc;
  doc.Parse<rapidjson::kParseFullPrecisionFlag>(metadata.acquisitionJson.c_str());
  if (doc.HasParseError() || !doc.IsObject())
    fail("Recording acquisition schema is invalid");
  const auto schema = json_string(doc, "schema");
  const bool runs = schema == "vectorwarp-recording-acquisition-v2";
  if (!runs && schema != "vectorwarp-recording-acquisition-v1")
    fail("Recording acquisition schema is invalid");
  const auto boolean = [&](const rapidjson::Value& object, const char* name) {
    if (!object.HasMember(name) || !object[name].IsBool())
      fail(std::string("Recording acquisition is missing ") + name);
    return object[name].GetBool();
  };
  for (const auto& item : {std::pair<const char*, uint32_t>{"channels", metadata.channels},
      {"sampleRateHz", metadata.sampleRate}, {"frequencyHz", metadata.frequency}})
    if (!doc.HasMember(item.first) || !doc[item.first].IsUint() || doc[item.first].GetUint() != item.second)
      fail("Recording acquisition RF dimensions differ from IQ header");
  const auto receiver = json_string(doc, "receiver");
  if (receiver != "Kraken" && receiver != "Usrp" && receiver != "HackRF" && receiver != "RspDuo")
    fail("Recording acquisition receiver is unsupported");
  if (receiver != "Kraken" && metadata.channels != 2)
    fail("Recording acquisition receiver topology requires two input channels");
  const auto epoch = decimal(doc, "epoch"), first = decimal(doc, "firstSample"), end = decimal(doc, "endSample");
  if (end <= first || end - first != metadata.samples)
    fail("Recording acquisition sample interval differs from IQ payload");
  if (result.sourceSampleAvailable && (result.epoch != epoch || result.sourceFirstSample != first))
    fail("Recording acquisition sample identity conflicts with sidecar");
  const bool positions = boolean(doc, "sourcePositionAvailable");
  if (json_string(doc, "frameSequenceSource") != "recorder-ingress" ||
      !doc.HasMember("frames") || !doc["frames"].IsArray() || doc["frames"].Empty() ||
      doc["frames"].Size() > acquisition::maximumFrames)
    fail("Recording acquisition frame lineage is missing or unbounded");
  uint64_t written = 0, previous = 0;
  for (const auto& frame : doc["frames"].GetArray()) {
    if (!frame.IsObject()) fail("Invalid acquisition frame lineage");
    RecordingFrameLineage value{decimal(frame, "sequence"), decimal(frame, "sourceFirst"),
      decimal(frame, "sourceCount"), decimal(frame, "sliceOffset"), decimal(frame, "count")};
    if (frame.HasMember("repeat")) {
      if (!runs) fail("Recording acquisition frame runs require v2 metadata");
      value.repeat = decimal(frame, "repeat");
    }
    if (!value.ingressSequence || (previous && (previous == UINT64_MAX || value.ingressSequence != previous + 1)) ||
        !value.repeat || value.repeat - 1 > UINT64_MAX - value.ingressSequence ||
        !value.count || !value.sourceCount || value.sourceCount > (metadata.format == "mchq" ? (1U << 24) : maxBlock) ||
        value.sourceFirst > UINT64_MAX - value.sourceCount ||
        value.sliceOffset > value.sourceCount || value.count > value.sourceCount - value.sliceOffset ||
        value.sourceFirst > UINT64_MAX - value.sliceOffset ||
        value.sourceFirst + value.sliceOffset != first + written ||
        value.repeat > (metadata.samples - written) / value.count ||
        (value.repeat != 1 && (metadata.format != "blah2" || value.sliceOffset ||
          value.count != value.sourceCount || frame.HasMember("originalMeanIq"))))
      fail("Recording acquisition frame lineage is discontinuous");
    if (frame.HasMember("originalMeanIq")) {
      const auto& means = frame["originalMeanIq"];
      if (metadata.format != "mchq" || !means.IsArray() || means.Size() != metadata.channels * 2)
        fail("Recording original frame conditioning dimensions are invalid");
      for (const auto& mean : means.GetArray()) {
        if (!mean.IsNumber() || !std::isfinite(mean.GetDouble()) || mean.GetDouble() < 0 || mean.GetDouble() > 255)
          fail("Recording original frame conditioning is invalid");
        value.originalMeanIq.push_back(mean.GetDouble());
      }
    }
    if (metadata.format == "mchq" && value.count != value.sourceCount && value.originalMeanIq.empty())
      fail("Trimmed native recording is missing original full-frame conditioning");
    written += value.count * value.repeat;
    previous = value.ingressSequence + value.repeat - 1; result.frames.push_back(value);
  }
  if (written != metadata.samples) fail("Recording acquisition frame count differs from IQ payload");
  result.epoch = epoch; result.sourceFirstSample = first;
  result.sourceSampleAvailable = positions || result.sourceSampleAvailable;
  result.receiver = receiver; result.acquisitionJson = metadata.acquisitionJson;
  result.acquisitionSha256 = acquisition::digest(metadata.acquisitionJson);
  result.acquisitionIntegrityValid = true;
  // Geographic declarations have their own capability/trust gate. This also
  // permits explicitly non-hardware synthetic fixtures without claiming gain
  // readback, calibrated geometry, or exact acquisition capability.
  if (doc.HasMember("geographicSites") || doc.HasMember("geographicSitesSha256")) {
    if (!doc.HasMember("geographicSites") ||
        (!doc["geographicSites"].IsNull() && !acquisition::validGeographicSites(doc["geographicSites"])))
      fail("Recording geographic sites contract is invalid");
    result.acquisitionContext.geographicSitesJson = acquisition::encode(doc["geographicSites"]);
    if (result.acquisitionContext.geographicSitesJson.size() > 16384 ||
        json_string(doc, "geographicSitesSha256") != acquisition::digest(result.acquisitionContext.geographicSitesJson))
      fail("Recording geographic sites digest is invalid");
  }
  if (!boolean(doc, "metadataAvailable")) {
    result.acquisitionReason = "Acquisition metadata unavailable; ordinary playback only";
    return runs;
  }
  result.gainProvenance = json_string(doc, "gainProvenance");
  if (result.gainProvenance != "sample-aligned-readback" &&
      result.gainProvenance != "sample-aligned-receiver-header" &&
      result.gainProvenance != "requested-settings-only" && result.gainProvenance != "unavailable")
    fail("Recording acquisition gain provenance strength is invalid");
  if (!doc.HasMember("perChannelGainDb") || !doc["perChannelGainDb"].IsArray() ||
      (!doc["perChannelGainDb"].Empty() && doc["perChannelGainDb"].Size() != metadata.channels))
    fail("Recording acquisition gains have invalid dimensions");
  std::vector<double> gains;
  for (const auto& value : doc["perChannelGainDb"].GetArray()) {
    if (!value.IsNumber() || !std::isfinite(value.GetDouble())) fail("Recording acquisition gain is nonfinite");
    gains.push_back(value.GetDouble());
  }
  if (!result.perChannelGainDb.empty() && result.perChannelGainDb != gains)
    fail("Recording acquisition gains conflict with sidecar");
  // Requested-only gains are retained in acquisitionJson but cannot become
  // applied-gain correction input to the radar pipeline.
  const bool appliedGains = result.gainProvenance == "sample-aligned-readback" ||
    result.gainProvenance == "sample-aligned-receiver-header";
  result.perChannelGainDb = appliedGains ? gains : std::vector<double>{};
  // Optional for old recordings. Missing requests must stay unavailable,
  // never be reconstructed from quantized/coerced hardware readback.
  if (doc.HasMember("requestedChannelGainDb")) {
    const auto& requested = doc["requestedChannelGainDb"];
    if (!requested.IsArray() || (!requested.Empty() && requested.Size() != metadata.channels))
      fail("Recording requested gains have invalid dimensions");
    for (const auto& value : requested.GetArray()) {
      if (!value.IsNumber() || !std::isfinite(value.GetDouble()))
        fail("Recording requested gain is nonfinite");
      result.acquisitionContext.requestedChannelGainDb.push_back(value.GetDouble());
    }
  }
  if (!doc.HasMember("receiverDetails") || !doc["receiverDetails"].IsObject() ||
      !doc.HasMember("topology") || !doc["topology"].IsObject())
    fail("Recording acquisition receiver details/topology are missing");
  const auto& topology = doc["topology"];
  auto& context = result.acquisitionContext;
  if (!topology.HasMember("referenceMode") || !topology["referenceMode"].IsString())
    fail("Recording acquisition reference mode is missing");
  context.referenceMode = topology["referenceMode"].GetString();
  context.coherent = boolean(topology, "coherent");
  const auto channels = [&](const char* name, std::vector<uint32_t>& output) {
    if (!topology.HasMember(name) || !topology[name].IsArray()) fail("Recording acquisition channel roles are missing");
    std::set<uint32_t> unique;
    for (const auto& id : topology[name].GetArray()) {
      if (!id.IsUint() || id.GetUint() >= metadata.channels || !unique.insert(id.GetUint()).second)
        fail("Recording acquisition channel roles are invalid");
      output.push_back(id.GetUint());
    }
  };
  channels("referenceChannels", context.referenceChannels);
  channels("surveillanceChannels", context.surveillanceChannels);
  bool topologyComplete = !context.surveillanceChannels.empty();
  if (context.referenceMode == "dedicated") {
    if (context.referenceChannels.size() != 1 ||
        std::find(context.surveillanceChannels.begin(), context.surveillanceChannels.end(),
          context.referenceChannels.front()) != context.surveillanceChannels.end())
      fail("Recording acquisition dedicated reference roles overlap");
  } else if (context.referenceMode == "array_eigenbeam") {
    if (!context.coherent || context.referenceChannels.size() < 2)
      fail("Recording acquisition array reference requires coherent channels");
  } else if (!context.referenceMode.empty()) fail("Recording acquisition reference mode is unsupported");
  else topologyComplete = false;
  if (!doc.HasMember("geometry") || (!doc["geometry"].IsObject() && !doc["geometry"].IsNull()))
    fail("Recording acquisition geometry identity is missing");
  context.geometryJson = acquisition::encode(doc["geometry"]);
  if (json_string(doc, "geometrySha256") != acquisition::digest(context.geometryJson))
    fail("Recording acquisition geometry digest is invalid");
  context.geometryConfirmed = boolean(doc, "geometryConfirmed");
  if (!doc.HasMember("manifoldSha256") || !doc["manifoldSha256"].IsObject() || doc["manifoldSha256"].MemberCount() > 32)
    fail("Recording acquisition manifold identities are missing or unbounded");
  for (auto it = doc["manifoldSha256"].MemberBegin(); it != doc["manifoldSha256"].MemberEnd(); ++it) {
    if (!it->value.IsString() || !it->name.GetStringLength() || it->name.GetStringLength() > 128 ||
        !acquisition::sha256(it->value.GetString()) ||
        !context.manifoldSha256.emplace(it->name.GetString(), it->value.GetString()).second)
      fail("Recording acquisition manifold identity is invalid");
  }
  const auto fingerprint = [&](const char* name) {
    if (!doc.HasMember(name) || !doc[name].IsString()) fail("Recording acquisition fingerprints are missing");
    return std::string(doc[name].GetString(), doc[name].GetStringLength());
  };
  context.configurationSha256 = fingerprint("configurationSha256"); context.codeSha256 = fingerprint("codeSha256");
  bool arrayGeometryComplete = context.geometryConfirmed && doc["geometry"].IsObject();
  if (arrayGeometryComplete) {
    const auto& geometry = doc["geometry"];
    arrayGeometryComplete = geometry.HasMember("units") && geometry["units"].IsString() &&
      std::string(geometry["units"].GetString()) == "m" &&
      geometry.HasMember("x_axis_bearing_deg_true") && geometry["x_axis_bearing_deg_true"].IsNumber() &&
      std::isfinite(geometry["x_axis_bearing_deg_true"].GetDouble()) &&
      geometry.HasMember("elements") && geometry["elements"].IsArray() &&
      geometry["elements"].Size() >= 2 && geometry["elements"].Size() <= metadata.channels;
    std::set<unsigned> located;
    if (arrayGeometryComplete) for (const auto& element : geometry["elements"].GetArray()) {
      if (!element.IsObject() || !element.HasMember("daq_channel") || !element["daq_channel"].IsUint() ||
          element["daq_channel"].GetUint() >= metadata.channels || !located.insert(element["daq_channel"].GetUint()).second ||
          !element.HasMember("position") || !element["position"].IsArray() || element["position"].Size() != 3) {
        arrayGeometryComplete = false; break;
      }
      for (const auto& coordinate : element["position"].GetArray())
        if (!coordinate.IsNumber() || !std::isfinite(coordinate.GetDouble())) arrayGeometryComplete = false;
    }
    for (const auto id : context.referenceChannels) arrayGeometryComplete = arrayGeometryComplete && located.count(id);
    for (const auto id : context.surveillanceChannels) arrayGeometryComplete = arrayGeometryComplete && located.count(id);
  }
  const bool geometryComplete = context.referenceMode != "array_eigenbeam" || arrayGeometryComplete;
  result.exactAcquisition = positions && topologyComplete && geometryComplete &&
    appliedGains && gains.size() == metadata.channels &&
    !doc["receiverDetails"].ObjectEmpty() && acquisition::sha256(context.configurationSha256) &&
    acquisition::sha256(context.codeSha256);
  result.acquisitionReason.clear();
  const auto missing = [&](bool available, const char* reason) {
    if (!available) { if (!result.acquisitionReason.empty()) result.acquisitionReason += "; "; result.acquisitionReason += reason; }
  };
  missing(positions, "Source-aligned sample/frame lineage unavailable");
  missing(topologyComplete, "Configured channel roles unavailable");
  missing(geometryComplete, "Confirmed array geometry unavailable");
  missing(appliedGains && gains.size() == metadata.channels, "Applied sample-aligned gain readback unavailable");
  missing(!doc["receiverDetails"].ObjectEmpty(), "Receiver identity/details unavailable");
  missing(acquisition::sha256(context.configurationSha256) && acquisition::sha256(context.codeSha256),
    "Configuration/code SHA-256 identity unavailable");
  return runs;
}
}

const std::vector<double>& RecordingProvenance::requested_gains_for_manifold() const {
  if (!acquisitionIntegrityValid || !acquisitionHashTrusted ||
      acquisitionContext.requestedChannelGainDb.empty())
    throw std::runtime_error("Measured replay manifold requires authenticated acquisition-time requested gains; applied gains cannot substitute");
  return acquisitionContext.requestedChannelGainDb;
}

struct RecordingWriter::Impl {
  struct Batch {
    std::unique_ptr<uint8_t[]> bytes;
    std::size_t size=0;
  };
  File file;
  RecordingMetadata metadata;
  bool closed=false;
  std::mutex mutex;
  std::condition_variable ready;
  std::deque<Batch> queue;
  Batch pending;
  // Capacity, including the producer's partial batch and the in-flight write,
  // is charged before copying. Padding cannot exceed the 64 MiB memory bound.
  std::size_t queuedBytes=0;
  bool stopping=false, aborted=false, inFlight=false;
  std::string writeError;
  RecordingFileCache<> cache;
  uint64_t writtenBytes=0;
  std::thread worker;
  Impl(const std::string& path,RecordingMetadata m):file(path,true),metadata(std::move(m)) {
    valid(metadata); metadata.samples=0;
    RecordingFileCacheIo::prepare(file.fd);
    std::array<uint8_t,64> header{}; std::copy(magic.begin(),magic.end(),header.begin());
    const bool acquisition = !metadata.acquisitionJson.empty();
    if (metadata.acquisitionJson.size() > acquisition::maximumJsonBytes)
      fail("Recording acquisition metadata exceeds bound");
    put32(header.data()+8,acquisition ? 2 : 1); put32(header.data()+12,acquisition ? 128 : 64); put32(header.data()+16,metadata.channels);
    put32(header.data()+20,metadata.sampleRate); put32(header.data()+24,metadata.frequency);
    put32(header.data()+28,1); // IEEE float32, little endian, channel-major blocks.
    if (acquisition) put32(header.data()+32,metadata.acquisitionJson.size());
    file.write(header.data(),header.size());
    if (acquisition) {
      const auto hash = acquisition::digest(metadata.acquisitionJson);
      file.write(hash.data(), hash.size());
      file.write(metadata.acquisitionJson.data(), metadata.acquisitionJson.size());
    }
    writtenBytes=file.position();
    worker=std::thread([this] {
      for (;;) {
        Batch payload;
        {
          std::unique_lock<std::mutex> lock(mutex);
          ready.wait(lock,[&] { return stopping || !queue.empty(); });
          if(queue.empty()) {
            if(stopping) return;
            continue;
          }
          payload=std::move(queue.front());
          queue.pop_front();
          inFlight=true;
        }
        try {
          file.write(payload.bytes.get(),payload.size);
          writtenBytes+=payload.size;
          cache.written(file.fd,writtenBytes);
        }
        catch(const std::exception& error) {
          std::lock_guard<std::mutex> lock(mutex);
          writeError=error.what();
          queuedBytes-=recordingBatchBytes*(queue.size()+1);
          queue.clear();
          inFlight=false;
          stopping=aborted=true;
          ready.notify_all();
          return;
        }
        payload.bytes.reset();
        {
          std::lock_guard<std::mutex> lock(mutex);
          queuedBytes-=recordingBatchBytes;
          inFlight=false;
        }
      }
    });
  }
  ~Impl() {
    abort();
    if(worker.joinable()) worker.join();
  }
  void abort() {
    std::lock_guard<std::mutex> lock(mutex);
    stopping=aborted=true;
    queue.clear(); pending={};
    queuedBytes=inFlight ? recordingBatchBytes : 0;
    ready.notify_all();
  }
  void check_running() {
    if(!writeError.empty()) fail(writeError);
    if(stopping) fail("Recording writer is stopped");
  }
  void publish_locked() {
    if(!pending.size) return;
    queue.push_back(std::move(pending)); pending={};
    if(queue.size()==1) ready.notify_one();
  }
  void copy(const void* source,std::size_t bytes) {
    const auto* cursor=static_cast<const uint8_t*>(source);
    while(bytes) {
      // No value initialization: every published byte is explicitly copied.
      if(!pending.bytes) pending.bytes.reset(new uint8_t[recordingBatchBytes]);
      const auto count=std::min(bytes,recordingBatchBytes-pending.size);
      std::memcpy(pending.bytes.get()+pending.size,cursor,count);
      pending.size+=count; cursor+=count; bytes-=count;
      if(pending.size==recordingBatchBytes) {
        std::lock_guard<std::mutex> lock(mutex);
        check_running(); publish_locked();
      }
    }
  }
  void append(const IqBlock& block,std::size_t offset,std::size_t count) {
    try {
      const auto bytes=16+count*metadata.channels*8;
      const auto additional=(pending.size+bytes+recordingBatchBytes-1)/recordingBatchBytes-
        (pending.bytes ? 1 : 0);
      {
        std::lock_guard<std::mutex> lock(mutex);
        check_running();
        if(additional>maxQueuedRecordingBytes/recordingBatchBytes ||
            queuedBytes>maxQueuedRecordingBytes-additional*recordingBatchBytes)
          fail("Recording disk cannot keep up; bounded write queue is full");
        queuedBytes+=additional*recordingBatchBytes;
      }
      std::array<uint8_t,16> header{};
      put32(header.data(),count); put64(header.data()+8,metadata.samples);
      copy(header.data(),header.size());
      if(native_float_iq_is_little_endian()) {
        for(const auto& channel:block)
          copy(channel.data()+offset,count*sizeof(std::complex<float>));
      } else {
        // Conversion preserves the identical format on big-endian hosts.
        for(const auto& channel:block)
          for(std::size_t i=offset;i<offset+count;++i) {
            std::array<uint8_t,8> sample{};
            putFloat(sample.data(),channel[i].real());
            putFloat(sample.data()+4,channel[i].imag());
            copy(sample.data(),sample.size());
          }
      }
      std::lock_guard<std::mutex> lock(mutex);
      check_running();
      metadata.samples+=count;
    } catch(...) { abort(); throw; }
  }
  void drain() {
    {
      std::lock_guard<std::mutex> lock(mutex);
      if(!aborted) publish_locked();
      stopping=true;
    }
    ready.notify_all();
    if(worker.joinable()) worker.join();
    std::lock_guard<std::mutex> lock(mutex);
    if(!writeError.empty()) fail(writeError);
    if(aborted) fail("Recording writer was aborted");
    if(!queue.empty() || queuedBytes) fail("Recording write queue did not drain");
  }
};
RecordingWriter::RecordingWriter(const std::string& path,RecordingMetadata m) {
  // Do this before O_EXCL creation: a configuration error must not leave a
  // misleading empty recording behind.
  valid(m);
  impl_=std::make_unique<Impl>(path,std::move(m));
}
RecordingWriter::~RecordingWriter()=default; // Unclean shutdown must not mark a file complete.
uint64_t RecordingWriter::samples() const { return impl_->metadata.samples; }
void RecordingWriter::abort() { impl_->abort(); }
bool RecordingWriter::abort_ready() const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->aborted && !impl_->inFlight;
}
void RecordingWriter::append(const IqBlock& block) {
  if(block.empty()) fail("Invalid recording block dimensions");
  const size_t count=block.front().size();
  for(const auto& channel:block)
    if(channel.size()!=count) fail("Recording channels are not aligned");
  append_slice(block,0,count);
}
void RecordingWriter::append_slice(const IqBlock& block,size_t offset,
    size_t requestedCount) {
  auto& s=*impl_;
  if(s.closed || block.size()!=s.metadata.channels || !requestedCount ||
      requestedCount>maxBlock || offset>block.front().size() ||
      requestedCount>block.front().size()-offset)
    fail("Invalid recording block dimensions");
  for(const auto& channel:block)
    if(offset>channel.size() || requestedCount>channel.size()-offset)
      fail("Recording channels are not aligned");
  finite_slice(block,offset,requestedCount);
  s.append(block,offset,requestedCount);
}
void RecordingWriter::close() {
  auto& s=*impl_; if(s.closed) return;
  s.drain();
  if(fsync(s.file.fd)) fail("Cannot flush recording to disk");
  std::array<uint8_t,16> complete{}; put64(complete.data(),s.metadata.samples); put64(complete.data()+8,1);
  s.file.seek(40); s.file.write(complete.data(),complete.size());
  if(fsync(s.file.fd)) fail("Cannot finalize recording");
  RecordingFileCache<>::durable(s.file.fd,s.writtenBytes);
  s.closed=true;
}

struct RecordingReader::Impl {
  struct Segment { uint64_t offset=0, sample=0; uint32_t count=0; };
  File file;
  ReplayOptions options;
  RecordingMetadata metadata;
  RecordingProvenance provenance;
  uint64_t fileSize=0,start=0,position=0;
  uint32_t legacySize=0;
  uint32_t skipSamples=0;
  std::vector<Segment> segments;
  HeimdallFrame::Header firstMchq{};
  bool allMchqFramesTimed=false;
  Impl(const std::string& path,ReplayOptions o):file(path),options(std::move(o)) {
    fileSize=file.size(); if(fileSize<8) fail("Replay file is empty or too short");
    std::array<uint8_t,8> prefix{}; file.read(prefix.data(),prefix.size()); file.seek(0);
    std::string format=options.format;
    if(format=="auto") {
      if(prefix==magic) format="blah2";
      else if(std::equal(prefix.begin(),prefix.begin()+4,"MCHQ") ||
              std::equal(prefix.begin(),prefix.begin()+4,"MCHT")) format="mchq";
      else if(options.receiver=="RspDuo") format="s16-interleaved";
      else if(options.receiver=="Usrp" && options.legacyBlockSamples) format="usrp-blocks";
      else fail("File format is unknown. Select its format; old USRP files also need the original block size");
    }
    metadata={options.channels,options.sampleRate,options.frequency,0,format};
    if(format=="blah2") {
      std::array<uint8_t,64> h{}; file.read(h.data(),h.size());
      const bool embedded = u32(h.data()+8) == 2 && u32(h.data()+12) == 128;
      if(!std::equal(magic.begin(),magic.end(),h.begin()) ||
          (!embedded && (u32(h.data()+8)!=1 || u32(h.data()+12)!=64)) || u32(h.data()+28)!=1)
        fail("Recording header/version is unsupported");
      metadata.channels=u32(h.data()+16); metadata.sampleRate=u32(h.data()+20); metadata.frequency=u32(h.data()+24);
      valid(metadata);
      if(u64(h.data()+48)!=1) fail("Recording was not finished cleanly; choose a completed recording");
      const uint64_t expected=u64(h.data()+40); start=64;
      if (embedded) {
        const auto length = u32(h.data()+32);
        if (!length || length > acquisition::maximumJsonBytes || uint64_t(length) + 128 > fileSize)
          fail("Recording embedded acquisition metadata has invalid size");
        std::array<char,64> digest{}; file.read(digest.data(),digest.size());
        metadata.acquisitionJson.resize(length); file.read(metadata.acquisitionJson.data(),length);
        if (acquisition::digest(metadata.acquisitionJson) != std::string(digest.data(),digest.size()))
          fail("Recording embedded acquisition SHA-256 is invalid");
        start = 128 + length;
      }
      while(file.position()<fileSize) {
        const auto offset=file.position();
        std::array<uint8_t,16> b{}; file.read(b.data(),b.size()); const auto n=u32(b.data());
        if(!n || n>maxBlock || u32(b.data()+4) || u64(b.data()+8)!=metadata.samples)
          fail("Recording block sequence is invalid");
        segments.push_back({offset,metadata.samples,n});
        skip(uint64_t(n)*metadata.channels*8); metadata.samples+=n;
      }
      if(expected!=metadata.samples) fail("Recording sample count does not match its header");
    } else if(format=="mchq") {
      metadata.sampleRate=2400000;
      bool firstFrame=true;
      HeimdallFrame::Header previous{};
      allMchqFramesTimed=true;
      while(file.position()<fileSize) {
        const auto offset=file.position();
        auto h=mchqHeader();
        if(!metadata.samples) {
          firstMchq=h; metadata.channels=h.numChannels;
          metadata.frequency=static_cast<uint32_t>(std::llround(h.frequencies[0]));
        }
        if(!h.timing.available) allMchqFramesTimed=false;
        if(!firstFrame && previous.timing.available && h.timing.available &&
            !HeimdallFrame::timing_contiguous(previous,h))
          fail("Kraken recording timing is discontinuous");
        previous=h; firstFrame=false;
        checkMchq(h); segments.push_back({offset,metadata.samples,h.samplesPerChannel});
        skip(HeimdallFrame::payload_size(h)); metadata.samples+=h.samplesPerChannel;
      }
    } else if(format=="s16-interleaved" || format=="s8-interleaved" || format=="usrp-blocks") {
      if(options.channels!=2) fail("Legacy raw recordings require two channels");
      legacySize=format=="s16-interleaved" ? 8 : format=="s8-interleaved" ? 4 : 16;
      if(fileSize%legacySize) fail("Recording ends in an incomplete I/Q sample");
      if(format=="usrp-blocks" && (!options.legacyBlockSamples || options.legacyBlockSamples>maxBlock ||
          fileSize%(uint64_t(options.legacyBlockSamples)*16)))
        fail("Old USRP recording needs its original block size and complete blocks");
      metadata.samples=fileSize/legacySize;
    } else fail("Unsupported replay file format");
    valid(metadata);
    if(!metadata.samples) fail("Recording contains no samples");
    if(metadata.channels!=options.channels) fail("Recording channel count differs from Settings");
    if(metadata.sampleRate!=options.sampleRate) fail("Recording sample rate differs from Settings");
    const uint64_t delta=metadata.frequency>options.frequency ? metadata.frequency-options.frequency : options.frequency-metadata.frequency;
    if(delta>(format=="mchq" ? 256U : 0U)) fail("Recording frequency differs from Settings");
    provenance=read_provenance(path,options,metadata);
    if(format=="blah2") read_portable_sidecar(path,metadata,fileSize,file,provenance);
    if(format=="mchq") {
      provenance.perChannelGainDb.assign(firstMchq.gains.begin(),firstMchq.gains.end());
      read_native_mchq_sidecar(path,metadata,fileSize,provenance);
      if(allMchqFramesTimed && firstMchq.timing.available) {
        provenance.epoch=firstMchq.timing.epoch;
        provenance.sourceFirstSample=firstMchq.timing.firstSample;
        provenance.sourceSampleAvailable=true;
      }
    }
    const bool embeddedAcquisition = !metadata.acquisitionJson.empty();
    std::string manifestDataHash;
    const auto manifestJson = read_acquisition_manifest(path,provenance,manifestDataHash);
    if (!manifestJson.empty()) {
      if (embeddedAcquisition && manifestJson != metadata.acquisitionJson)
        fail("Embedded acquisition metadata conflicts with manifest");
      metadata.acquisitionJson = manifestJson;
    }
    const bool acquisitionRuns = read_acquisition(metadata, provenance);
    // The v2 contract binds every entry to stored callback boundaries, even
    // when all repeat values are omitted or one. Preserve v1 reader semantics.
    if (format == "blah2" && acquisitionRuns) {
      size_t segment = 0;
      for (const auto& frame : provenance.frames) {
        if (frame.repeat > segments.size() - segment)
          fail("Portable acquisition frame run exceeds stored blocks");
        for (uint64_t index = 0; index < frame.repeat; ++index)
          if (segments[segment++].count != frame.count)
            fail("Portable acquisition frame run boundary mismatch");
      }
      if (segment != segments.size()) fail("Portable acquisition lineage does not cover stored blocks");
    }
    if (format == "mchq" && !provenance.frames.empty()) {
      if (provenance.frames.size() != segments.size()) fail("Native acquisition lineage does not match stored frames");
      for (size_t index = 0; index < segments.size(); ++index)
        if (provenance.frames[index].count != segments[index].count)
          fail("Native acquisition lineage frame boundary mismatch");
    }
    if (!options.expectedRecordingSha256.empty() || !metadata.acquisitionJson.empty() || !manifestDataHash.empty()) {
      if (provenance.recordingSha256.empty()) {
        const auto integrity = acquisition::fileDigest(file.fd);
        if (integrity.first != fileSize) fail("Recording changed while verifying acquisition integrity");
        provenance.recordingSha256 = integrity.second;
      }
      if (!options.expectedRecordingSha256.empty() &&
          (!acquisition::sha256(options.expectedRecordingSha256) ||
           options.expectedRecordingSha256 != provenance.recordingSha256))
        fail("Recording SHA-256 differs from trusted replay acquisition identity");
      if (!manifestDataHash.empty() && manifestDataHash != provenance.recordingSha256)
        fail("Acquisition manifest is bound to different recording bytes");
      if (!options.expectedAcquisitionSha256.empty() &&
          (!acquisition::sha256(options.expectedAcquisitionSha256) ||
           options.expectedAcquisitionSha256 != provenance.acquisitionSha256))
        fail("Acquisition SHA-256 differs from trusted replay metadata identity");
      provenance.acquisitionHashTrusted = !options.expectedRecordingSha256.empty() &&
        (embeddedAcquisition || !options.expectedAcquisitionSha256.empty());
    }
    provenance.geographicSitesAvailable = provenance.acquisitionIntegrityValid &&
      provenance.acquisitionHashTrusted && provenance.acquisitionContext.geographicSitesJson != "null";
    if (provenance.geographicSitesAvailable)
      provenance.geographicSitesReason = "Authenticated operator-configured acquisition sites; not surveyed or hardware-attested";
    else if (provenance.acquisitionContext.geographicSitesJson != "null")
      provenance.geographicSitesReason = "Recorded geographic sites lack externally trusted acquisition hashes; replay geography disabled";
    if (options.requireExactAcquisition &&
        (!provenance.exactAcquisition || !provenance.acquisitionHashTrusted))
      fail("Exact acquisition replay requires trusted IQ and acquisition SHA-256 identities, applied sample-aligned gains, topology, geometry and lineage");
    if (options.requireExactAcquisition && options.validateAcquisitionContext) {
      const auto& recorded = provenance.acquisitionContext;
      const auto& configured = options.expectedAcquisitionContext;
      // Acquisition identities constrain the explicit replay configuration;
      // they never select receiver-specific radar algorithms. Full YAML/code
      // hashes identify the recording, but cannot equal a replay config whose
      // input path, acceleration selection or qualification controls differ.
      if (recorded.referenceMode != configured.referenceMode ||
          recorded.referenceChannels != configured.referenceChannels ||
          recorded.surveillanceChannels != configured.surveillanceChannels ||
          recorded.coherent != configured.coherent ||
          (!configured.requestedChannelGainDb.empty() &&
           recorded.requestedChannelGainDb != configured.requestedChannelGainDb) ||
          recorded.geometryConfirmed != configured.geometryConfirmed ||
          recorded.geometryJson != configured.geometryJson ||
          recorded.manifoldSha256 != configured.manifoldSha256)
        fail("Exact acquisition replay configuration differs from recorded channel roles, requested gains, coherence, geometry or manifold identities");
    }
    if(options.provenanceMode!="legacy-unqualified" && options.provenanceMode!="decision-grade")
      fail("Replay provenance mode must be legacy-unqualified or decision-grade");
    if(options.provenanceMode=="decision-grade" && !provenance.decisionGrade)
      fail("Decision-grade replay requires exact, bounded v2 RF-time provenance");
    file.seek(start);
  }
  void skip(uint64_t bytes) { const auto p=file.position(); if(p>fileSize || bytes>fileSize-p) fail("Recording is truncated"); file.seek(p+bytes); }
  HeimdallFrame::Header mchqHeader() {
    std::array<uint8_t,32> bytes{}; file.read(bytes.data(),bytes.size()); auto h=HeimdallFrame::decode_header(bytes);
    std::vector<uint8_t> values(HeimdallFrame::metadata_size(h)); file.read(values.data(),values.size());
    HeimdallFrame::decode_metadata(h,values);
    if(!HeimdallFrame::is_synchronized_data(h)) fail("Kraken recording contains calibration or retuning data");
    for(auto f:h.frequencies) if(!std::isfinite(f) || f<=0 || double(f)>UINT32_MAX) fail("Invalid recording frequency");
    for(auto f:h.frequencies) if(std::abs(double(f)-options.frequency)>256) fail("Recording frequency differs from Settings");
    for(auto g:h.gains) if(!std::isfinite(g)) fail("Invalid recording gain metadata");
    return h;
  }
  void checkMchq(const HeimdallFrame::Header& h) {
    if(h.numChannels!=firstMchq.numChannels || h.frequencies!=firstMchq.frequencies || h.gains!=firstMchq.gains ||
        h.frequencyChangeCounter!=firstMchq.frequencyChangeCounter || h.currentGroupIndex!=firstMchq.currentGroupIndex)
      fail("Kraken recording changes channel count, tuning or gain");
  }
};
RecordingReader::RecordingReader(const std::string& path,ReplayOptions o):impl_(std::make_unique<Impl>(path,std::move(o))) {}
RecordingReader::~RecordingReader()=default;
const RecordingMetadata& RecordingReader::metadata() const { return impl_->metadata; }
const RecordingProvenance& RecordingReader::provenance() const { return impl_->provenance; }
void RecordingReader::rewind() { impl_->file.seek(impl_->start); impl_->position=0; impl_->skipSamples=0; }
void RecordingReader::seek(uint64_t sample) {
  auto& s=*impl_;
  if(sample>s.metadata.samples) fail("Replay seek exceeds recording length");
  s.skipSamples=0;
  if(sample==s.metadata.samples){s.file.seek(s.fileSize);s.position=sample;return;}
  if(s.metadata.format=="s16-interleaved"||s.metadata.format=="s8-interleaved") {
    s.file.seek(sample*s.legacySize);s.position=sample;return;
  }
  if(s.metadata.format=="usrp-blocks") {
    const uint64_t first=(sample/s.options.legacyBlockSamples)*s.options.legacyBlockSamples;
    s.file.seek(first*s.legacySize);s.position=first;
    s.skipSamples=uint32_t(sample-first);return;
  }
  auto it=std::upper_bound(s.segments.begin(),s.segments.end(),sample,
    [](uint64_t value,const Impl::Segment& segment){return value<segment.sample;});
  if(it!=s.segments.begin())--it;
  if(it==s.segments.end()||sample<it->sample||sample>=it->sample+it->count)
    fail("Replay seek index is invalid");
  s.file.seek(it->offset);s.position=it->sample;s.skipSamples=uint32_t(sample-it->sample);
}
bool RecordingReader::read(IqBlock& block) {
  auto& s=*impl_; if(s.position==s.metadata.samples) { block.clear(); return false; }
  uint32_t count=0;
  if(s.metadata.format=="mchq") {
    auto h=s.mchqHeader(); s.checkMchq(h); count=h.samplesPerChannel;
    if(s.provenance.sourceSampleAvailable && h.timing.available &&
        (h.timing.epoch!=s.provenance.epoch ||
         h.timing.firstSample!=s.provenance.sourceFirstSample+s.position))
      fail("Kraken replay sample provenance changed during playback");
    std::vector<uint8_t> payload(HeimdallFrame::payload_size(h)); s.file.read(payload.data(),payload.size());
    if (!s.provenance.frames.empty()) {
      const auto segment = std::lower_bound(s.segments.begin(), s.segments.end(), s.position,
        [](const Impl::Segment& value, uint64_t sample) { return value.sample < sample; });
      if (segment == s.segments.end() || segment->sample != s.position)
        fail("Native acquisition frame position mismatch");
      block=HeimdallFrame::decode_payload(h,payload,
        s.provenance.frames[std::distance(s.segments.begin(),segment)].originalMeanIq);
    } else block=HeimdallFrame::decode_payload(h,payload);
  } else {
    if(s.metadata.format=="blah2") {
      std::array<uint8_t,16> h{}; s.file.read(h.data(),h.size()); count=u32(h.data());
      if(!count || count>maxBlock || u64(h.data()+8)!=s.position || u32(h.data()+4)) fail("Recording changed during playback");
    } else count=s.metadata.format=="usrp-blocks" ? s.options.legacyBlockSamples : std::min<uint64_t>(16384,s.metadata.samples-s.position);
    const unsigned bytesPerSample=s.metadata.format=="blah2" ? s.metadata.channels*8 : s.legacySize;
    std::vector<uint8_t> payload(size_t(count)*bytesPerSample); s.file.read(payload.data(),payload.size());
    block.assign(s.metadata.channels,std::vector<std::complex<float>>(count));
    const bool floatBlock=s.metadata.format=="blah2" || s.metadata.format=="usrp-blocks";
    for(unsigned ch=0;ch<s.metadata.channels;++ch) for(unsigned i=0;i<count;++i) {
      if(floatBlock) { const auto* p=payload.data()+(size_t(ch)*count+i)*8; block[ch][i]={floating(p),floating(p+4)}; }
      else if(s.legacySize==8) {
        const auto* p=payload.data()+size_t(i)*8+ch*4;
        const auto component=[](const uint8_t* p) { const uint16_t bits=uint16_t(p[0])|uint16_t(p[1])<<8; return float(bits<32768 ? int(bits) : int(bits)-65536); };
        block[ch][i]={component(p),component(p+2)};
      } else {
        const auto* p=payload.data()+size_t(i)*4+ch*2;
        const auto component=[](uint8_t v) { return float(v<128 ? int(v) : int(v)-256); };
        block[ch][i]={component(p[0]),component(p[1])};
      }
    }
  }
  if(count>s.metadata.samples-s.position) fail("Recording length changed during playback");
  finite(block); s.position+=count;
  if(s.skipSamples) {
    const uint32_t discard=std::min(s.skipSamples,count);s.skipSamples-=discard;
    for(auto& channel:block)channel.erase(channel.begin(),channel.begin()+discard);
    if(block.front().empty())return read(block);
  }
  return true;
}
}
