#include "process/ambiguity/GpuWidebandProcess.h"
#include "process/ambiguity/GpuWidebandFallback.h"
#include "process/ambiguity/GpuWorkerLifetime.h"
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <chrono>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <sstream>
#include <fcntl.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <signal.h>
#include <poll.h>
#include <unistd.h>
#include <thread>

namespace {
using FloatChannels=std::vector<std::vector<std::complex<float>>>;
void verifyLeased(const std::vector<blah2::wideband_gpu::LeasedLaneOutput>& output,
  const FloatChannels& input,uint64_t first,
  const blah2::wideband_gpu::Config& config)
{
  REQUIRE(output.size()==config.lanes.size());
  for(size_t lane=0;lane<output.size();++lane){
    const auto& actual=output[lane];const auto& geometry=config.lanes[lane];
    const size_t remainder=(geometry.decimation-first%geometry.decimation)%geometry.decimation;
    const size_t count=remainder<input.front().size()?
      1+(input.front().size()-1-remainder)/geometry.decimation:0;
    REQUIRE(actual.frequencyHz==geometry.frequencyHz);
    REQUIRE(actual.inputSamplesPerOutput==geometry.decimation);
    REQUIRE(actual.hasFirstInputSample==(count!=0));
    REQUIRE(actual.firstInputSample==(count?first+remainder:0));
    REQUIRE(actual.samples.owner);
    REQUIRE(std::holds_alternative<blah2::iq::BorrowedSamples::FloatChannels>(
      actual.samples.channels));
    const auto& channels=std::get<blah2::iq::BorrowedSamples::FloatChannels>(
      actual.samples.channels);
    REQUIRE(channels.size()==input.size());
    for(size_t channel=0;channel<input.size();++channel){
      REQUIRE(channels[channel].size()==count);
      for(size_t sample=0;sample<count;++sample){
        const auto& expected=input[channel][remainder+sample*geometry.decimation];
        REQUIRE(std::memcmp(&channels[channel][sample],&expected,
          sizeof(expected))==0);
      }
    }
  }
}
}

TEST_CASE("wideband recovery admits only transient worker transport loss")
{
  using blah2::wideband_gpu::recoverableWorkerFailure;
  REQUIRE(recoverableWorkerFailure(
    "Wideband GPU worker transport failed (worker killed by signal 9)"));
  REQUIRE(recoverableWorkerFailure("Wideband GPU worker timed out or failed"));
  REQUIRE(recoverableWorkerFailure(
    "Wideband GPU worker failed; recreate transport"));
  REQUIRE_FALSE(recoverableWorkerFailure(
    "Wideband GPU response sample position differs"));
  REQUIRE_FALSE(recoverableWorkerFailure(
    "Wideband GPU output shape invalid"));
  REQUIRE_FALSE(recoverableWorkerFailure("GPU numerical qualification failed"));
}

TEST_CASE("wideband parent rejects a legacy worker trace-policy wire tag")
{
  struct Environment {~Environment(){unsetenv("VW_MOCK_WIDEBAND_FAILURE");}} environment;
  REQUIRE(setenv("VW_MOCK_WIDEBAND_FAILURE","legacy-reply",1)==0);
  const blah2::wideband_gpu::Config config{8000,2,8,
    {{100000000,2,0,std::vector<double>(9,1.0/9)}}};
  blah2::GpuProcessOptions options;options.executable=GPU_WIDEBAND_MOCK;
  options.argument="--mock";options.warmup=false;
  REQUIRE_THROWS_WITH(blah2::wideband_gpu::createProcess(config,"mock",options),
    "Wideband GPU worker rejected request");
}

TEST_CASE("wideband shared output leases preserve concurrent frames and bound reuse")
{
  struct Environment {
    ~Environment(){unsetenv("VW_MOCK_WIDEBAND_BORROWED");}
  } environment;
  const blah2::wideband_gpu::Config config{240000,3,32,{
    {100000000,4,-30000,std::vector<double>(97,1.0/97)},
    {101000000,8,30000,std::vector<double>(193,1.0/193)}}};
  blah2::GpuProcessOptions options;options.executable=GPU_WIDEBAND_MOCK;
  options.argument="--mock";options.warmup=true;
  FloatChannels input(config.channels,std::vector<std::complex<float>>(31));
  for(size_t channel=0;channel<input.size();++channel)
    for(size_t sample=0;sample<input[channel].size();++sample)
      input[channel][sample]={float(100*channel+sample),-float(channel)};
  input[0][1]={-0.0f,std::numeric_limits<float>::quiet_NaN()};
  input[1][5]={std::numeric_limits<float>::infinity(),-0.0f};
  auto next=input;
  for(auto& channel:next)for(auto& sample:channel)sample={-17.0f,23.0f};
  const uint64_t first=(uint64_t(1)<<40)+3;
  for(const bool borrowedModule:{false,true}){
    CAPTURE(borrowedModule);
    if(borrowedModule)REQUIRE(setenv("VW_MOCK_WIDEBAND_BORROWED","1",1)==0);
    else unsetenv("VW_MOCK_WIDEBAND_BORROWED");
    auto process=blah2::wideband_gpu::createLeasedProcess(config,"mock",options);
    auto* leases=dynamic_cast<blah2::wideband_gpu::LeasedOutputBackend*>(process.get());
    REQUIRE(leases);REQUIRE(leases->supportsLeasedOutput());
    // Warmup and a qualification-style owned finish precede steady leasing.
    const auto qualified=process->process(input,first);
    REQUIRE(qualified.front().firstInputSample==first+1);
    process->reset();
    process->begin(input,first);
    auto prior=leases->finishLeasedFrame();verifyLeased(prior,input,first,config);
    const auto* priorAddress=std::get<blah2::iq::BorrowedSamples::FloatChannels>(
      prior.front().samples.channels).front().data();
    auto lastOwner=prior.front().samples.owner;
    std::weak_ptr<const void> lifetime=lastOwner;
    process->begin(next,first+31);
    REQUIRE(process->frameInFlight());
    REQUIRE_THROWS(process->process(input,first));
    REQUIRE_THROWS(process->reset());
    verifyLeased(prior,input,first,config); // The next worker may be writing now.
    auto completed=leases->finishLeasedFrame();
    verifyLeased(completed,next,first+31,config);
    REQUIRE(std::get<blah2::iq::BorrowedSamples::FloatChannels>(
      completed.front().samples.channels).front().data()!=priorAddress);
    verifyLeased(prior,input,first,config);
    const auto stagingBefore=process->lastTiming().stagingMs;
    REQUIRE_THROWS(process->begin(input,first+62));
    REQUIRE_THROWS(process->process(input,first+62));
    std::vector<std::deque<std::complex<double>>> storage(config.channels,
      std::deque<std::complex<double>>(31,{2,3}));
    std::vector<const std::deque<std::complex<double>>*> deques;
    for(const auto& channel:storage)deques.push_back(&channel);
    REQUIRE_THROWS(process->beginDeques(deques,first+62));
    REQUIRE_FALSE(process->frameInFlight());
    REQUIRE(process->lastTiming().stagingMs==stagingBefore);
    process->reset(); // Reset never clears slots or the bytes pinned by owners.
    verifyLeased(prior,input,first,config);
    verifyLeased(completed,next,first+31,config);
    prior.clear();
    REQUIRE_THROWS(process->begin(input,first+62)); // Copied owner still pins it.
    lastOwner.reset();REQUIRE(lifetime.expired());
    process->begin(input,first+62);
    auto reused=leases->finishLeasedFrame();
    verifyLeased(reused,input,first+62,config);
    REQUIRE(std::get<blah2::iq::BorrowedSamples::FloatChannels>(
      reused.front().samples.channels).front().data()==priorAddress);
    verifyLeased(completed,next,first+31,config);
    reused.clear();
    const auto owned=process->process(input,first+93); // Mixed API uses free slot.
    REQUIRE(owned.front().channels.size()==config.channels);
    verifyLeased(completed,next,first+31,config);
    const FloatChannels shortInput(config.channels,
      std::vector<std::complex<float>>(1,{11,-0.0f}));
    process->begin(shortInput,7);
    auto empty=leases->finishLeasedFrame();verifyLeased(empty,shortInput,7,config);
    empty.clear();
    const FloatChannels noInput(config.channels);
    process->begin(noInput,8);empty=leases->finishLeasedFrame();
    verifyLeased(empty,noInput,8,config);empty.clear();
    process.reset();verifyLeased(completed,next,first+31,config);
    // Completion and all copies of its owner may outlive transport destruction.
    auto delayed=completed;completed.clear();verifyLeased(delayed,next,first+31,config);
  }
}

