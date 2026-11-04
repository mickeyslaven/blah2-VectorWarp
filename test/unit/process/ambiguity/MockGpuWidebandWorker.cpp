#include "process/ambiguity/GpuWidebandProcess.h"
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <unistd.h>
#include <sys/socket.h>

namespace {
constexpr uint32_t magic=0x42574701;
struct Header{uint32_t tag=magic,inputRate=0,channels=0,maxInput=0,lanes=0;uint64_t bytes=0;char device[96]{};};
struct LaneWire{uint64_t frequency=0;uint32_t decimation=0,taps=0;double center=0;};
struct Frame{uint32_t tag=magic,op=1,samples=0,reserved=0;uint64_t sequence=0,first=0;};
struct Reply{uint32_t tag=magic,status=0,lanes=0;uint64_t sequence=0;uint32_t samples[blah2::wideband_gpu::maxLanes]{};uint64_t first[blah2::wideband_gpu::maxLanes]{};uint8_t has[blah2::wideband_gpu::maxLanes]{};char reason[256]{};char device[256]{};};
void readAll(void* data,size_t bytes){auto*p=static_cast<char*>(data);while(bytes){auto n=read(3,p,bytes);if(n<=0)_exit(9);p+=n;bytes-=size_t(n);}}
int customWorker(const std::string& mode){Header h;readAll(&h,sizeof(h));std::vector<uint32_t> decimation;for(uint32_t i=0;i<h.lanes;++i){LaneWire w;readAll(&w,sizeof(w));decimation.push_back(w.decimation);std::vector<double> taps(w.taps);readAll(taps.data(),taps.size()*sizeof(double));}Reply ready;ready.lanes=h.lanes;std::strcpy(ready.device,"mock-custom");send(3,&ready,sizeof(ready),0);Frame frame;readAll(&frame,sizeof(frame));Reply response;response.sequence=frame.sequence;response.lanes=h.lanes;for(uint32_t lane=0;lane<h.lanes;++lane){const uint32_t remainder=uint32_t(frame.first%decimation[lane]),first=remainder?decimation[lane]-remainder:0;response.samples[lane]=first<frame.samples?1+(frame.samples-1-first)/decimation[lane]:0;response.has[lane]=response.samples[lane]!=0;response.first[lane]=response.has[lane]?frame.first+first:0;}if(mode=="partial"){send(3,&response,sizeof(response)/2,0);std::this_thread::sleep_for(std::chrono::milliseconds(250));return 0;}if(mode=="malformed-count")++response.samples[0];else if(mode=="malformed-first")++response.first[0];else if(mode=="malformed-has")response.has[0]=2;else if(mode=="malformed-total")response.samples[0]=UINT32_MAX;send(3,&response,sizeof(response),0);return 0;}
}

class Mock final:public blah2::wideband_gpu::Backend
{
  blah2::wideband_gpu::Config config_;
public:
  explicit Mock(blah2::wideband_gpu::Config c):config_(std::move(c)){}
  std::string deviceName()const override{return "mock-wideband";}
  void reset()override{}
  std::vector<blah2::wideband_gpu::LaneOutput> process(
    const std::vector<std::vector<std::complex<float>>>& input,uint64_t first)override{
    const char* mode=std::getenv("VW_MOCK_WIDEBAND_FAILURE");
    if(mode&&std::string(mode)=="hang")std::this_thread::sleep_for(std::chrono::milliseconds(250));
    if(mode&&std::string(mode)=="crash")_exit(7);
    std::vector<blah2::wideband_gpu::LaneOutput> result;
    for(const auto& lane:config_.lanes){blah2::wideband_gpu::LaneOutput out;out.frequencyHz=lane.frequencyHz;out.inputSamplesPerOutput=lane.decimation;out.channels.resize(input.size());for(size_t sample=0;sample<input.front().size();++sample)if((first+sample)%lane.decimation==0){if(!out.hasFirstInputSample){out.hasFirstInputSample=true;out.firstInputSample=first+sample;}for(size_t channel=0;channel<input.size();++channel)out.channels[channel].push_back(input[channel][sample]);}result.push_back(std::move(out));}return result;
  }
};
int main(){const char* raw=std::getenv("VW_MOCK_WIDEBAND_FAILURE");const std::string mode=raw?raw:"";if(mode=="startup-hang")std::this_thread::sleep_for(std::chrono::milliseconds(250));if(mode=="startup-crash")_exit(8);if(mode=="partial"||mode.rfind("malformed-",0)==0)return customWorker(mode);return blah2::wideband_gpu::runWorker([](const auto& c,const auto&){return std::make_unique<Mock>(c);});}
