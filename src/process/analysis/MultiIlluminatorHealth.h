#pragma once

#include <cstdint>
#include <string>

namespace blah2::multi_illuminator {

struct LaneHealthStatus {
  std::string illuminatorId;
  uint64_t frequencyHz = 0;
  uint64_t acceptedCpis = 0;
  uint64_t rejectedCpis = 0;
  bool hasLastCpi = false;
  bool lastAccepted = false;
  uint64_t lastFirstInputSample = 0;
  uint64_t lastEndInputSample = 0;
  std::string lastReason;
};

} // namespace blah2::multi_illuminator
