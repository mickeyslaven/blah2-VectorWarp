#ifndef FLOATCAPTUREBLOCKS_H
#define FLOATCAPTUREBLOCKS_H

#include <algorithm>
#include <complex>
#include <cstddef>
#include <deque>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <utility>
#include <vector>

// Only the pool is shared across producer/consumer threads. Queue ownership
// remains with the existing capture locks and consumer thread respectively.
class FloatCaptureBlockPool
{
public:
  using Sample = std::complex<float>;
  using Buffer = std::vector<Sample>;
  struct Stats { std::size_t allocated, limit, allocations, reuses; };

private:
  const std::size_t blockSamples_, limit_;
  mutable std::mutex mutex_;
  std::vector<Buffer> free_;
  std::size_t allocated_ = 0, allocations_ = 0, reuses_ = 0;

public:
  FloatCaptureBlockPool(std::size_t blockSamples, std::size_t capacity,
                        std::size_t warmBlocks = 0)
    : blockSamples_(blockSamples),
      // One partial block may straddle the configured sample budget. The
      // processing vector already exists outside that queued-sample budget.
      limit_(blockSamples ? capacity / blockSamples +
        (capacity % blockSamples != 0) + 1 : 0)
  {
    if (!blockSamples)
      throw std::invalid_argument("Native float capture block must be nonempty");
    free_.reserve(limit_);
    // The live caller warms only two CPIs before starting the receiver. Avoid
    // first-touching a full CPI in recv(), without reserving the entire backlog
    // budget on machines with constrained RAM. Further growth stays bounded.
    for (std::size_t i = 0; i < std::min(warmBlocks, limit_); ++i) {
      free_.emplace_back(blockSamples_);
      ++allocated_;
      ++allocations_;
    }
  }

  std::size_t block_samples() const { return blockSamples_; }

  Buffer acquire()
  {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!free_.empty()) {
        auto result = std::move(free_.back());
        free_.pop_back();
        ++reuses_;
        return result;
      }
      if (allocated_ == limit_)
        throw std::runtime_error("Native float capture block pool exhausted");
      ++allocated_;
    }
    try {
      Buffer result(blockSamples_);
      std::lock_guard<std::mutex> lock(mutex_);
      ++allocations_;
      return result;
    } catch (...) {
      std::lock_guard<std::mutex> lock(mutex_);
      --allocated_;
      throw;
    }
  }

  void recycle(Buffer&& buffer) noexcept
  {
    std::lock_guard<std::mutex> lock(mutex_);
    // A zero-copy take exchanges the queued allocation for the caller's old
    // vector. An empty/different-sized caller surrenders that pool allocation;
    // it must not permanently consume a pool slot (e.g. CPU fallback frames).
    if (buffer.size() == blockSamples_ && buffer.capacity() == blockSamples_)
      free_.push_back(std::move(buffer));
    else
      --allocated_;
  }

  Stats stats() const
  {
    std::lock_guard<std::mutex> lock(mutex_);
    return {allocated_, limit_, allocations_, reuses_};
  }
};

struct FloatCaptureBlock
{
  using Buffer = FloatCaptureBlockPool::Buffer;
  std::shared_ptr<FloatCaptureBlockPool> pool;
  Buffer data;
  std::size_t first = 0, count = 0;

  FloatCaptureBlock() = default;
  FloatCaptureBlock(std::shared_ptr<FloatCaptureBlockPool> owner, Buffer values)
    : pool(std::move(owner)), data(std::move(values)) {}
  FloatCaptureBlock(FloatCaptureBlock&& other) noexcept
    : pool(std::move(other.pool)), data(std::move(other.data)),
      first(other.first), count(other.count) {}
  FloatCaptureBlock& operator=(FloatCaptureBlock&&) = delete;
  FloatCaptureBlock(const FloatCaptureBlock&) = delete;
  FloatCaptureBlock& operator=(const FloatCaptureBlock&) = delete;
  ~FloatCaptureBlock() { if (pool) pool->recycle(std::move(data)); }
};

class FloatCaptureStorage;

// A move-only envelope owns both samples and their recycling destination.
// Destruction/reset of the producer cannot invalidate transferred blocks.
class FloatCaptureBlockBatch
{
  friend class FloatCaptureStorage;
  std::deque<FloatCaptureBlock> blocks_;
  std::size_t samples_ = 0;

public:
  struct Transfer { std::size_t swapped = 0, copied = 0; };
  FloatCaptureBlockBatch() = default;
  FloatCaptureBlockBatch(FloatCaptureBlockBatch&& other)
    : samples_(other.samples_)
  { blocks_.swap(other.blocks_); other.samples_ = 0; }
  FloatCaptureBlockBatch& operator=(FloatCaptureBlockBatch&& other) noexcept
  {
    if (this != &other) {
      clear();
      blocks_.swap(other.blocks_);
      samples_ = other.samples_;
      other.samples_ = 0;
    }
    return *this;
  }
  FloatCaptureBlockBatch(const FloatCaptureBlockBatch&) = delete;
  FloatCaptureBlockBatch& operator=(const FloatCaptureBlockBatch&) = delete;

