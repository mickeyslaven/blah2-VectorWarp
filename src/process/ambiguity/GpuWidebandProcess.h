#pragma once
#include "process/ambiguity/GpuProcess.h"
#include "process/ambiguity/GpuWidebandChannelizer.h"
#include "data/CanonicalSignal.h"
#include <functional>
#include <string_view>

namespace blah2::wideband_gpu
{
using Factory=std::function<std::unique_ptr<Backend>(const Config&,const std::string&)>;
std::unique_ptr<Backend> createProcess(const Config&,const std::string&,
  const GpuProcessOptions& = {});
// Optional parent-only output ownership. Module-v1 interfaces stay unchanged.
// A completed lease pins its shared mapping and output slot across reset,
// subsequent frames, worker failure, and Process destruction. Two slots bound
// storage; submission refuses before staging if every slot is still owned.
struct LeasedLaneOutput
{
  uint64_t frequencyHz = 0, firstInputSample = 0;
  bool hasFirstInputSample = false;
  uint32_t inputSamplesPerOutput = 0;
  iq::BorrowedSamples samples;
};
class LeasedOutputBackend
{
public:
  virtual ~LeasedOutputBackend() = default;
  virtual bool supportsLeasedOutput() const noexcept = 0;
  virtual std::vector<LeasedLaneOutput> finishLeasedFrame() = 0;
};
// Uses two output slots only within the existing 512-MiB geometry budget;
// larger geometries retain the single-slot owned output path. The original
// createProcess remains a single-slot protocol consumer for compatibility.
std::unique_ptr<Backend> createLeasedProcess(const Config&,const std::string&,
  const GpuProcessOptions& = {});
// Only worker transport/lifetime loss is safe to recreate automatically.
// Geometry, protocol identity, shape, and numerical failures remain sticky.
bool recoverableWorkerFailure(std::string_view) noexcept;
int runWorker(const Factory&);
}
