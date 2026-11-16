#include "data/IqData.h"
#include "data/CaptureChunkQueue.h"
#include "capture/usrp/UsrpIngress.h"
#include <atomic>
#include <chrono>
#include <future>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {
void require(bool condition, const char* reason) {
  if (!condition) throw std::runtime_error(reason);
}
template<class F> void rejects(F action) {
  try { action(); } catch (const std::runtime_error&) { return; }
  throw std::runtime_error("Out-of-range FIFO operation accepted");
}

void contiguous_float_contracts() {
  using Sample = std::complex<float>;
  // Receiver fragments need not divide the CPI. A partial stays in producer
  // ownership until it is complete, then transfers without copying samples.
  IqData a(20), b(20);
  a.enable_float_capture(10); b.enable_float_capture(10);
  UsrpIngressTiming timing;
  std::vector<Sample> x(12), y(12);
  for (unsigned i = 0; i < x.size(); ++i) {
    x[i] = {float(i) + .25f, -float(i)};
    y[i] = {float(i) + 100.5f, float(i)};
  }
  append_usrp_block(a, b, x.data(), y.data(), 6, timing);
  require(a.take_float_blocks().empty() && b.take_float_blocks().empty() &&
    a.get_length() == 6 && b.get_length() == 6,
    "Partial native CPI escaped producer storage");
  append_usrp_block(a, b, x.data() + 6, y.data() + 6, 6, timing);
  FloatCaptureChunkQueue ca, cb;
  ca.push(a.take_float_blocks(10)); cb.push(b.take_float_blocks(10));
  require(a.get_length() == 2 && b.get_length() == 2 &&
    ca.size() == 10 && cb.size() == 10,
    "Native transfer lost receiver tail or paired CPI");
  std::vector<Sample> stagedA(10), stagedB(10);
  ca.take(10, stagedA); cb.take(10, stagedB);
  require(ca.last_transfer().swapped == 10 && !ca.last_transfer().copied &&
    cb.last_transfer().swapped == 10 && !cb.last_transfer().copied,
    "Complete native CPI was copied instead of transferred");
  for (unsigned i = 0; i < 10; ++i)
    require(stagedA[i] == x[i] && stagedB[i] == y[i],
      "Contiguous capture changed channel pairing or sample order");
  // Clear models a receiver epoch transition. An already acquired CPI owns
  // its samples independently and must survive reuse by the new epoch.
  a.clear(); b.clear();
  const std::vector<Sample> next(10, {42, -24});
  append_usrp_block(a, b, next.data(), next.data(), 10, timing);
  require(stagedA.front() == x.front() && stagedA.back() == x[9],
    "New capture epoch overwrote an acquired CPI");
  ca.push(a.take_float_blocks(10)); cb.push(b.take_float_blocks(10));
  ca.clear(); cb.clear(); // Pending old-generation discard returns ownership.
  a.clear(); b.clear();
  append_usrp_block(a, b, next.data(), next.data(), 10, timing);
  ca.push(a.take_float_blocks(10)); cb.push(b.take_float_blocks(10));
  ca.take(10, stagedA); cb.take(10, stagedB);
  require(stagedA == next && stagedB == next,
    "Cleared native epoch leaked old samples");

  // Consumer and producer still share the exact configured sample budget.
  const std::vector<Sample> twoCpis(20, {7, -8});
  append_usrp_block(a, b, twoCpis.data(), twoCpis.data(), 20, timing);
  ca.push(a.take_float_blocks(10)); cb.push(b.take_float_blocks(10));
  a.set_capacity_unlocked(10); b.set_capacity_unlocked(10);
  ca.take(10, stagedA); cb.take(10, stagedB);
  append_usrp_block(a, b, next.data(), next.data(), 10, timing);
  require(a.get_length() + ca.size() == 20 &&
    b.get_length() + cb.size() == 20, "Native aggregate budget changed");
  const auto committed = timing.samples;
  rejects([&] { append_usrp_block(a, b, next.data(), next.data(), 1, timing); });
  require(timing.samples == committed && !a.get_dropped_samples() &&
    !b.get_dropped_samples() && a.get_length() == 10 && b.get_length() == 10,
    "Full native queue retired or partially committed paired samples");
  ca.take(10, stagedA); cb.take(10, stagedB);
  a.set_capacity_unlocked(20); b.set_capacity_unlocked(20);
  append_usrp_block(a, b, next.data(), next.data(), 10, timing);
  require(a.get_length() == 20 && b.get_length() == 20,
    "Native consumer retirement failed to return producer capacity");

  // General FIFO overflow and fragmented drains retain their old semantics.
  for (unsigned capacity = 1; capacity <= 16; ++capacity)
    for (unsigned block : {1u, 3u, 7u, 20u})
      for (unsigned count = 0; count <= 25; ++count) {
        IqData plain(capacity), native(capacity);
        native.enable_float_capture(block);
        for (unsigned i = 0; i < capacity; ++i) {
          plain.push_back({double(i), -double(i)});
          native.push_back({double(i), -double(i)});
        }
        std::vector<Sample> incoming(count, {1.25f, -2.5f});
        plain.append_unlocked(incoming); native.append_unlocked(incoming);
        require(plain.get_data() == native.get_data() &&
          plain.get_dropped_samples() == native.get_dropped_samples(),
          "Contiguous native overflow differs from ordinary FIFO");
        const auto take = capacity / 2;
        require(plain.drain_front(take) == native.drain_front(take) &&
          plain.take_all() == native.take_all(),
          "Contiguous native fragmented fallback changed samples");
      }

  // A CPU fallback uses an initially empty conversion vector. Returning it
  // must surrender the pool slot instead of leaking a slot on every CPI.
  IqData fallback(20); fallback.enable_float_capture(10);
  for (unsigned frame = 0; frame < 40; ++frame) {
    fallback.append_unlocked(next);
    FloatCaptureChunkQueue q; q.push(fallback.take_float_blocks(10));
    std::vector<Sample> converted;
    q.take(10, converted);
    require(converted == next && fallback.float_capture_pool_stats().allocated == 0,
      "CPU fallback leaked a native capture pool slot");
  }
  // Shutdown destroys the producer before the last transferred block drains.
  FloatCaptureChunkQueue survivor;
  {
    IqData source(10); source.enable_float_capture(10);
    source.append_unlocked(next); survivor.push(source.take_float_blocks(10));
  }
  survivor.take(10, stagedA);
  require(stagedA == next, "Producer shutdown invalidated a borrowed CPI");

  // Receiver acquisition and consumer retirement share only the bounded pool.
  // Exercise cross-thread reuse independently of their separately locked FIFOs.
  auto concurrent = std::make_shared<FloatCaptureBlockPool>(64, 192);
  std::atomic<bool> start{false};
  std::vector<std::future<void>> workers;
  for (unsigned worker = 0; worker < 4; ++worker)
    workers.push_back(std::async(std::launch::async, [&, worker] {
      while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
      for (unsigned iteration = 0; iteration < 1000; ++iteration) {
        FloatCaptureBlock block(concurrent, concurrent->acquire());
        std::fill(block.data.begin(), block.data.end(), Sample(worker, iteration));
        std::this_thread::yield();
        require(std::all_of(block.data.begin(), block.data.end(),
          [&](const Sample value) { return value == Sample(worker, iteration); }),
          "Concurrent recycling overwrote an acquired capture block");
      }
    }));
  start.store(true, std::memory_order_release);
  for (auto& worker : workers) worker.get();
  const auto concurrentStats = concurrent->stats();
  require(concurrentStats.allocated <= 4 && concurrentStats.limit == 4 &&
    concurrentStats.allocations <= 4 && concurrentStats.reuses >= 3996,
    "Concurrent capture recycling exceeded its fixed allocation bound");
  std::vector<FloatCaptureBlockPool::Buffer> held;
  for (unsigned i = 0; i < concurrentStats.limit; ++i)
    held.push_back(concurrent->acquire());
  rejects([&] { concurrent->acquire(); });
  for (auto& buffer : held) concurrent->recycle(std::move(buffer));
  require(concurrent->stats().allocated == 4,
    "Pool exhaustion changed ownership or the allocation bound");

  // Warm only two blocks, irrespective of a much larger sample budget. The
  // fixed high-water allocation bound is ceil(budget / CPI) plus one block
  // for head/tail fragmentation; active staging remains separately owned.
  for (unsigned capacity : {20u, 23u, 80u}) {
    IqData warm(capacity); warm.enable_float_capture(10, 2);
    const auto before = warm.float_capture_pool_stats();
    require(before.allocated == 2 && before.allocations == 2 &&
      before.limit == (capacity + 9) / 10 + 1,
      "Native warmup allocated the backlog budget or lost its partial bound");
    std::vector<Sample> boundedInput(capacity, {3, -4});
    warm.append_unlocked(boundedInput);
    FloatCaptureChunkQueue heldQueue;
    heldQueue.push(warm.take_float_blocks());
    require(warm.get_length() + heldQueue.size() == capacity &&
      warm.float_capture_pool_stats().allocated <= before.limit,
      "Native warmup changed the aggregate queued-sample budget");
    heldQueue.take(10, stagedA);
    warm.set_capacity_unlocked(capacity - heldQueue.size());
    warm.append_unlocked(next);
    require(warm.get_length() + heldQueue.size() == capacity &&
      !warm.get_dropped_samples() &&
      warm.float_capture_pool_stats().allocated <= before.limit,
      "Native partial-block refill exceeded its queue/allocation budget");
  }

  // Pointer exchange and pool allocation counters are deterministic timing
  // contracts; the elapsed time is reported, not a platform-dependent gate.
  constexpr std::size_t samples = 262144, frames = 64;
  IqData timed(samples * 2); timed.enable_float_capture(samples);
  std::vector<Sample> input(samples, {1.25f, -.5f}), staging(samples);
  const auto* firstStorage = staging.data();
  FloatCaptureChunkQueue queue;
  std::chrono::steady_clock::duration taking{};
  const Sample* firstQueued = nullptr;
  for (std::size_t frame = 0; frame < frames; ++frame) {
    timed.append_unlocked(input); queue.push(timed.take_float_blocks(samples));
    const auto before = std::chrono::steady_clock::now();
    queue.take(samples, staging);
    taking += std::chrono::steady_clock::now() - before;
    if (!frame) firstQueued = staging.data();
    require(staging.data() == (frame % 2 ? firstStorage : firstQueued) &&
      queue.last_transfer().swapped == samples && !queue.last_transfer().copied,
      "Native staging lost its constant-time ownership exchange");
  }
  const auto stats = timed.float_capture_pool_stats();
  require(stats.allocations == 1 && stats.reuses == frames - 1 &&
    stats.allocated <= stats.limit, "Steady native capture allocated per CPI");
  std::cout << "Native FP32 capture: " << frames << " CPIs, "
    << stats.allocations << " pool allocation, zero staging-copy samples, "
    << std::chrono::duration<double, std::micro>(taking).count() / frames
    << " us/take\n";
}
}
int main() {
  try {
    contiguous_float_contracts();
    for (unsigned first = 0; first <= 40; ++first) {
      for (unsigned second = 0; second <= 40; ++second) {
        for (unsigned take = 0; take <= first + second; ++take) {
          CaptureChunkQueue chunks;
          std::deque<std::complex<double>> a, b;
          for (unsigned i = 0; i < first; ++i) a.push_back({double(i), 0});
          for (unsigned i = 0; i < second; ++i) b.push_back({double(first + i), 0});
          chunks.push(std::move(a)); chunks.push(std::move(b));
          const auto block = chunks.take(take);
          require(block.size() == take && chunks.size() == first + second - take,
            "Chunk queue sizes changed");
          for (unsigned i = 0; i < take; ++i)
            require(block[i].real() == i, "Chunk queue reordered a CPI");
          const auto tail = chunks.take(chunks.size());
          for (unsigned i = 0; i < tail.size(); ++i)
            require(tail[i].real() == take + i, "Chunk queue reordered its tail");

          CaptureChunkQueue floatChunks;
          std::deque<std::complex<double>> floatA, floatB;
          for (unsigned i = 0; i < first; ++i)
            floatA.push_back({double(i) + .25, -double(i) - .5});
          for (unsigned i = 0; i < second; ++i)
            floatB.push_back({double(first + i) + .25,
              -double(first + i) - .5});
          floatChunks.push(std::move(floatA));
          floatChunks.push(std::move(floatB));
          std::vector<std::complex<float>> converted;
          floatChunks.take_float(take, converted);
          require(converted.size() == take &&
            floatChunks.size() == first + second - take,
            "Float chunk conversion sizes changed");
          for (unsigned i = 0; i < take; ++i)
            require(converted[i] == std::complex<float>(float(i) + .25f,
              -float(i) - .5f), "Float chunk conversion reordered a CPI");
        }
      }
    }
    // Every split, including the ownership-transfer path with a short tail.
    for (unsigned length = 0; length <= 80; ++length) {
      for (unsigned count = 0; count <= length; ++count) {
        IqData split(80);
        for (unsigned i = 0; i < length; ++i) split.push_back({double(i), -double(i)});
        const auto* front = length ? &split.view_data().front() : nullptr;
        const auto drained = split.drain_front(count);
        require(drained.size() == count && split.get_length() == length - count,
          "Drain sizes changed");
        if (count && count >= length - count && count != length / 2)
          require(&drained.front() == front, "Large-prefix drain copied the CPI");
        for (unsigned i = 0; i < count; ++i)
          require(drained[i] == std::complex<double>(i, -double(i)), "Drain reordered prefix");
        for (unsigned i = count; i < length; ++i)
          require(split.view_data()[i - count] == std::complex<double>(i, -double(i)),
            "Drain reordered retained tail");
      }
    }
    // Pointer/count bulk append must be identical to existing per-sample FIFO.
    for (unsigned capacity = 1; capacity <= 25; ++capacity) {
      for (unsigned initial = 0; initial <= capacity; ++initial) {
        for (unsigned count = 0; count <= 30; ++count) {
          IqData old(capacity), bulk(capacity);
          for (unsigned i = 0; i < initial; ++i) {
            old.push_back({double(i), double(i)}); bulk.push_back({double(i), double(i)});
          }
          std::vector<std::complex<float>> block(count);
          for (unsigned i = 0; i < count; ++i) {
            block[i] = {i + .25f, -float(i) - .5f}; old.push_back(std::complex<double>(block[i]));
          }
          bulk.append_unlocked(block.data(), block.size());
          require(old.view_data() == bulk.view_data(), "Bulk append differs from per-sample FIFO");
          require(old.get_dropped_samples() == bulk.get_dropped_samples(),
            "Bulk append overflow accounting differs from per-sample FIFO");
        }
      }
    }
    IqData zero(0);
    zero.append_unlocked(nullptr, 0);
    const std::complex<float> single{1, 2};
    zero.append_unlocked(&single, 1);
    require(zero.get_length() == 0, "Zero-capacity queue grew");
    zero.push_back({3, 4});
    require(zero.get_dropped_samples() == 2, "Zero-capacity retirement was not counted");
    bool nullRejected = false;
    try { zero.append_unlocked(nullptr, 1); }
    catch (const std::invalid_argument&) { nullRejected = true; }
    require(nullRejected, "Null positive-length append accepted");
    IqData nativeFloat(5);
    nativeFloat.enable_float_capture();
    const std::vector<std::complex<float>> nativeSamples{
      {1.25f, -2.5f}, {3.5f, 4.75f}, {-5.25f, 6.5f}};
    nativeFloat.append_unlocked(nativeSamples);
    require(nativeFloat.get_length() == nativeSamples.size(),
      "Native float FIFO length changed");
    auto nativeBlock = nativeFloat.take_all_float();
    require(std::vector<std::complex<float>>(nativeBlock.begin(), nativeBlock.end()) ==
      nativeSamples && nativeFloat.get_length() == 0,
      "Native float transfer changed samples");
    nativeFloat.append_unlocked(nativeSamples);
    const auto nativeFallback = nativeFloat.take_all();
    require(nativeFallback.size() == nativeSamples.size() &&
      nativeFallback[0] == std::complex<double>(nativeSamples[0]) &&
      nativeFallback[2] == std::complex<double>(nativeSamples[2]),
      "Native float CPU fallback conversion changed samples");
    nativeFloat.push_back({7.0, -8.0});
    nativeFloat.push_back({9.0, -10.0});
    require(nativeFloat.get_length() == 2 &&
      nativeFloat.pop_front() == std::complex<double>(7.0, -8.0),
      "Native float callback producer changed sample order");
    nativeFloat.discard_front(1);
    require(nativeFloat.get_length() == 0,
      "Native float callback producer did not retire samples");
    FloatCaptureChunkQueue nativeChunks;
    nativeChunks.push(std::move(nativeBlock));
    std::vector<std::complex<float>> nativeStaging;
    nativeChunks.take(nativeSamples.size(), nativeStaging);
    require(nativeStaging == nativeSamples && nativeChunks.size() == 0,
      "Native float chunk staging changed samples");
    // A two-CPI aggregate configuration must accept coherent receive blocks
    // that do not divide the CPI. Proactive partial handoff produces exactly
    // one ordered CPI and a two-sample consumer tail for both storage types.
    CaptureChunkQueue partialDouble;
    FloatCaptureChunkQueue partialFloat;
    for (unsigned block = 0; block < 2; ++block) {
      std::deque<std::complex<double>> doubles;
      std::deque<std::complex<float>> floats;
      for (unsigned i = 0; i < 6; ++i) {
        const unsigned value = block * 6 + i;
        doubles.push_back({double(value), -double(value)});
        floats.push_back({float(value), -float(value)});
      }
      partialDouble.push(std::move(doubles));
      partialFloat.push(std::move(floats));
    }
    const auto nonDivisibleCpi = partialDouble.take(10);
    std::vector<std::complex<float>> nonDivisibleFloatCpi;
    partialFloat.take(10, nonDivisibleFloatCpi);
    require(nonDivisibleCpi.size() == 10 &&
      nonDivisibleFloatCpi.size() == 10 && partialDouble.size() == 2 &&
      partialFloat.size() == 2 && nonDivisibleCpi.back().real() == 9 &&
      nonDivisibleFloatCpi.back().real() == 9,
      "Non-divisible receive blocks did not cross a CPI boundary coherently");
    // Whole-deque handoff transfers ownership, not budget. After retaining a
    // two-sample consumer tail, the producer may own at most18 of a configured
    // aggregate20; refill must keep their sum exactly bounded.
    IqData budgetedProducer(20);
    std::vector<std::complex<float>> firstReceive(12, {1, -1});
    budgetedProducer.append_unlocked(firstReceive);
    auto movedReceive = budgetedProducer.take_all();
    CaptureChunkQueue budgetedConsumer;
    budgetedConsumer.push(std::move(movedReceive));
    auto firstCpi = budgetedConsumer.take(10);
    require(firstCpi.size() == 10 && budgetedConsumer.size() == 2,
      "Budget fixture did not retain its partial consumer tail");
    budgetedProducer.set_capacity_unlocked(18);
    std::vector<std::complex<float>> refill(18, {2, -2});
    budgetedProducer.append_unlocked(refill);
    require(budgetedProducer.get_length() + budgetedConsumer.size() == 20 &&
      budgetedProducer.get_dropped_samples() == 0,
      "Producer refill exceeded aggregate capture budget");
    rejects([&] { budgetedProducer.set_capacity_unlocked(17); });
    IqData creditedProducer(80);
    creditedProducer.set_capacity_unlocked(20); // pending owns60
    std::vector<std::complex<float>> fullProducer(20, {3, -3});
    creditedProducer.append_unlocked(fullProducer);
    creditedProducer.set_capacity_unlocked(30); // pending consumes one CPI
    std::vector<std::complex<float>> creditedBlock(10, {4, -4});
    creditedProducer.append_unlocked(creditedBlock);
    require(creditedProducer.get_length() == 30 &&
      creditedProducer.get_dropped_samples() == 0,
      "Consumed pending CPI did not restore producer headroom");
    IqData data(5);
    data.discard_front(0);
    require(data.drain_front(0).empty(), "Empty drain returned samples");
    rejects([&] { data.discard_front(1); });
    for (int i = 0; i < 7; ++i) data.push_back({double(i), -double(i)});
    require(data.get_dropped_samples() == 2, "Per-sample FIFO overflow count changed");
    const auto* original = &data.view_data().front();
    auto whole = data.drain_front(5);
    require(whole.size() == 5 && &whole.front() == original,
      "Full drain copied the sample block");
    require(data.get_length() == 0 && whole.front().real() == 2,
      "Full drain changed FIFO order");
    data.replace(std::move(whole));
    data.lock();
    auto transferred = data.take_all();
    data.unlock();
    require(transferred.size() == 5 && transferred.front().real() == 2 &&
      transferred.back().real() == 6, "Whole FIFO transfer changed samples");
    require(data.get_length() == 0, "Whole FIFO transfer did not empty queue");
    whole = std::move(transferred);
    data.replace(std::move(whole));
    auto prefix = data.drain_front(2);
    require(prefix.size() == 2 && prefix.front().real() == 2 &&
      prefix.back().real() == 3 && data.view_data().front().real() == 4,
      "Partial drain changed FIFO order");
    const auto remaining = data.get_data();
    rejects([&] { data.discard_front(4); });
    rejects([&] { data.drain_front(4); });
    require(data.view_data() == remaining, "Rejected drain mutated IQ");
    data.discard_front(1);
    require(data.get_length() == 2 && data.view_data().front().real() == 5,
      "Bulk discard changed retained tail");
    data.discard_front(2);
    require(data.get_length() == 0, "Full discard retained samples");
    require(data.get_dropped_samples() == 2, "Intentional drain/discard counted as overflow");
    data.replace({{1, 2}, {3, 4}, {5, 6}});
    const auto* writable = &data.view_data().front();
    auto& sameSize = data.resize_for_write(3);
    require(&sameSize.front() == writable && sameSize.size() == 3,
      "Same-size writable resize replaced sample storage");
    const auto beforeOversizedResize = data.get_data();
    rejects([&] { data.resize_for_write(6); });
    require(data.view_data() == beforeOversizedResize,
      "Oversized writable resize changed input");
    auto& resized = data.resize_for_write(5);
    resized[3] = {7, 8};
    resized[4] = {9, 10};
    require(data.get_length() == 5 && data.view_data()[4] == std::complex<double>(9, 10),
      "Writable resize did not expose the requested block");
    for (unsigned repeat = 0; repeat < 256; ++repeat) {
      data.append_unlocked({{1, 2}, {3, 4}, {5, 6}, {7, 8}, {9, 10}, {11, 12}});
      require(data.get_length() == 5 && data.view_data().front().real() == 3,
        "Append lost newest-samples contract");
      data.clear();
    }
    data.replace({{100000000.25, 100000000.5}, {3, 4}, {5, 6}});
    const auto* owned = &data.view_data().front();
    const std::complex<float> estimate[] = {{100000000, 100000000}, {1, 2}};
    const auto unmodified = data.get_data();
    rejects([&] { data.subtract_clutter(nullptr, 2); });
    rejects([&] { data.subtract_clutter(estimate, 4); });
    require(data.view_data() == unmodified, "Rejected clutter estimate changed input");
    data.subtract_clutter(estimate, 2);
    require(&data.view_data().front() == owned && data.get_length() == 2,
      "Clutter subtraction copied the block or retained unfiltered samples");
    require(data.view_data()[0] == std::complex<double>(.25, .5) &&
      data.view_data()[1] == std::complex<double>(2, 2),
      "Clutter subtraction lost FP64 cancellation precision");
    IqData pairedA(4), pairedB(4);
    const int16_t paired[] = {1, 2, 101, 102, 3, 4, 103, 104,
                              5, 6, 105, 106, 7, 8, 107, 108};
    pairedA.assign_paired_i16(paired, 4, pairedB);
    require(pairedA.view_data()[2] == std::complex<double>(5, 6) &&
      pairedB.view_data()[2] == std::complex<double>(105, 106),
      "Direct paired signed16 conversion changed channel order");
    const auto* pairedAStorage = &pairedA.view_data().front();
    const auto* pairedBStorage = &pairedB.view_data().front();
    pairedA.assign_paired_i16(paired, 4, pairedB);
    require(&pairedA.view_data().front() == pairedAStorage &&
      &pairedB.view_data().front() == pairedBStorage,
      "Direct paired conversion did not reuse completed CPI storage");
    const std::complex<double> filtered[] = {{9, 10}, {11, 12}, {13, 14}, {15, 16}};
    pairedB.assign_complex(filtered, 4);
    require(&pairedB.view_data().front() == pairedBStorage &&
      pairedB.view_data()[2] == filtered[2],
      "Filtered complex CPI replacement did not reuse storage or changed samples");
    bool pairedRejected = false;
    try { pairedA.assign_paired_i16(nullptr, 1, pairedB); }
    catch (const std::invalid_argument&) { pairedRejected = true; }
    require(pairedRejected, "Invalid direct paired conversion was accepted");
    data.replace({{100000000.25, 100000000.5}, {3, 4}, {5, 6}});
    const auto* doubleOwned = &data.view_data().front();
    const std::complex<double> doubleEstimate[] = {{200000000, 200000000}, {2, 4}};
    const auto doubleUnmodified = data.get_data();
    rejects([&] { data.subtract_clutter(nullptr, 2, 2); });
    rejects([&] { data.subtract_clutter(doubleEstimate, 4, 2); });
    rejects([&] { data.subtract_clutter(doubleEstimate, 2, 0); });
    require(data.view_data() == doubleUnmodified,
      "Rejected FP64 clutter estimate changed input");
    data.subtract_clutter(doubleEstimate, 2, 2);
    require(&data.view_data().front() == doubleOwned && data.get_length() == 2,
      "FP64 clutter subtraction copied the block or retained unfiltered samples");
    require(data.view_data()[0] == std::complex<double>(.25, .5) &&
      data.view_data()[1] == std::complex<double>(2, 2),
      "FP64 clutter subtraction changed the divide-before-subtract result");
    std::cout << "IQ FIFO ownership and retirement fixtures passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n'; return 1;
  }
}
