#include "process/ambiguity/Acceleration.h"
#include <dlfcn.h>
#include <chrono>
#include <cmath>
#include <iostream>
#include <random>
#include <thread>

namespace {
using Complex = std::complex<double>;
void require(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}
void runCase(unsigned channels, uint32_t fs, uint32_t samples,
  int low, int high, int minimum, int maximum, bool rounded,
  const std::string& device, const std::string& mode, double scale = 1) {
  std::vector<std::unique_ptr<Ambiguity>> processors, controls;
  std::vector<std::unique_ptr<IqData>> data, truth;
  std::vector<Ambiguity*> cpu;
  std::vector<IqData*> inputs;
  for (unsigned channel = 0; channel < channels; ++channel) {
    processors.push_back(std::make_unique<Ambiguity>(minimum, maximum, low, high, fs, samples, rounded));
    controls.push_back(std::make_unique<Ambiguity>(minimum, maximum, low, high, fs, samples, rounded));
    data.push_back(std::make_unique<IqData>(samples)); truth.push_back(std::make_unique<IqData>(samples));
    cpu.push_back(processors.back().get()); inputs.push_back(data.back().get());
  }
  auto& first = *cpu.front();
  blah2::Acceleration acceleration(mode, {first.get_nfft(), first.get_n_doppler_bins(),
    first.get_n_delay_bins(), channels, minimum}, first.get_n_corr(), fs,
    first.get_doppler_middle(), device);
  if (mode != "cpu") require(acceleration.status().state == "checking", acceleration.status().reason);
  std::mt19937 random(42);
  std::normal_distribution<double> noise;
  double maximumError = 0;
  for (unsigned frame = 0; frame < 5; ++frame) {
    std::deque<Complex> reference;
    for (uint32_t i = 0; i < samples; ++i) reference.emplace_back(scale * noise(random), scale * noise(random));
    for (unsigned channel = 0; channel < channels; ++channel) {
      std::deque<Complex> values;
      for (uint32_t i = 0; i < samples; ++i) {
        Complex value = reference[(i + samples - (channel + 1)) % samples] *
          std::polar(0.7, 2 * std::acos(-1.0) * 23 * i / fs);
        value += Complex(scale * .1 * noise(random), scale * .1 * noise(random));
        values.push_back(value);
      }
      auto copy = values;
      data[channel]->replace(std::move(values)); truth[channel]->replace(std::move(copy));
    }
    acceleration.process(reference, inputs, cpu, [&] {
      for (unsigned channel = 0; channel < channels; ++channel)
        cpu[channel]->process(reference, data[channel].get());
    });
    for (unsigned channel = 0; channel < channels; ++channel) {
      auto expected = controls[channel]->process(reference, truth[channel].get());
      auto actual = cpu[channel]->result();
      require(actual->delay == expected->delay && actual->doppler == expected->doppler, "Map coordinates differ");
      require(data[channel]->get_length() == truth[channel]->get_length(), "Input consumption differs");
      double signal = 0, error = 0;
      for (size_t d = 0; d < actual->data.size(); ++d)
        for (size_t r = 0; r < actual->data[d].size(); ++r) {
          signal += std::norm(expected->data[d][r]);
          error += std::norm(expected->data[d][r] - actual->data[d][r]);
        }
      const double relative = std::sqrt(error / std::max(signal, 1e-30));
      maximumError = std::max(maximumError, relative);
      require(relative < 1e-4, "GPU map differs from independent CPU result");
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  const auto& status = acceleration.status();
  if (mode == "gpu") require(status.active == "vulkan", status.reason);
  if (mode == "cpu") require(status.active == "cpu" && status.device.empty(), "CPU mode opened GPU");
  std::cout << "PASS channels=" << channels << " samples=" << samples << " offset=" << first.get_doppler_middle()
    << " range_fft=" << first.get_nfft() << " doppler_bins=" << first.get_n_doppler_bins()
    << " rounded=" << rounded << " mode=" << mode << " active=" << status.active
    << " relative_error=" << maximumError << " cpu_ms=" << status.cpuMs
    << " gpu_ms=" << status.gpuMs << " device=" << status.device << std::endl;
}
}
int main(int argc, char** argv) {
  try {
    const std::string device = argc > 1 ? argv[1] : "auto";
    if (device == "--list" || device == "--limits") {
      std::string executable(argv[0]);
      auto path = executable.substr(0, executable.find_last_of('/')) + "/blah2-gpu-vulkan.so";
      void* module = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
      require(module, dlerror() ? "Cannot load GPU module" : "Missing GPU module");
      auto list = reinterpret_cast<blah2::GpuList>(dlsym(module, "blah2_gpu_devices"));
      require(list, "Missing discovery function");
      const auto devices = list();
      for (const auto& gpu : devices) std::cout << gpu.id << '\t' << gpu.name << '\t' << gpu.memoryBytes << '\n';
      if (device == "--limits") {
        require(!devices.empty(), "Capacity checks require a real GPU");
        auto create = reinterpret_cast<blah2::GpuCreate>(dlsym(module, "blah2_gpu_create"));
        require(create, "Missing GPU factory");
        for (const auto& gpu : devices)
          for (const blah2::GpuGeometry geometry : {
            blah2::GpuGeometry{0, 21, 26, 1, -5},
            blah2::GpuGeometry{128, 21, 26, 9, -5},
            blah2::GpuGeometry{128, 21, 26, 1, -128},
            blah2::GpuGeometry{65535, 65535, 65535, 8, 0}}) {
            bool rejected = false;
            try { std::unique_ptr<blah2::GpuBackend> processor(create(blah2::GPU_ABI, &geometry, gpu.id.c_str())); }
            catch (const std::exception& error) {
              const std::string message = error.what();
              rejected = message.find("dimensions") != std::string::npos || message.find("capacity") != std::string::npos;
            }
            require(rejected, "Unsafe GPU geometry was not rejected by its capacity check");
          }
        std::cout << "PASS hardware capacity guards\n";
      }
      dlclose(module); return 0;
    }
    const std::string mode = device == "--cpu" ? "cpu" : "gpu";
    if (argc > 2 && std::string(argv[2]) == "--prime") {
      for (int high : {530, 2600})
        runCase(2, 48000, 4800, -high, high, -2, 4, false, device, mode);
      return 0;
    }
    if (argc > 2 && std::string(argv[2]) == "--matrix") {
      // Production-sized frames at every supported input count. Channels here
      // counts processed surveillance paths, not the receiver's total inputs.
      for (unsigned channels : {1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u})
        runCase(channels, 2400000, 480000, -800, 800, -10, 245, true, device, mode);
      for (uint32_t samples : {120000u, 240000u, 1200000u})
        runCase(2, 2400000, samples, -800, 800, -10, 245, true, device, mode);
      // Prime-length FFT tables are created during initialization. These cases
      // regress the early-binding bug that returned an all-zero Doppler map.
      for (int high : {530, 2600})
        runCase(2, 48000, 4800, -high, high, -2, 4, false, device, mode);
      runCase(2, 2400000, 480000, -1000, 600, -10, 245, false, device, mode);
      if (mode == "gpu")
        runCase(5, 2400000, 480000, -800, 800, -10, 245, true, device, "auto");
      return 0;
    }
    if (argc > 2 && std::string(argv[2]) == "--full-only") {
      runCase(5, 2400000, 480000, -800, 800, -10, 245, true, device, mode); return 0;
    }
    for (unsigned channels : {1u, 2u, 4u, 5u, 7u, 8u})
      runCase(channels, 48000, 4800, -100, 100, -5, 20, true, device, mode);
    runCase(2, 48000, 4800, -150, 50, -10, 10, false, device, mode);
    runCase(2, 48000, 4800, 0, 200, 0, 20, true, device, mode, 1e-5);
    runCase(2, 48000, 4800, -100, 100, -5, 20, true, device, mode, 1e3);
    if (argc < 3 || std::string(argv[2]) != "--quick")
      runCase(5, 2400000, 480000, -800, 800, -10, 245, true, device, mode);
    if (mode == "gpu") runCase(2, 48000, 4800, -100, 100, -5, 20, true, device, "auto");
    return 0;
  } catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
}
