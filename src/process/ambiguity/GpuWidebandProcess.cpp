#include "process/ambiguity/GpuWidebandProcess.h"
#include "process/utility/FixedExecutor.h"
#include <algorithm>
#include <chrono>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <stdexcept>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#include <thread>
extern char** environ;

namespace blah2::wideband_gpu
{
namespace
{
constexpr uint32_t magic=0x42574701;
struct Header{uint32_t tag=magic,inputRate=0,channels=0,maxInput=0,lanes=0;uint64_t bytes=0;char device[96]{};};
struct LaneWire{uint64_t frequency=0;uint32_t decimation=0,taps=0;double center=0;};
struct Frame{uint32_t tag=magic,op=1,samples=0,reserved=0;uint64_t sequence=0,first=0;};
struct Reply{uint32_t tag=magic,status=0,lanes=0;uint64_t sequence=0;uint32_t samples[maxLanes]{};uint64_t first[maxLanes]{};uint8_t has[maxLanes]{};char reason[256]{},device[256]{};};
void io(int fd,void* data,size_t bytes,bool writing){auto*p=static_cast<unsigned char*>(data);while(bytes){ssize_t n;if(writing)n=::send(fd,p,bytes,MSG_NOSIGNAL);else n=::read(fd,p,bytes);if(n<0&&errno==EINTR)continue;if(n<=0)throw std::runtime_error("Wideband GPU worker transport failed");p+=n;bytes-=size_t(n);}}
void receive(int fd,void* data,size_t bytes,unsigned timeout){auto*p=static_cast<unsigned char*>(data);const auto deadline=std::chrono::steady_clock::now()+std::chrono::milliseconds(timeout);while(bytes){const auto now=std::chrono::steady_clock::now();if(now>=deadline)throw std::runtime_error("Wideband GPU worker timed out or failed");const auto left=std::chrono::duration_cast<std::chrono::milliseconds>(deadline-now).count();pollfd descriptor{fd,POLLIN,0};int ready;do{ready=poll(&descriptor,1,int(std::max<int64_t>(1,std::min<int64_t>(left,2000000000))));}while(ready<0&&errno==EINTR);if(ready<=0||!(descriptor.revents&POLLIN))throw std::runtime_error("Wideband GPU worker timed out or failed");const ssize_t n=::read(fd,p,bytes);if(n<0&&errno==EINTR)continue;if(n<=0)throw std::runtime_error("Wideband GPU worker transport failed");p+=n;bytes-=size_t(n);}}
uint64_t inputElements(const Config&c){return uint64_t(c.channels)*c.maxInputSamples;}
size_t converterWorkers(const Config&c){return std::max<size_t>(1,c.channels);}
uint64_t mappingBytes(const Plan&p){return (inputElements(p.config())+p.layout().outputElements)*sizeof(std::complex<float>);}
void reap(pid_t child)noexcept{if(child<=0)return;int status=0;pid_t result;const auto deadline=std::chrono::steady_clock::now()+std::chrono::milliseconds(100);do{result=waitpid(child,&status,WNOHANG);if(result==child||(result<0&&errno!=EINTR))return;std::this_thread::sleep_for(std::chrono::milliseconds(1));}while(std::chrono::steady_clock::now()<deadline);kill(child,SIGKILL);try{std::thread([child]{int value;while(waitpid(child,&value,0)<0&&errno==EINTR){}}).detach();}catch(...){}}
class Process final:public Backend{
 Config config_;Plan plan_;GpuProcessOptions options_;utility::FixedExecutor converter_;Timing timing_;int socket_=-1,memory_=-1;pid_t child_=-1;std::complex<float>*shared_=nullptr;uint64_t bytes_=0,sequence_=0,pendingSequence_=0;bool pending_=false;Dispatch pendingDispatch_;std::chrono::steady_clock::time_point pendingBegin_{};std::string device_;
 void stop()noexcept{if(socket_>=0){Frame q;q.op=2;::send(socket_,&q,sizeof(q),MSG_NOSIGNAL);close(socket_);socket_=-1;}reap(child_);child_=-1;if(shared_){munmap(shared_,bytes_);shared_=nullptr;}if(memory_>=0){close(memory_);memory_=-1;}}
 std::string childState(){if(child_<=0)return " (worker already reaped)";int status=0;pid_t result=0;const auto deadline=std::chrono::steady_clock::now()+std::chrono::milliseconds(20);do{result=waitpid(child_,&status,WNOHANG);if(result==child_||result<0)break;std::this_thread::sleep_for(std::chrono::milliseconds(1));}while(std::chrono::steady_clock::now()<deadline);if(result!=child_)return " (worker status unavailable)";child_=-1;if(WIFEXITED(status))return " (worker exited "+std::to_string(WEXITSTATUS(status))+")";if(WIFSIGNALED(status))return " (worker killed by signal "+std::to_string(WTERMSIG(status))+")";return " (worker stopped unexpectedly)";}
 Reply reply(unsigned timeout){Reply r;try{receive(socket_,&r,sizeof(r),timeout);}catch(const std::exception& error){throw std::runtime_error(std::string(error.what())+childState());}if(r.tag!=magic||r.status){r.reason[255]=0;throw std::runtime_error(r.reason[0]?r.reason:"Wideband GPU worker rejected request");}return r;}
 void beginSubmit(size_t n,uint64_t first){if(pending_)throw std::logic_error("Wideband GPU frame already in flight");pendingDispatch_=plan_.prepare(first,uint32_t(n));Frame f;f.samples=n;f.first=first;f.sequence=++sequence_;pendingBegin_=std::chrono::steady_clock::now();try{io(socket_,&f,sizeof(f),true);}catch(const std::exception& error){throw std::runtime_error(std::string(error.what())+childState());}pendingSequence_=f.sequence;pending_=true;}
 std::vector<LaneOutput> finishSubmit(){if(!pending_)throw std::logic_error("Wideband GPU frame is not in flight");Reply r;try{const auto elapsed=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-pendingBegin_);if(elapsed>=std::chrono::milliseconds(options_.frameMs))throw std::runtime_error("Wideband GPU worker frame deadline expired"+childState());const unsigned remaining=unsigned(std::max<int64_t>(1,options_.frameMs-elapsed.count()));r=reply(remaining);}catch(...){pending_=false;throw;}pending_=false;if(r.sequence!=pendingSequence_||r.lanes!=config_.lanes.size())throw std::runtime_error("Wideband GPU response identity differs");uint64_t expectedTotal=0;for(uint32_t lane=0;lane<r.lanes;++lane){const auto& expected=pendingDispatch_.lanes[lane];const bool has=expected.outputSamples!=0;if(r.has[lane]>1||bool(r.has[lane])!=has||r.samples[lane]!=expected.outputSamples)throw std::runtime_error("Wideband GPU response lane shape differs");const uint64_t expectedFirst=has?pendingDispatch_.firstInputSample+expected.firstRemainder:0;if(r.first[lane]!=expectedFirst)throw std::runtime_error("Wideband GPU response sample position differs");const uint64_t elements=uint64_t(expected.outputSamples)*config_.channels;if(elements>plan_.layout().outputElements||expectedTotal>plan_.layout().outputElements-elements)throw std::runtime_error("Wideband GPU response output bound differs");expectedTotal+=elements;}if(expectedTotal!=pendingDispatch_.outputElements||expectedTotal>plan_.layout().outputElements)throw std::runtime_error("Wideband GPU response total output differs");const auto*d=shared_+inputElements(config_);std::vector<LaneOutput> out;uint64_t offset=0;for(uint32_t lane=0;lane<r.lanes;++lane){LaneOutput o;o.frequencyHz=config_.lanes[lane].frequencyHz;o.inputSamplesPerOutput=config_.lanes[lane].decimation;o.hasFirstInputSample=r.has[lane];o.firstInputSample=r.first[lane];o.channels.resize(config_.channels);for(uint32_t c=0;c<config_.channels;++c){o.channels[c].assign(d+offset,d+offset+r.samples[lane]);offset+=r.samples[lane];}out.push_back(std::move(o));}timing_.workerMs=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-pendingBegin_).count();return out;}
 std::vector<LaneOutput> submit(size_t n,uint64_t first){beginSubmit(n,first);return finishSubmit();}
public:
 Process(Config c,const std::string&device,GpuProcessOptions options):config_(std::move(c)),plan_(config_),options_(options),converter_(converterWorkers(config_)){
  int peer=-1,childSocket=-1,childMemory=-1;posix_spawn_file_actions_t actions;bool actionsReady=false;
  try{
  if(!options_.startupMs||!options_.frameMs)
    throw std::invalid_argument("Wideband GPU worker deadlines must be nonzero");
  if(device.size()>=sizeof(Header{}.device))
    throw std::invalid_argument("Wideband GPU device identity too long");
  bytes_=mappingBytes(plan_);
#ifdef __linux__
  memory_=int(syscall(SYS_memfd_create,"vectorwarp-wideband",0));
#else
  char temporary[]="/tmp/vectorwarp-wideband.XXXXXX";memory_=mkstemp(temporary);if(memory_>=0)unlink(temporary);
#endif
  if(memory_<0||ftruncate(memory_,off_t(bytes_)))
    throw std::runtime_error("Wideband GPU shared memory failed");
  shared_=static_cast<std::complex<float>*>(mmap(nullptr,bytes_,
    PROT_READ|PROT_WRITE,MAP_SHARED,memory_,0));
  if(shared_==MAP_FAILED){shared_=nullptr;
    throw std::runtime_error("Wideband GPU mapping failed");}
  int pair[2];if(socketpair(AF_UNIX,SOCK_STREAM,0,pair))throw std::runtime_error("Wideband GPU socket failed");socket_=pair[0];peer=pair[1];childSocket=fcntl(peer,F_DUPFD_CLOEXEC,10);childMemory=fcntl(memory_,F_DUPFD_CLOEXEC,10);if(childSocket<0||childMemory<0)throw std::runtime_error("Wideband GPU descriptor duplication failed");fcntl(socket_,F_SETFD,FD_CLOEXEC);fcntl(peer,F_SETFD,FD_CLOEXEC);fcntl(memory_,F_SETFD,FD_CLOEXEC);if(posix_spawn_file_actions_init(&actions))throw std::runtime_error("Wideband GPU spawn actions failed");actionsReady=true;posix_spawn_file_actions_adddup2(&actions,childSocket,3);posix_spawn_file_actions_adddup2(&actions,childMemory,4);const std::string exe=options.executable.empty()?gpuSiblingPath("blah2-gpu-worker"):options.executable;const std::string arg=options.argument.empty()?"--wideband-v1":options.argument;char*argv[]={const_cast<char*>(exe.c_str()),const_cast<char*>(arg.c_str()),nullptr};int e=posix_spawn(&child_,exe.c_str(),&actions,nullptr,argv,environ);posix_spawn_file_actions_destroy(&actions);actionsReady=false;close(childSocket);childSocket=-1;close(childMemory);childMemory=-1;close(peer);peer=-1;if(e)throw std::runtime_error("Wideband GPU worker spawn failed");Header h;h.inputRate=config_.inputRateHz;h.channels=config_.channels;h.maxInput=config_.maxInputSamples;h.lanes=config_.lanes.size();h.bytes=bytes_;std::memcpy(h.device,device.data(),device.size());io(socket_,&h,sizeof(h),true);for(const auto&l:config_.lanes){LaneWire w{l.frequencyHz,l.decimation,uint32_t(l.taps.size()),l.centerOffsetHz};io(socket_,&w,sizeof(w),true);io(socket_,const_cast<double*>(l.taps.data()),l.taps.size()*sizeof(double),true);}auto r=reply(options_.startupMs);device_=r.device;
  if(options_.warmup){std::fill_n(shared_,inputElements(config_),std::complex<float>{});submit(config_.maxInputSamples,0);reset();timing_={};}
  }catch(...){if(actionsReady)posix_spawn_file_actions_destroy(&actions);if(childSocket>=0)close(childSocket);if(childMemory>=0)close(childMemory);if(peer>=0)close(peer);stop();throw;}
 }
 ~Process(){stop();}
 std::string deviceName()const override{return device_;}
 Timing lastTiming()const noexcept override{return timing_;}
 void reset()override{if(pending_)throw std::logic_error("Cannot reset wideband GPU with frame in flight");Frame f;f.op=3;f.sequence=++sequence_;io(socket_,&f,sizeof(f),true);auto r=reply(options_.frameMs);if(r.sequence!=sequence_)throw std::runtime_error("Wideband GPU reset sequence differs");plan_.reset();}
 bool supportsSplitFrame()const noexcept override{return true;}
 bool frameInFlight()const noexcept override{return pending_;}
  std::vector<LaneOutput> process(const std::vector<std::vector<std::complex<float>>>&input,uint64_t first)override{if(input.size()!=config_.channels)throw std::invalid_argument("Wideband GPU channel count changed");const size_t n=input.empty()?0:input.front().size();if(n>config_.maxInputSamples)throw std::invalid_argument("Wideband GPU input exceeds capacity");for(const auto&c:input)if(c.size()!=n)throw std::invalid_argument("Wideband GPU input channels unpaired");const auto begin=std::chrono::steady_clock::now();converter_.parallel_for(config_.channels,[&](size_t c){std::copy(input[c].begin(),input[c].end(),shared_+uint64_t(c)*config_.maxInputSamples);});timing_.stagingMs=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-begin).count();return submit(n,first);}
  void beginDeques(const std::vector<const std::deque<std::complex<double>>*>&input,uint64_t first)override{if(pending_)throw std::logic_error("Wideband GPU frame already in flight");if(input.size()!=config_.channels)throw std::invalid_argument("Wideband GPU channel count changed");const size_t n=input.empty()?0:input.front()->size();if(n>config_.maxInputSamples)throw std::invalid_argument("Wideband GPU input exceeds capacity");for(uint32_t c=0;c<config_.channels;++c)if(!input[c]||input[c]->size()!=n)throw std::invalid_argument("Wideband GPU deque input differs");const auto begin=std::chrono::steady_clock::now();converter_.parallel_for(config_.channels,[&](size_t c){auto source=input[c]->begin();auto* destination=shared_+uint64_t(c)*config_.maxInputSamples;for(size_t index=0;index<n;++index)*destination++=std::complex<float>(*source++);});timing_.stagingMs=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-begin).count();beginSubmit(n,first);}
  std::vector<LaneOutput> finishFrame()override{return finishSubmit();}
  std::vector<LaneOutput> processDeques(const std::vector<const std::deque<std::complex<double>>*>&input,uint64_t first)override{beginDeques(input,first);return finishFrame();}
};
}
std::unique_ptr<Backend> createProcess(const Config&c,const std::string&d,const GpuProcessOptions&o){return std::make_unique<Process>(c,d,o);}
int runWorker(const Factory&factory){try{Header h;io(3,&h,sizeof(h),false);if(h.tag!=magic||!h.lanes||h.lanes>maxLanes)throw std::runtime_error("Wideband GPU worker header invalid");Config c;c.inputRateHz=h.inputRate;c.channels=h.channels;c.maxInputSamples=h.maxInput;c.lanes.reserve(h.lanes);for(uint32_t i=0;i<h.lanes;++i){LaneWire w;io(3,&w,sizeof(w),false);if(!w.taps||w.taps>maxFilterTaps)throw std::runtime_error("Wideband GPU tap count invalid");LaneConfig l;l.frequencyHz=w.frequency;l.decimation=w.decimation;l.centerOffsetHz=w.center;l.taps.resize(w.taps);io(3,l.taps.data(),l.taps.size()*sizeof(double),false);c.lanes.push_back(std::move(l));}Plan plan(c);if(mappingBytes(plan)!=h.bytes)throw std::runtime_error("Wideband GPU mapping size differs");auto*shared=static_cast<std::complex<float>*>(mmap(nullptr,h.bytes,PROT_READ|PROT_WRITE,MAP_SHARED,4,0));if(shared==MAP_FAILED)throw std::runtime_error("Wideband GPU worker mapping failed");auto backend=factory(c,h.device);Reply ready;ready.lanes=h.lanes;std::strncpy(ready.device,backend->deviceName().c_str(),sizeof(ready.device)-1);io(3,&ready,sizeof(ready),true);for(;;){Frame f;io(3,&f,sizeof(f),false);if(f.tag!=magic)throw std::runtime_error("Wideband GPU frame invalid");if(f.op==2)break;if(f.op==3){backend->reset();Reply r;r.sequence=f.sequence;r.lanes=h.lanes;io(3,&r,sizeof(r),true);continue;}if(f.op!=1||f.samples>c.maxInputSamples)throw std::runtime_error("Wideband GPU frame shape invalid");Backend::InputView view;view.samples=f.samples;for(uint32_t ch=0;ch<c.channels;++ch)view.channels.push_back(shared+uint64_t(ch)*c.maxInputSamples);auto out=backend->processView(view,f.first);if(out.size()!=c.lanes.size())throw std::runtime_error("Wideband GPU lane count changed");auto*d=shared+inputElements(c);uint64_t offset=0;Reply r;r.sequence=f.sequence;r.lanes=out.size();for(size_t lane=0;lane<out.size();++lane){r.samples[lane]=out[lane].channels.empty()?0:out[lane].channels.front().size();r.first[lane]=out[lane].firstInputSample;r.has[lane]=out[lane].hasFirstInputSample;for(const auto&ch:out[lane].channels){if(ch.size()!=r.samples[lane]||offset+ch.size()>plan.layout().outputElements)throw std::runtime_error("Wideband GPU output shape invalid");std::copy(ch.begin(),ch.end(),d+offset);offset+=ch.size();}}io(3,&r,sizeof(r),true);}munmap(shared,h.bytes);return 0;}catch(const std::exception&e){Reply r;r.status=1;std::strncpy(r.reason,e.what(),sizeof(r.reason)-1);try{io(3,&r,sizeof(r),true);}catch(...){}return 2;}}
}
