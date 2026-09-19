#pragma once
#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace vectorwarp_gpu_bench {
enum class ReadbackMode { cached, direct, chunked };
inline ReadbackMode readbackMode(const char* value) {
  const std::string mode = value ? value : "cached";
  if (mode == "cached") return ReadbackMode::cached;
  if (mode == "direct") return ReadbackMode::direct;
  if (mode == "chunked") return ReadbackMode::chunked;
  throw std::invalid_argument(
    "VECTORWARP_GPU_FIR_READBACK must be cached, direct, or chunked");
}
inline const char* readbackName(ReadbackMode mode) {
  switch (mode) {
    case ReadbackMode::cached: return "cached";
    case ReadbackMode::direct: return "direct";
    case ReadbackMode::chunked: return "chunked";
  }
  return "invalid";
}
struct ReadbackTiming {
  double mappedCopyMs = 0;
  double finiteCheckMs = 0;
  double fp64SubtractMs = 0;
};
inline double elapsed(std::chrono::steady_clock::time_point start) {
  return std::chrono::duration<double,std::milli>(
    std::chrono::steady_clock::now()-start).count();
}
inline void requireFinite(const std::complex<float>* values, size_t count) {
  for (size_t i = 0; i < count; ++i)
    if (!std::isfinite(values[i].real()) || !std::isfinite(values[i].imag()))
      throw std::runtime_error("Non-finite GPU FIR output; benchmark rejected");
}
inline ReadbackTiming readbackSubtract(const std::complex<float>* mapped,
    size_t count, std::vector<std::complex<float>>& cache,
    std::complex<double>* surveillance, ReadbackMode mode) {
  if (!mapped || !count || !surveillance)
    throw std::invalid_argument("Invalid GPU FIR readback storage");
  ReadbackTiming timing;
  if (mode == ReadbackMode::direct) {
    auto before = std::chrono::steady_clock::now();
    requireFinite(mapped, count);
    timing.finiteCheckMs = elapsed(before);
    before = std::chrono::steady_clock::now();
    for (size_t i = 0; i < count; ++i)
      surveillance[i] -= std::complex<double>(mapped[i]);
    timing.fp64SubtractMs = elapsed(before);
    return timing;
  }
  constexpr size_t tile = 16384;
  const size_t cacheNeeded = mode == ReadbackMode::chunked ? std::min(tile,count) : count;
  if (cache.size() < cacheNeeded) cache.resize(cacheNeeded);
  if (mode == ReadbackMode::cached) {
    auto before = std::chrono::steady_clock::now();
    std::memcpy(cache.data(), mapped, count*sizeof(*mapped));
    timing.mappedCopyMs = elapsed(before);
    before = std::chrono::steady_clock::now();
    requireFinite(cache.data(), count);
    timing.finiteCheckMs = elapsed(before);
    before = std::chrono::steady_clock::now();
    for (size_t i = 0; i < count; ++i)
      surveillance[i] -= std::complex<double>(cache[i]);
    timing.fp64SubtractMs = elapsed(before);
    return timing;
  }
  // 128 KiB of FP32 complex output plus 256 KiB of FP64 surveillance per tile.
  // A late invalid tile may modify earlier private-y tiles, but the caller must
  // reject before assigning that private buffer to external surveillance.
  for (size_t begin = 0; begin < count; begin += tile) {
    const size_t length = std::min(tile, count-begin);
    auto before = std::chrono::steady_clock::now();
    std::memcpy(cache.data(), mapped+begin, length*sizeof(*mapped));
    timing.mappedCopyMs += elapsed(before);
    before = std::chrono::steady_clock::now();
    requireFinite(cache.data(), length);
    timing.finiteCheckMs += elapsed(before);
    before = std::chrono::steady_clock::now();
    for (size_t i = 0; i < length; ++i)
      surveillance[begin+i] -= std::complex<double>(cache[i]);
    timing.fp64SubtractMs += elapsed(before);
  }
  return timing;
}
}
