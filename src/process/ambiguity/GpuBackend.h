#pragma once
#include <complex>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace blah2 {
// Kind 1 belonged to the retired mixed-worker FIR route. Never reuse it.
enum class GpuWorkKind : uint32_t { radar = 0, retiredFir = 1 };
// All lengths are configuration-derived. No capture backend or site assumptions.
struct GpuGeometry {
  uint32_t range, doppler, delays, channels;
  int32_t delayMin;
  // Optional batched clutter geometry. Zero samples keeps ambiguity-only
  // source callers compatible; GPU_ABI 4 rejects binary geometry/contract mismatches.
  uint32_t clutterSamples = 0, clutterBins = 0;
  int32_t clutterDelayMin = 0;
  // ABI 4 reserves the retired FIR tag and trailing fields. Keep their layout
  // for old workers/modules, but require radar kind and zero reserved fields.
  GpuWorkKind kind = GpuWorkKind::radar;
  uint32_t firSamples = 0, firTaps = 0, firFft = 0, firPercent = 0;
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
// Optional interfaces deliberately live beside, rather than inside, GpuBackend:
// existing test/in-process backends keep the version-1 virtual table unchanged.
struct GpuFrameBuffers {
  std::complex<float>* reference = nullptr;
  size_t referenceCount = 0;
  std::complex<float>* surveillance = nullptr;
  size_t surveillanceCount = 0;
  std::complex<float>* output = nullptr;
  size_t outputCount = 0;
};
class GpuFrameBackend {
public:
  virtual ~GpuFrameBackend() = default;
  virtual GpuFrameBuffers frameBuffers() = 0;
  virtual void processFrame() = 0;
};
class GpuBufferBackend {
public:
  virtual ~GpuBufferBackend() = default;
  virtual void processBuffers(const std::complex<float>* reference, size_t referenceCount,
    const std::complex<float>* surveillance, size_t surveillanceCount,
    std::complex<float>* output, size_t outputCount) = 0;
};
struct GpuClutterBuffers {
  std::complex<float>* reference = nullptr;
  size_t referenceCount = 0;
  std::complex<float>* surveillance = nullptr;
  size_t surveillanceCount = 0;
  // The output is the estimated clutter, not the filtered surveillance. The
  // parent subtracts it from its immutable FP64 input to avoid cancellation in
  // FP32 while retaining GPU FFT/filter execution.
  std::complex<float>* output = nullptr;
  size_t outputCount = 0;
};
class GpuClutterFrameBackend {
public:
  virtual ~GpuClutterFrameBackend() = default;
  virtual bool clutterAvailable() const = 0;
  virtual GpuClutterBuffers clutterBuffers() = 0;
  // False is a data/precision rejection: the worker remains healthy and the
  // caller must run this frame on CPU. Exceptions remain transport/device faults.
  virtual bool processClutterFrame() = 0;
};
// Optional host-side scheduling interface; existing worker/module vtables and
// wire layouts are unchanged. The callback runs once before exposing results,
// and may overlap isolated worker execution. It must not mutate shared buffers.
class GpuClutterHostWorkBackend {
public:
  virtual ~GpuClutterHostWorkBackend() = default;
  virtual bool processClutterFrameWithHostWork(const std::function<void()>& hostWork) = 0;
};
class GpuClutterBufferBackend {
public:
  virtual ~GpuClutterBufferBackend() = default;
  virtual bool processClutterBuffers(const std::complex<float>* reference, size_t referenceCount,
    const std::complex<float>* surveillance, size_t surveillanceCount,
    std::complex<float>* output, size_t outputCount) = 0;
};
// Module ABI is versioned, built with the executable, and loaded only by explicit
// path beside the executable (never an untrusted current-working-directory path).
constexpr unsigned GPU_ABI = 4;
using GpuCreate = GpuBackend* (*)(unsigned, const GpuGeometry*, const char*);
using GpuList = std::vector<GpuDevice> (*)();
// Diagnostic JSON only; not the geometry/worker ABI. Driver strings are bounded
// and escaped independently of the optional Vulkan module's other interfaces.
inline std::string gpuDiagnosticString(const std::string& value) {
  std::string result = "\"";
  for (size_t i = 0; i < value.size() && i < 240; ++i) {
    const unsigned char c = value[i];
    if (c == '"' || c == '\\') result += '\\';
    if (c < 32 || c >= 127) result += '?';
    else result += static_cast<char>(c);
  }
  return result + '"';
}
using GpuDriverStatus = std::string (*)(unsigned);
}