TEST_CASE("wideband leases survive timeout crash malformed output and transport destruction")
{
  struct Environment {
    ~Environment(){unsetenv("VW_MOCK_WIDEBAND_BORROWED");
      unsetenv("VW_MOCK_WIDEBAND_FAILURE");}
  } environment;
  REQUIRE(setenv("VW_MOCK_WIDEBAND_BORROWED","1",1)==0);
  const blah2::wideband_gpu::Config config{8000,2,8,
    {{100000000,2,0,std::vector<double>(9,1.0/9)}}};
  blah2::GpuProcessOptions options;options.executable=GPU_WIDEBAND_MOCK;
  options.argument="--mock";options.warmup=false;options.frameMs=80;
  const FloatChannels input(config.channels,
    std::vector<std::complex<float>>(8,{3.0f,-7.0f}));
  for(const char* fault:{"borrowed-second-crash","borrowed-second-hang",
      "borrowed-second-malformed"}){
    CAPTURE(fault);REQUIRE(setenv("VW_MOCK_WIDEBAND_FAILURE",fault,1)==0);
    auto process=blah2::wideband_gpu::createLeasedProcess(config,"mock",options);
    auto* leases=dynamic_cast<blah2::wideband_gpu::LeasedOutputBackend*>(process.get());
    REQUIRE(leases);process->begin(input,0);
    const auto held=leases->finishLeasedFrame();verifyLeased(held,input,0,config);
    process->begin(input,8);REQUIRE_THROWS(leases->finishLeasedFrame());
    REQUIRE_FALSE(process->frameInFlight());
    REQUIRE_THROWS(process->begin(input,16));
    REQUIRE_THROWS(process->reset());
    verifyLeased(held,input,0,config);
    process.reset();verifyLeased(held,input,0,config);
  }
  // A syntactically successful but malformed wire reply must not grant a view.
  for(const char* fault:{"malformed-count","malformed-first","malformed-has",
      "malformed-total"}){
    CAPTURE(fault);REQUIRE(setenv("VW_MOCK_WIDEBAND_FAILURE",fault,1)==0);
    auto process=blah2::wideband_gpu::createLeasedProcess(config,"mock",options);
    auto* leases=dynamic_cast<blah2::wideband_gpu::LeasedOutputBackend*>(process.get());
    REQUIRE(leases);process->begin(input,1);
    REQUIRE_THROWS(leases->finishLeasedFrame());
    REQUIRE_THROWS(process->begin(input,9));
  }
}

TEST_CASE("wideband legacy transport cannot expose its single output slot")
{
  const blah2::wideband_gpu::Config config{8000,2,8,
    {{100000000,2,0,std::vector<double>(9,1.0/9)}}};
  blah2::GpuProcessOptions options;options.executable=GPU_WIDEBAND_MOCK;
  options.argument="--mock";options.warmup=false;
  auto process=blah2::wideband_gpu::createProcess(config,"mock",options);
  auto* leases=dynamic_cast<blah2::wideband_gpu::LeasedOutputBackend*>(process.get());
  REQUIRE(leases);REQUIRE_FALSE(leases->supportsLeasedOutput());
  process->begin(FloatChannels(2,std::vector<std::complex<float>>(8,{1,2})),0);
  REQUIRE_THROWS(leases->finishLeasedFrame());
  REQUIRE(process->frameInFlight());
  REQUIRE(process->finishFrame().front().channels.front().size()==4);
}

