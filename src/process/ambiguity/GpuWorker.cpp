#include "GpuProcess.h"
#include <dlfcn.h>
#include <stdexcept>

int main() {
  void* module = nullptr;
  const int result = blah2::runGpuWorker([&](const blah2::GpuGeometry& geometry,
      const std::string& device) -> std::unique_ptr<blah2::GpuBackend> {
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
