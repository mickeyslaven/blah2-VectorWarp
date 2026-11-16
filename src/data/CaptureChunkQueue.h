#ifndef CAPTURECHUNKQUEUE_H
#define CAPTURECHUNKQUEUE_H

#include <algorithm>
#include <complex>
#include <cstddef>
#include <deque>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <vector>
#include "FloatCaptureBlocks.h"

// Consumer-owned capture chunks. Producer FIFOs enter by O(1) move; only the
// fragment crossing a CPI boundary is copied when a complete block is taken.
class CaptureChunkQueue
{
  std::deque<std::deque<std::complex<double>>> chunks;
  std::size_t samples = 0;

public:
  std::size_t size() const { return samples; }
  bool empty() const { return samples == 0; }

  void clear()
  {
    chunks.clear();
    samples = 0;
  }

  void push(std::deque<std::complex<double>>&& chunk)
  {
    if (chunk.empty()) return;
    if (chunk.size() > std::numeric_limits<std::size_t>::max() - samples)
      throw std::overflow_error("Capture chunk queue size exhausted");
    samples += chunk.size();
    chunks.push_back(std::move(chunk));
  }

  std::deque<std::complex<double>> take(std::size_t count)
  {
    if (count > samples)
      throw std::runtime_error("Pending capture is shorter than one CPI");
    std::deque<std::complex<double>> result;
    while (result.size() < count)
    {
      auto& chunk = chunks.front();
      const std::size_t needed = count - result.size();
      if (result.empty() && chunk.size() <= needed)
      {
        result.swap(chunk);
        chunks.pop_front();
        continue;
      }
      if (chunk.size() <= needed)
      {
        result.insert(result.end(),
          std::make_move_iterator(chunk.begin()),
          std::make_move_iterator(chunk.end()));
        chunks.pop_front();
        continue;
      }
      const auto end = chunk.begin() + needed;
      result.insert(result.end(),
        std::make_move_iterator(chunk.begin()),
        std::make_move_iterator(end));
      chunk.erase(chunk.begin(), end);
    }
    samples -= count;
    return result;
  }

  void take_float(std::size_t count,
                  std::vector<std::complex<float>>& result)
  {
    if (count > samples)
      throw std::runtime_error("Pending capture is shorter than one CPI");
    result.resize(count);
    std::size_t written = 0;
    while (written < count)
    {
      auto& chunk = chunks.front();
      const std::size_t copied = std::min(count - written, chunk.size());
      auto source = chunk.begin();
      for (std::size_t index = 0; index < copied; ++index, ++source)
        result[written + index] = std::complex<float>(*source);
      if (copied == chunk.size()) chunks.pop_front();
      else chunk.erase(chunk.begin(), chunk.begin() + copied);
      written += copied;
    }
    samples -= count;
  }
};

// Native FP32 CPI storage moves from producer to consumer to staging. The
// caller's preceding staging allocation returns to the producer pool.
class FloatCaptureChunkQueue
{
  FloatCaptureBlockBatch chunks;
  FloatCaptureBlockBatch::Transfer lastTransfer;

public:
  std::size_t size() const { return chunks.size(); }
  void clear() { chunks.clear(); lastTransfer = {}; }
  void push(FloatCaptureBlockBatch&& blocks) { chunks.push(std::move(blocks)); }
  void push(std::deque<std::complex<float>>&& chunk)
  {
    chunks.push(std::move(chunk));
  }
  auto last_transfer() const { return lastTransfer; }
  void take(std::size_t count, std::vector<std::complex<float>>& result)
  {
    lastTransfer = chunks.take(count, result);
  }
};

#endif
