#pragma once

#include <fftw3.h>

#include <atomic>
#include <stdexcept>

namespace blah2 {

// FFTW 3.3.8 exposes the planner-thread setter but not its getter. Keep the
// process-configured value alongside the setter so planning scopes can restore
// it on every supported FFTW release. Frozen mixed-worker DSP sources include
// this utility intentionally; it contains no DSP implementation or layout.
inline std::atomic<int>& fftw_planner_thread_setting() {
  static std::atomic<int> setting{1};
  return setting;
}

inline int fftw_planner_threads() noexcept {
  return fftw_planner_thread_setting().load(std::memory_order_relaxed);
}

inline void set_fftw_planner_threads(int threads) {
  if (threads < 1) throw std::invalid_argument("FFTW planner thread count must be positive");
  fftw_plan_with_nthreads(threads);
  fftw_planner_thread_setting().store(threads, std::memory_order_relaxed);
}

} // namespace blah2
