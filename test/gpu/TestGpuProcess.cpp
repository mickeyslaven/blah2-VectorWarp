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
    for (const std::string mode : {"ok", "hang-fir-reference", "hang-fir-final",
        "error-fir-reference", "error-fir-final"}) {
      blah2::GpuProcessOptions options;
      options.executable = blah2::gpuSiblingPath("testGpuWorkerDouble");
      options.argument = mode; options.startupMs = 300; options.frameMs = 150;
      const auto start = std::chrono::steady_clock::now();
      bool failed = false;
      try {
        auto worker = blah2::createGpuFirProcess(10000, 410, "auto", options);
        auto* fir = dynamic_cast<blah2::GpuFirFrameBackend*>(worker.get());
        check(fir && fir->firAvailable(), "FIR capability was not negotiated");
        auto buffers = fir->firBuffers();
        check(buffers.referenceCount == 4917 && buffers.weightCount == 410 &&
          buffers.outputCount == 4917, "FIR shared-frame geometry differs");
        try { fir->submitFirWeights(); check(false, "FIR final-before-reference was accepted"); }
        catch (const std::logic_error&) {}
        for (size_t i = 0; i < buffers.referenceCount; ++i)
          buffers.reference[i] = {float(i), -float(i)};
        buffers.weights[0] = {2, 3};
        const auto submitStart = std::chrono::steady_clock::now();
        fir->submitFirReference();
        check(std::chrono::steady_clock::now() - submitStart < std::chrono::milliseconds(100),
          "FIR reference submission waited for worker execution");
        try { fir->submitFirReference(); check(false, "Duplicate FIR reference was accepted"); }
        catch (const std::logic_error&) {}
        fir->submitFirWeights();
        try { fir->submitFirWeights(); check(false, "Duplicate FIR final was accepted"); }
        catch (const std::logic_error&) {}
        fir->finishFir();
        check(buffers.output[17] == buffers.reference[17] + buffers.weights[0],
          "FIR shared-memory output differs");
        // The completed sequence can be reused without stale output or state.
        buffers.reference[0] = {-7, 9}; buffers.weights[0] = {1, -2};
        fir->submitFirReference(); fir->submitFirWeights(); fir->finishFir();
        check(buffers.output[0] == std::complex<float>(-6, 7),
          "FIR worker reused stale frame data");
      } catch (const std::runtime_error& error) {
        failed = true;
        if (mode.find("hang-fir") == 0)
          check(std::string(error.what()).find("FIR execution timed out after 150 ms") != std::string::npos,
            "FIR timeout lost its phase/deadline");
        std::cout << "Recovered " << mode << ": " << error.what() << '\n';
      }
      check(failed == (mode != "ok"), "FIR worker failure expectation differs");
      check(std::chrono::steady_clock::now() - start < std::chrono::seconds(3),
        "FIR worker recovery was not bounded");
      int status;
      check(waitpid(-1, &status, WNOHANG) == -1 && errno == ECHILD,
        "FIR worker was not reaped");
      std::cout << "PASS fir-isolation=" << mode << '\n';
    }
    for (const auto& invalid : {std::pair<uint32_t,uint32_t>{0, 1},
        {1000, 0}, {1000, 1001}, {10000001, 410}}) {
      bool rejected = false;
      try { (void)blah2::createGpuFirProcess(invalid.first, invalid.second, "auto"); }
      catch (const std::runtime_error&) { rejected = true; }
      check(rejected, "Malformed FIR geometry was accepted");
    }
    {
      blah2::GpuProcessOptions options;
      options.executable = blah2::gpuSiblingPath("testGpuWorkerDouble");
      options.argument = "no-fir"; options.startupMs = 300; options.frameMs = 150;
      auto worker = blah2::createGpuFirProcess(10000, 410, "auto", options);
      auto* fir = dynamic_cast<blah2::GpuFirFrameBackend*>(worker.get());
      check(fir && !fir->firAvailable(), "Missing FIR backend advertised FIR capability");
      bool rejected = false;
      try { (void)fir->firBuffers(); } catch (const std::runtime_error&) { rejected = true; }
      check(rejected, "Unavailable FIR backend exposed shared buffers");
    }
    {
      blah2::GpuProcessOptions options;
      options.executable = blah2::gpuSiblingPath("testGpuWorkerDouble");
      options.argument = "hang-fir-reference"; options.startupMs = 300; options.frameMs = 150;
      const auto start = std::chrono::steady_clock::now();
      {
        auto worker = blah2::createGpuFirProcess(10000, 410, "auto", options);
        auto* fir = dynamic_cast<blah2::GpuFirFrameBackend*>(worker.get());
        fir->submitFirReference();
      }
      check(std::chrono::steady_clock::now() - start < std::chrono::seconds(3),
        "Outstanding FIR reference was not bounded during destruction");
      int status;
      check(waitpid(-1, &status, WNOHANG) == -1 && errno == ECHILD,
        "Outstanding FIR worker was not reaped");
    }
    unsigned invalidVariant = 0;
    for (auto geometry : {blah2::GpuGeometry{}, blah2::GpuGeometry{}}) {
      geometry.kind = blah2::GpuWorkKind::fir; geometry.firSamples = 10000;
      geometry.firTaps = 410; geometry.firFft = 2048; geometry.firPercent = 50;
      if (!invalidVariant++) geometry.firFft = 1024; else geometry.range = 1;
      bool rejected = false;
      try { (void)blah2::createGpuProcess(geometry, "auto"); }
      catch (const std::runtime_error&) { rejected = true; }
      check(rejected, "Malformed tagged FIR geometry was accepted");
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
      check(heapBudget(2ULL << 30) == (512ULL << 20) &&
        heapBudget(64ULL << 30) == (2ULL << 30),
        "GPU allocation allowance did not scale with device capacity");
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