TEST_CASE("wideband worker rejects invalid mapping sizes and selected output slots before work")
{
  // Deliberately construct hostile wire requests, independently of Process's
  // reservation logic. Module ABI remains unchanged; wire v2 binds trace policy.
  constexpr uint32_t magic=0x42574702;
  struct Header{uint32_t tag=magic,inputRate=8000,channels=2,maxInput=8,lanes=1,traceFrames=0;
    uint64_t bytes=0;char device[96]{};};
  struct LaneWire{uint64_t frequency=100000000;uint32_t decimation=2,taps=9;double center=0;};
  struct Frame{uint32_t tag=magic,op=1,samples=8,reserved=0;uint64_t sequence=1,first=0;};
  struct Reply{uint32_t tag=magic,status=0,lanes=0;uint64_t sequence=0;
    uint32_t samples[blah2::wideband_gpu::maxLanes]{};
    uint64_t first[blah2::wideband_gpu::maxLanes]{};
    uint8_t has[blah2::wideband_gpu::maxLanes]{};char reason[256]{},device[256]{};};
  struct NeverProcess final:blah2::wideband_gpu::Backend {
    std::string deviceName()const override{return "wire-bounds";}
    void reset()override{}
    std::vector<blah2::wideband_gpu::LaneOutput> process(const FloatChannels&,uint64_t)override
      {throw std::runtime_error("unexpected backend work");}
  };
  for(const std::string fault:{"single-slot","double-slot","mapping","file","legacy-header"}){
    CAPTURE(fault);
    struct Fixture {
      FILE* file=std::tmpfile();int sockets[2]{-1,-1},childSocket=-1,childMemory=-1;
      pid_t child=-1;
      ~Fixture(){
        for(int fd:{sockets[0],sockets[1],childSocket,childMemory})if(fd>=0)close(fd);
        if(child>0){kill(child,SIGKILL);while(waitpid(child,nullptr,0)<0&&errno==EINTR){}}
        if(file)std::fclose(file);
      }
    } fixture;
    REQUIRE(fixture.file);REQUIRE(socketpair(AF_UNIX,SOCK_STREAM,0,fixture.sockets)==0);
    Header header;header.bytes=(16+(fault=="double-slot"?2:1)*8)*sizeof(std::complex<float>);
    if(fault=="legacy-header")header.tag=0x42574701;
    const auto fileBytes=header.bytes-(fault=="file"?1:0);
    if(fault=="mapping")++header.bytes;
    REQUIRE(ftruncate(fileno(fixture.file),off_t(fileBytes))==0);
    fixture.childSocket=fcntl(fixture.sockets[1],F_DUPFD,10);
    fixture.childMemory=fcntl(fileno(fixture.file),F_DUPFD,10);
    REQUIRE(fixture.childSocket>=0);REQUIRE(fixture.childMemory>=0);
    const std::string expectedParent=std::to_string(getpid());
    fixture.child=fork();REQUIRE(fixture.child>=0);
    if(fixture.child==0){
      if(setenv(blah2::gpu_worker_lifetime::parentVariable,expectedParent.c_str(),1))_exit(90);
      if(dup2(fixture.childSocket,3)<0||dup2(fixture.childMemory,4)<0)_exit(90);
      _exit(blah2::wideband_gpu::runWorker([](const auto&,const auto&)
        ->std::unique_ptr<blah2::wideband_gpu::Backend>{return std::make_unique<NeverProcess>();}));
    }
    close(fixture.sockets[1]);fixture.sockets[1]=-1;
    close(fixture.childSocket);fixture.childSocket=-1;
    close(fixture.childMemory);fixture.childMemory=-1;
    const auto exchange=[&](void* buffer,size_t bytes,bool writing){
      auto* data=static_cast<char*>(buffer);
      while(bytes){
        if(!writing){pollfd ready{fixture.sockets[0],POLLIN,0};
          REQUIRE(poll(&ready,1,1000)>0);}
        const auto count=writing?send(fixture.sockets[0],data,bytes,0):
          read(fixture.sockets[0],data,bytes);
        REQUIRE(count>0);data+=count;bytes-=size_t(count);}
    };
    LaneWire lane;double taps[9]{};
    exchange(&header,sizeof(header),true);
    if(fault!="legacy-header"){
      exchange(&lane,sizeof(lane),true);exchange(taps,sizeof(taps),true);
    }
    Reply reply;exchange(&reply,sizeof(reply),false);
    if(fault=="single-slot"||fault=="double-slot"){
      REQUIRE(reply.status==0);
      Frame frame;frame.reserved=fault=="single-slot"?1:2;
      exchange(&frame,sizeof(frame),true);exchange(&reply,sizeof(reply),false);
    }
    REQUIRE(reply.status==1);
    const std::string reason=reply.reason;
    REQUIRE(reason.find(fault=="mapping"?"mapping size":fault=="file"?
      "file size":fault=="legacy-header"?"header invalid":"frame shape")!=std::string::npos);
    int status=0;pid_t exited;
    do{exited=waitpid(fixture.child,&status,0);}while(exited<0&&errno==EINTR);
    REQUIRE(exited==fixture.child);fixture.child=-1;
    REQUIRE(WIFEXITED(status));REQUIRE(WEXITSTATUS(status)==2);
  }
}

TEST_CASE("wideband worker isolates inherited descriptors even with closed standard streams")
{
  struct Fixture {
    int descriptor=-1;
    ~Fixture(){if(descriptor>=0)close(descriptor);unsetenv("VW_MOCK_WIDEBAND_INHERITED_FD");}
  } fixture;
  const int source=open("/dev/null",O_RDONLY);
  REQUIRE(source>=0);
  fixture.descriptor=fcntl(source,F_DUPFD,100);close(source);
  REQUIRE(fixture.descriptor>=100);
  REQUIRE(setenv("VW_MOCK_WIDEBAND_INHERITED_FD",
    std::to_string(fixture.descriptor).c_str(),1)==0);
  const auto exercise=[] {
    blah2::wideband_gpu::Config config{8000,2,8,
      {{100000000,2,0,std::vector<double>(9,1.0/9)}}};
    blah2::GpuProcessOptions options;options.executable=GPU_WIDEBAND_MOCK;
    options.argument="--mock";options.warmup=false;
    auto process=blah2::wideband_gpu::createProcess(config,"mock",options);
    return process->process(std::vector<std::vector<std::complex<float>>>(2,
      std::vector<std::complex<float>>(8)),0).size()==1;
  };
  REQUIRE(exercise());
  REQUIRE(fcntl(fixture.descriptor,F_GETFD)>=0); // Parent ownership is unchanged.
  const pid_t child=fork();
  REQUIRE(child>=0);
  if(child==0){
    close(0);close(1);close(2);
    if(setenv("VW_MOCK_WIDEBAND_CLOSED_STD","1",1))_exit(2);
    try{_exit(exercise()?0:3);}catch(...){_exit(4);}
  }
  int status=0;pid_t result;
  do{result=waitpid(child,&status,0);}while(result<0&&errno==EINTR);
  REQUIRE(result==child);
  REQUIRE(WIFEXITED(status));
  REQUIRE(WEXITSTATUS(status)==0);
}

