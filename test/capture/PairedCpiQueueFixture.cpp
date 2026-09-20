#include "capture/PairedCpiQueue.h"
#include <stdexcept>
#include <vector>

static std::vector<int16_t> samples(uint32_t first, uint32_t count) {
  std::vector<int16_t> value(size_t(count) * 4);
  for (uint32_t i = 0; i < count; ++i) for (unsigned ch = 0; ch < 4; ++ch)
    value[4 * i + ch] = int16_t(first + i + ch * 100);
  return value;
}
static void check(bool condition) {
  if (!condition) throw std::runtime_error("Paired CPI queue check failed");
}
int main() {
  PairedCpiQueue queue(4, 1);
  auto a = samples(0, 3), b = samples(3, 1), c = samples(4, 4), d = samples(8, 4);
  queue.push(a.data(), 3, 0); queue.push(b.data(), 1, 3);
  size_t slot = 0; check(queue.acquire(slot));
  const auto& first = queue.block(slot);
  check(first.first == 0);
  check(first.iq[0] == 0);
  check(first.iq[12] == 3);
  queue.release(slot);
  queue.push(c.data(), 4, 4); queue.push(d.data(), 4, 8); // bounded overflow retires exactly one CPI
  const auto overflow = queue.stats(); check(overflow.droppedCpis == 1 && overflow.discardedSamples == 4);
  check(queue.acquire(slot)); check(queue.block(slot).first == 8); queue.release(slot);
  queue.push(a.data(), 3, 20); queue.discontinuity();
  const auto gap = queue.stats(); check(gap.discontinuities == 2 && gap.discardedSamples == 7);
  queue.push(b.data(), 2, 0xfffffffeu); queue.close();
  const auto stopped = queue.stats(); check(stopped.trailingSamples == 2 && stopped.discardedSamples == 7);
  check(queue.backlog_samples() == 0 && !queue.acquire(slot));
}
