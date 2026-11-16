#pragma once

#include <atomic>
#include <chrono>
#include <stdexcept>

namespace blah2::integration {

// Optional cooperative budget for diagnostic workers only. Ordinary replay
// callers omit it and retain the existing numerical contract.
struct EvidenceWorkBudget {
  std::chrono::steady_clock::time_point deadline;
  const std::atomic<bool>* stop = nullptr;
  void check() const {
    if ((stop && stop->load(std::memory_order_relaxed)) ||
        std::chrono::steady_clock::now() >= deadline)
      throw std::runtime_error("Temporal shadow work budget exhausted");
  }
};

} // namespace blah2::integration
