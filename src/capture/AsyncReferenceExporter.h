#pragma once

#include "data/IqData.h"

#include <asio.hpp>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// Bounded best-effort copy of completed reference CPIs. Submission performs
// no network or disk I/O and waits only for a bounded in-memory bookkeeping
// critical section, never for capacity. If transport falls behind, only this
// optional copy is dropped; Owl's native CPI is untouched.
class AsyncReferenceExporter {
public:
  struct Stats { uint64_t submitted=0,sent=0,dropped=0,failures=0; };

  AsyncReferenceExporter(std::string endpoint, uint32_t sampleRate,
    uint32_t frequency, uint32_t samples);
  ~AsyncReferenceExporter();
  AsyncReferenceExporter(const AsyncReferenceExporter&) = delete;
  AsyncReferenceExporter& operator=(const AsyncReferenceExporter&) = delete;

  bool try_submit(const IqData& reference, uint64_t epoch,
    uint64_t firstSample) noexcept;
  Stats stats() const noexcept;

private:
  struct Frame {
    std::vector<int16_t> iq;
    uint64_t sequence=0,epoch=0,firstSample=0;
    bool free=true;
  };
  const std::string host_;
  const uint16_t port_;
  const uint32_t sampleRate_,frequency_,samples_;
  std::mutex mutex_;
  std::condition_variable ready_;
  std::vector<Frame> frames_;
  std::deque<size_t> queue_;
  std::thread worker_;
  std::atomic<bool> stopping_{false};
  std::atomic<uint64_t> submitted_{0},sent_{0},dropped_{0},failures_{0};
  uint64_t nextSequence_=0;
  asio::io_context io_;
  asio::ip::tcp::socket socket_{io_};

  void run() noexcept;
  void connect();
  void send_frame(const Frame& frame);
};
