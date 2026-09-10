#include "process/ambiguity/GpuProcess.h"
#include "process/ambiguity/Acceleration.h"
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <sys/wait.h>
#include <cerrno>

void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
int main() {
  try {
    const blah2::GpuGeometry geometry{128, 21, 26, 1, -5};
    std::vector<std::complex<float>> reference(128 * 21, {1.5f, -2}), surveillance(reference), output;
    for (const std::string mode : {"ok", "hang-init", "crash-init", "error-init",
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
      } catch (const std::runtime_error& error) {
        failed = true;
        const std::string reason = error.what();
        if (mode == "hang-init")
          check(reason.find("startup timed out after 300 ms") != std::string::npos,
            "Startup timeout lost its phase/deadline");
        if (mode == "hang-frame")
          check(reason.find("frame execution timed out after 150 ms") != std::string::npos,
            "Frame timeout lost its phase/deadline");
        std::cout << "Recovered " << mode << ": " << error.what() << '\n';
      }
      check(failed == (mode != "ok"), "Worker failure expectation differs");
      check(std::chrono::steady_clock::now() - start < std::chrono::seconds(3), "GPU worker recovery was not bounded");
      int status;
      check(waitpid(-1, &status, WNOHANG) == -1 && errno == ECHILD, "GPU worker was not reaped");
      std::cout << "PASS isolation=" << mode << '\n';
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