TEST_CASE("wideband worker transports one capture to independent lanes")
{
  blah2::wideband_gpu::Config config{240000,5,64,{
    {100000000,4,-30000,std::vector<double>(97,1.0/97)},
    {101000000,8,30000,std::vector<double>(193,1.0/193)}}};
  blah2::GpuProcessOptions options;options.executable=GPU_WIDEBAND_MOCK;
  options.argument="--mock";options.warmup=false;
  auto process=blah2::wideband_gpu::createProcess(config,"mock",options);
  REQUIRE(process->deviceName()=="mock-wideband");
  std::vector<std::vector<std::complex<float>>> input(5,
    std::vector<std::complex<float>>(31));
  for(size_t c=0;c<input.size();++c)for(size_t i=0;i<input[c].size();++i)
    input[c][i]={float(100*c+i),float(c)};
  std::vector<std::deque<std::complex<double>>> deques(5);
  std::vector<const std::deque<std::complex<double>>*> views;
  for(const auto& channel:input){deques[views.size()].insert(
    deques[views.size()].end(),channel.begin(),channel.end());
    views.push_back(&deques[views.size()]);}
  auto output=process->processDeques(views,3);
  REQUIRE(output.size()==2);
  REQUIRE(output[0].hasFirstInputSample);
  REQUIRE(output[0].firstInputSample==4);
  REQUIRE(output[0].channels.size()==5);
  REQUIRE(output[0].channels[3].front()==input[3][1]);
  REQUIRE(output[1].firstInputSample==8);
  REQUIRE(output[1].channels[4].size()==4);
  process->reset();
  auto after=process->process(input,1000);
  REQUIRE(after[0].firstInputSample==1000);
}

TEST_CASE("wideband worker rejects changing capture shape")
{
  blah2::wideband_gpu::Config config{8000,2,8,
    {{100000000,2,0,std::vector<double>(9,1.0/9)}}};
  blah2::GpuProcessOptions options;options.executable=GPU_WIDEBAND_MOCK;
  options.argument="--mock";options.warmup=false;
  auto process=blah2::wideband_gpu::createProcess(config,"mock",options);
  REQUIRE_THROWS(process->process({std::vector<std::complex<float>>(4)},0));
  REQUIRE_THROWS(process->process(
    std::vector<std::vector<std::complex<float>>>(2,
      std::vector<std::complex<float>>(9)),0));
}

TEST_CASE("wideband parent rejects malformed worker output geometry")
{
  blah2::wideband_gpu::Config config{8000,2,8,
    {{100000000,2,0,std::vector<double>(9,1.0/9)}}};
  blah2::GpuProcessOptions options;options.executable=GPU_WIDEBAND_MOCK;
  options.argument="--mock";options.warmup=false;
  const std::vector<std::vector<std::complex<float>>> input(2,
    std::vector<std::complex<float>>(8));
  for(const char* mode:{"malformed-count","malformed-first",
      "malformed-has","malformed-total"})
  {
    setenv("VW_MOCK_WIDEBAND_FAILURE",mode,1);
    auto process=blah2::wideband_gpu::createProcess(config,"mock",options);
    REQUIRE_THROWS(process->process(input,1));
  }
  unsetenv("VW_MOCK_WIDEBAND_FAILURE");
}

TEST_CASE("wideband worker timeout and loss fail closed")
{
  blah2::wideband_gpu::Config config{8000,2,8,
    {{100000000,2,0,std::vector<double>(9,1.0/9)}}};
  blah2::GpuProcessOptions options;options.executable=GPU_WIDEBAND_MOCK;
  options.argument="--mock";options.frameMs=40;options.warmup=false;
  for(const char* mode:{"hang","partial"}){setenv("VW_MOCK_WIDEBAND_FAILURE",mode,1);
    auto process=blah2::wideband_gpu::createProcess(config,"mock",options);
    REQUIRE_THROWS(process->process(std::vector<std::vector<std::complex<float>>>(
      2,std::vector<std::complex<float>>(8)),0));}
  setenv("VW_MOCK_WIDEBAND_FAILURE","crash",1);
  auto process=blah2::wideband_gpu::createProcess(config,"mock",options);
  try{process->process(std::vector<std::vector<std::complex<float>>>(2,
      std::vector<std::complex<float>>(8)),0);FAIL("worker crash accepted");}
  catch(const std::exception& error){REQUIRE(std::string(error.what()).find(
      "worker exited 7")!=std::string::npos);}
  unsetenv("VW_MOCK_WIDEBAND_FAILURE");
  options.frameMs=0;
  REQUIRE_THROWS(blah2::wideband_gpu::createProcess(config,"mock",options));
}

TEST_CASE("wideband repeated startup failure is bounded and recoverable")
{
  blah2::wideband_gpu::Config config{8000,2,8,
    {{100000000,2,0,std::vector<double>(9,1.0/9)}}};
  blah2::GpuProcessOptions options;options.executable=GPU_WIDEBAND_MOCK;
  options.argument="--mock";options.startupMs=20;options.warmup=false;
  const auto begin=std::chrono::steady_clock::now();
  for(const char* mode:{"startup-hang","startup-crash","startup-hang",
      "startup-crash"}){setenv("VW_MOCK_WIDEBAND_FAILURE",mode,1);
    REQUIRE_THROWS(blah2::wideband_gpu::createProcess(config,"mock",options));}
  unsetenv("VW_MOCK_WIDEBAND_FAILURE");
  REQUIRE(std::chrono::steady_clock::now()-begin<std::chrono::seconds(2));
  // Keep the deliberately tiny deadline above for the bounded-failure check,
  // but do not require a healthy process spawn to finish in 20 ms on slower
  // Intel CI hosts.
  options.startupMs=200;
  auto process=blah2::wideband_gpu::createProcess(config,"mock",options);
  REQUIRE(process->deviceName()=="mock-wideband");
}