  std::size_t size() const { return samples_; }
  bool empty() const { return samples_ == 0; }
  void clear() { blocks_.clear(); samples_ = 0; }

  void push(FloatCaptureBlockBatch&& other)
  {
    if (other.samples_ > std::numeric_limits<std::size_t>::max() - samples_)
      throw std::overflow_error("Float capture block count exhausted");
    while (!other.blocks_.empty()) {
      const auto count = other.blocks_.front().count;
      blocks_.push_back(std::move(other.blocks_.front()));
      other.blocks_.pop_front();
      samples_ += count;
      other.samples_ -= count;
    }
  }

  // Compatibility for small legacy/test chunks. Native producers use pooled
  // blocks directly and never enter this copying adapter.
  void push(std::deque<std::complex<float>>&& values)
  {
    if (values.empty()) return;
    if (values.size() > std::numeric_limits<std::size_t>::max() - samples_)
      throw std::overflow_error("Float capture block count exhausted");
    FloatCaptureBlock block({}, {values.begin(), values.end()});
    block.count = values.size();
    blocks_.push_back(std::move(block));
    samples_ += values.size();
  }

  Transfer take(std::size_t count, std::vector<std::complex<float>>& result)
  {
    if (count > samples_)
      throw std::runtime_error("Pending float capture is shorter than one CPI");
    if (count && blocks_.front().first == 0 &&
        blocks_.front().count == count &&
        blocks_.front().data.size() == count) {
      result.swap(blocks_.front().data);
      blocks_.pop_front();
      samples_ -= count;
      return {count, 0};
    }
    result.resize(count);
    std::size_t written = 0;
    while (written < count) {
      auto& block = blocks_.front();
      const auto copied = std::min(count - written, block.count);
      std::copy_n(block.data.data() + block.first, copied,
        result.data() + written);
      block.first += copied;
      block.count -= copied;
      samples_ -= copied;
      written += copied;
      if (!block.count) blocks_.pop_front();
    }
    return {0, count};
  }

  void discard(std::size_t count)
  {
    if (count > samples_)
      throw std::runtime_error("Discard exceeds native float capture samples");
    while (count) {
      auto& block = blocks_.front();
      const auto retired = std::min(count, block.count);
      block.first += retired;
      block.count -= retired;
      samples_ -= retired;
      count -= retired;
      if (!block.count) blocks_.pop_front();
    }
  }

  template<class Complex = std::complex<double>>
  std::deque<Complex> copy(std::size_t count) const
  {
    if (count > samples_)
      throw std::runtime_error("Copy exceeds native float capture samples");
    std::deque<Complex> result;
    for (const auto& block : blocks_) {
      const auto copied = std::min(count, block.count);
      const auto first = block.data.begin() + block.first;
      result.insert(result.end(), first, first + copied);
      count -= copied;
      if (!count) break;
    }
    return result;
  }
};

class FloatCaptureStorage
{
  std::shared_ptr<FloatCaptureBlockPool> pool_;
  FloatCaptureBlockBatch queued_;

public:
  FloatCaptureStorage(std::size_t blockSamples, std::size_t capacity,
                      std::size_t warmBlocks = 0)
  {
    if (!blockSamples)
      throw std::invalid_argument("Native float capture block must be nonempty");
    pool_ = std::make_shared<FloatCaptureBlockPool>(blockSamples, capacity,
      warmBlocks);
  }
  std::size_t size() const { return queued_.size(); }
  bool empty() const { return queued_.empty(); }
  void clear() { queued_.clear(); }
  auto stats() const { return pool_->stats(); }
  void discard(std::size_t count) { queued_.discard(count); }

  template<class Complex = std::complex<double>>
  std::deque<Complex> copy(std::size_t count) const
  { return queued_.copy<Complex>(count); }

  void append(const std::complex<float>* values, std::size_t count)
  {
    if (count && !values) throw std::invalid_argument("Null native float block");
    while (count) {
      if (queued_.blocks_.empty() || queued_.blocks_.back().first +
          queued_.blocks_.back().count == pool_->block_samples()) {
        FloatCaptureBlock block(pool_, pool_->acquire());
        queued_.blocks_.push_back(std::move(block));
      }
      auto& block = queued_.blocks_.back();
      const auto end = block.first + block.count;
      const auto copied = std::min(count, block.data.size() - end);
      std::copy_n(values, copied, block.data.data() + end);
      block.count += copied;
      queued_.samples_ += copied;
      values += copied;
      count -= copied;
    }
  }

  FloatCaptureBlockBatch take_complete(std::size_t minimumSamples = 0)
  {
    FloatCaptureBlockBatch result;
    while (!queued_.blocks_.empty()) {
      auto& block = queued_.blocks_.front();
      // Keep normal sub-CPI receive fragments in the writable producer block.
      // A requested minimum also supports fragmented legacy/overflow tails.
      if (block.first + block.count < block.data.size() &&
          result.size() >= minimumSamples) break;
      const auto count = block.count;
      result.blocks_.push_back(std::move(block));
      queued_.blocks_.pop_front();
      result.samples_ += count;
      queued_.samples_ -= count;
    }
    return result;
  }
};

#endif
