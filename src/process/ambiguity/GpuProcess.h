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
  // Wideband workers pre-touch the shared mapping and execute/reset one
  // maximum-size capture before accepting live input. Tests may disable it.
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

// Shader/pipeline construction is a one-time cost and may exceed one CPI, but
// an optional accelerator that cannot become ready within a bounded number of
// capture intervals must not hold acquisition startup for the transport's old
// 30-second default. Short CPIs retain a five-second driver/cache allowance;
// long CPIs may use up to twenty intervals, never beyond the transport bound.
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

// A live frame wait must leave capture-queue recovery headroom. The historical
// five-second transport default consumes ten 0.5-second CPIs before failing,
// which can fill an otherwise healthy bounded input queue before the caller can
// recover. Permit three CPI intervals (with a small scheduling floor): this
// tolerates one bounded OS/driver scheduling excursion while retaining at
// least half of the normal seven-CPI live input queue for recovery, while
// retaining the transport's explicit upper bound for long/offline frames.
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
