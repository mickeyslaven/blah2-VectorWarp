#include "process/ambiguity/GpuDriverStatus.h"
#include <cstdlib>
#include <stdexcept>

// Offline sibling module. It has no Vulkan dependency or GPU create export.
extern "C" std::string blah2_gpu_driver_status(unsigned version) {
  if (version != 1) throw std::runtime_error("bad diagnostic version");
  if (std::getenv("VW_FIXTURE_DRIVER_THROW")) throw std::runtime_error("fixture\n\"failure\\");
  if (std::getenv("VW_FIXTURE_DRIVER_OVERSIZE")) return std::string(32769, 'x');
  return "{\"version\":1,\"available\":true,\"qualification\":\"not-run\",\"devices\":[{\"name\":" +
    blah2::gpuDiagnosticString("fixture\n\"GPU\\") + ",\"bounded\":" +
    blah2::gpuDiagnosticString(std::string(300, 'x')) + "}]}";
}
