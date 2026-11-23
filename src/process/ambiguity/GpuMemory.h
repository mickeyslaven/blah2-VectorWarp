#pragma once
#include <algorithm>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

namespace blah2::gpu_memory {
// These values intentionally match Vulkan 1.0 VkMemoryPropertyFlagBits. Keeping
// the policy independent of Vulkan makes capability and alignment decisions
// testable without a physical GPU.
constexpr uint32_t deviceLocal = 0x1;
constexpr uint32_t hostVisible = 0x2;
constexpr uint32_t hostCoherent = 0x4;
constexpr uint32_t hostCached = 0x8;
constexpr uint32_t noType = std::numeric_limits<uint32_t>::max();
constexpr uint64_t maximumDeviceBytes = 2ULL << 30;
constexpr uint64_t maximumAggregateBytes = 4ULL << 30;
inline uint64_t heapBudget(uint64_t heapBytes) {
  return std::min(maximumDeviceBytes, heapBytes / 4);
}

struct AllocationPolicy {
  uint64_t bytes = 0;
  uint64_t deviceBytes = 0;
  bool separateHostHeaps = true;
  bool aggregateIncludesHostHeaps = false;
};

struct Type { uint32_t flags = 0, heap = 0; };
struct Heap {
  uint64_t bytes = 0;
  bool device = false;
  // UINT64_MAX means the driver did not expose a free-memory budget.
  uint64_t available = UINT64_MAX;
};
struct Properties { std::vector<Type> types; std::vector<Heap> heaps; };

inline uint64_t heapAllowance(const Heap& heap) {
  if (heap.available == UINT64_MAX) return heapBudget(heap.bytes);
  const uint64_t available = std::min(heap.bytes, heap.available);
  // Reserve one quarter of physical capacity beyond memory already in use.
  const uint64_t reserve = heap.bytes / 4;
  return std::min(maximumDeviceBytes, available > reserve ? available - reserve : 0);
}

// A supervisor's lane reservation covers both host and device allocations.
// Parse without stoull's whitespace/sign acceptance or an unbounded copy, and
// never let an override enlarge the device's conservative default allowance.
inline AllocationPolicy allocationPolicy(const Properties& properties, const char* bounded) {
  uint64_t deviceBytes = 0;
  for (const auto& heap : properties.heaps)
    if (heap.device) deviceBytes = std::max(deviceBytes, heapAllowance(heap));
  if (!deviceBytes) throw std::runtime_error("GPU memory safety reserve leaves no allocation budget");
  AllocationPolicy policy{deviceBytes, deviceBytes, true, false};
  if (!bounded) return policy;
  uint64_t requested = 0;
  unsigned count = 0;
  for (const char* digit = bounded; *digit; ++digit) {
    if (++count > 20 || *digit < '0' || *digit > '9')
      throw std::invalid_argument("GPU lane allocation ceiling is invalid");
    const uint64_t value = uint64_t(*digit - '0');
    if (requested > (maximumAggregateBytes - value) / 10)
      throw std::invalid_argument("GPU lane allocation ceiling exceeds policy");
    requested = requested * 10 + value;
  }
  if (!count || !requested)
    throw std::invalid_argument("GPU lane allocation ceiling must be positive");
  policy.bytes = requested;
  // The supervisor quota never rewrites the hardware-derived device proof.
  policy.aggregateIncludesHostHeaps = true;
  return policy;
}

// Reserve actual Vulkan allocation sizes, including FFT-owned tables/scratch.
// Types that alias one heap must share its allowance. Subtraction before the
// comparison also rejects oversized requests without overflowing a counter.
class AllocationBudget {
public:
  struct Rejection {
    const char* reason = "none";
    uint32_t type = noType, heap = noType;
    uint64_t requested = 0, used = 0, heapUsed = 0, heapLimit = 0,
      deviceUsed = 0, requiredAtLeast = 0;
  };
private:
  Properties properties_;
  uint64_t limit_, deviceLimit_, used_ = 0, peak_ = 0;
  uint64_t deviceUsed_ = 0, devicePeak_ = 0;
  bool separateHostHeaps_ = false;
  std::vector<uint64_t> heapUsed_;
  Rejection rejection_;
  bool reject(uint32_t type,uint64_t bytes,const char* reason) {
    const uint32_t heap=type<properties_.types.size()?properties_.types[type].heap:noType;
    rejection_={reason,type,heap,bytes,used_,heap<heapUsed_.size()?heapUsed_[heap]:0,
      heap<properties_.heaps.size()?heapAllowance(properties_.heaps[heap]):0,
      deviceUsed_,bytes>UINT64_MAX-used_?UINT64_MAX:used_+bytes};
    return false;
  }
public:
  AllocationBudget(AllocationPolicy policy, Properties properties)
    : properties_(std::move(properties)), limit_(policy.bytes),
      deviceLimit_(policy.deviceBytes), separateHostHeaps_(policy.separateHostHeaps),
      heapUsed_(properties_.heaps.size()) {
    if (!limit_ || !deviceLimit_)
      throw std::invalid_argument("GPU allocation policy is invalid");
    if (separateHostHeaps_ && !policy.aggregateIncludesHostHeaps) {
      for (const auto& heap : properties_.heaps) if (!heap.device) {
        const uint64_t allowance = heapAllowance(heap);
        limit_ = allowance > std::numeric_limits<uint64_t>::max() - limit_ ?
          std::numeric_limits<uint64_t>::max() : limit_ + allowance;
      }
    }
  }
  bool reserve(uint32_t type, uint64_t bytes) {
    if (type >= properties_.types.size() || !bytes)
      return reject(type,bytes,"invalid-request");
    if(bytes > limit_ - used_)return reject(type,bytes,"aggregate-budget");
    const auto heap = properties_.types[type].heap;
    if (heap >= heapUsed_.size()) return reject(type,bytes,"invalid-heap");
    if (separateHostHeaps_ && properties_.heaps[heap].device &&
        bytes > deviceLimit_ - deviceUsed_) return reject(type,bytes,"device-budget");
    const auto heapLimit = heapAllowance(properties_.heaps[heap]);
    if (bytes > heapLimit - heapUsed_[heap]) return reject(type,bytes,"heap-budget");
    heapUsed_[heap] += bytes; used_ += bytes; peak_ = std::max(peak_, used_);
    if (properties_.heaps[heap].device) {
      deviceUsed_ += bytes;
      devicePeak_ = std::max(devicePeak_, deviceUsed_);
    }
    return true;
  }
  void release(uint32_t type, uint64_t bytes) {
    const auto heap = properties_.types.at(type).heap;
    if (properties_.heaps.at(heap).device) deviceUsed_ -= bytes;
    heapUsed_.at(heap) -= bytes; used_ -= bytes;
  }
  uint64_t used() const { return used_; }
  uint64_t peak() const { return peak_; }
  uint64_t limit() const { return limit_; }
  uint64_t deviceUsed() const { return deviceUsed_; }
  uint64_t devicePeak() const { return devicePeak_; }
  uint64_t deviceLimit() const { return deviceLimit_; }
  uint64_t heapLimit(uint32_t heap) const { return heapAllowance(properties_.heaps.at(heap)); }
  const Rejection& lastRejection() const { return rejection_; }
  void clearRejection() { rejection_={}; }
  // Call after releasing a failed driver allocation's reservation. Distinguish
  // actual driver exhaustion from our unchanged conservative policy ceilings.
  void driverRejected(uint32_t type,uint64_t bytes) { reject(type,bytes,"vulkan-allocation"); }
};

inline uint32_t chooseType(uint32_t compatible, uint32_t required,
    uint32_t preferred, const Properties& properties) {
  uint32_t best = noType, bestScore = 0;
  for (uint32_t i = 0; i < properties.types.size() && i < 32; ++i) {
    const auto& type = properties.types[i];
    if (!(compatible & (1u << i)) || (type.flags & required) != required ||
        type.heap >= properties.heaps.size()) continue;
    const uint32_t score = __builtin_popcount(type.flags & preferred);
    if (best == noType || score > bestScore) { best = i; bestScore = score; }
  }
  return best;
}

// AUTO uses direct mappings only for a unified-memory topology: an integrated
// device whose selected heap is its largest device-local heap. This is more
// conservative than treating HOST_VISIBLE|DEVICE_LOCAL alone as proof that CPU
// access is fast (for example, a discrete-GPU BAR may expose those flags).
inline bool autoDirect(bool integrated, uint32_t heap, uint64_t requiredBytes,
    const Properties& properties) {
  if (!integrated || heap >= properties.heaps.size() || !properties.heaps[heap].device)
    return false;
  uint64_t largest = 0;
  for (const auto& item : properties.heaps) if (item.device) largest = std::max(largest, item.bytes);
  const uint64_t size = properties.heaps[heap].bytes;
  return size == largest && requiredBytes <= heapBudget(size);
}

struct Range { uint64_t offset = 0, bytes = 0; bool whole = false; };
inline Range alignedRange(uint64_t offset, uint64_t bytes, uint64_t allocation,
    uint64_t atom) {
  if (!bytes || offset >= allocation || bytes > allocation - offset || !atom) return {};
  const uint64_t begin = offset - offset % atom;
  const uint64_t endValue = offset + bytes;
  if (endValue > std::numeric_limits<uint64_t>::max() - (atom - 1)) return {};
  const uint64_t end = ((endValue + atom - 1) / atom) * atom;
  if (end >= allocation) return {begin, 0, true};
  return {begin, end - begin, false};
}

// Smallest 2/3/5-smooth value at least minimum. These lengths avoid Bluestein
// transforms for the zero-padded linear clutter convolution. Zero means the
// requested length cannot be represented in the uint32_t VkFFT geometry.
inline uint32_t nextSmooth(uint64_t minimum) {
  if (!minimum || minimum > std::numeric_limits<uint32_t>::max()) return 0;
  constexpr uint64_t limit = std::numeric_limits<uint32_t>::max();
  uint64_t best = std::numeric_limits<uint64_t>::max();
  for (uint64_t two = 1; two <= limit; ) {
    for (uint64_t three = two; three <= limit; ) {
      for (uint64_t five = three; five <= limit; ) {
        if (five >= minimum) { best = std::min(best, five); break; }
        if (five > limit / 5) break;
        five *= 5;
      }
      if (three > limit / 3) break;
      three *= 3;
    }
    if (two > limit / 2) break;
    two *= 2;
  }
  return best <= limit ? uint32_t(best) : 0;
}
}
