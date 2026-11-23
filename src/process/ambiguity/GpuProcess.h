#pragma once
#include "GpuBackend.h"
#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>
#include <stdexcept>

namespace blah2 {
struct GpuProcessOptions {
  // Test injection only; production always uses the sibling worker executable.
  std::string executable;
  std::string argument;
  unsigned startupMs = 30000;
  unsigned frameMs = 5000;
  // Pre-touch and execute/reset one maximum-size capture; tests may disable it.
  bool warmup = true;
  // Live capture ignores inherited transport/clutter profiler settings.
  // Explicit offline callers may collect bounded diagnostics for teardown.
  bool offlineTransportTrace = false;
  // Zero retains the normal per-device policy. A GPU lane reservation
  // caps ALL Vulkan allocations (including staging and FFT-owned scratch).
  uint64_t allocationLimitBytes = 0;
  // Capture lane identity for explicit offline qualification; inert otherwise.
  uint32_t qualificationLane = 0;
};

// Optional GPU startup allows twenty CPIs with a five-second driver/cache floor,
// capped by the transport maximum so failed acceleration cannot stall capture.
inline unsigned gpuCpiStartupDeadlineMs(double cpiMilliseconds,
  unsigned transportMaximumMs, double cpiAllowance = 20,
  unsigned minimumMs = 5000)
{
  if (!std::isfinite(cpiMilliseconds) || cpiMilliseconds <= 0 ||
      !transportMaximumMs || !std::isfinite(cpiAllowance) ||
      cpiAllowance <= 0 || !minimumMs)
    throw std::invalid_argument("GPU startup deadline is invalid");
  const double available = std::max(double(minimumMs),
    std::ceil(cpiMilliseconds * cpiAllowance));
  return static_cast<unsigned>(std::min(double(transportMaximumMs), available));
}

// Required residency has no fallback; retain the transport bound for smaller
// clutter-batch retries. Optional routes keep the CPI-derived deadline.
inline unsigned gpuResidentStartupDeadlineMs(double cpiMilliseconds,
  unsigned transportMaximumMs, bool required)
{
  return required ? transportMaximumMs :
    gpuCpiStartupDeadlineMs(cpiMilliseconds, transportMaximumMs);
}

// Three CPIs with a scheduling floor leave recovery room in a seven-CPI queue;
// the transport maximum still bounds long or offline frames.
inline unsigned gpuCpiFrameDeadlineMs(double cpiMilliseconds,
  unsigned transportMaximumMs, double cpiAllowance = 3,
  unsigned minimumMs = 500)
{
  if (!std::isfinite(cpiMilliseconds) || cpiMilliseconds <= 0 ||
      !transportMaximumMs || !std::isfinite(cpiAllowance) ||
      cpiAllowance <= 0 || !minimumMs)
    throw std::invalid_argument("GPU frame deadline is invalid");
  const double available = std::max(double(minimumMs),
    std::ceil(cpiMilliseconds * cpiAllowance));
  return static_cast<unsigned>(std::min(double(transportMaximumMs), available));
}
using GpuFactory = std::function<std::unique_ptr<GpuBackend>(const GpuGeometry&, const std::string&)>;
std::unique_ptr<GpuBackend> createGpuProcess(const GpuGeometry&, const std::string&,
  const GpuProcessOptions& = {});
// Checked IPC reservation bytes, including physical page rounding on every OS. Use
// before spawning a lane worker to reserve mapping plus Vulkan allocations.
uint64_t gpuProcessMappingBytes(const GpuGeometry&);
std::string gpuSiblingPath(const char* filename);
// Bounded enumeration in the isolated sibling worker; never loads a driver
// into the capture process. Options override the child only in focused tests.
std::string queryGpuDriverStatus(const GpuProcessOptions& = {});
int runGpuWorker(const GpuFactory&);
}
