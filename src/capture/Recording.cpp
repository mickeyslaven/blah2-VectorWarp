#include "Recording.h"
#include "kraken/HeimdallFrame.h"
#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <fcntl.h>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

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
  uint64_t fileSize=0,start=0,position=0;
  uint32_t legacySize=0;
  HeimdallFrame::Header firstMchq{};
  Impl(const std::string& path,ReplayOptions o):file(path),options(std::move(o)) {
    fileSize=file.size(); if(fileSize<8) fail("Replay file is empty or too short");
    std::array<uint8_t,8> prefix{}; file.read(prefix.data(),prefix.size()); file.seek(0);
    std::string format=options.format;
    if(format=="auto") {
      if(prefix==magic) format="blah2";
      else if(std::equal(prefix.begin(),prefix.begin()+4,"MCHQ")) format="mchq";
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
      while(file.position()<fileSize) {
        auto h=mchqHeader();
        if(!metadata.samples) {
          firstMchq=h; metadata.channels=h.numChannels;
          metadata.frequency=static_cast<uint32_t>(std::llround(h.frequencies[0]));
        }
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
void RecordingReader::rewind() { impl_->file.seek(impl_->start); impl_->position=0; }
bool RecordingReader::read(IqBlock& block) {
  auto& s=*impl_; if(s.position==s.metadata.samples) { block.clear(); return false; }
  uint32_t count=0;
  if(s.metadata.format=="mchq") {
    auto h=s.mchqHeader(); s.checkMchq(h); count=h.samplesPerChannel;
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
