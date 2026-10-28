#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace blah2 {
struct IqClipRequest {
  uint64_t revision = 0;
  int64_t requestedEventTimeMs = 0;
  int64_t windowStartMs = 0;
  int64_t windowEndMs = 0;
  std::vector<std::string> eventIds;
  std::vector<std::string> passIds;
};

struct IqClipStatus {
  uint64_t revision = 0;
  bool terminal = false;
  std::string file;
  std::string error;
  uint64_t samples = 0;
};
}
