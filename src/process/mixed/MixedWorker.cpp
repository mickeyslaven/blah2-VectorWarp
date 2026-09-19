#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "MixedProtocol.h"
#include "process/ambiguity/Ambiguity.h"
#include "process/clutter/WienerHopf.h"
#include <algorithm>
#include <cerrno>
#include <cmath>
#include <csignal>
#include <complex>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <fftw3.h>
#include <fstream>
#include <stdexcept>
#include <sys/mman.h>
#if defined(__linux__)
#include <sys/prctl.h>
#endif
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>
namespace {
using namespace blah2::mixed;
using Complex=std::complex<double>;
Message receive(){
  Message message;
  ssize_t count;
  do{count=recv(3,&message,sizeof(message),MSG_TRUNC);}while(count<0&&errno==EINTR);
  if(count!=sizeof(message)||message.protocol!=version||
     message.bytes!=sharedBytes||message.sampleCount!=samples||
     message.rowCount!=rows||message.delayCount!=delays)
    throw std::runtime_error("Invalid mixed worker message");
  return message;
}
void sendMessage(Message message){
  ssize_t count;
  do{count=send(3,&message,sizeof(message),MSG_NOSIGNAL);}while(count<0&&errno==EINTR);
  if(count!=sizeof(message))throw std::runtime_error("Mixed worker reply failed");
}
void setting(const char* key,const char* value){
  if(setenv(key,value,1))throw std::runtime_error("Mixed worker configuration failed");
}
}
int main(){
  using namespace blah2::mixed;
  Complex* shared=nullptr;
  try{
#if !defined(__linux__)
    throw std::runtime_error("Mixed V3D worker requires Linux");
#else
    const pid_t parent=getppid();
    if(parent==1||prctl(PR_SET_PDEATHSIG,SIGKILL)||getppid()!=parent)
      throw std::runtime_error("Mixed worker parent is unavailable");
#endif
    const auto init=receive();
    if(init.operation!=Operation::initialize||init.sequence)
      throw std::runtime_error("Invalid mixed worker initialization");
    struct stat memory{};
    if(fstat(4,&memory)||uint64_t(memory.st_size)!=sharedBytes)
      throw std::runtime_error("Mixed worker shared memory size changed");
    void* mapping=mmap(nullptr,sharedBytes,PROT_READ|PROT_WRITE,MAP_SHARED,4,0);
    if(mapping==MAP_FAILED)throw std::runtime_error("Mixed worker map failed");
    shared=static_cast<Complex*>(mapping);
    // Frozen qualified geometry and shares are child-owned; application mode
    // and caller environment cannot silently change the mixed algorithm.
    setting("VECTORWARP_FFTW_PLAN","measure");
    setting("VECTORWARP_CLUTTER_WORKERS","2");
    setting("VECTORWARP_GPU_BLOCKED_CORRELATION","1");
    setting("VECTORWARP_GPU_FIR_PERCENT","50");
    setting("VECTORWARP_GPU_FIR_FFT","2048");
    setting("VECTORWARP_GPU_FIR_EARLY_REFERENCE","1");
    setting("VECTORWARP_GPU_FIR_ASYNC_FINISH","1");
    setting("VECTORWARP_GPU_FIR_READBACK","cached");
    setting("VECTORWARP_GPU_PARTIAL_AMBIGUITY","1");
    setting("VECTORWARP_BENCH_RANGE_WORKERS","2");
    setting("BLAH2_BENCH_RANGE_POWER2","1");
    // The qualified benchmark configured FFTW's global planner count before
    // Wiener construction.  Wiener uses it to cap persistent CPU helpers.
    if(fftw_init_threads()==0)throw std::runtime_error("Mixed worker FFTW thread initialization failed");
    fftw_plan_with_nthreads(4);
    if(fftw_planner_nthreads()!=4)
      throw std::runtime_error("Mixed worker FFTW thread setting changed");
    WienerHopf clutter(-10,400,samples);
    if(clutter.cpu_worker_slots()!=2)
      throw std::runtime_error("Mixed worker clutter CPU worker count changed");
    Ambiguity ambiguity(-10,400,-300,300,2000000,samples,true);
    if(ambiguity.get_n_doppler_bins()!=rows||ambiguity.get_n_delay_bins()!=delays||
        ambiguity.get_n_corr()!=samples/rows||ambiguity.get_nfft()!=4096)
      throw std::runtime_error("Mixed worker geometry changed");
    Message ready;ready.operation=Operation::ready;sendMessage(ready);
    std::ofstream timingLog;
    if(const char* path=std::getenv("VECTORWARP_MIXED_WORKER_LOG")){
      timingLog.open(path,std::ios::app);
      if(!timingLog)throw std::runtime_error("Mixed worker timing log open failed");
      timingLog << "{\"event\":\"startup\",\"fftw_threads\":"
        << fftw_planner_nthreads() << ",\"clutter_cpu_worker_slots\":"
        << clutter.cpu_worker_slots() << "}\n" << std::flush;
    }
    std::vector<Complex> completed(mapSamples+tailSamples);
    uint64_t sequence=0;
    for(;;){
      const auto request=receive();
      if(request.operation==Operation::quit)return 0;
      const bool fp64Frame=request.operation==Operation::frame;
      const bool pairedReference0=request.operation==Operation::pairedFrameReference0;
      const bool pairedReference1=request.operation==Operation::pairedFrameReference1;
      if((!fp64Frame&&!pairedReference0&&!pairedReference1)||request.sequence!=sequence+1)
        throw std::runtime_error("Mixed worker frame sequence changed");
      sequence=request.sequence;
      const auto frameStarted=timingLog.is_open() ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
      const auto inputPrepared=timingLog.is_open() ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
      const bool clutterAccepted=fp64Frame ?
        clutter.process_borrowed_input(shared,samples,shared+samples,samples,true) :
        clutter.process_borrowed_paired_i16(reinterpret_cast<const int16_t*>(shared),samples,
          pairedReference0 ? 0 : 1);
      if(!clutterAccepted)
        throw std::runtime_error("Mixed clutter solve rejected CPI");
      const auto clutterFinished=timingLog.is_open() ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
      const auto view=clutter.filtered_view();
      const auto* map=ambiguity.process_borrowed(view.rotatedReference,view.samples,
        view.filteredSurveillance,view.samples,view.delayMin);
      const auto ambiguityFinished=timingLog.is_open() ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
      uint64_t output=0;
      if(map->data.size()!=rows)throw std::runtime_error("Mixed map rows changed");
      for(const auto& row:map->data){
        if(row.size()!=delays)throw std::runtime_error("Mixed map columns changed");
        for(const auto& value:row){
          if(!std::isfinite(value.real())||!std::isfinite(value.imag()))
            throw std::runtime_error("Mixed map is non-finite");
          completed[output++]=value;
        }
      }
      if(output!=mapSamples)throw std::runtime_error("Mixed map rows changed");
      for(uint32_t i=usedSamples;i<samples;++i){
        const auto value=view.filteredSurveillance[i];
        if(!std::isfinite(value.real())||!std::isfinite(value.imag()))
          throw std::runtime_error("Mixed surveillance tail is non-finite");
        completed[output++]=value;
      }
      if(output!=completed.size())throw std::runtime_error("Mixed output shape changed");
      std::copy(completed.begin(),completed.end(),shared+inputSamples);
      const auto published=timingLog.is_open() ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
      Message response;response.operation=Operation::ready;response.sequence=sequence;
      sendMessage(response);
      if(timingLog.is_open()){
        auto ms=[](auto first,auto last){return std::chrono::duration<double,std::milli>(last-first).count();};
        timingLog << "{\"sequence\":" << sequence
          << ",\"input_preparation_ms\":" << ms(frameStarted,inputPrepared)
          << ",\"clutter_ms\":" << ms(inputPrepared,clutterFinished)
          << ",\"ambiguity_ms\":" << ms(clutterFinished,ambiguityFinished)
          << ",\"publication_ms\":" << ms(ambiguityFinished,published)
          << "}\n" << std::flush;
      }
    }
  }catch(const std::exception& error){
    Message response;response.operation=Operation::failure;
    std::snprintf(response.reason,sizeof(response.reason),"%s",error.what());
    try{sendMessage(response);}catch(...){}
    if(shared)munmap(shared,sharedBytes);
    return 1;
  }
}
