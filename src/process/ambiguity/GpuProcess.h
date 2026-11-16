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
  // Wideband workers pre-touch the shared mapping and execute/reset one
  // maximum-size capture before accepting live input. Tests may disable it.
  bool warmup = true;
  // Zero retains the normal per-device policy. A GPU lane reservation
  // caps ALL Vulkan allocations (including staging and FFT-owned scratch).
  uint64_t allocationLimitBytes = 0;
};
using GpuFactory = std::function<std::unique_ptr<GpuBackend>(const GpuGeometry&, const std::string&)>;
std::unique_ptr<GpuBackend> createGpuProcess(const GpuGeometry&, const std::string&,
  const GpuProcessOptions& = {});
// Checked IPC reservation bytes, including physical page rounding on every OS. Use
// before spawning a lane worker to reserve mapping plus Vulkan allocations.
uint64_t gpuProcessMappingBytes(const GpuGeometry&);
std::unique_ptr<GpuBackend> createGpuFirProcess(uint32_t samples, uint32_t taps,
  const std::string&, const GpuProcessOptions& = {});
std::string gpuSiblingPath(const char* filename);
// Bounded enumeration in the isolated sibling worker; never loads a driver
// into the capture process. Options override the child only in focused tests.
std::string queryGpuDriverStatus(const GpuProcessOptions& = {});
int runGpuWorker(const GpuFactory&);
}
