#pragma once
#include "data/IqData.h"
#include <algorithm>
#include <chrono>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <stdexcept>

// Diagnostic durations are monotonic host timings, not RF timestamps. Keep
// logging out of the receive hot path; emit the aggregate only on failure.
struct UsrpIngressTiming {
  using Clock = std::chrono::steady_clock;
  Clock::time_point previousReturn{};
  std::uint64_t blocks = 0, samples = 0;
  double maxServiceGapMs = 0, maxQueueWaitMs = 0, maxQueueWriteMs = 0;

  static double milliseconds(Clock::time_point start, Clock::time_point end) {
    return std::chrono::duration<double, std::milli>(end - start).count();
  }
  void before_receive(Clock::time_point now = Clock::now()) {
    if (previousReturn != Clock::time_point{})
      maxServiceGapMs = std::max(maxServiceGapMs, milliseconds(previousReturn, now));
  }
  void received(Clock::time_point now = Clock::now()) { previousReturn = now; }
  std::string summary() const {
    std::ostringstream out;
    out << std::fixed << std::setprecision(3)
        << " [host receive diagnostics: accepted_blocks=" << blocks
        << ", accepted_samples_per_channel=" << samples
        << ", max_between_reads_ms=" << maxServiceGapMs
        << ", max_queue_wait_ms=" << maxQueueWaitMs
        << ", max_queue_write_ms=" << maxQueueWriteMs << "]";
    return out.str();
  }
};

inline void append_usrp_block(IqData& first, IqData& second,
    const std::complex<float>* a, const std::complex<float>* b,
    std::size_t count, UsrpIngressTiming& timing) {
  if (&first == &second || (count && (!a || !b)))
    throw std::invalid_argument("[USRP] Invalid paired receive buffers.");
  const auto start = UsrpIngressTiming::Clock::now();
  std::unique_lock<IqData> firstLock(first);
  std::unique_lock<IqData> secondLock(second);
  const auto locked = UsrpIngressTiming::Clock::now();
  timing.maxQueueWaitMs = std::max(timing.maxQueueWaitMs,
    UsrpIngressTiming::milliseconds(start, locked));
  const std::size_t size = first.get_length();
  if (size != second.get_length())
    throw std::runtime_error("[USRP] Input channels lost sample alignment; input stopped.");
  if (size > first.get_n() || size > second.get_n() ||
      count > first.get_n() - size || count > second.get_n() - size)
    throw std::runtime_error("[USRP] IQ processing queue is full (" +
      std::to_string(size) + " samples queued; incoming block " +
      std::to_string(count) + "). Input stopped before dropping samples. "
      "Check processing load, recording speed and host stalls; reduce the workload "
      "if processing cannot keep up, then restart.");
  first.append_unlocked(a, count);
  second.append_unlocked(b, count);
  timing.maxQueueWriteMs = std::max(timing.maxQueueWriteMs,
    UsrpIngressTiming::milliseconds(locked, UsrpIngressTiming::Clock::now()));
  if (count) { ++timing.blocks; timing.samples += count; }
}
