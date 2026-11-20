#pragma once
#include <algorithm>
#include <cstdint>
#include <mutex>
#include <stdexcept>

namespace blah2::resident {
// Reservations precede worker creation. Mapped IPC plus every worker's actual
// Vulkan allocations fit the aggregate ceiling even during startup/failure.
// Each lane owns its worker/state. The shared execution mutex serializes worker
// construction/destruction and resident transactions. Portable transactions
// use worker-local locks and may overlap up to the bounded lane executor size.
struct LaneBudget {
  std::mutex execution;
  const uint32_t sessions;
  const uint64_t totalBytes, bytesPerSession;
  LaneBudget(uint32_t lanes, uint32_t maximumSessions = 8,
      uint64_t maximumBytes = 3ULL << 30)
    : sessions(std::min(lanes, maximumSessions)), totalBytes(maximumBytes),
      bytesPerSession(sessions ? maximumBytes / sessions : 0) {
    if (!lanes || !maximumSessions || maximumSessions > 8 ||
        maximumBytes < (64ULL << 20) || maximumBytes > (4ULL << 30))
      throw std::invalid_argument("Resident lane aggregate budget is invalid");
  }
  uint64_t allocationLimit(uint32_t lane, uint64_t mappingBytes) const {
    if (lane >= sessions)
      throw std::runtime_error("aggregate-gpu-capacity-exhausted: resident session cap");
    if (mappingBytes >= bytesPerSession ||
        bytesPerSession - mappingBytes < (16ULL << 20))
      throw std::runtime_error("aggregate-gpu-capacity-exhausted: resident mapping reservation");
    return bytesPerSession - mappingBytes;
  }
};
}