TEST_CASE("wideband startup deadline includes a backpressured filter upload")
{
  struct Environment {~Environment(){unsetenv("VW_MOCK_WIDEBAND_FAILURE");}} environment;
  blah2::wideband_gpu::Config config{8000,2,8,{}};
  // Eight MiB cannot fit in the startup socket while the child is not reading.
  for(unsigned lane=0;lane<8;++lane)
    config.lanes.push_back({100000000+lane,2,0,
      std::vector<double>(blah2::wideband_gpu::maxFilterTaps,1e-6)});
  blah2::GpuProcessOptions options;options.executable=GPU_WIDEBAND_MOCK;
  options.argument="--mock";options.startupMs=40;options.warmup=false;
  REQUIRE(setenv("VW_MOCK_WIDEBAND_FAILURE","startup-no-read",1)==0);
  const auto started=std::chrono::steady_clock::now();
  REQUIRE_THROWS(blah2::wideband_gpu::createProcess(config,"mock",options));
  REQUIRE(std::chrono::steady_clock::now()-started<std::chrono::milliseconds(700));
  // A slow reader with remaining startup budget still receives every tap.
  REQUIRE(setenv("VW_MOCK_WIDEBAND_FAILURE","startup-hang",1)==0);
  options.startupMs=2000;
  auto process=blah2::wideband_gpu::createProcess(config,"mock",options);
  REQUIRE(process->deviceName()=="mock-wideband");
}

TEST_CASE("optional wideband failure keeps CPU ready without changing lane mode")
{
  const uint64_t first=1234;bool reset=false;uint64_t cpuFirst=0;
  const bool gpu=blah2::wideband_gpu::attemptOptionalWithFallback(
    []{throw std::runtime_error("device lost");},
    [&](const std::exception&){reset=true;});
  if(!gpu)cpuFirst=first;
  REQUIRE_FALSE(gpu);REQUIRE(reset);REQUIRE(cpuFirst==first);
  reset=false;
  REQUIRE(blah2::wideband_gpu::attemptOptionalWithFallback([]{},
    [&](const std::exception&){reset=true;}));
  REQUIRE_FALSE(reset); // A capable GPU is still preferred.
  // A failed CPU/reset handler is not swallowed or retried as another GPU fault.
  REQUIRE_THROWS_AS(blah2::wideband_gpu::attemptOptionalWithFallback(
    []{throw std::runtime_error("device lost");},
    [](const std::exception&){throw std::logic_error("CPU reset failed");}),
    std::logic_error);
}

TEST_CASE("wideband work longer than a CPI retains the bounded transport allowance")
{
  struct Environment {~Environment(){unsetenv("VW_MOCK_WIDEBAND_FAILURE");}} environment;
  REQUIRE(setenv("VW_MOCK_WIDEBAND_FAILURE","hang",1)==0);
  const blah2::wideband_gpu::Config config{8000,2,8,
    {{100000000,2,0,std::vector<double>(9,1.0/9)}}};
  blah2::GpuProcessOptions options;options.executable=GPU_WIDEBAND_MOCK;
  options.argument="--mock";options.warmup=false;
  REQUIRE(options.frameMs==5000);
  auto process=blah2::wideband_gpu::createProcess(config,"mock",options);
  const FloatChannels input(2,std::vector<std::complex<float>>(8,{3,-7}));
  // The mock completes in 250 ms; this capture is only one millisecond long.
  const auto output=process->process(input,1234);
  REQUIRE(output.front().firstInputSample==1234);
  REQUIRE(output.front().channels.front()==
    std::vector<std::complex<float>>(4,{3,-7}));
}

TEST_CASE("GPU construction has a bounded multi-CPI startup allowance")
{
  using blah2::gpuCpiStartupDeadlineMs;
  REQUIRE(gpuCpiStartupDeadlineMs(500,30000)==10000);
  REQUIRE(gpuCpiStartupDeadlineMs(100,30000)==5000);
  REQUIRE(gpuCpiStartupDeadlineMs(2000,30000)==30000);
  REQUIRE(gpuCpiStartupDeadlineMs(500,7000)==7000);
  REQUIRE(gpuCpiStartupDeadlineMs(500,30000,8)==5000);
  REQUIRE_THROWS_AS(gpuCpiStartupDeadlineMs(0,30000),std::invalid_argument);
  REQUIRE_THROWS_AS(gpuCpiStartupDeadlineMs(500,0),std::invalid_argument);
  REQUIRE_THROWS_AS(gpuCpiStartupDeadlineMs(500,30000,0),std::invalid_argument);
  REQUIRE_THROWS_AS(gpuCpiStartupDeadlineMs(500,30000,20,0),std::invalid_argument);
  using blah2::gpuCpiFrameDeadlineMs;
  REQUIRE(gpuCpiFrameDeadlineMs(500,5000)==1500);
  REQUIRE(gpuCpiFrameDeadlineMs(100,5000)==500);
  REQUIRE(gpuCpiFrameDeadlineMs(2000,5000)==5000);
  REQUIRE(gpuCpiFrameDeadlineMs(5000,5000)==5000);
  REQUIRE_THROWS_AS(gpuCpiFrameDeadlineMs(0,5000),std::invalid_argument);
  REQUIRE_THROWS_AS(gpuCpiFrameDeadlineMs(500,0),std::invalid_argument);
}

