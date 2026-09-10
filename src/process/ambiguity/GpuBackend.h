#pragma once
#include <complex>
#include <cstdint>
#include <string>
#include <vector>

namespace blah2 {
// All lengths are configuration-derived. No capture backend or site assumptions.
struct GpuGeometry {
  uint32_t range, doppler, delays, channels;
  int32_t delayMin;
};
struct GpuDevice { std::string id, name; uint64_t memoryBytes; };
class GpuBackend {
public:
  virtual ~GpuBackend() = default;
  virtual GpuDevice device() const = 0;
  virtual void process(const std::vector<std::complex<float>>& reference,
    const std::vector<std::complex<float>>& surveillance,
    std::vector<std::complex<float>>& output) = 0;
};
// Module ABI is versioned, built with the executable, and loaded only by explicit
// path beside the executable (never an untrusted current-working-directory path).
constexpr unsigned GPU_ABI = 1;
using GpuCreate = GpuBackend* (*)(unsigned, const GpuGeometry*, const char*);
using GpuList = std::vector<GpuDevice> (*)();
}
