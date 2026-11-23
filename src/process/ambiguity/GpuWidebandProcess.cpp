#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "process/ambiguity/GpuWidebandProcess.h"
#include "data/meta/StreamProfile.h"
#include "process/ambiguity/GpuArtifactBinding.h"
#include "process/ambiguity/GpuWorkerLifetime.h"
#include "process/utility/FixedExecutor.h"
#include <algorithm>
#include <chrono>
#include <cerrno>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <dirent.h>
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
constexpr uint32_t magic=0x42574702; // v2: explicit parent-authorized trace policy.
struct Header{uint32_t tag=magic,inputRate=0,channels=0,maxInput=0,lanes=0,traceFrames=0;uint64_t bytes=0;char device[96]{};};
struct LaneWire{uint64_t frequency=0;uint32_t decimation=0,taps=0;double center=0;};
struct Frame{uint32_t tag=magic,op=1,samples=0,reserved=0;uint64_t sequence=0,first=0;};
struct Reply{uint32_t tag=magic,status=0,lanes=0;uint64_t sequence=0;uint32_t samples[maxLanes]{};uint64_t first[maxLanes]{};uint8_t has[maxLanes]{};char reason[256]{},device[256]{};};
void io(int fd,void* data,size_t bytes,bool writing){auto*p=static_cast<unsigned char*>(data);while(bytes){ssize_t n;if(writing)n=::send(fd,p,bytes,MSG_NOSIGNAL);else n=::read(fd,p,bytes);if(n<0&&errno==EINTR)continue;if(n<=0)throw std::runtime_error("Wideband GPU worker transport failed");p+=n;bytes-=size_t(n);}}
void sendStartup(int fd,const void* data,size_t bytes,
  std::chrono::steady_clock::time_point deadline)
{
  auto* cursor=static_cast<const unsigned char*>(data);
  while(bytes){
    const auto now=std::chrono::steady_clock::now();
    if(now>=deadline)throw std::runtime_error("Wideband GPU worker startup send timed out");
    const ssize_t count=::send(fd,cursor,bytes,MSG_NOSIGNAL|MSG_DONTWAIT);
    if(count>0){cursor+=count;bytes-=size_t(count);continue;}
    if(count<0&&errno==EINTR)continue;
    if(count<0&&(errno==EAGAIN||errno==EWOULDBLOCK)){
      const auto left=std::chrono::duration_cast<std::chrono::milliseconds>(deadline-now).count();
      pollfd descriptor{fd,POLLOUT,0};
      const int ready=poll(&descriptor,1,int(std::max<int64_t>(1,std::min<int64_t>(left,2000000000))));
      if(ready<0&&errno==EINTR)continue;
      if(ready==0)continue; // Recheck the single startup deadline.
      if(ready>0&&(descriptor.revents&POLLOUT))continue;
    }
    throw std::runtime_error("Wideband GPU worker startup transport failed");
  }
}
void receive(int fd,void* data,size_t bytes,gpu_receive_deadline::Clock::time_point deadline){
 auto*p=static_cast<unsigned char*>(data);
 while(bytes){
  gpu_receive_deadline::waitReadable(fd,deadline,"Wideband GPU worker timed out or failed");
  const ssize_t n=recv(fd,p,bytes,MSG_DONTWAIT);
  if(n<0&&(errno==EINTR||errno==EAGAIN||errno==EWOULDBLOCK))continue;
  if(n<=0)throw std::runtime_error("Wideband GPU worker transport failed");
  p+=n;bytes-=size_t(n);
 }
}
uint64_t inputElements(const Config&c){return uint64_t(c.channels)*c.maxInputSamples;}
size_t converterWorkers(const Config&c){return std::max<size_t>(1,c.channels);}
uint64_t mappingBytes(const Plan&p,unsigned slots=1){return (inputElements(p.config())+slots*p.layout().outputElements)*sizeof(std::complex<float>);}
constexpr uint64_t sharedBudget=512ULL<<20;
bool twoOutputSlotsFit(const Plan& plan)
{return plan.layout().totalBytes+plan.layout().outputElements*sizeof(std::complex<float>)<=sharedBudget;}
struct SharedMapping
{
  std::complex<float>* data=nullptr;
  uint64_t bytes=0;
  SharedMapping(int fd,uint64_t size):bytes(size){
    auto* mapped=mmap(nullptr,bytes,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);
    if(mapped==MAP_FAILED)throw std::runtime_error("Wideband GPU mapping failed");
    data=static_cast<std::complex<float>*>(mapped);
  }
  ~SharedMapping(){if(data)munmap(data,bytes);}
};
struct OutputLease
{
  std::shared_ptr<SharedMapping> mapping;
  unsigned slot=0;
};
// Only explicitly offline callers consult this setting. Each process collects
// at most 256 records, including warmup, and writes them only during teardown.
// Inherited service environments cannot turn transport logging on in live DSP.
unsigned transportTraceFrameLimit()noexcept
{
  const char* value=std::getenv("VECTORWARP_WIDEBAND_TRACE_FRAMES");
  if(!value||!*value)return 0;
  unsigned result=0;
  for(;*value;++value){
    if(*value<'0'||*value>'9')return 0;
    result=std::min(256u,result*10+unsigned(*value-'0'));
  }
  return result;
}
class TraceRecords
{
  std::vector<std::string> records_;
public:
  template<class... Args> void append(const char* format,Args... args)
  {
    char line[1024];
    const int count=std::snprintf(line,sizeof(line),format,args...);
    if(count>0)records_.emplace_back(line,std::min(size_t(count),sizeof(line)-1));
  }
  ~TraceRecords()
  {
    // No diagnostic I/O participates in a frame's reply or capture deadline.
    // Offline shutdown may wait for its explicitly requested diagnostic sink.
    for(const auto& line:records_)std::fwrite(line.data(),1,line.size(),stderr);
  }
};
using TraceClock=std::chrono::steady_clock;
double traceMilliseconds(TraceClock::time_point end,TraceClock::time_point begin)
{return std::chrono::duration<double,std::milli>(end-begin).count();}
struct OutputTraceTiming
{
  TraceClock::time_point begin,backendEnd,copyBegin,copyEnd;
  bool borrowed=false;
};
// Both module capabilities must agree with the worker's independent geometry
// before any output is copied. The borrowed view never escapes this call.
void copyWorkerOutput(Backend& backend,const Config& config,
  const Backend::InputView& input,const Dispatch& expected,
  std::complex<float>* destination,Reply& reply,OutputTraceTiming* timing)
{
  if(timing)timing->begin=TraceClock::now();
  if(auto* borrowed=dynamic_cast<BorrowedOutputBackend*>(&backend)){
    const auto output=borrowed->processBorrowed(input,expected.firstInputSample);
    if(timing){timing->backendEnd=TraceClock::now();timing->borrowed=true;}
    const auto& actual=output.dispatch;
    if(actual.firstInputSample!=expected.firstInputSample||
        actual.inputSamples!=expected.inputSamples||
        actual.outputElements!=expected.outputElements||
        actual.lanes.size()!=expected.lanes.size()||
        (actual.outputElements&&!output.data))
      throw std::runtime_error("Wideband GPU borrowed output shape invalid");
    for(size_t lane=0;lane<expected.lanes.size();++lane){
      const auto& a=actual.lanes[lane];const auto& e=expected.lanes[lane];
      if(a.outputOffset!=e.outputOffset||a.outputSamples!=e.outputSamples||
          a.firstRemainder!=e.firstRemainder||a.decimation!=e.decimation)
        throw std::runtime_error("Wideband GPU borrowed output lane differs");
    }
    // The independent Plan bounds outputElements to the shared allocation.
    // Copy the completed mapped GPU output straight into IPC, without a full
    // bank of temporary LaneOutput allocations and another sample copy.
    if(timing)timing->copyBegin=TraceClock::now();
    if(expected.outputElements)
      std::memcpy(destination,output.data,
        size_t(expected.outputElements)*sizeof(std::complex<float>));
    if(timing)timing->copyEnd=TraceClock::now();
  }else{
    const auto output=backend.processView(input,expected.firstInputSample);
    if(timing)timing->backendEnd=TraceClock::now();
    if(output.size()!=config.lanes.size())
      throw std::runtime_error("Wideband GPU lane count changed");
    for(size_t lane=0;lane<output.size();++lane){
      const auto& actual=output[lane];const auto& e=expected.lanes[lane];
      const bool has=e.outputSamples!=0;
      if(actual.frequencyHz!=config.lanes[lane].frequencyHz||
          actual.inputSamplesPerOutput!=e.decimation||
          actual.hasFirstInputSample!=has||
          actual.firstInputSample!=(has?expected.firstInputSample+e.firstRemainder:0)||
          actual.channels.size()!=config.channels)
        throw std::runtime_error("Wideband GPU output lane differs");
      for(const auto& channel:actual.channels)
        if(channel.size()!=e.outputSamples)
          throw std::runtime_error("Wideband GPU output shape invalid");
    }
    if(timing)timing->copyBegin=TraceClock::now();
    for(size_t lane=0;lane<output.size();++lane){
      const auto& e=expected.lanes[lane];
      for(size_t channel=0;channel<config.channels;++channel)
        std::copy(output[lane].channels[channel].begin(),
          output[lane].channels[channel].end(),
          destination+e.outputOffset+channel*e.outputSamples);
    }
    if(timing)timing->copyEnd=TraceClock::now();
  }
  reply.lanes=expected.lanes.size();
  for(size_t lane=0;lane<expected.lanes.size();++lane){
    const auto& e=expected.lanes[lane];
    reply.samples[lane]=e.outputSamples;reply.has[lane]=e.outputSamples!=0;
    reply.first[lane]=reply.has[lane]?expected.firstInputSample+e.firstRemainder:0;
  }
}
void reap(pid_t child)noexcept{if(child<=0)return;int status=0;pid_t result;const auto deadline=std::chrono::steady_clock::now()+std::chrono::milliseconds(100);do{result=waitpid(child,&status,WNOHANG);if(result==child||(result<0&&errno!=EINTR))return;std::this_thread::sleep_for(std::chrono::milliseconds(1));}while(std::chrono::steady_clock::now()<deadline);kill(child,SIGKILL);try{std::thread([child]{int value;while(waitpid(child,&value,0)<0&&errno==EINTR){}}).detach();}catch(...){}}
class Process final:public Backend,public LeasedOutputBackend{
 Config config_;Plan plan_;GpuProcessOptions options_;utility::FixedExecutor converter_;Timing timing_;int socket_=-1,memory_=-1;pid_t child_=-1;std::complex<float>*shared_=nullptr;uint64_t bytes_=0,sequence_=0,pendingSequence_=0;bool pending_=false;Dispatch pendingDispatch_;std::chrono::steady_clock::time_point pendingBegin_{};std::string device_;
 unsigned traceRemaining_=0;
 TraceRecords traceRecords_;
 std::shared_ptr<SharedMapping> mapping_;
 std::shared_ptr<OutputLease> pendingLease_;
 std::array<std::weak_ptr<OutputLease>,2> outputLeases_;
 unsigned outputSlots_=1;
 bool failed_=false;
 void stop()noexcept{if(socket_>=0){Frame q;q.op=2;::send(socket_,&q,sizeof(q),MSG_NOSIGNAL|MSG_DONTWAIT);close(socket_);socket_=-1;}reap(child_);child_=-1;pendingLease_.reset();mapping_.reset();shared_=nullptr;if(memory_>=0){close(memory_);memory_=-1;}}
 std::shared_ptr<OutputLease> reserveOutput(){
  if(failed_)throw std::runtime_error("Wideband GPU worker failed; recreate transport");
  if(pending_)throw std::logic_error("Wideband GPU frame already in flight");
  for(unsigned slot=0;slot<outputSlots_;++slot)if(outputLeases_[slot].expired()){
    auto lease=std::make_shared<OutputLease>();lease->mapping=mapping_;lease->slot=slot;
    outputLeases_[slot]=lease;return lease;
  }
  throw std::logic_error("Wideband GPU output slots are still leased");
 }
 std::string childState(){if(child_<=0)return " (worker already reaped)";int status=0;pid_t result=0;const auto deadline=std::chrono::steady_clock::now()+std::chrono::milliseconds(20);do{result=waitpid(child_,&status,WNOHANG);if(result==child_||result<0)break;std::this_thread::sleep_for(std::chrono::milliseconds(1));}while(std::chrono::steady_clock::now()<deadline);if(result!=child_)return " (worker status unavailable)";child_=-1;if(WIFEXITED(status))return " (worker exited "+std::to_string(WEXITSTATUS(status))+")";if(WIFSIGNALED(status))return " (worker killed by signal "+std::to_string(WTERMSIG(status))+")";return " (worker stopped unexpectedly)";}
 Reply reply(gpu_receive_deadline::Clock::time_point deadline){Reply r;try{receive(socket_,&r,sizeof(r),deadline);}catch(const std::exception& error){throw std::runtime_error(std::string(error.what())+childState());}if(r.tag!=magic||r.status){r.reason[255]=0;throw std::runtime_error(r.reason[0]?r.reason:"Wideband GPU worker rejected request");}return r;}
 Reply reply(unsigned timeout){return reply(gpu_receive_deadline::Clock::now()+std::chrono::milliseconds(timeout));}
 void beginSubmit(size_t n,uint64_t first,std::shared_ptr<OutputLease> lease){if(pending_)throw std::logic_error("Wideband GPU frame already in flight");pendingDispatch_=plan_.prepare(first,uint32_t(n));Frame f;f.samples=n;f.first=first;f.reserved=lease->slot;f.sequence=++sequence_;pendingBegin_=std::chrono::steady_clock::now();try{io(socket_,&f,sizeof(f),true);}catch(const std::exception& error){failed_=true;throw std::runtime_error(std::string(error.what())+childState());}pendingLease_=std::move(lease);pendingSequence_=f.sequence;pending_=true;}
 std::vector<LaneOutput> finishSubmitImpl(std::vector<LeasedLaneOutput>* leased){
  const bool trace=traceRemaining_!=0;
  const auto finishBegin=trace?TraceClock::now():TraceClock::time_point{};
  Reply r;
  try{
    // Split-frame callers may consume a completed reply only after prior-CPI
    // CPU work. Host scheduling delay is not evidence of a slow GPU. Drain an
    // already-ready reply even after the deadline, but never grant a fresh
    // frame timeout: an unfinished/partial reply receives no new wait.
    r=reply(pendingBegin_+std::chrono::milliseconds(options_.frameMs));
  }catch(...){pending_=false;throw;}
  const auto received=trace?TraceClock::now():TraceClock::time_point{};
  pending_=false;
  if(r.sequence!=pendingSequence_||r.lanes!=config_.lanes.size())
    throw std::runtime_error("Wideband GPU response identity differs");
  uint64_t expectedTotal=0;
  for(uint32_t lane=0;lane<r.lanes;++lane){
    const auto& expected=pendingDispatch_.lanes[lane];const bool has=expected.outputSamples!=0;
    if(r.has[lane]>1||bool(r.has[lane])!=has||r.samples[lane]!=expected.outputSamples)
      throw std::runtime_error("Wideband GPU response lane shape differs");
    const uint64_t expectedFirst=has?pendingDispatch_.firstInputSample+expected.firstRemainder:0;
    if(r.first[lane]!=expectedFirst)
      throw std::runtime_error("Wideband GPU response sample position differs");
    const uint64_t elements=uint64_t(expected.outputSamples)*config_.channels;
    if(elements>plan_.layout().outputElements||expectedTotal>plan_.layout().outputElements-elements)
      throw std::runtime_error("Wideband GPU response output bound differs");
    expectedTotal+=elements;
  }
  if(expectedTotal!=pendingDispatch_.outputElements||expectedTotal>plan_.layout().outputElements)
    throw std::runtime_error("Wideband GPU response total output differs");
  const auto importBegin=trace?TraceClock::now():TraceClock::time_point{};
  const auto*d=shared_+inputElements(config_)+pendingLease_->slot*plan_.layout().outputElements;
  std::vector<LaneOutput> out;uint64_t offset=0;
  for(uint32_t lane=0;lane<r.lanes;++lane){
    if(leased){
      LeasedLaneOutput o;o.frequencyHz=config_.lanes[lane].frequencyHz;
      o.inputSamplesPerOutput=config_.lanes[lane].decimation;
      o.hasFirstInputSample=r.has[lane];o.firstInputSample=r.first[lane];
      o.samples.owner=pendingLease_;
      for(uint32_t c=0;c<config_.channels;++c){
        std::get<iq::BorrowedSamples::FloatChannels>(o.samples.channels).push_back({d+offset,r.samples[lane]});offset+=r.samples[lane];
      }
      leased->push_back(std::move(o));continue;
    }
    LaneOutput o;o.frequencyHz=config_.lanes[lane].frequencyHz;
    o.inputSamplesPerOutput=config_.lanes[lane].decimation;
    o.hasFirstInputSample=r.has[lane];o.firstInputSample=r.first[lane];
    o.channels.resize(config_.channels);
    for(uint32_t c=0;c<config_.channels;++c){
      o.channels[c].assign(d+offset,d+offset+r.samples[lane]);offset+=r.samples[lane];
    }
    out.push_back(std::move(o));
  }
  const auto completed=TraceClock::now();
  timing_.workerMs=traceMilliseconds(completed,pendingBegin_);
  if(trace){
    --traceRemaining_;
    // submit_to_finish includes overlapped caller work. finish_wait and import
    // isolate actual completion costs; none of these values is GPU kernel time.
    traceRecords_.append("WIDEBAND_TRANSPORT role=parent sequence=%llu first=%llu input_samples=%u output_bytes=%llu mode=%s import_bytes=%llu finish_wait_ms=%.6f reply_validate_ms=%.6f import_ms=%.6f submit_to_finish_ms=%.6f\n",
      static_cast<unsigned long long>(pendingSequence_),
      static_cast<unsigned long long>(pendingDispatch_.firstInputSample),
      pendingDispatch_.inputSamples,
      static_cast<unsigned long long>(expectedTotal*sizeof(std::complex<float>)),
      leased?"leased":"owned",leased?0ULL:static_cast<unsigned long long>(expectedTotal*sizeof(std::complex<float>)),
      traceMilliseconds(received,finishBegin),traceMilliseconds(importBegin,received),
      traceMilliseconds(completed,importBegin),timing_.workerMs);
  }
  pendingLease_.reset();
  return out;
 }
 std::vector<LaneOutput> finishSubmit(std::vector<LeasedLaneOutput>* leased=nullptr){
  if(!pending_)throw std::logic_error("Wideband GPU frame is not in flight");
  try{return finishSubmitImpl(leased);}
  catch(...){pending_=false;failed_=true;pendingLease_.reset();throw;}
 }
 std::vector<LaneOutput> submit(size_t n,uint64_t first){beginSubmit(n,first,reserveOutput());return finishSubmit();}
public:
 Process(Config c,const std::string&device,GpuProcessOptions options,bool leased=false):config_(std::move(c)),plan_(config_),options_(options),converter_(converterWorkers(config_)){
  int peer=-1,childSocket=-1,childMemory=-1;posix_spawn_file_actions_t actions;bool actionsReady=false;
#ifdef __APPLE__
  posix_spawnattr_t attributes;
  bool attributesReady=false;
  // Snapshot logging descriptors before IPC can reuse a closed standard fd.
  const bool standardOpen[]={fcntl(0,F_GETFD)>=0,fcntl(1,F_GETFD)>=0,fcntl(2,F_GETFD)>=0};
#endif
  try{
  traceRemaining_=options_.offlineTransportTrace?transportTraceFrameLimit():0;
  if(!options_.startupMs||!options_.frameMs)
    throw std::invalid_argument("Wideband GPU worker deadlines must be nonzero");
  if(device.size()>=sizeof(Header{}.device))
    throw std::invalid_argument("Wideband GPU device identity too long");
  outputSlots_=leased&&twoOutputSlotsFit(plan_)?2:1;
  bytes_=mappingBytes(plan_,outputSlots_);
#ifdef __linux__
  memory_=int(syscall(SYS_memfd_create,"vectorwarp-wideband",0));
#else
  char temporary[]="/tmp/vectorwarp-wideband.XXXXXX";memory_=mkstemp(temporary);if(memory_>=0)unlink(temporary);
#endif
  if(memory_<0||ftruncate(memory_,off_t(bytes_)))
    throw std::runtime_error("Wideband GPU shared memory failed");
  mapping_=std::make_shared<SharedMapping>(memory_,bytes_);shared_=mapping_->data;
  int pair[2];
  if(socketpair(AF_UNIX,SOCK_STREAM,0,pair))throw std::runtime_error("Wideband GPU socket failed");
  socket_=pair[0];peer=pair[1];
  // Darwin MSG_DONTWAIT alone does not bound sends waiting for buffer space.
  const int flags=fcntl(socket_,F_GETFL);
  if(flags<0||fcntl(socket_,F_SETFL,flags|O_NONBLOCK))
    throw std::runtime_error("Wideband GPU nonblocking transport failed");
  childSocket=fcntl(peer,F_DUPFD_CLOEXEC,10);
  childMemory=fcntl(memory_,F_DUPFD_CLOEXEC,10);
  if(childSocket<0||childMemory<0||fcntl(socket_,F_SETFD,FD_CLOEXEC)||
      fcntl(peer,F_SETFD,FD_CLOEXEC)||fcntl(memory_,F_SETFD,FD_CLOEXEC))
    throw std::runtime_error("Wideband GPU descriptor duplication failed");
  if(posix_spawn_file_actions_init(&actions))
    throw std::runtime_error("Wideband GPU spawn actions failed");
  actionsReady=true;
  if(posix_spawn_file_actions_adddup2(&actions,childSocket,3)||
      posix_spawn_file_actions_adddup2(&actions,childMemory,4))
    throw std::runtime_error("Wideband GPU descriptor setup failed");
  gpu_artifact_binding::spawnActions(actions,false);
  // Inherit only the two protocol descriptors and existing standard logging.
  // Capture/output sockets must not stay alive in an isolated GPU worker.
#if defined(__APPLE__)
  if(posix_spawnattr_init(&attributes))
    throw std::runtime_error("Wideband GPU worker isolation failed");
  attributesReady=true;
  if(posix_spawnattr_setflags(&attributes,POSIX_SPAWN_CLOEXEC_DEFAULT))
    throw std::runtime_error("Wideband GPU descriptor isolation failed");
  for(int fd=0;fd<3;++fd)
    if(standardOpen[fd]&&posix_spawn_file_actions_addinherit_np(&actions,fd))
      throw std::runtime_error("Wideband GPU logging inheritance failed");
#elif defined(__GLIBC__) && defined(__GLIBC_MINOR__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 34))
  if(posix_spawn_file_actions_addclosefrom_np(&actions,gpu_artifact_binding::active()?7:5))
    throw std::runtime_error("Wideband GPU descriptor isolation failed");
#else
  DIR* descriptors=opendir("/proc/self/fd");
  if(!descriptors)throw std::runtime_error("Wideband GPU descriptor inspection failed");
  int closeError=0;
  while(const auto* entry=readdir(descriptors)){
    const int fd=std::atoi(entry->d_name);
    if(fd>=(gpu_artifact_binding::active()?7:5)&&(closeError=posix_spawn_file_actions_addclose(&actions,fd)))break;
  }
  closedir(descriptors);
  if(closeError)throw std::runtime_error("Wideband GPU descriptor isolation failed");
#endif
  const std::string exe=options.executable.empty()?gpuSiblingPath("blah2-gpu-worker"):options.executable;
  const std::string arg=options.argument.empty()?"--wideband-v1":options.argument;
  char*argv[]={const_cast<char*>(exe.c_str()),const_cast<char*>(arg.c_str()),nullptr};
  std::vector<std::string> childEnvironment;
  for(char** item=environ;*item;++item)
    if(!gpu_artifact_binding::inherited(*item)&&!replay_gpu_qualification::inherited(*item))
      childEnvironment.emplace_back(*item);
  gpu_artifact_binding::appendEnvironment(childEnvironment);
  gpu_offline_diagnostics::appendEnvironment(childEnvironment,options_.offlineTransportTrace);
  gpu_worker_lifetime::bindParent(childEnvironment);
  std::vector<char*> childEnvironmentPointers;
  for(auto& item:childEnvironment)childEnvironmentPointers.push_back(item.data());
  childEnvironmentPointers.push_back(nullptr);
  const int error=posix_spawn(&child_,exe.c_str(),&actions,
#ifdef __APPLE__
    &attributes,
#else
    nullptr,
#endif
    argv,childEnvironmentPointers.data());
  posix_spawn_file_actions_destroy(&actions);actionsReady=false;
#ifdef __APPLE__
  posix_spawnattr_destroy(&attributes);attributesReady=false;
#endif
  close(childSocket);childSocket=-1;close(childMemory);childMemory=-1;close(peer);peer=-1;
  if(error){child_=-1;throw std::runtime_error("Wideband GPU worker spawn failed");}
  gpu_artifact_binding::spawned(child_,exe);
  const auto startupDeadline=std::chrono::steady_clock::now()+
    std::chrono::milliseconds(options_.startupMs);
  Header h;h.inputRate=config_.inputRateHz;h.channels=config_.channels;
  h.maxInput=config_.maxInputSamples;h.lanes=config_.lanes.size();
  h.traceFrames=traceRemaining_;h.bytes=bytes_;
  std::memcpy(h.device,device.data(),device.size());
  sendStartup(socket_,&h,sizeof(h),startupDeadline);
  for(const auto& lane:config_.lanes){
    LaneWire wire{lane.frequencyHz,lane.decimation,uint32_t(lane.taps.size()),lane.centerOffsetHz};
    sendStartup(socket_,&wire,sizeof(wire),startupDeadline);
    sendStartup(socket_,lane.taps.data(),lane.taps.size()*sizeof(double),startupDeadline);
  }
  const auto ready=reply(startupDeadline);
  device_=ready.device;
  if(options_.warmup){std::fill_n(shared_,inputElements(config_),std::complex<float>{});submit(config_.maxInputSamples,0);reset();timing_={};}
  }catch(...){
#ifdef __APPLE__
    if(attributesReady)posix_spawnattr_destroy(&attributes);
#endif
    if (actionsReady) posix_spawn_file_actions_destroy(&actions);
    if (childSocket >= 0) close(childSocket);
    if (childMemory >= 0) close(childMemory);
    if (peer >= 0) close(peer);
    stop();
    throw;
  }
 }
 ~Process(){stop();}
 std::string deviceName()const override{return device_;}
 Timing lastTiming()const noexcept override{return timing_;}
 void reset()override{if(pending_)throw std::logic_error("Cannot reset wideband GPU with frame in flight");if(failed_)throw std::runtime_error("Wideband GPU worker failed; recreate transport");try{Frame f;f.op=3;f.sequence=++sequence_;io(socket_,&f,sizeof(f),true);auto r=reply(options_.frameMs);if(r.sequence!=sequence_)throw std::runtime_error("Wideband GPU reset sequence differs");plan_.reset();}catch(...){failed_=true;throw;}}
 bool supportsSplitFrame()const noexcept override{return true;}
 bool frameInFlight()const noexcept override{return pending_;}
  std::vector<LaneOutput> process(const std::vector<std::vector<std::complex<float>>>&input,uint64_t first)override{begin(input,first);return finishSubmit();}
  void begin(const std::vector<std::vector<std::complex<float>>>&input,uint64_t first)override{if(pending_)throw std::logic_error("Wideband GPU frame already in flight");if(input.size()!=config_.channels)throw std::invalid_argument("Wideband GPU channel count changed");const size_t n=input.empty()?0:input.front().size();if(n>config_.maxInputSamples)throw std::invalid_argument("Wideband GPU input exceeds capacity");for(const auto&c:input)if(c.size()!=n)throw std::invalid_argument("Wideband GPU input channels unpaired");auto lease=reserveOutput();const auto started=std::chrono::steady_clock::now();converter_.parallel_for(config_.channels,[&](size_t c){std::copy(input[c].begin(),input[c].end(),shared_+uint64_t(c)*config_.maxInputSamples);});timing_.stagingMs=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-started).count();beginSubmit(n,first,std::move(lease));}
  void beginDeques(const std::vector<const std::deque<std::complex<double>>*>&input,uint64_t first)override{if(pending_)throw std::logic_error("Wideband GPU frame already in flight");if(input.size()!=config_.channels||(!input.empty()&&!input.front()))throw std::invalid_argument("Wideband GPU channel count changed");const size_t n=input.empty()?0:input.front()->size();if(n>config_.maxInputSamples)throw std::invalid_argument("Wideband GPU input exceeds capacity");for(uint32_t c=0;c<config_.channels;++c)if(!input[c]||input[c]->size()!=n)throw std::invalid_argument("Wideband GPU deque input differs");auto lease=reserveOutput();const auto begin=std::chrono::steady_clock::now();converter_.parallel_for(config_.channels,[&](size_t c){auto source=input[c]->begin();auto* destination=shared_+uint64_t(c)*config_.maxInputSamples;for(size_t index=0;index<n;++index)*destination++=std::complex<float>(*source++);});timing_.stagingMs=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-begin).count();beginSubmit(n,first,std::move(lease));}
  std::vector<LaneOutput> finishFrame()override{return finishSubmit();}
  bool supportsLeasedOutput()const noexcept override{return outputSlots_==2;}
  std::vector<LeasedLaneOutput> finishLeasedFrame()override{if(!supportsLeasedOutput())throw std::logic_error("Wideband GPU leased output exceeds shared budget");std::vector<LeasedLaneOutput> result;finishSubmit(&result);return result;}
  std::vector<LaneOutput> processDeques(const std::vector<const std::deque<std::complex<double>>*>&input,uint64_t first)override{beginDeques(input,first);return finishFrame();}
};
}
bool recoverableWorkerFailure(std::string_view reason) noexcept
{
  if (reason.find("Wideband GPU worker ") == std::string_view::npos) return false;
  return reason.find("transport failed") != std::string_view::npos ||
    reason.find("timed out") != std::string_view::npos ||
    reason.find("failed; recreate transport") != std::string_view::npos;
}
std::unique_ptr<Backend> createProcess(const Config&c,const std::string&d,const GpuProcessOptions&o){return std::make_unique<Process>(c,d,o);}
std::unique_ptr<Backend> createLeasedProcess(const Config&c,const std::string&d,const GpuProcessOptions&o){return std::make_unique<Process>(c,d,o,true);}
int runWorker(const Factory&factory)
{
  if(!gpu_worker_lifetime::protectWorker())return 2;
  try{
    gpu_artifact_binding::workerStart();
    Header h;io(3,&h,sizeof(h),false);
    if(h.tag!=magic||!h.lanes||h.lanes>maxLanes||h.traceFrames>256)
      throw std::runtime_error("Wideband GPU worker header invalid");
    Config c;c.inputRateHz=h.inputRate;c.channels=h.channels;
    c.maxInputSamples=h.maxInput;c.lanes.reserve(h.lanes);
    for(uint32_t i=0;i<h.lanes;++i){
      LaneWire w;io(3,&w,sizeof(w),false);
      if(!w.taps||w.taps>maxFilterTaps)
        throw std::runtime_error("Wideband GPU tap count invalid");
      LaneConfig l;l.frequencyHz=w.frequency;l.decimation=w.decimation;
      l.centerOffsetHz=w.center;l.taps.resize(w.taps);
      io(3,l.taps.data(),l.taps.size()*sizeof(double),false);
      c.lanes.push_back(std::move(l));
    }
    Plan plan(c);
    const unsigned outputSlots=h.bytes==mappingBytes(plan)?1:
      (twoOutputSlotsFit(plan)&&h.bytes==mappingBytes(plan,2)?2:0);
    if(!outputSlots)
      throw std::runtime_error("Wideband GPU mapping size differs");
    struct stat sharedStat{};
    if(fstat(4,&sharedStat)||sharedStat.st_size<0||
        uint64_t(sharedStat.st_size)!=h.bytes)
      throw std::runtime_error("Wideband GPU shared file size differs");
    auto*shared=static_cast<std::complex<float>*>(mmap(nullptr,h.bytes,
      PROT_READ|PROT_WRITE,MAP_SHARED,4,0));
    if(shared==MAP_FAILED)throw std::runtime_error("Wideband GPU worker mapping failed");
    auto backend=factory(c,h.device);Reply ready;ready.lanes=h.lanes;
    std::strncpy(ready.device,backend->deviceName().c_str(),sizeof(ready.device)-1);
    io(3,&ready,sizeof(ready),true);
    unsigned traceRemaining=h.traceFrames;
    TraceRecords traceRecords;
    stream_profile::Worker streamProfile("wideband-worker");
    for(;;){
      Frame f;io(3,&f,sizeof(f),false);
      if(f.tag!=magic)throw std::runtime_error("Wideband GPU frame invalid");
      if(f.op==2)break;
      if(f.op==3){
        backend->reset();plan.reset();Reply r;r.sequence=f.sequence;r.lanes=h.lanes;
        io(3,&r,sizeof(r),true);continue;
      }
      if(f.op!=1||f.samples>c.maxInputSamples||f.reserved>=outputSlots)
        throw std::runtime_error("Wideband GPU frame shape invalid");
      const bool trace=traceRemaining!=0;
      const bool profile=streamProfile.enabled();
      stream_profile::WorkRecord profileRecord;
      if(profile){profileRecord.sequence=f.sequence;profileRecord.first=f.first;
        profileRecord.identity=f.samples;profileRecord.begin=stream_profile::Activity::capture(true);}
      const auto workerBegin=trace?TraceClock::now():TraceClock::time_point{};
      OutputTraceTiming traceTiming;
      Backend::InputView view;view.samples=f.samples;
      for(uint32_t ch=0;ch<c.channels;++ch)
        view.channels.push_back(shared+uint64_t(ch)*c.maxInputSamples);
      const auto expected=plan.prepare(f.first,f.samples);
      Reply r;r.sequence=f.sequence;
      copyWorkerOutput(*backend,c,view,expected,
        shared+inputElements(c)+f.reserved*plan.layout().outputElements,r,
        (trace||profile)?&traceTiming:nullptr);
      io(3,&r,sizeof(r),true);
      if(profile){profileRecord.time={traceTiming.begin,traceTiming.backendEnd,
        traceTiming.copyBegin,traceTiming.copyEnd};streamProfile.record(profileRecord);}
      if(trace){
        --traceRemaining;
        const double total=traceMilliseconds(TraceClock::now(),workerBegin);
        traceRecords.append("WIDEBAND_TRANSPORT role=worker sequence=%llu first=%llu input_samples=%u output_bytes=%llu mode=%s backend_ms=%.6f validate_ms=%.6f copy_ms=%.6f worker_total_ms=%.6f\n",
          static_cast<unsigned long long>(f.sequence),
          static_cast<unsigned long long>(f.first),f.samples,
          static_cast<unsigned long long>(expected.outputElements)*sizeof(std::complex<float>),
          traceTiming.borrowed?"borrowed":"owned",
          traceMilliseconds(traceTiming.backendEnd,traceTiming.begin),
          traceMilliseconds(traceTiming.copyBegin,traceTiming.backendEnd),
          traceMilliseconds(traceTiming.copyEnd,traceTiming.copyBegin),total);
      }
    }
    munmap(shared,h.bytes);return 0;
  }catch(const std::exception&e){
    Reply r;r.status=1;std::strncpy(r.reason,e.what(),sizeof(r.reason)-1);
    try{io(3,&r,sizeof(r),true);}catch(...){}
    return 2;
  }
}
}