TEST_CASE("wideband warmup success cannot make first real frame failure fatal")
{
  struct Environment {
    ~Environment(){unsetenv("VW_MOCK_WIDEBAND_BORROWED");
      unsetenv("VW_MOCK_WIDEBAND_FAILURE");}
  } environment;
  REQUIRE(setenv("VW_MOCK_WIDEBAND_BORROWED","1",1)==0);
  const blah2::wideband_gpu::Config config{8000,2,8,
    {{100000000,2,0,std::vector<double>(9,1.0/9)}}};
  blah2::GpuProcessOptions options;options.executable=GPU_WIDEBAND_MOCK;
  options.argument="--mock";options.warmup=true;
  options.frameMs=80; // Short injected timeout keeps stuck-worker tests bounded.
  const FloatChannels input(2,std::vector<std::complex<float>>(8,{3,-7}));
  const uint64_t first=1234;
  for(const bool split:{false,true})
    for(const char* fault:{"","borrowed-second-hang","borrowed-second-crash",
        "borrowed-second-malformed"}){
      CAPTURE(split,fault);
      REQUIRE(setenv("VW_MOCK_WIDEBAND_FAILURE",fault,1)==0);
      // The constructor executes and resets a successful maximum-size warmup.
      // The injected second call represents failure after other lane workers
      // have been initialized; this must not escape the optional-stage boundary.
      auto process=blah2::wideband_gpu::createLeasedProcess(config,"mock",options);
      unsigned failures=0;uint64_t cpuFirst=0;FloatChannels cpuInput;
      std::vector<blah2::wideband_gpu::LaneOutput> output;
      const auto begin=std::chrono::steady_clock::now();
      const bool success=blah2::wideband_gpu::attemptOptionalWithFallback([&]{
        if(split){process->begin(input,first);output=process->finishFrame();}
        else output=process->process(input,first);
      },[&](const std::exception& error){
        REQUIRE_FALSE(std::string(error.what()).empty());
        ++failures;process.reset();
      });
      if(!success){cpuFirst=first;cpuInput=input;}
      REQUIRE(std::chrono::steady_clock::now()-begin<std::chrono::seconds(2));
      if(*fault){
        REQUIRE_FALSE(success);REQUIRE_FALSE(process);REQUIRE(failures==1);
        REQUIRE(output.empty());REQUIRE(cpuFirst==first);REQUIRE(cpuInput==input);
      }else{
        REQUIRE(success);REQUIRE(process);REQUIRE(failures==0);
        REQUIRE(output.front().firstInputSample==first);REQUIRE(cpuInput.empty());
      }
    }
}

TEST_CASE("wideband first real block qualification is fail closed")
{
  blah2::wideband_gpu::LaneOutput lane;lane.frequencyHz=100000000;
  lane.firstInputSample=16;lane.hasFirstInputSample=true;
  lane.inputSamplesPerOutput=8;lane.channels={{{1,2},{3,4}},{{5,6},{7,8}}};
  const std::vector<blah2::wideband_gpu::LaneOutput> cpu{lane};
  auto gpu=cpu;
  auto accepted=blah2::wideband_gpu::qualifyFirstBlock(cpu,gpu,120,500);
  REQUIRE(accepted.accepted);
  REQUIRE(accepted.status==blah2::wideband_gpu::QualificationStatus::passed);
  REQUIRE(accepted.worstRms==0);
  // Correct work remains GPU-selected even when it takes longer than one CPI.
  REQUIRE(blah2::wideband_gpu::qualifyFirstBlock(cpu,gpu,499,500).accepted);

  gpu=cpu;for(auto& channel:gpu[0].channels)
    std::fill(channel.begin(),channel.end(),std::complex<float>{});
  auto zeroCpu=gpu;
  const auto inconclusive=blah2::wideband_gpu::qualifyFirstBlock(
    zeroCpu,gpu,120,500);
  REQUIRE_FALSE(inconclusive.accepted);
  REQUIRE(inconclusive.status==
    blah2::wideband_gpu::QualificationStatus::inconclusive);

  gpu=cpu;gpu[0].channels[1][1]+=std::complex<float>(.1f,0);
  REQUIRE_FALSE(blah2::wideband_gpu::qualifyFirstBlock(
    cpu,gpu,120,500).accepted);
  gpu=cpu;gpu[0].firstInputSample++;
  REQUIRE_FALSE(blah2::wideband_gpu::qualifyFirstBlock(
    cpu,gpu,120,500).accepted);
  gpu=cpu;gpu[0].channels[0].pop_back();
  REQUIRE_FALSE(blah2::wideband_gpu::qualifyFirstBlock(
    cpu,gpu,120,500).accepted);
  REQUIRE(blah2::wideband_gpu::qualifyFirstBlock(
    cpu,cpu,501,500).accepted);
  REQUIRE_FALSE(blah2::wideband_gpu::qualifyFirstBlock(
    cpu,cpu,std::numeric_limits<double>::quiet_NaN(),500).accepted);
  gpu=cpu;gpu[0].channels[0][0]={
    std::numeric_limits<float>::quiet_NaN(),0};
  REQUIRE_FALSE(blah2::wideband_gpu::qualifyFirstBlock(
    cpu,gpu,120,500).accepted);
}

TEST_CASE("wideband split frame enforces one in flight and reset guards")
{
  blah2::wideband_gpu::Config config{8000,2,8,
    {{100000000,2,0,std::vector<double>(9,1.0/9)}}};
  blah2::GpuProcessOptions options;options.executable=GPU_WIDEBAND_MOCK;
  options.argument="--mock";options.warmup=false;options.frameMs=40;
  std::vector<std::deque<std::complex<double>>> storage(2,
    std::deque<std::complex<double>>(8,{1,0}));
  std::vector<const std::deque<std::complex<double>>*> input{
    &storage[0],&storage[1]};
  auto process=blah2::wideband_gpu::createProcess(config,"mock",options);
  REQUIRE(process->supportsSplitFrame());
  process->beginDeques(input,12);
  REQUIRE(process->frameInFlight());
  REQUIRE_THROWS(process->beginDeques(input,20));
  REQUIRE_THROWS(process->reset());
  auto output=process->finishFrame();
  REQUIRE_FALSE(process->frameInFlight());
  REQUIRE(output.front().firstInputSample==12);
  REQUIRE_THROWS(process->finishFrame());
  std::vector<std::vector<std::complex<float>>> owned(2,
    std::vector<std::complex<float>>(8,{1,0}));
  process->begin(owned,24);
  REQUIRE(process->frameInFlight());
  output=process->finishFrame();
  REQUIRE(output.front().firstInputSample==24);

  // A fast device must not be rejected merely because prior-frame CPU work
  // delays collection. The ready response is validated without a new deadline.
  process->begin(owned,32);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  output=process->finishFrame();
  REQUIRE(output.front().firstInputSample==32);
  REQUIRE_FALSE(process->frameInFlight());

  setenv("VW_MOCK_WIDEBAND_FAILURE","partial",1);
  auto failed=blah2::wideband_gpu::createProcess(config,"mock",options);
  failed->beginDeques(input,0);
  std::this_thread::sleep_for(std::chrono::milliseconds(60));
  const auto expired=std::chrono::steady_clock::now();
  REQUIRE_THROWS(failed->finishFrame());
  REQUIRE(std::chrono::steady_clock::now()-expired<
    std::chrono::milliseconds(100));
  REQUIRE_FALSE(failed->frameInFlight());
  unsetenv("VW_MOCK_WIDEBAND_FAILURE");
}

