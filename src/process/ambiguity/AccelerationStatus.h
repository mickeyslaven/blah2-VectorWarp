#pragma once
#include <string>
#include <cstdint>
#include <vector>
namespace blah2 {
struct AccelerationMember {
  std::string laneId, backend, device, state, reason;
  bool gpuExecuted = false, cpuExecuted = false, positioned = false;
  uint64_t firstInputSample = 0, endInputSample = 0;
  // Completed GPU work is independent of the backend selected after admission.
  std::string executedBackend;
};
struct AccelerationStatus {
  std::string requested = "auto", active = "cpu", device, state = "unavailable", reason;
  double cpuMs = 0, gpuMs = 0;
  std::vector<AccelerationMember> members;
  std::string gpuExecutionBackend;
};
}
