#pragma once
#include <string>
namespace blah2 {
struct AccelerationStatus {
  std::string requested = "auto", active = "cpu", device, state = "unavailable", reason;
  double cpuMs = 0, gpuMs = 0;
};
}
