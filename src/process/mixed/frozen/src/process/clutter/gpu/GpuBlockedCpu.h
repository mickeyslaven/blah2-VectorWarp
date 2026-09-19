#pragma once
#include "../CorrelationWorker.h"
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <vector>

namespace vectorwarp_gpu_corr_bench {
inline void correlateCpuBlocks(
    std::vector<std::unique_ptr<vectorwarp_clutter::CorrelationWorker>>& workers,
    const vectorwarp_clutter::Complex* x,
    const vectorwarp_clutter::Complex* y,
    uint32_t samples, uint32_t taps, uint32_t length, uint32_t firstBlock,
    fftw_plan plan, vectorwarp_clutter::Complex* caller,
    vectorwarp_clutter::Complex* a, vectorwarp_clutter::Complex* b) {
  if (!x || !y || !plan || !caller || !a || !b || !samples || !taps ||
      taps > length || taps > samples)
    throw std::invalid_argument("Invalid CPU correlation block storage");
  const uint32_t hop = length - taps + 1;
  const uint32_t total = uint32_t((uint64_t(samples) + hop - 1) / hop);
  if (firstBlock >= total)
    throw std::invalid_argument("Invalid CPU correlation block boundary");
  if (firstBlock == 0 && !workers.empty()) {
    // Keep the frozen CPU/FIR-only control exactly on its prior path.
    vectorwarp_clutter::correlate(workers, x, y, samples, taps,
      length, plan, caller, a, b);
    return;
  }
  const uint32_t slots = uint32_t(workers.size()) + 1;
  for (uint32_t block = firstBlock; block < total; block += slots) {
    size_t submitted = 0;
    try {
      for (; submitted < workers.size(); ++submitted) {
        const uint32_t next = block + uint32_t(submitted) + 1;
        if (next >= total) break;
        workers[submitted]->start({x, y, samples, taps, length,
          uint64_t(next) * hop, plan});
      }
      vectorwarp_clutter::correlation_block(x, y, samples, taps, length,
        uint64_t(block) * hop, plan, caller);
      vectorwarp_clutter::accumulate_correlation(caller, length, a, b);
    } catch (...) {
      vectorwarp_clutter::drain_workers(workers, submitted);
      throw;
    }
    for (size_t worker = 0; worker < submitted; ++worker) {
      try {
        vectorwarp_clutter::accumulate_correlation(workers[worker]->finish(),
          length, a, b);
      } catch (...) {
        for (size_t remaining = worker + 1; remaining < submitted; ++remaining)
          try { (void)workers[remaining]->finish(); } catch (...) {}
        throw;
      }
    }
  }
}
}
