#include "process/ambiguity/GpuWidebandProcess.h"
#include "process/ambiguity/GpuWidebandFallback.h"
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <thread>

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
  auto process=blah2::wideband_gpu::createProcess(config,"mock",options);
  REQUIRE(process->deviceName()=="mock-wideband");
}

TEST_CASE("wideband auto mode keeps CPU ready and retries the same block")
{
  REQUIRE(blah2::wideband_gpu::needsCpuFallback("auto",true));
  REQUIRE_FALSE(blah2::wideband_gpu::needsCpuFallback("gpu",true));
  const uint64_t first=1234;bool reset=false;uint64_t cpuFirst=0;
  const bool gpu=blah2::wideband_gpu::attemptWithAutoFallback(true,[]{throw std::runtime_error("device lost");},[&](const std::exception&){reset=true;});
  if(!gpu)cpuFirst=first;
  REQUIRE_FALSE(gpu);REQUIRE(reset);REQUIRE(cpuFirst==first);
  REQUIRE_THROWS(blah2::wideband_gpu::attemptWithAutoFallback(false,[]{throw std::runtime_error("device lost");},[](const std::exception&){}));
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
  REQUIRE_FALSE(blah2::wideband_gpu::qualifyFirstBlock(
    cpu,cpu,501,500).accepted);
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
