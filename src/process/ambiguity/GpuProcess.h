#pragma once
#include "GpuBackend.h"
#include <functional>
#include <memory>

namespace blah2 {
struct GpuProcessOptions {
  // Test injection only; production always uses the sibling worker executable.
  std::string executable;
  std::string argument;
  unsigned startupMs = 30000;
  unsigned frameMs = 5000;
};
using GpuFactory = std::function<std::unique_ptr<GpuBackend>(const GpuGeometry&, const std::string&)>;
std::unique_ptr<GpuBackend> createGpuProcess(const GpuGeometry&, const std::string&,
  const GpuProcessOptions& = {});
std::string gpuSiblingPath(const char* filename);
int runGpuWorker(const GpuFactory&);
}
