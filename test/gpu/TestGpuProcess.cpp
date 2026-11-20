#include "process/ambiguity/GpuProcess.h"
#include "process/ambiguity/Acceleration.h"
#include "process/ambiguity/GpuMemory.h"
#include "WorkerIsolationFixture.h"
#include <algorithm>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <sys/wait.h>
#include <cerrno>
#include <cstdlib>
#include <cstdio>
#include <fcntl.h>
#include <unistd.h>
#include <thread>

void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void profilePreservation(const blah2::GpuGeometry& geometry) {
  struct Fixture {
    std::string prior=std::getenv("VECTORWARP_STREAM_PROFILE_FRAMES")?
      std::getenv("VECTORWARP_STREAM_PROFILE_FRAMES"):"";
    bool present=std::getenv("VECTORWARP_STREAM_PROFILE_FRAMES")!=nullptr;
    FILE* file=std::tmpfile();int saved=-1;
    Fixture(){
      check(file,"Cannot create worker profile capture");std::fflush(stderr);saved=dup(STDERR_FILENO);
      check(saved>=0 && dup2(fileno(file),STDERR_FILENO)>=0,"Cannot redirect worker profile capture");
      setenv("VECTORWARP_STREAM_PROFILE_FRAMES","1",1);
    }
    ~Fixture(){
      std::fflush(stderr);if(saved>=0){dup2(saved,STDERR_FILENO);close(saved);}if(file)std::fclose(file);
      if(present)setenv("VECTORWARP_STREAM_PROFILE_FRAMES",prior.c_str(),1);
      else unsetenv("VECTORWARP_STREAM_PROFILE_FRAMES");
    }
    std::string text(){std::fflush(stderr);std::rewind(file);std::string result;char buffer[4096];
      while(const auto n=std::fread(buffer,1,sizeof(buffer),file))result.append(buffer,n);
      check(result.size()<65536,"Worker profile exceeded its bounded fixture output");return result;}
  } fixture;
  blah2::GpuProcessOptions options;options.executable=blah2::gpuSiblingPath("testGpuWorkerDouble");
  options.argument="raw-profile";options.startupMs=2000;options.frameMs=150;
  {
    auto worker=blah2::createGpuProcess(geometry,"auto",options);
    auto* frame=dynamic_cast<blah2::GpuFrameBackend*>(worker.get());
    auto* clutter=dynamic_cast<blah2::GpuClutterFrameBackend*>(worker.get());
    const auto buffers=frame->frameBuffers();buffers.reference[0]={1.25f,-2.5f};
    frame->processFrame();check(clutter->processClutterFrame(),"Profile fixture rejected clutter");
    frame->processFrame(); // One CPI = two transactions, never a third record.
    check(buffers.output[0]==buffers.reference[0],"Profiler changed worker numerical output");
  }
  const auto output=fixture.text();
  for(const std::string role:{"portable-parent","portable-worker"}) {
    unsigned count=0;std::size_t at=0;const auto needle="role="+role+" ";
    while((at=output.find(needle,at))!=std::string::npos){++count;at+=needle.size();}
    check(count==2,"EOF cleanup lost worker records or exceeded fixed profile cap");
    check(output.find(needle+"sequence=1 ")!=std::string::npos &&
      output.find(needle+"sequence=2 ")!=std::string::npos,"Parent/worker profile sequence differs");
  }
  check(output.find("TEST_PORTABLE_PROFILE_BACKEND_DESTROYED")!=std::string::npos,
    "Parent killed a healthy backend before bounded profile teardown completed");
  check(output.find("phase1_us=")!=std::string::npos && output.find("peer_pid=")!=std::string::npos,
    "Worker correlation timestamps or process identity are missing");
  options.argument="hang-clutter";
  auto worker=blah2::createGpuProcess(geometry,"auto",options);bool failed=false;
  const auto start=std::chrono::steady_clock::now();
  try{dynamic_cast<blah2::GpuClutterFrameBackend&>(*worker).processClutterFrame();}
  catch(const std::runtime_error&){failed=true;}
  worker.reset();
  check(failed && std::chrono::steady_clock::now()-start<std::chrono::seconds(2),
    "Opt-in record drain removed the bounded hung-worker failure policy");
  check(fixture.text().find("outcome=-1")!=std::string::npos,
    "Parent failed transaction disappeared from bounded profile");
}
int main() {
  // An inherited recipe must never enable qualification in ordinary callers.
  setenv("BLAH2_REPLAY_GPU_QUALIFICATION", "untrusted-inherited-recipe", 1);
  try {
    const blah2::GpuGeometry geometry{128, 21, 26, 1, -5, 64, 4, -2};
    worker_isolation_fixture::verify([&](bool absent) {
      blah2::GpuProcessOptions options;
      options.executable = blah2::gpuSiblingPath(absent ? "missing-test-worker" : "testGpuWorkerDouble");
      options.argument = "fd-closed";
      options.startupMs = 2000; options.frameMs = 150;
      auto worker = blah2::createGpuProcess(geometry, "auto", options);
    });
    profilePreservation(geometry);
    setenv("BLAH2_GPU_CLUTTER_PROFILE_FRAMES","2",1);
    setenv("VECTORWARP_GPU_OFFLINE_DIAGNOSTICS","untrusted inherited recipe",1);
    for(bool offline:{false,true}){
      blah2::GpuProcessOptions options;
      options.executable=blah2::gpuSiblingPath("testGpuWorkerDouble");
      options.argument=offline?"diagnostics-offline":"diagnostics-live";
      options.offlineTransportTrace=offline;
      auto worker=blah2::createGpuProcess(geometry,"auto",options);
      check(std::string(std::getenv("BLAH2_GPU_CLUTTER_PROFILE_FRAMES"))=="2",
        "Diagnostic authorization mutated the parent environment");
    }
    unsetenv("BLAH2_GPU_CLUTTER_PROFILE_FRAMES");
    unsetenv("VECTORWARP_GPU_OFFLINE_DIAGNOSTICS");
    const uint64_t expectedMapping = (128ULL * 21 * 2 + 21 * 26 + 64 * 3) * sizeof(std::complex<float>);
    const uint64_t page = uint64_t(sysconf(_SC_PAGESIZE));
    check(blah2::gpuProcessMappingBytes(geometry) == ((expectedMapping + page - 1) / page) * page,
      "Portable mapping reservation lost physical page rounding");
    bool hugeRejected = false;
    try { (void)blah2::gpuProcessMappingBytes({65535, 65535, 65535, 8}); }
    catch (const std::exception&) { hugeRejected = true; }
    check(hugeRejected, "Portable reservation overflow/capacity was accepted");
    struct SavedAllocationEnvironment {
      const char* previous = std::getenv("BLAH2_GPU_ALLOCATION_LIMIT_BYTES");
      bool present = previous != nullptr;
      std::string value = previous ? previous : "";
      ~SavedAllocationEnvironment() {
        if (present) setenv("BLAH2_GPU_ALLOCATION_LIMIT_BYTES", value.c_str(), 1);
        else unsetenv("BLAH2_GPU_ALLOCATION_LIMIT_BYTES");
      }
    } savedAllocationEnvironment;
    for (const std::string mode : {"budget-set", "budget-clear"}) {
      check(setenv("BLAH2_GPU_ALLOCATION_LIMIT_BYTES", "12345", 1) == 0,
        "Cannot prepare allocation environment fixture");
      blah2::GpuProcessOptions options;
      options.executable = blah2::gpuSiblingPath("testGpuWorkerDouble");
      options.argument = mode;
      options.allocationLimitBytes = mode == "budget-set" ? 32ULL << 20 : 0;
      auto worker = blah2::createGpuProcess(geometry, "auto", options);
      check(std::string(std::getenv("BLAH2_GPU_ALLOCATION_LIMIT_BYTES")) == "12345",
        "Lane construction mutated the parent's environment");
    }
    unsetenv("BLAH2_GPU_ALLOCATION_LIMIT_BYTES");
    struct SavedHostWorkEnvironment {
      const char* previous = std::getenv("BLAH2_GPU_CLUTTER_HOST_WORK_OVERLAP");
      bool present = previous != nullptr;
      std::string value = previous ? previous : "";
      ~SavedHostWorkEnvironment() {
        if (present) setenv("BLAH2_GPU_CLUTTER_HOST_WORK_OVERLAP", value.c_str(), 1);
        else unsetenv("BLAH2_GPU_CLUTTER_HOST_WORK_OVERLAP");
      }
    } savedHostWorkEnvironment;
    for (const char* setting : std::vector<const char*>{nullptr, "", "0", "1", "true"}) {
      if (setting) setenv("BLAH2_GPU_CLUTTER_HOST_WORK_OVERLAP", setting, 1);
      else unsetenv("BLAH2_GPU_CLUTTER_HOST_WORK_OVERLAP");
      const bool overlap = setting && std::string(setting) == "1";
      blah2::GpuProcessOptions options;
      options.executable = blah2::gpuSiblingPath("testGpuWorkerDouble");
      options.argument = "raw"; options.startupMs = 2000; options.frameMs = 150;
      auto worker = blah2::createGpuProcess(geometry, "auto", options);
      auto* hostWork = dynamic_cast<blah2::GpuClutterHostWorkBackend*>(worker.get());
      check(hostWork, "Worker lost its optional host-work interface");
      // Construction snapshots the setting: changing it cannot change the
      // ordering/deadline contract of an already-created worker.
      setenv("BLAH2_GPU_CLUTTER_HOST_WORK_OVERLAP", overlap ? "0" : "1", 1);
      unsigned quickCalls = 0;
      check(hostWork->processClutterFrameWithHostWork([&] { ++quickCalls; }) &&
        quickCalls == 1, "Host-work setting changed valid clutter or callback count");
      unsigned hostCalls = 0;
      check(hostWork->processClutterFrameWithHostWork([&] {
        ++hostCalls;
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
      }), "Slow host work rejected an already-ready clutter reply");
      check(hostCalls == 1, "Slow host work was skipped or retried");
      check(hostWork->processClutterFrameWithHostWork({}),
        "Slow host work left a stale response/sequence");
    }
    setenv("BLAH2_GPU_CLUTTER_HOST_WORK_OVERLAP", "1", 1);
    {
      blah2::GpuProcessOptions options;
      options.executable = blah2::gpuSiblingPath("testGpuWorkerDouble");
      options.argument = "hang-clutter"; options.startupMs = 2000; options.frameMs = 400;
      auto worker = blah2::createGpuProcess(geometry, "auto", options);
      auto* hostWork = dynamic_cast<blah2::GpuClutterHostWorkBackend*>(worker.get());
      unsigned calls = 0; bool timedOut = false;
      const auto started = std::chrono::steady_clock::now();
      try {
        hostWork->processClutterFrameWithHostWork([&] {
          ++calls;
          std::this_thread::sleep_for(std::chrono::milliseconds(450));
        });
      } catch (const std::runtime_error& error) {
        timedOut = std::string(error.what()).find("timed out") != std::string::npos;
      }
      check(calls == 1 && timedOut, "Delayed host work accepted an unfinished GPU frame");
      check(std::chrono::steady_clock::now()-started < std::chrono::milliseconds(750),
        "Expired GPU work received a fresh frame timeout after host work");
    }
    // The ordinary accept/reject/failure fixtures below exercise the default.
    unsetenv("BLAH2_GPU_CLUTTER_HOST_WORK_OVERLAP");
    const int source = open("/dev/null", O_RDONLY);
    check(source >= 0, "Cannot open inherited descriptor fixture");
    const int inherited = fcntl(source, F_DUPFD, 100);
    close(source);
    check(inherited >= 100, "Cannot prepare inherited descriptor fixture");
    setenv("BLAH2_TEST_INHERITED_FD", std::to_string(inherited).c_str(), 1);
    std::vector<std::complex<float>> reference(128 * 21, {1.5f, -2}), surveillance(reference), output;
    for (const std::string mode : {"ok", "fd-closed", "raw", "reject-clutter", "hang-clutter", "hang-init", "crash-init", "error-init",
        "hang-frame", "crash-frame", "error-frame", "bad-packet", "absent"}) {
      const auto start = std::chrono::steady_clock::now();
      blah2::GpuProcessOptions options;
      options.executable = blah2::gpuSiblingPath(mode == "absent" ? "missing-test-worker" : "testGpuWorkerDouble");
      // Keep the injected startup deadline exact while allowing a cold Mach-O
      // executable to start under CI load. Production's startup limit is 30 s.
      options.argument = mode; options.startupMs = mode == "hang-init" ? 300 : 2000;
      options.frameMs = 150;
      bool failed = false;
      try {
        auto worker = blah2::createGpuProcess(geometry, "auto", options);
        worker->process(reference, surveillance, output);
        check(output.size() == 21 * 26 && output.front() == reference.front(), "Shared-memory output differs");
        reference.front() = {2, 3};
        worker->process(reference, surveillance, output);
        check(output.front() == reference.front(), "Worker reused a stale frame");
        if (mode == "raw" || mode == "reject-clutter" || mode == "hang-clutter") {
          auto* frames = dynamic_cast<blah2::GpuFrameBackend*>(worker.get());
          check(frames, "Process did not expose its shared frame");
          auto frame = frames->frameBuffers();
          check(frame.referenceCount == reference.size() &&
            frame.surveillanceCount == surveillance.size() &&
            frame.outputCount == output.size(), "Shared-frame geometry differs");
          std::fill_n(frame.reference, frame.referenceCount, std::complex<float>{4, -1});
          std::fill_n(frame.surveillance, frame.surveillanceCount, std::complex<float>{});
          frames->processFrame();
          check(frame.output[0] == std::complex<float>(4, -1),
            "Direct shared-frame output differs");
          auto* clutter = dynamic_cast<blah2::GpuClutterFrameBackend*>(worker.get());
          check(clutter && clutter->clutterAvailable(), "Raw clutter capability was not negotiated");
          auto clutterFrame = clutter->clutterBuffers();
          check(clutterFrame.referenceCount == 64 && clutterFrame.surveillanceCount == 64 &&
            clutterFrame.outputCount == 64, "Shared clutter geometry differs");
          std::fill_n(clutterFrame.reference, clutterFrame.referenceCount,
            std::complex<float>{1, 0});
          for (size_t i = 0; i < clutterFrame.surveillanceCount; ++i)
            clutterFrame.surveillance[i] = {float(i), -float(i)};
          const bool clutterAccepted = clutter->processClutterFrame();
          check(clutterAccepted == (mode != "reject-clutter"),
            "Clutter precision rejection was not preserved across IPC");
          if (clutterAccepted)
            check(std::all_of(clutterFrame.output,
              clutterFrame.output + clutterFrame.outputCount,
              [](auto value) { return value == std::complex<float>{}; }),
              "Raw clutter estimate output differs");
          else {
            frames->processFrame();
            check(frame.output[0] == std::complex<float>(4, -1),
              "Clutter precision rejection stopped the healthy worker");
          }
          auto* hostWork = dynamic_cast<blah2::GpuClutterHostWorkBackend*>(worker.get());
          check(hostWork, "Process lost optional clutter host-work interface");
          unsigned hostCalls = 0;
          check(hostWork->processClutterFrameWithHostWork([&] { ++hostCalls; }) == clutterAccepted &&
            hostCalls == 1, "Clutter host work changed result or invocation count");
          frames->processFrame();
          check(frame.output[0] == std::complex<float>(4, -1),
            "Clutter host work left an unread response or stale sequence");
        } else {
          auto* clutter = dynamic_cast<blah2::GpuClutterFrameBackend*>(worker.get());
          check(clutter && !clutter->clutterAvailable(),
            "Legacy worker advertised raw clutter support");
        }
      } catch (const std::runtime_error& error) {
        failed = true;
        const std::string reason = error.what();
        if (mode == "hang-init")
          check(reason.find("startup timed out after 300 ms") != std::string::npos,
            "Startup timeout lost its phase/deadline");
        if (mode == "hang-frame")
          check(reason.find("frame execution timed out after 150 ms") != std::string::npos,
            "Frame timeout lost its phase/deadline");
        if (mode == "hang-clutter")
          check(reason.find("clutter execution timed out after 150 ms") != std::string::npos,
            "Clutter timeout lost its phase/deadline");
        std::cout << "Recovered " << mode << ": " << error.what() << '\n';
      }
      check(failed == (mode != "ok" && mode != "fd-closed" && mode != "raw" && mode != "reject-clutter"),
        "Worker failure expectation differs");
      check(std::chrono::steady_clock::now() - start < std::chrono::seconds(3), "GPU worker recovery was not bounded");
      int status;
      // Darwin briefly exposes an internal child after posix_spawn returns
      // ENOENT without a PID. The kernel reaps it asynchronously. Only this test
      // owns all children; production must never waitpid(-1) and steal a child.
      const auto reapDeadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
      pid_t pending;
      do {
        pending = waitpid(-1, &status, WNOHANG);
        check(pending <= 0, "GPU worker required test-side reaping");
        if (pending == -1 && errno == ECHILD) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      } while (std::chrono::steady_clock::now() < reapDeadline);
      check(pending == -1 && errno == ECHILD, "GPU worker was not reaped");
      std::cout << "PASS isolation=" << mode << '\n';
    }
    close(inherited);
    unsetenv("BLAH2_TEST_INHERITED_FD");
    {
      using namespace blah2::gpu_memory;
      Properties properties{{
        {deviceLocal, 0}, {hostVisible | hostCoherent, 1},
        {deviceLocal | hostVisible, 0},
        {deviceLocal | hostVisible | hostCoherent | hostCached, 0}},
        {{8ULL << 30, true}, {16ULL << 30, false}}};
      check(chooseType(0xf, deviceLocal | hostVisible,
        hostCoherent | hostCached, properties) == 3, "Preferred mapped memory type was not selected");
      check(chooseType(0x3, deviceLocal | hostVisible, 0, properties) == noType,
        "Incompatible mapped memory type was accepted");
      check(autoDirect(true, 0, 128ULL << 20, properties),
        "Unified direct-memory topology was rejected");
      check(!autoDirect(false, 0, 128ULL << 20, properties),
        "Discrete BAR topology was assumed to be fast");
      check(!autoDirect(true, 0, 3ULL << 30, properties),
        "Direct-memory heap budget was ignored");
      const auto middle = alignedRange(129, 2, 1024, 128);
      check(middle.offset == 128 && middle.bytes == 128 && !middle.whole,
        "Noncoherent middle range alignment differs");
      const auto tail = alignedRange(900, 10, 910, 256);
      check(tail.offset == 768 && tail.whole, "Noncoherent tail range was not bounded");
      check(!alignedRange(900, 11, 910, 256).whole,
        "Out-of-allocation mapped range was accepted");
      check(nextSmooth(480209) == 486000 && nextSmooth(1) == 1 &&
        nextSmooth(uint64_t(UINT32_MAX)+1) == 0,
        "Smooth clutter convolution length differs");
      AllocationBudget budget(40, {{{deviceLocal, 0}, {hostVisible, 1},
        {deviceLocal | hostVisible, 0}}, {{100, true}, {200, false}}});
      check(budget.reserve(0, 20) && !budget.reserve(2, 6) && budget.used() == 20,
        "Aliased Vulkan memory types exceeded their shared heap allowance");
      check(budget.lastRejection().requested==6&&budget.lastRejection().used==20&&
        budget.lastRejection().heap==0&&budget.lastRejection().heapUsed==20&&
        budget.lastRejection().heapLimit==25&&budget.lastRejection().requiredAtLeast==26&&
        std::string(budget.lastRejection().reason)=="heap-budget",
        "Failed larger allocation lost actual byte requirements");
      check(budget.reserve(1, 20) && !budget.reserve(1, 1) && budget.used() == 40,
        "Vulkan allocations exceeded the aggregate allowance");
      budget.release(0, 20);
      check(budget.lastRejection().used==40&&budget.lastRejection().requiredAtLeast==41,
        "Unwinding allocations rewrote rejection-time byte requirements");
      budget.clearRejection();
      check(budget.lastRejection().requested==0,"New batch retained a stale rejection receipt");
      check(budget.reserve(2, 5) && budget.used() == 25 && budget.peak() == 40,
        "Released FFT scratch was not returned to the allocation budget");
      check(!budget.reserve(0, UINT64_MAX) && !budget.reserve(99, 1) &&
        !budget.reserve(0, 0) && budget.used() == 25,
        "Invalid Vulkan allocation changed the budget");
      AllocationBudget discrete(40, {{{deviceLocal, 0}, {hostVisible, 1},
        {deviceLocal | hostVisible, 0}}, {{100, true}, {200, false}}}, true);
      check(discrete.reserve(0, 20) && discrete.reserve(1, 40) &&
        discrete.used() == 60 && discrete.deviceUsed() == 20 &&
        discrete.limit() == 90 && discrete.deviceLimit() == 40 &&
        !discrete.reserve(1, 11) && discrete.reserve(2, 5) &&
        !discrete.reserve(2, 1),
        "Separate host heap was charged against the device allocation ceiling");
      check(heapBudget(2ULL << 30) == (512ULL << 20) &&
        heapBudget(64ULL << 30) == (2ULL << 30),
        "GPU allocation allowance did not scale with device capacity");
      const auto unrestricted = allocationPolicy(64ULL << 30, nullptr);
      const auto bounded = allocationPolicy(64ULL << 30, "40");
      check(unrestricted.bytes == maximumDeviceBytes && unrestricted.separateHostHeaps &&
        bounded.bytes == 40 && !bounded.separateHostHeaps,
        "Lane quota did not select one combined host/device allowance");
      check(allocationPolicy(100, "40").bytes == 25 &&
        allocationPolicy(64ULL << 30, "2147483648").bytes == maximumDeviceBytes,
        "Allocation override enlarged hardware default or rejected policy boundary");
      for (const char* invalid : {"", "0", "-1", "+1", " 40", "40 ", "4x", "1.0",
          "2147483649", "18446744073709551615", "18446744073709551616",
          "000000000000000000000000000000000000000000000000001"}) {
        bool rejected = false;
        try { (void)allocationPolicy(64ULL << 30, invalid); }
        catch (const std::invalid_argument&) { rejected = true; }
        check(rejected, "Malformed or over-policy allocation quota was accepted");
      }
      AllocationBudget boundedDiscrete(bounded.bytes,
        {{{deviceLocal, 0}, {hostVisible, 1}}, {{400, true}, {800, false}}},
        bounded.separateHostHeaps);
      check(boundedDiscrete.reserve(0, 20) && boundedDiscrete.reserve(1, 20) &&
        boundedDiscrete.used() == 40 && boundedDiscrete.limit() == 40 &&
        !boundedDiscrete.reserve(1, 1) && !boundedDiscrete.reserve(0, 1),
        "Portable/resident lane quota added a second host-heap allowance");
      boundedDiscrete.release(0, 20);
      check(boundedDiscrete.reserve(1, 20) && boundedDiscrete.used() == 40 &&
        !boundedDiscrete.reserve(0, 1),
        "Releasing device allocation let host memory escape the lane quota");
      AllocationBudget large(heapBudget(64ULL << 30),
        {{{deviceLocal, 0}}, {{64ULL << 30, true}}});
      check(large.reserve(0, 2ULL << 30) && !large.reserve(0, 1),
        "Large-device GPU allocation ceiling was not enforced");
      check(autoDirect(true, 0, 1ULL << 30, properties),
        "A capable unified-memory heap was kept at the former fixed cap");
      std::cout << "PASS memory-policy\n";
    }
    // An actual crashing/hanging worker must preserve the radar frame for CPU,
    // not merely throw successfully in an isolated IPC unit test.
    for (const std::string mode : {"hang-frame", "crash-frame"}) {
      constexpr unsigned samples = 4800;
      Ambiguity cpu(-5, 20, -100, 100, 48000, samples, true);
      IqData input(samples);
      std::deque<std::complex<double>> raw(samples, {1, 0});
      input.replace(std::deque<std::complex<double>>(raw));
      cpu.process(raw, &input);
      const auto expected = cpu.result()->data;
      blah2::GpuProcessOptions options;
      options.executable = blah2::gpuSiblingPath("testGpuWorkerDouble");
      options.argument = mode; options.startupMs = 1000; options.frameMs = 150;
      blah2::Acceleration acceleration("gpu", {cpu.get_nfft(), cpu.get_n_doppler_bins(),
        cpu.get_n_delay_bins(), 1, -5}, cpu.get_n_corr(), 48000, 0, "auto",
        [&](const auto& g, const auto& d) { return blah2::createGpuProcess(g, d, options); });
      check(acceleration.status().state == "checking", "Fault worker did not initialize");
      unsigned calls = 0;
      for (unsigned frame = 0; frame < 2; ++frame) {
        input.replace(std::deque<std::complex<double>>(raw));
        acceleration.process(raw, {&input}, {&cpu}, [&] { ++calls; cpu.process(raw, &input); });
        check(cpu.result()->data == expected, "Worker fault changed the CPU radar result");
        check(input.get_length() == samples - cpu.get_n_corr() * cpu.get_n_doppler_bins(), "Worker fault consumed input twice");
      }
      check(calls == 2 && acceleration.status().state == "fallback", "Worker fault did not recover on CPU");
      std::cout << "PASS same-frame=" << mode << '\n';
    }
    return 0;
  } catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
}
