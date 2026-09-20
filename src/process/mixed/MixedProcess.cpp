#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "MixedProcess.h"
#include "data/IqData.h"
#include "process/ambiguity/GpuProcess.h"
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <string>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <stdexcept>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#if defined(__linux__)
#include <linux/memfd.h>
#endif
extern char** environ;
namespace blah2::mixed {
namespace {
using Clock=std::chrono::steady_clock;
void sendMessage(int fd,const Message& message){
  ssize_t count;
  do {count=send(fd,&message,sizeof(message),MSG_NOSIGNAL|MSG_DONTWAIT);}
  while(count<0&&errno==EINTR);
  if(count!=sizeof(message))throw std::runtime_error("Mixed worker connection failed");
}
Message receiveMessage(int fd,unsigned timeoutMs){
  const auto deadline=Clock::now()+std::chrono::milliseconds(timeoutMs);
  for(;;){
    const auto left=std::chrono::duration_cast<std::chrono::milliseconds>(deadline-Clock::now()).count();
    if(left<=0)throw std::runtime_error("Mixed worker timed out");
    pollfd p{fd,POLLIN,0};
    const int result=poll(&p,1,std::min<int64_t>(left,INT32_MAX));
    if(result<0&&errno==EINTR)continue;
    if(result<0)throw std::runtime_error("Mixed worker poll failed");
    if(!result)continue;
    Message response;
    const auto count=recv(fd,&response,sizeof(response),MSG_TRUNC|MSG_DONTWAIT);
    if(count<0&&(errno==EINTR||errno==EAGAIN))continue;
    if(count!=sizeof(response)||response.protocol!=version||
       response.bytes!=sharedBytes||response.sampleCount!=samples||
       response.rowCount!=rows||response.delayCount!=delays)
      throw std::runtime_error("Mixed worker response invalid");
    response.reason[sizeof(response.reason)-1]='\0';
    return response;
  }
}
void reap(pid_t process)noexcept{
  if(process<=0)return;
  int status=0;pid_t result;
  do{result=waitpid(process,&status,WNOHANG);}while(result<0&&errno==EINTR);
  if(result!=0)return;
  kill(process,SIGKILL);
  const auto deadline=Clock::now()+std::chrono::milliseconds(100);
  do{
    result=waitpid(process,&status,WNOHANG);
    if(result==process||(result<0&&errno!=EINTR))return;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }while(Clock::now()<deadline);
  try{std::thread([process]{int code;while(waitpid(process,&code,0)<0&&errno==EINTR){}}).detach();}
  catch(...){}
}
}
struct Process::Impl {
  int socket=-1,memory=-1;
  pid_t process=-1;
  std::complex<double>* shared=nullptr;
  uint64_t sequence=0;
  bool prepared=false;
  Operation preparedOperation=Operation::frame;
  double preparationMs=0;
  ProcessOptions options;
  std::ofstream timingLog;
  void stop()noexcept{
    prepared=false;
    if(socket>=0){close(socket);socket=-1;}
    reap(process);process=-1;
    if(shared){munmap(shared,sharedBytes);shared=nullptr;}
    if(memory>=0){close(memory);memory=-1;}
  }
  explicit Impl(ProcessOptions selected):options(std::move(selected)){
#if !defined(__linux__)
    throw std::runtime_error("Mixed V3D worker requires Linux");
#else
    int childSocket=-1,childMemory=-1;
    posix_spawn_file_actions_t actions;bool actionsReady=false;
    try{
      if(const char* path=std::getenv("VECTORWARP_MIXED_PROCESS_LOG")) {
        timingLog.open(path,std::ios::app);
        if(!timingLog)throw std::runtime_error("Cannot open mixed process timing log");
      }
      int sockets[2];
      if(socketpair(AF_UNIX,SOCK_SEQPACKET|SOCK_CLOEXEC,0,sockets))
        throw std::runtime_error("Cannot create mixed worker connection");
      socket=sockets[0];childSocket=fcntl(sockets[1],F_DUPFD_CLOEXEC,10);close(sockets[1]);
      memory=memfd_create("blah2-mixed-frame",MFD_CLOEXEC|MFD_ALLOW_SEALING);
      if(childSocket<0||memory<0||ftruncate(memory,sharedBytes)||
          fcntl(memory,F_ADD_SEALS,F_SEAL_GROW|F_SEAL_SHRINK|F_SEAL_SEAL)<0)
        throw std::runtime_error("Cannot allocate mixed shared frame");
      void* mapping=mmap(nullptr,sharedBytes,PROT_READ|PROT_WRITE,MAP_SHARED,memory,0);
      if(mapping==MAP_FAILED)throw std::runtime_error("Cannot map mixed shared frame");
      shared=static_cast<std::complex<double>*>(mapping);
      childMemory=fcntl(memory,F_DUPFD_CLOEXEC,10);
      if(childMemory<0||posix_spawn_file_actions_init(&actions))
        throw std::runtime_error("Cannot prepare mixed worker");
      actionsReady=true;
      if(posix_spawn_file_actions_adddup2(&actions,childSocket,3)||
          posix_spawn_file_actions_adddup2(&actions,childMemory,4))
        throw std::runtime_error("Cannot prepare mixed worker descriptors");
#if defined(__GLIBC__) && defined(__GLIBC_MINOR__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 34))
      if(posix_spawn_file_actions_addclosefrom_np(&actions,5))
        throw std::runtime_error("Cannot isolate mixed worker descriptors");
#else
      DIR* descriptors=opendir("/proc/self/fd");
      if(!descriptors)throw std::runtime_error("Cannot inspect mixed worker descriptors");
      while(const auto* entry=readdir(descriptors)){
        const int fd=std::atoi(entry->d_name);
        if(fd>4)posix_spawn_file_actions_addclose(&actions,fd);
      }
      closedir(descriptors);
#endif
      if(!options.startupMs||!options.frameMs||options.frameMs>5000)
        throw std::invalid_argument("Invalid mixed worker timeout");
      std::string executable=options.executable.empty()?
        gpuSiblingPath("blah2-mixed-worker"):options.executable;
      char* argv[]={executable.data(),
        options.argument.empty()?nullptr:options.argument.data(),nullptr};
      const int error=posix_spawn(&process,executable.c_str(),&actions,nullptr,argv,environ);
      if(error){process=-1;throw std::runtime_error("Mixed worker is unavailable");}
      close(childSocket);childSocket=-1;close(childMemory);childMemory=-1;
      posix_spawn_file_actions_destroy(&actions);actionsReady=false;
      Message init;sendMessage(socket,init);
      const auto response=receiveMessage(socket,options.startupMs);
      if(response.operation==Operation::failure)throw std::runtime_error(response.reason);
      if(response.operation!=Operation::ready||response.sequence)
        throw std::runtime_error("Mixed worker startup response invalid");
    }catch(...){
      if(actionsReady)posix_spawn_file_actions_destroy(&actions);
      if(childSocket>=0)close(childSocket);
      if(childMemory>=0)close(childMemory);
      stop();throw;
    }
#endif
  }
  ~Impl(){stop();}
  void prepare_paired_i16(const int16_t* input,uint32_t count,
      IqData& reference,IqData& surveillance,uint32_t referenceChannel){
    prepared=false;
    if(!shared||socket<0||count!=samples||referenceChannel>1)
      throw std::invalid_argument("Mixed paired CPI dimensions changed");
    const auto begin=timingLog.is_open()?Clock::now():Clock::time_point{};
    if(referenceChannel==0)
      reference.assign_paired_i16(input,count,surveillance);
    else
      surveillance.assign_paired_i16(input,count,reference);
    // This frame operation uses the mapping as packed bytes, not Complexs.
    std::memcpy(static_cast<void*>(shared),input,pairedInputBytes);
    preparedOperation=referenceChannel==0?Operation::pairedFrameReference0:
      Operation::pairedFrameReference1;
    preparationMs=timingLog.is_open()?
      std::chrono::duration<double,std::milli>(Clock::now()-begin).count():0;
    prepared=true;
  }
  void run(const std::deque<std::complex<double>>& reference,
      const std::deque<std::complex<double>>& surveillance,
      std::vector<std::complex<double>>& map,
      std::vector<std::complex<double>>& tail){
    prepared=false;
    if(!shared||socket<0||reference.size()!=samples||surveillance.size()!=samples)
      throw std::invalid_argument("Mixed worker CPI dimensions changed");
    // Neither input deque is retired until all output validation completes.
    const auto begin=timingLog.is_open()?Clock::now():Clock::time_point{};
    std::copy_n(reference.begin(),samples,shared);
    std::copy_n(surveillance.begin(),samples,shared+samples);
    const auto copied=timingLog.is_open()?Clock::now():Clock::time_point{};
    exchange(map,tail,begin,copied,Operation::frame);
  }
  void run_prepared(std::vector<std::complex<double>>& map,
      std::vector<std::complex<double>>& tail){
    if(!prepared||!shared||socket<0)
      throw std::logic_error("Mixed worker has no prepared CPI");
    // Consume before sending: a failed request must never reuse this frame.
    prepared=false;
    const auto begin=timingLog.is_open()?Clock::now():Clock::time_point{};
    exchange(map,tail,begin,begin,preparedOperation);
  }
  void exchange(std::vector<std::complex<double>>& map,
      std::vector<std::complex<double>>& tail,
      Clock::time_point begin,Clock::time_point copied,Operation operation){
    try{
      Message request;request.operation=operation;request.sequence=++sequence;
      sendMessage(socket,request);
      const auto response=receiveMessage(socket,options.frameMs);
      const auto received=timingLog.is_open()?Clock::now():Clock::time_point{};
      if(response.operation==Operation::failure)throw std::runtime_error(response.reason);
      if(response.operation!=Operation::ready||response.sequence!=sequence)
        throw std::runtime_error("Mixed worker frame sequence invalid");
      const auto* result=shared+inputSamples;
      if(!std::all_of(result,result+mapSamples+tailSamples,[](const auto& v){
          return std::isfinite(v.real())&&std::isfinite(v.imag());}))
        throw std::runtime_error("Mixed worker returned non-finite map or tail");
      std::vector<std::complex<double>> candidate(result,result+mapSamples);
      std::vector<std::complex<double>> candidateTail(result+mapSamples,
        result+mapSamples+tailSamples);
      map=std::move(candidate);tail=std::move(candidateTail);
      if(timingLog.is_open()) {
        const auto end=Clock::now();
        const auto elapsed=[](auto a,auto b){
          return std::chrono::duration<double,std::milli>(b-a).count();
        };
        timingLog<<"{\"sequence\":"<<sequence
          <<",\"packed_input\":"<<(operation!=Operation::frame?"true":"false")
          <<",\"decode_and_pack_ms\":"<<(operation!=Operation::frame?preparationMs:0)
          <<",\"input_copy_ms\":"<<elapsed(begin,copied)
          <<",\"worker_roundtrip_ms\":"<<elapsed(copied,received)
          <<",\"result_validate_copy_ms\":"<<elapsed(received,end)
          <<",\"total_ms\":"<<elapsed(begin,end)<<"}\n";
      }
    }catch(...){stop();throw;}
  }
};
Process::Process(ProcessOptions options):impl_(std::make_unique<Impl>(std::move(options))){}
Process::~Process()=default;
void Process::run(const std::deque<std::complex<double>>& reference,
    const std::deque<std::complex<double>>& surveillance,
    std::vector<std::complex<double>>& map,
    std::vector<std::complex<double>>& tail){impl_->run(reference,surveillance,map,tail);}
void Process::prepare_paired_i16(const int16_t* input,uint32_t count,
    IqData& reference,IqData& surveillance,uint32_t referenceChannel){
  impl_->prepare_paired_i16(input,count,reference,surveillance,referenceChannel);
}
void Process::run_prepared(std::vector<std::complex<double>>& map,
    std::vector<std::complex<double>>& tail){impl_->run_prepared(map,tail);}
}