TEST_CASE("wideband borrowed and legacy output preserve samples and independent frame ownership")
{
  struct Environment {
    ~Environment(){unsetenv("VW_MOCK_WIDEBAND_BORROWED");}
  } environment;
  const blah2::wideband_gpu::Config config{240000,3,32,{
    {100000000,4,-30000,std::vector<double>(97,1.0/97)},
    {101000000,8,30000,std::vector<double>(193,1.0/193)}}};
  blah2::GpuProcessOptions options;options.executable=GPU_WIDEBAND_MOCK;
  options.argument="--mock";options.warmup=true;
  const auto verify=[&](const auto& output,const auto& input,uint64_t first){
    REQUIRE(output.size()==config.lanes.size());
    for(size_t lane=0;lane<output.size();++lane){
      const auto& actual=output[lane];const auto& geometry=config.lanes[lane];
      const size_t remainder=(geometry.decimation-first%geometry.decimation)%
        geometry.decimation;
      const size_t count=remainder<input.front().size()?
        1+(input.front().size()-1-remainder)/geometry.decimation:0;
      REQUIRE(actual.frequencyHz==geometry.frequencyHz);
      REQUIRE(actual.inputSamplesPerOutput==geometry.decimation);
      REQUIRE(actual.hasFirstInputSample==(count!=0));
      REQUIRE(actual.firstInputSample==(count?first+remainder:0));
      REQUIRE(actual.channels.size()==input.size());
      for(size_t channel=0;channel<input.size();++channel){
        REQUIRE(actual.channels[channel].size()==count);
        for(size_t sample=0;sample<count;++sample){
          const auto& expected=input[channel][remainder+sample*geometry.decimation];
          REQUIRE(std::memcmp(&actual.channels[channel][sample],&expected,
            sizeof(expected))==0);
        }
      }
    }
  };
  // The legacy double has only the unchanged module-v1 Backend interface.
  for(const bool borrowed:{false,true}){
    if(borrowed)REQUIRE(setenv("VW_MOCK_WIDEBAND_BORROWED","1",1)==0);
    else unsetenv("VW_MOCK_WIDEBAND_BORROWED");
    auto process=blah2::wideband_gpu::createProcess(config,"mock",options);
    REQUIRE(process->deviceName()==(borrowed?"mock-wideband-borrowed":"mock-wideband"));
    std::vector<std::vector<std::complex<float>>> input(config.channels,
      std::vector<std::complex<float>>(31));
    for(size_t channel=0;channel<input.size();++channel)
      for(size_t sample=0;sample<input[channel].size();++sample)
        input[channel][sample]={float(100*channel+sample),-float(channel)};
    input[0][1]={-0.0f,std::numeric_limits<float>::quiet_NaN()};
    input[1][5]={std::numeric_limits<float>::infinity(),-0.0f};
    const uint64_t first=(uint64_t(1)<<40)+3;
    process->begin(input,first);
    const auto prior=process->finishFrame();verify(prior,input,first);
    auto next=input;
    for(auto& channel:next)for(auto& sample:channel)sample={-17.0f,23.0f};
    process->begin(next,first+31);
    REQUIRE(process->frameInFlight());
    verify(prior,input,first);
    const auto completed=process->finishFrame();verify(completed,next,first+31);
    verify(prior,input,first); // IPC and module buffers have now been reused.
    process->reset();
    const std::vector<std::vector<std::complex<float>>> shortInput(config.channels,
      std::vector<std::complex<float>>(1,{11.0f,-0.0f}));
    verify(process->process(shortInput,7),shortInput,7); // Both lanes empty.
    verify(process->process(shortInput,8),shortInput,8); // Both lanes populated.
    const std::vector<std::vector<std::complex<float>>> empty(config.channels);
    verify(process->process(empty,9),empty,9);
    verify(process->process(input,first),input,first); // Discontinuous position.
    process.reset();verify(prior,input,first); // Independent backend lifetime.
  }
}

TEST_CASE("wideband worker rejects malformed borrowed output before copying")
{
  struct Environment {
    ~Environment(){unsetenv("VW_MOCK_WIDEBAND_BORROWED");
      unsetenv("VW_MOCK_WIDEBAND_FAILURE");}
  } environment;
  REQUIRE(setenv("VW_MOCK_WIDEBAND_BORROWED","1",1)==0);
  const blah2::wideband_gpu::Config config{8000,2,8,
    {{100000000,2,0,std::vector<double>(9,1.0/9)}}};
  blah2::GpuProcessOptions options;options.executable=GPU_WIDEBAND_MOCK;
  options.argument="--mock";options.warmup=false;
  const std::vector<std::vector<std::complex<float>>> input(config.channels,
    std::vector<std::complex<float>>(config.maxInputSamples,{1.0f,-2.0f}));
  for(const char* mode:{"borrowed-null","borrowed-total","borrowed-first",
      "borrowed-input","borrowed-lanes","borrowed-offset","borrowed-count",
      "borrowed-remainder","borrowed-decimation"}){
    CAPTURE(mode);
    REQUIRE(setenv("VW_MOCK_WIDEBAND_FAILURE",mode,1)==0);
    auto process=blah2::wideband_gpu::createProcess(config,"mock",options);
    try{process->process(input,1);FAIL("Malformed borrowed output accepted");}
    catch(const std::exception& error){
      REQUIRE(std::string(error.what()).find("borrowed output")!=std::string::npos);
    }
  }
}

