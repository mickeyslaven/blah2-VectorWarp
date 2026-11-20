#pragma once

#include <atomic>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <vector>
#include "process/utility/FixedExecutor.h"

namespace blah2::integration {

// Optional cooperative budget for diagnostic workers only. Ordinary replay
// callers omit it and retain the existing numerical contract.
struct EvidenceWorkBudget {
  std::chrono::steady_clock::time_point deadline;
  const std::atomic<bool>* stop = nullptr;
  utility::FixedExecutor* executor = nullptr;
  const std::atomic<uint64_t>* generation = nullptr;
  uint64_t expectedGeneration = 0;
  void check() const {
    if (generation && generation->load(std::memory_order_relaxed) != expectedGeneration)
      throw std::runtime_error("Temporal evidence work generation invalidated");
    if ((stop && stop->load(std::memory_order_relaxed)) ||
        std::chrono::steady_clock::now() >= deadline)
      throw std::runtime_error("Temporal evidence work budget exhausted");
  }
  void forEach(std::size_t count, const std::function<void(std::size_t)>& work) const {
    if (executor) executor->borrow(count, work, [&] { check(); });
    else for (std::size_t index = 0; index < count; ++index) { check(); work(index); }
  }
};

// Fixed scratch ceiling is part of the existing small-workspace allowance.
// Tiles compute independent levels; only this ordered consumer reduces them.
inline constexpr std::size_t temporalTileScratchBytes = 1u << 20;
template<class Compute, class Reduce>
void orderedRowTiles(uint32_t rows, uint32_t columns, uint32_t fields,
    const EvidenceWorkBudget& budget, Compute compute, Reduce reduce) {
  const std::size_t slots = std::min<std::size_t>(8, budget.executor->size());
  const std::size_t rowWords = std::size_t(columns) * fields;
  if (!rowWords || rowWords * slots > temporalTileScratchBytes / sizeof(double))
    throw std::invalid_argument("Temporal row tile exceeds bounded scratch");
  const std::size_t tileRows = std::min<std::size_t>((rows + slots - 1) / slots,
    temporalTileScratchBytes / sizeof(double) / rowWords / slots);
  const std::size_t tileWords = tileRows * rowWords;
  std::vector<double> levels(tileWords * slots);
  for (std::size_t first = 0; first < rows; first += tileRows * slots) {
    const auto count = std::min(slots, (rows - first + tileRows - 1) / tileRows);
    budget.forEach(count, [&](std::size_t tile) {
      const auto begin = first + tile * tileRows;
      compute(uint32_t(begin), uint32_t(std::min<std::size_t>(rows, begin + tileRows)),
        levels.data() + tile * tileWords);
    });
    for (std::size_t tile = 0; tile < count; ++tile) {
      budget.check();
      const auto begin = first + tile * tileRows;
      reduce(uint32_t(begin), uint32_t(std::min<std::size_t>(rows, begin + tileRows)),
        levels.data() + tile * tileWords);
    }
  }
}

// Exclude a one-time setup interval without resetting work already charged to
// the observer. This preserves one aggregate deadline for all ordinary work.
inline EvidenceWorkBudget excludeOneTimeSetup(
    EvidenceWorkBudget budget,
    std::chrono::steady_clock::time_point setupStart,
    std::chrono::steady_clock::time_point setupComplete) {
  if (setupComplete > setupStart)
    budget.deadline += setupComplete - setupStart;
  return budget;
}

} // namespace blah2::integration
