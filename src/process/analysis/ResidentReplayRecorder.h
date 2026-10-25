#pragma once
#include "ResidentReplaySnapshot.h"
#include "process/ambiguity/GpuResidentBackend.h"
#include <cerrno>
#include <fcntl.h>
#include <limits>
#include <stdexcept>
#include <string>
#include <unistd.h>

namespace blah2::resident_replay {
// Replay-only file sink. Bounds include all selected frames, not just each
// individual snapshot. Existing artifacts are never overwritten.
struct RecorderConfig {
 bool enabled=false;
 std::string prefix;
 uint64_t firstFrame=0,lastFrame=0,maxTotalBytes=0,sourceEpoch=0,
  firstEpochSample=0,endEpochSample=0;
};
inline Hash parseHash(const std::string& text){
 if(text.size()!=64)throw std::invalid_argument("Replay digest needs 64 lowercase hex digits");
 Hash value{};for(size_t i=0;i<64;++i){const char c=text[i];
  if(!((c>='0'&&c<='9')||(c>='a'&&c<='f')))throw std::invalid_argument("Invalid replay digest");
  value[i/2]|=uint8_t((c<='9'?c-'0':c-'a'+10)<<(i%2?0:4));}
 return value;
}
inline void validateRecorder(const RecorderConfig& r,const resident::Config& c,bool replay,bool resident){
 if(!r.enabled)return;
 const auto n=c.geometry.clutterSamples;
 if(!replay||!resident||r.prefix.empty()||r.prefix.front()!='/'||!r.firstFrame||
    r.lastFrame<r.firstFrame||r.lastFrame-r.firstFrame+1!=c.replay.maxFrames||
    !n||!r.sourceEpoch||r.endEpochSample<=r.firstEpochSample||
    r.lastFrame>(r.endEpochSample-r.firstEpochSample)/n||
    !r.maxTotalBytes||r.maxTotalBytes>(2ULL<<30)||
    requiredBytes(c)>r.maxTotalBytes/c.replay.maxFrames)
  throw std::invalid_argument("Native snapshot recorder requires bounded resident file replay and source sample identity");
 validateExportConfig(c);
}
inline void checkSource(const RecorderConfig& r,const resident::Config& c,uint64_t frame,
 bool available,uint64_t epoch,uint64_t first,uint64_t end,const std::vector<double>& gains){
 if(!r.enabled)return;
 if(!frame||frame>(r.endEpochSample-r.firstEpochSample)/c.geometry.clutterSamples||
    !available||epoch!=r.sourceEpoch||
    first!=r.firstEpochSample+(frame-1)*c.geometry.clutterSamples||
    end-first!=c.geometry.clutterSamples||gains!=c.replay.actualGainDb)
  throw std::runtime_error("Native snapshot source sample/gain identity differs from admitted recording");
}
inline void writeExclusive(const std::string& path,const std::vector<uint8_t>& bytes){
 const int fd=::open(path.c_str(),O_WRONLY|O_CREAT|O_EXCL,0600);
 if(fd<0)throw std::runtime_error("Cannot exclusively create native snapshot artifact");
 size_t at=0;while(at<bytes.size()){
  const auto written=::write(fd,bytes.data()+at,bytes.size()-at);
  if(written<0&&errno==EINTR)continue;
  if(written<=0){::close(fd);throw std::runtime_error("Native snapshot artifact write failed");}
  at+=size_t(written);
 }
 if(::close(fd))throw std::runtime_error("Native snapshot artifact close failed");
}
}
