#pragma once

#include <asio.hpp>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// Optional RSPduo reference-copy transport. Submission never performs network
// I/O and never waits for a slot: exporter backpressure drops only the copy.
class AsyncReferenceExporter {
public:
  struct Stats {
    uint64_t submitted = 0, sent = 0, dropped = 0, failures = 0;
  };

  AsyncReferenceExporter(std::string endpoint, uint32_t sampleRate,
    uint32_t frequency, uint32_t samples, uint32_t channel);
  ~AsyncReferenceExporter();
  AsyncReferenceExporter(const AsyncReferenceExporter&) = delete;
  AsyncReferenceExporter& operator=(const AsyncReferenceExporter&) = delete;

  // pairedIq is [A-I,A-Q,B-I,B-Q] for each sample.
  bool try_submit(const int16_t* pairedIq, uint32_t samples,
    uint64_t epoch, uint64_t firstSample) noexcept;
  Stats stats() const noexcept;

private:
  struct Frame {
    std::vector<int16_t> iq;
    uint64_t sequence = 0, epoch = 0, firstSample = 0;
    bool free = true;
  };

  const std::string host_;
  const uint16_t port_;
  const uint32_t sampleRate_, frequency_, samples_, channel_;
  mutable std::mutex mutex_;
  std::condition_variable ready_;
  std::vector<Frame> frames_;
  std::deque<size_t> queue_;
  std::thread worker_;
  std::atomic<bool> stopping_{false};
  std::atomic<uint64_t> submitted_{0}, sent_{0}, dropped_{0}, failures_{0};
  uint64_t nextSequence_ = 0;
  asio::io_context io_;
  asio::ip::tcp::socket socket_{io_};

  void run() noexcept;
  void connect();
  void send_frame(const Frame& frame);
};
