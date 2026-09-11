#include "process/ambiguity/GpuProcess.h"
#include "process/ambiguity/Acceleration.h"
#include "process/ambiguity/GpuMemory.h"
#include <algorithm>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <sys/wait.h>
#include <cerrno>

void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
int main() {
  try {
    const blah2::GpuGeometry geometry{128, 21, 26, 1, -5, 64, 4, -2};
    std::vector<std::complex<float>> reference(128 * 21, {1.5f, -2}), surveillance(reference), output;
    for (const std::string mode : {"ok", "raw", "reject-clutter", "hang-clutter", "hang-init", "crash-init", "error-init",
        "hang-frame", "crash-frame", "error-frame", "bad-packet", "absent"}) {
      const auto start = std::chrono::steady_clock::now();
      blah2::GpuProcessOptions options;
      options.executable = blah2::gpuSiblingPath(mode == "absent" ? "missing-test-worker" : "testGpuWorkerDouble");
      options.argument = mode; options.startupMs = 300; options.frameMs = 150;
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
      check(failed == (mode != "ok" && mode != "raw" && mode != "reject-clutter"),
        "Worker failure expectation differs");
      check(std::chrono::steady_clock::now() - start < std::chrono::seconds(3), "GPU worker recovery was not bounded");
      int status;
      check(waitpid(-1, &status, WNOHANG) == -1 && errno == ECHILD, "GPU worker was not reaped");
      std::cout << "PASS isolation=" << mode << '\n';
    }
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
      check(budget.reserve(1, 20) && !budget.reserve(1, 1) && budget.used() == 40,
        "Vulkan allocations exceeded the aggregate allowance");
      budget.release(0, 20);
      check(budget.reserve(2, 5) && budget.used() == 25 && budget.peak() == 40,
        "Released FFT scratch was not returned to the allocation budget");
      check(!budget.reserve(0, UINT64_MAX) && !budget.reserve(99, 1) &&
        !budget.reserve(0, 0) && budget.used() == 25,
        "Invalid Vulkan allocation changed the budget");
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
