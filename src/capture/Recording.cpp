#include "Recording.h"
#include "kraken/HeimdallFrame.h"
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <stdexcept>
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
  if(result.receiver!=options.receiver) fail("Recording provenance receiver differs from Settings");
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
}

struct RecordingWriter::Impl {
  File file;
  RecordingMetadata metadata;
  bool closed=false;
  Impl(const std::string& path,RecordingMetadata m):file(path,true),metadata(std::move(m)) {
    valid(metadata); metadata.samples=0;
    std::array<uint8_t,64> header{}; std::copy(magic.begin(),magic.end(),header.begin());
    put32(header.data()+8,1); put32(header.data()+12,64); put32(header.data()+16,metadata.channels);
    put32(header.data()+20,metadata.sampleRate); put32(header.data()+24,metadata.frequency);
    put32(header.data()+28,1); // IEEE float32, little endian, channel-major blocks.
    file.write(header.data(),header.size());
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
void RecordingWriter::append(const IqBlock& block) {
  auto& s=*impl_;
  if(s.closed || block.size()!=s.metadata.channels || block.front().empty() || block.front().size()>maxBlock)
    fail("Invalid recording block dimensions");
  const uint32_t count=block.front().size();
  for(const auto& channel:block) if(channel.size()!=count) fail("Recording channels are not aligned");
  finite(block);
  std::array<uint8_t,16> header{}; put32(header.data(),count); put64(header.data()+8,s.metadata.samples);
  std::vector<uint8_t> payload(size_t(count)*s.metadata.channels*8);
  auto* p=payload.data();
  for(const auto& channel:block) for(auto value:channel) { putFloat(p,value.real()); putFloat(p+4,value.imag()); p+=8; }
  s.file.write(header.data(),header.size()); s.file.write(payload.data(),payload.size()); s.metadata.samples+=count;
}
void RecordingWriter::close() {
  auto& s=*impl_; if(s.closed) return;
  if(fsync(s.file.fd)) fail("Cannot flush recording to disk");
  std::array<uint8_t,16> complete{}; put64(complete.data(),s.metadata.samples); put64(complete.data()+8,1);
  s.file.seek(40); s.file.write(complete.data(),complete.size());
  if(fsync(s.file.fd)) fail("Cannot finalize recording");
  s.closed=true;
}

struct RecordingReader::Impl {
  File file;
  ReplayOptions options;
  RecordingMetadata metadata;
  RecordingProvenance provenance;
  uint64_t fileSize=0,start=0,position=0;
  uint32_t legacySize=0;
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
      if(!std::equal(magic.begin(),magic.end(),h.begin()) || u32(h.data()+8)!=1 || u32(h.data()+12)!=64 || u32(h.data()+28)!=1)
        fail("Recording header/version is unsupported");
      metadata.channels=u32(h.data()+16); metadata.sampleRate=u32(h.data()+20); metadata.frequency=u32(h.data()+24);
      valid(metadata);
      if(u64(h.data()+48)!=1) fail("Recording was not finished cleanly; choose a completed recording");
      const uint64_t expected=u64(h.data()+40); start=64;
      while(file.position()<fileSize) {
        std::array<uint8_t,16> b{}; file.read(b.data(),b.size()); const auto n=u32(b.data());
        if(!n || n>maxBlock || u32(b.data()+4) || u64(b.data()+8)!=metadata.samples)
          fail("Recording block sequence is invalid");
        skip(uint64_t(n)*metadata.channels*8); metadata.samples+=n;
      }
      if(expected!=metadata.samples) fail("Recording sample count does not match its header");
    } else if(format=="mchq") {
      metadata.sampleRate=2400000;
      bool firstFrame=true;
      HeimdallFrame::Header previous{};
      allMchqFramesTimed=true;
      while(file.position()<fileSize) {
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
        checkMchq(h); skip(HeimdallFrame::payload_size(h)); metadata.samples+=h.samplesPerChannel;
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
    if(format=="mchq") {
      provenance.perChannelGainDb.assign(firstMchq.gains.begin(),firstMchq.gains.end());
      read_native_mchq_sidecar(path,metadata,fileSize,provenance);
      // Native MCHT clips retain their original coherent sample epoch in every
      // frame. This is relative sample provenance, not exact UTC and not a v2
      // decision-grade recording contract. Preserve it so offline replay can
      // join processor output to the separately authenticated clip sidecar.
      if(allMchqFramesTimed && firstMchq.timing.available) {
        provenance.epoch=firstMchq.timing.epoch;
        provenance.sourceFirstSample=firstMchq.timing.firstSample;
        provenance.sourceSampleAvailable=true;
      }
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
void RecordingReader::rewind() { impl_->file.seek(impl_->start); impl_->position=0; }
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
    block=HeimdallFrame::decode_payload(h,payload);
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
  finite(block); s.position+=count; return true;
}
}
