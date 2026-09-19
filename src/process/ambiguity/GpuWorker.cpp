#include "GpuProcess.h"
#include "GpuDriverStatus.h"
#include "MacGpuCompatibility.h"
#include <dlfcn.h>
#include <iostream>
#include <stdexcept>

int main(int argc, char** argv) {
  if (argc == 2 && std::string(argv[1]) == "--driver-status") {
    // Enumeration only: no logical device, FFT plan, IQ, or persistent cache.
    // The caller must bound this separate process, just like processing workers.
    blah2::applyMacGpuCompatibilityPolicy();
    void* diagnostic = dlopen(blah2::gpuSiblingPath("blah2-gpu-vulkan.so").c_str(), RTLD_NOW | RTLD_LOCAL);
    std::string response;
    int code = 0;
    try {
      if (!diagnostic) throw std::runtime_error("GPU module or its Vulkan loader is unavailable");
      const auto status = reinterpret_cast<blah2::GpuDriverStatus>(dlsym(diagnostic, "blah2_gpu_driver_status"));
      if (!status) throw std::runtime_error("GPU module does not provide driver diagnostics; update VectorWarp");
      response = status(1);
      if (response.size() > 32768) throw std::runtime_error("GPU driver diagnostic exceeded its output limit");
    } catch (const std::exception& error) {
      response = "{\"version\":1,\"available\":false,\"devices\":[],\"qualification\":\"not-run\",\"error\":" +
        blah2::gpuDiagnosticString(error.what()) + "}";
      code = 1;
    }
    if (diagnostic) dlclose(diagnostic);
    std::cout << response << '\n';
    return code;
  }
  if (argc != 1) { std::cerr << "Usage: blah2-gpu-worker [--driver-status]\n"; return 2; }
  void* module = nullptr;
  const int result = blah2::runGpuWorker([&](const blah2::GpuGeometry& geometry,
      const std::string& device) -> std::unique_ptr<blah2::GpuBackend> {
    blah2::applyMacGpuCompatibilityPolicy();
    module = dlopen(blah2::gpuSiblingPath("blah2-gpu-vulkan.so").c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!module) {
      const char* detail = dlerror();
      throw std::runtime_error(std::string("GPU module load failed: ") +
        (detail ? detail : "unknown loader error") + "; using CPU");
    }
    auto create = reinterpret_cast<blah2::GpuCreate>(dlsym(module, "blah2_gpu_create"));
    if (!create) throw std::runtime_error("GPU module is incompatible; rebuild blah2");
    return std::unique_ptr<blah2::GpuBackend>(create(blah2::GPU_ABI, &geometry, device.c_str()));
  });
  if (module) dlclose(module);
  return result;
}
