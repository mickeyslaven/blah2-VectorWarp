#pragma once

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <vector>

// A preallocated, bounded handoff for coherent two-channel CPI blocks.  The
// producer is the RSPduo callback pair; queue overflow retires whole CPIs.
class PairedCpiQueue {
public:
  struct Block { std::vector<int16_t> iq; uint32_t first = 0; uint64_t epoch = 0; };
  struct Stats { uint64_t published = 0, taken = 0, droppedCpis = 0,
    discardedSamples = 0, trailingSamples = 0, discontinuities = 0; size_t highWater = 0; };
private:
  const uint32_t samples_;
  std::vector<Block> blocks_;
  std::vector<size_t> free_, ready_;
  size_t head_ = 0, count_ = 0, writing_ = 0, filled_ = 0, loan_ = 0;
  uint32_t expected_ = 0;
  uint64_t epoch_ = 0;
  bool sequence_ = false, borrowed_ = false, closed_ = false;
  std::mutex producerMutex_;
  mutable std::mutex mutex_;
  std::condition_variable available_;
  std::atomic<size_t> readyCount_{0}, partialCount_{0};
  Stats stats_;

  void discard_ready_locked() {
    while (count_) { free_.push_back(ready_[head_]); head_ = (head_ + 1) % ready_.size(); --count_;
      ++stats_.droppedCpis; stats_.discardedSamples += samples_; }
    readyCount_.store(0, std::memory_order_release);
  }
public:
  PairedCpiQueue(uint32_t samples, size_t readyCapacity) : samples_(samples),
      blocks_(readyCapacity + 2), ready_(readyCapacity) {
    if (!samples || !readyCapacity || readyCapacity > 1024)
      throw std::invalid_argument("Invalid paired CPI queue size");
    free_.reserve(blocks_.size());
    for (size_t i = 0; i < blocks_.size(); ++i) { blocks_[i].iq.resize(size_t(samples) * 4); if (i) free_.push_back(i); }
  }
  PairedCpiQueue(const PairedCpiQueue&) = delete;
  PairedCpiQueue& operator=(const PairedCpiQueue&) = delete;
  uint32_t samples() const { return samples_; }
  size_t backlog_samples() const { return readyCount_.load(std::memory_order_acquire) * samples_ + partialCount_.load(std::memory_order_acquire); }

  // A reset or counter gap invalidates partial and queued old-epoch data.
  void discontinuity() {
    std::lock_guard<std::mutex> producer(producerMutex_); std::lock_guard<std::mutex> lock(mutex_);
    discard_ready_locked(); stats_.discardedSamples += filled_; filled_ = 0; partialCount_.store(0, std::memory_order_release); sequence_ = false; ++epoch_; ++stats_.discontinuities;
  }
  void push(const int16_t* iq, uint32_t count, uint32_t first) {
    if (!iq || !count) throw std::invalid_argument("Invalid paired CPI callback");
    std::lock_guard<std::mutex> producer(producerMutex_);
    if (closed_) throw std::runtime_error("Producer used closed paired CPI queue");
    if (sequence_ && first != expected_) { std::lock_guard<std::mutex> lock(mutex_); discard_ready_locked();
      stats_.discardedSamples += filled_; filled_ = 0; partialCount_.store(0, std::memory_order_release); ++epoch_; ++stats_.discontinuities; }
    sequence_ = true; expected_ = first + count;
    for (uint32_t offset = 0; offset < count;) {
      if (!filled_) { blocks_[writing_].first = first + offset; blocks_[writing_].epoch = epoch_; }
      const size_t take = std::min<size_t>(samples_ - filled_, count - offset);
      std::memcpy(blocks_[writing_].iq.data() + 4 * filled_, iq + 4 * size_t(offset), take * 4 * sizeof(int16_t));
      filled_ += take; partialCount_.store(filled_, std::memory_order_release); offset += take;
      if (filled_ != samples_) continue;
      { std::lock_guard<std::mutex> lock(mutex_);
        if (count_ == ready_.size()) { free_.push_back(ready_[head_]); head_ = (head_ + 1) % ready_.size(); --count_;
          ++stats_.droppedCpis; stats_.discardedSamples += samples_; }
        ready_[(head_ + count_) % ready_.size()] = writing_; ++count_; ++stats_.published;
        readyCount_.store(count_, std::memory_order_release); stats_.highWater = std::max(stats_.highWater, count_);
        if (free_.empty()) throw std::logic_error("Paired CPI queue pool invariant");
        writing_ = free_.back(); free_.pop_back(); filled_ = 0; partialCount_.store(0, std::memory_order_release);
      }
      available_.notify_one();
    }
  }
  void close() { std::lock_guard<std::mutex> producer(producerMutex_); std::lock_guard<std::mutex> lock(mutex_);
    if (closed_) return;
    closed_ = true; stats_.trailingSamples += filled_; filled_ = 0;
    partialCount_.store(0, std::memory_order_release); available_.notify_all(); }
  bool acquire(size_t& slot, bool wait = false) { std::unique_lock<std::mutex> lock(mutex_);
    if (borrowed_) throw std::logic_error("Only one paired CPI consumer is allowed");
    if (wait) available_.wait(lock, [&] { return count_ || closed_; });
    if (!count_) return false;
    slot = ready_[head_]; head_ = (head_ + 1) % ready_.size(); --count_;
    readyCount_.store(count_, std::memory_order_release); loan_ = slot; borrowed_ = true; ++stats_.taken; return true; }
  const Block& block(size_t slot) const { return blocks_.at(slot); }
  void release(size_t slot) { std::lock_guard<std::mutex> lock(mutex_); if (!borrowed_ || slot != loan_) throw std::logic_error("Invalid paired CPI release"); borrowed_ = false; free_.push_back(slot); }
  Stats stats() const { std::lock_guard<std::mutex> lock(mutex_); return stats_; }
};