TEST_CASE("live wideband ignores inherited traces with a blocked logging sink")
{
  int descriptors[2];REQUIRE(pipe(descriptors)==0);
  struct Pipe {
    int* descriptors;
    ~Pipe(){close(descriptors[0]);close(descriptors[1]);}
  } pipeOwner{descriptors};
  const int flags=fcntl(descriptors[1],F_GETFL);
  REQUIRE(flags>=0);
  REQUIRE(fcntl(descriptors[1],F_SETFL,flags|O_NONBLOCK)==0);
  char fill[4096]{};
  while(write(descriptors[1],fill,sizeof(fill))>0){}
  REQUIRE((errno==EAGAIN||errno==EWOULDBLOCK));
  REQUIRE(fcntl(descriptors[1],F_SETFL,flags)==0);
  const pid_t child=fork();REQUIRE(child>=0);
  if(child==0){
    if(dup2(descriptors[1],STDERR_FILENO)<0)_exit(3);
    close(descriptors[0]);close(descriptors[1]);
    setenv("VECTORWARP_WIDEBAND_TRACE_FRAMES","256",1);
    unsetenv("VW_MOCK_WIDEBAND_FAILURE");
    unsetenv("VW_MOCK_WIDEBAND_BORROWED");
    try{
      const blah2::wideband_gpu::Config config{8000,2,8,
        {{100000000,2,0,std::vector<double>(9,1.0/9)}}};
      blah2::GpuProcessOptions options;options.executable=GPU_WIDEBAND_MOCK;
      options.argument="--mock";options.warmup=false;options.frameMs=200;
      auto process=blah2::wideband_gpu::createProcess(config,"mock",options);
      const FloatChannels input(2,std::vector<std::complex<float>>(8,{3,-7}));
      for(unsigned frame=0;frame<3;++frame){
        const auto output=process->process(input,frame*8);
        if(output.size()!=1||output.front().firstInputSample!=frame*8)_exit(4);
      }
      process.reset();
      _exit(0);
    }catch(...){_exit(2);}
  }
  int status=0;pid_t result=0;
  const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(3);
  do{
    result=waitpid(child,&status,WNOHANG);
    if(result!=0)break;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }while(std::chrono::steady_clock::now()<deadline);
  if(result!=child){kill(child,SIGKILL);while(waitpid(child,&status,0)<0&&errno==EINTR){}}
  REQUIRE(result==child);
  REQUIRE(WIFEXITED(status));
  REQUIRE(WEXITSTATUS(status)==0);
}

TEST_CASE("wideband transport trace is opt in and bounded without changing output")
{
  struct TraceCapture {
    FILE* file=std::tmpfile();
    int saved=-1;
    ~TraceCapture(){
      if(saved>=0){std::fflush(stderr);dup2(saved,STDERR_FILENO);close(saved);}
      if(file)std::fclose(file);
      unsetenv("VECTORWARP_WIDEBAND_TRACE_FRAMES");
      unsetenv("VW_MOCK_WIDEBAND_BORROWED");
    }
    std::string read(){
      std::fflush(stderr);std::rewind(file);
      std::string text;char line[1024];
      while(std::fgets(line,sizeof(line),file))text+=line;
      return text;
    }
  };
  const blah2::wideband_gpu::Config config{8000,2,8,
    {{100000000,2,0,std::vector<double>(9,1.0/9)}}};
  blah2::GpuProcessOptions options;options.executable=GPU_WIDEBAND_MOCK;
  options.argument="--mock";options.warmup=false;
  options.offlineTransportTrace=true;
  const std::vector<std::vector<std::complex<float>>> input(config.channels,
    std::vector<std::complex<float>>(config.maxInputSamples,{3.0f,-7.0f}));
  for(const bool borrowed:{false,true})
    for(const char* setting:{"unset","0","invalid","-1","2","99999999999999999999"}){
      CAPTURE(borrowed,setting);
      TraceCapture captured;REQUIRE(captured.file!=nullptr);
      captured.saved=dup(STDERR_FILENO);REQUIRE(captured.saved>=0);
      REQUIRE(dup2(fileno(captured.file),STDERR_FILENO)>=0);
      if(borrowed)REQUIRE(setenv("VW_MOCK_WIDEBAND_BORROWED","1",1)==0);
      else unsetenv("VW_MOCK_WIDEBAND_BORROWED");
      if(std::string(setting)=="unset")unsetenv("VECTORWARP_WIDEBAND_TRACE_FRAMES");
      else REQUIRE(setenv("VECTORWARP_WIDEBAND_TRACE_FRAMES",setting,1)==0);
      const unsigned limit=std::string(setting)=="2"?2:
        (std::string(setting)=="99999999999999999999"?256:0);
      auto process=blah2::wideband_gpu::createProcess(config,"mock",options);
      for(unsigned frame=0;frame<std::max(3u,limit+1);++frame){
        // Reset must not replenish the trace budget or change sample lineage.
        if(frame==1)process->reset();
        const auto output=process->process(input,uint64_t(frame)*8);
        REQUIRE(output.size()==1);
        REQUIRE(output[0].firstInputSample==uint64_t(frame)*8);
        REQUIRE(output[0].channels[0]==std::vector<std::complex<float>>(4,{3.0f,-7.0f}));
      }
      std::fflush(stderr);
      REQUIRE(std::ftell(captured.file)==0); // No per-CPI diagnostic writes.
      process.reset();
      std::istringstream records(captured.read());std::string line;
      unsigned workerCount=0,parentCount=0;
      while(std::getline(records,line)){
        REQUIRE(line.find("WIDEBAND_TRANSPORT ")==0);
        REQUIRE(line.find(" output_bytes=64 ")!=std::string::npos);
        REQUIRE(line.find(" input_samples=8 ")!=std::string::npos);
        const bool worker=line.find(" role=worker ")!=std::string::npos;
        if(worker){
          ++workerCount;
          REQUIRE(line.find(borrowed?" mode=borrowed ":" mode=owned ")!=std::string::npos);
          REQUIRE(line.find(" backend_ms=")!=std::string::npos);
          REQUIRE(line.find(" copy_ms=")!=std::string::npos);
          REQUIRE(line.find(" worker_total_ms=")!=std::string::npos);
        }else{
          ++parentCount;REQUIRE(line.find(" role=parent ")!=std::string::npos);
          REQUIRE(line.find(" finish_wait_ms=")!=std::string::npos);
          REQUIRE(line.find(" import_ms=")!=std::string::npos);
          REQUIRE(line.find(" submit_to_finish_ms=")!=std::string::npos);
        }
        const unsigned ordinal=worker?workerCount:parentCount;
        const auto sequence=ordinal==1?1:ordinal+1; // Reset consumed sequence 2.
        REQUIRE(line.find(" sequence="+std::to_string(sequence)+" ")!=std::string::npos);
        REQUIRE(line.find(" first="+std::to_string(uint64_t(ordinal-1)*8)+" ")!=std::string::npos);
        std::istringstream fields(line);std::string field;
        while(fields>>field){
          const auto position=field.find("_ms=");
          if(position!=std::string::npos){
            const double value=std::stod(field.substr(position+4));
            REQUIRE(value>=0);REQUIRE(std::isfinite(value));
          }
        }
      }
      REQUIRE(workerCount==limit);REQUIRE(parentCount==limit);
    }
}
