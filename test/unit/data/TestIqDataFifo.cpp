#include "data/IqData.h"
#include "data/CaptureChunkQueue.h"
#include "capture/usrp/UsrpIngress.h"
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <future>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <tuple>

struct FloatCaptureBlockPoolTestAccess {
  static void allocator(FloatCaptureBlockPool& pool,
      FloatCaptureBlockPool::Buffer(*allocate)(std::size_t,void*),void* context) {
    pool.reserveAllocator_=allocate;pool.reserveAllocatorContext_=context;
  }
  static void retirer(FloatCaptureBlockPool& pool,
      FloatCaptureBlockPool::ReserveRetirer retire,void* context) {
    pool.reserveRetirer_=retire;pool.reserveRetirerContext_=context;
  }
};

namespace {
void require(bool condition, const char* reason) {
  if (!condition) throw std::runtime_error(reason);
}
template<class F> void rejects(F action) {
  try { action(); } catch (const std::runtime_error&) { return; }
  throw std::runtime_error("Out-of-range FIFO operation accepted");
}

void contiguous_double_contracts() {
  using Sample = std::complex<double>;
  IqData block(8);
  auto& scratch = block.resize_contiguous_for_write(8);
  const auto* address = scratch.data();
  for (size_t i = 0; i < scratch.size(); ++i) scratch[i] = {100000000.25 + i, 100000000.5 - i};
  const auto original = scratch;
  require(block.take_storage_metrics().allocations == 1, "FP64 scratch was not allocated once");
  const auto view = block.sample_view();
  std::vector<Sample> copied(view.begin(), view.end());
  require(copied == original && view.data() == address && view.end() - view.begin() == 8,
    "Contiguous FP64 view changed bits, iterator range, or ownership");
  std::vector<std::complex<float>> narrowed(3);
  view.copy_to(narrowed.data(), 3, 2);
  for (size_t i = 0; i < narrowed.size(); ++i)
    require(narrowed[i] == std::complex<float>(original[i + 2]), "FP64 view changed GPU narrowing boundary");
  const std::complex<float> estimate[] = {{100000000, 100000000}, {100000000, 100000000}};
  rejects([&] { block.subtract_clutter(nullptr, 2); });
  require(block.sample_view()[0] == original[0], "Rejected estimate mutated contiguous input");
  block.subtract_clutter(estimate, 2);
  require(block.get_length() == 2 && block.sample_view().data() == address &&
    block.sample_view()[0] == Sample(.25, .5), "Contiguous subtraction lost FP64 cancellation");
  block.discard_front(2);
  require(block.get_length() == 0, "Contiguous consumption retained logical samples");
  auto& reused = block.resize_contiguous_for_write(8);
  require(reused.data() == address && reused[7] == original[7],
    "Consumed FP64 scratch was reallocated or unnecessarily initialized");
  std::copy(original.begin(), original.end(), reused.begin());
  block.discard_front(3);
  require(block.get_length() == 5 && block.sample_view()[0] == original[3],
    "Contiguous consumption changed the short tail");
  block.assign_complex(original.data(), 8);
  const Sample doubleEstimate[] = {{200000000, 200000000}, {200000000, 200000000}};
  block.subtract_clutter(doubleEstimate, 2, 2);
  require(block.sample_view()[0] == Sample(.25, .5), "Contiguous FP64 divisor changed cancellation");
  block.clear();
  require(block.get_length() == 0 && block.resize_contiguous_for_write(8).data() == address,
    "Reset did not retain private FP64 scratch");
  block.assign_complex(original.data(), 8);
  const auto nativeCosts = block.take_storage_metrics();
  require(!nativeCosts.allocations && !nativeCosts.materializations && !nativeCosts.materializedSamples,
    "Native FP64 operations silently materialized or allocated");
  const auto& legacy = block.view_data();
  require(std::equal(legacy.begin(), legacy.end(), original.begin()), "Legacy view changed contiguous samples");
  block.view_data();
  const auto legacyCosts = block.take_storage_metrics();
  require(legacyCosts.materializations == 1 && legacyCosts.materializedSamples == 8 &&
    legacyCosts.materializationMs >= 0, "Legacy conversion was hidden or repeated");
  block.resize_for_write(8)[0] = {7, -8};
  require(block.sample_view()[0] == Sample(7, -8), "Legacy mutation left a stale FP64 reader");
  require(block.resize_contiguous_for_write(8)[0] == Sample(7, -8),
    "Returning to contiguous storage lost legacy mutations");
  const auto owned = block.take_all();
  block.resize_contiguous_for_write(8)[0] = {123, 456};
  require(owned[0] == Sample(7, -8) && owned[7] == original[7],
    "EOF drain borrowed storage from the reusable scratch");
}

void stream_profile_contracts() {
  blah2::stream_profile::Histogram histogram;
  for (unsigned i=1;i<=100;++i) histogram.observe(i);
  require(histogram.count==100 && histogram.percentile(.5)==50 &&
    histogram.percentile(.95)==95 && histogram.percentile(.99)==99 &&
    histogram.maximum==100 && histogram.over10==90,
    "Stream profile quantiles or repeated tail counts changed");
  histogram.observe(1500);
  require(histogram.percentile(1)==1500 && histogram.over500==1,
    "Stream profile overflow understated the measured tail");
  blah2::stream_profile::Histogram fractional;fractional.observe(.01);
  require(fractional.percentile(.5)==.5,
    "Stream profile quantile is not a conservative half-ms bound");
}

void stream_profile_smoke() {
  using namespace blah2::stream_profile;
  require(frameLimit()==2,"Run profile smoke with VECTORWARP_STREAM_PROFILE_FRAMES=2");
  Consumer consumer;
  Worker worker("fixture-worker");
  Receiver receiver;
  for(unsigned i=0;i<3;++i){
    const auto now=Clock::now();
    CycleRecord cycle;cycle.sequence=i+1;cycle.epoch=7;
    cycle.time.fill(now);cycle.threadBegin=Activity::capture(true);
    cycle.processBegin=Activity::capture(true,true);
    consumer.record(cycle);consumer.complete(now);
    WorkRecord work;work.sequence=i+1;work.time.fill(now);
    work.begin=Activity::capture(true);worker.record(work);
    ReceiveRecord receive;receive.when=micros(now);receive.phases[3]=double(i+1);
    receive.sampled=receiver.sample();receive.begin=receive.end=Activity::capture(receive.sampled);
    receiver.record(receive,0);
  }
  require(!consumer.enabled()&&!worker.enabled(),"Profile record storage exceeded its fixed bound");
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

  // Trackerclock1 CPU fallback exchanges an initially empty destination and
  // releases that slot; the next receive can allocate its bounded replacement.
  IqData fallback(20); fallback.enable_float_capture(10);
  for (unsigned frame = 0; frame < 40; ++frame) {
    fallback.append_unlocked(next);
    FloatCaptureChunkQueue q; q.push(fallback.take_float_blocks(10));
    std::vector<Sample> converted;
    q.take(10, converted);
    const auto stats = fallback.float_capture_pool_stats();
    require(converted == next && stats.allocated == 0 && stats.allocations == frame+1 &&
      q.last_transfer().swapped == 10 && !q.last_transfer().copied,
      "CPU fallback changed samples or permanently consumed a pool slot");
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
  auto concurrent = std::make_shared<FloatCaptureBlockPool>(64, 192, 4);
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

  // Two prepared blocks are a startup reservation, never an allocation cap.
  // Hold every allowed block simultaneously, then replace incompatible storage.
  for (unsigned capacity : {20u, 23u, 80u}) {
    auto lazy = std::make_shared<FloatCaptureBlockPool>(10, capacity, 2);
    const auto before = lazy->stats();
    require(before.allocated==2 && before.allocations==2,
      "Lazy capture eagerly allocated its backlog");
    std::vector<FloatCaptureBlockPool::Buffer> held;
    for (unsigned i=0;i<before.limit;++i)held.push_back(lazy->acquire());
    require(held.size()>2 && lazy->stats().allocated==before.limit &&
      lazy->stats().allocations==before.limit,"Lazy pool did not grow to its configured bound");
    rejects([&]{lazy->acquire();});
    held.back().clear();lazy->recycle(std::move(held.back()));held.pop_back();
    held.push_back(lazy->acquire());
    require(lazy->stats().allocated==before.limit &&
      lazy->stats().allocations==before.limit+1,"Lazy replacement consumed a permanent pool slot");
    for(auto& buffer:held)lazy->recycle(std::move(buffer));
    held.clear();
    for(unsigned i=0;i<before.limit;++i)held.push_back(lazy->acquire());
    require(lazy->stats().allocations==before.limit+1,
      "Recycled lazy pool allocated unnecessarily");
    for(auto& buffer:held)lazy->recycle(std::move(buffer));
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

void asynchronous_reserve_contracts() {
  using Pool=FloatCaptureBlockPool;
  using Sample=Pool::Sample;
  for(unsigned capacity:{128u,145u,512u}) {
    auto pool=std::make_shared<Pool>(64,capacity,2);
    const auto bound=pool->stats().limit;
    pool->prepare_reserve();pool->prepare_reserve();
    require(pool->stats().allocated==std::min<std::size_t>(3,bound) &&
      pool->reserve_stats().target==1,"Reserve preparation precommitted the backlog or repeated");
    std::vector<Pool::Buffer> held;
    for(std::size_t i=0;i<bound;++i){
      held.push_back(pool->acquire());
      std::fill(held.back().begin(),held.back().end(),Sample(float(i),-float(i)));
      require(pool->stats().allocated<=bound,"In-flight reserve escaped the allocation bound");
    }
    rejects([&]{pool->acquire();});
    // An incompatible fallback destination releases its slot. Race the helper
    // for its replacement, including the final in-flight allocation at limit.
    for(unsigned round=0;round<40;++round){
      Pool::Buffer empty;
      held.back().swap(empty);
      pool->recycle(std::move(held.back()));
      held.back()=pool->acquire();
      require(held.back().size()==64 && pool->stats().allocated<=bound,
        "Bounded reserve replacement failed or exceeded its pool limit");
    }
    for(std::size_t i=0;i+1<held.size();++i)
      require(std::all_of(held[i].begin(),held[i].end(),[&](Sample v){return v==Sample(float(i),-float(i));}),
        "Reserve replacement overwrote an outstanding block");
    for(auto& buffer:held)pool->recycle(std::move(buffer));
    require(pool->reserve_stats().misses>0,"Reserve misses were not accounted");
  }
  auto pool=std::make_shared<Pool>(64,512,2);pool->prepare_reserve();
  std::vector<Pool::Buffer> held;
  for(unsigned i=0;i<3;++i)held.push_back(pool->acquire());
  const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
  while(!pool->reserve_stats().available && std::chrono::steady_clock::now()<deadline)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  require(pool->reserve_stats().available==1 && pool->reserve_stats().asyncAllocations==1 &&
    pool->reserve_stats().lazyAllocations==0 && pool->stats().allocated==4,
    "Helper did not first-touch exactly one reserve outside acquisition");
  for(auto& buffer:held)pool->recycle(std::move(buffer));

  std::vector<std::future<void>> threads;
  for(unsigned channel=0;channel<4;++channel)
    threads.push_back(std::async(std::launch::async,[pool,channel]{
      for(unsigned frame=0;frame<400;++frame){
        FloatCaptureBlock block(pool,pool->acquire());
        std::fill(block.data.begin(),block.data.end(),Sample(float(channel),float(frame)));
        std::this_thread::yield();
        require(std::all_of(block.data.begin(),block.data.end(),[&](Sample v){return v==Sample(float(channel),float(frame));}),
          "Async reserve changed concurrently held samples");
        require(pool->stats().allocated<=pool->stats().limit,"Concurrent reserve exceeded memory bound");
      }
    }));
  for(auto& thread:threads){
    if(thread.wait_for(std::chrono::seconds(5))!=std::future_status::ready)std::abort();
    thread.get();
  }
  // Receiver reset/destruction cannot invalidate transferred storage or its
  // helper. Original sample order and paired accepted counts remain exact.
  FloatCaptureChunkQueue survivor;
  std::vector<Sample> expected(128);
  for(unsigned i=0;i<expected.size();++i)expected[i]={float(i),-float(i)};
  {
    IqData a(512),b(512);a.enable_float_capture(64,2);b.enable_float_capture(64,2);
    a.prepare_float_capture_reserve();b.prepare_float_capture_reserve();
    UsrpIngressTiming timing;
    for(unsigned at=0;at<expected.size();){const auto count=std::min(17u,unsigned(expected.size())-at);
      append_usrp_block(a,b,expected.data()+at,expected.data()+at,count,timing);at+=count;}
    require(timing.samples==128 && a.get_data()==b.get_data(),"Reserve changed paired sample lineage");
    survivor.push(a.take_float_blocks());a.clear();b.clear();
  }
  std::vector<Sample> actual;survivor.take(128,actual);
  require(actual==expected,"Async reserve shutdown/reset invalidated delayed samples");
}

// No allocator timing assumptions: stop the helper after it reserves a slot,
// while neither the pool mutex nor either receiver lock is held.
struct ReserveGate {
  std::mutex mutex;std::condition_variable changed;
  unsigned entered=0,permits=0,failures=0;
  static FloatCaptureBlockPool::Buffer allocate(std::size_t size,void* context) {
    auto& gate=*static_cast<ReserveGate*>(context);
    std::unique_lock<std::mutex> lock(gate.mutex);++gate.entered;gate.changed.notify_all();
    if(!gate.changed.wait_for(lock,std::chrono::seconds(5),[&]{return gate.permits!=0;}))std::abort();
    --gate.permits;
    if(gate.failures){--gate.failures;throw std::bad_alloc();}
    lock.unlock();return FloatCaptureBlockPool::Buffer(size);
  }
  void wait(unsigned count) {
    std::unique_lock<std::mutex> lock(mutex);
    if(!changed.wait_for(lock,std::chrono::seconds(5),[&]{return entered>=count;}))std::abort();
  }
  void release(bool fail=false) {
    std::lock_guard<std::mutex> lock(mutex);++permits;if(fail)++failures;changed.notify_all();
  }
};

struct RetireGate {
  std::mutex mutex;std::condition_variable changed;
  unsigned entered=0,permits=0;
  static void retire(FloatCaptureBlockPool::Buffer&& buffer,void* context) {
    auto& gate=*static_cast<RetireGate*>(context);
    std::unique_lock<std::mutex> lock(gate.mutex);++gate.entered;gate.changed.notify_all();
    if(!gate.changed.wait_for(lock,std::chrono::seconds(10),[&]{return gate.permits!=0;}))std::abort();
    --gate.permits;lock.unlock();
    FloatCaptureBlockPool::Buffer retired(std::move(buffer));
  }
  void wait(unsigned count) {
    std::unique_lock<std::mutex> lock(mutex);
    if(!changed.wait_for(lock,std::chrono::seconds(5),[&]{return entered>=count;}))std::abort();
  }
  void release() {
    std::lock_guard<std::mutex> lock(mutex);++permits;changed.notify_all();
  }
};

void deterministic_reserve_contracts() {
  using Pool=FloatCaptureBlockPool;
  // limit=4, warm=2+1: the helper owns the last slot and acquire must wait
  // rather than exhaust or allocate a fifth block.
  {
    ReserveGate gate;Pool pool(8,24,2);
    FloatCaptureBlockPoolTestAccess::allocator(pool,ReserveGate::allocate,&gate);
    pool.prepare_reserve();std::vector<Pool::Buffer> held;
    for(unsigned i=0;i<3;++i)held.push_back(pool.acquire());
    gate.wait(1);
    require(pool.stats().allocated==4 && pool.reserve_stats().pending==1,
      "Helper did not reserve the last counted slot before allocation");
    auto fourth=std::async(std::launch::async,[&]{return pool.acquire();});
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
    while(!pool.reserve_stats().waits && std::chrono::steady_clock::now()<deadline)std::this_thread::yield();
    require(pool.reserve_stats().waits==1,"Final-slot acquisition did not wait for helper");
    gate.release();
    if(fourth.wait_for(std::chrono::seconds(5))!=std::future_status::ready)std::abort();
    held.push_back(fourth.get());
    require(held.back().size()==8 && pool.stats().allocated==4 &&
      pool.reserve_stats().lazyAllocations==0,"Pending final slot was duplicated or exhausted");
    for(auto& block:held)pool.recycle(std::move(block));
  }
  // A helper allocation can become redundant when a consumer recycles a block
  // concurrently. Retiring that full block must not hold the pool mutex, but it
  // remains charged until destruction so physical storage cannot exceed limit.
  {
    ReserveGate allocation;RetireGate retirement;Pool pool(8,24,2);
    FloatCaptureBlockPoolTestAccess::allocator(pool,ReserveGate::allocate,&allocation);
    FloatCaptureBlockPoolTestAccess::retirer(pool,RetireGate::retire,&retirement);
    pool.prepare_reserve();std::vector<Pool::Buffer> held;
    for(unsigned i=0;i<3;++i)held.push_back(pool.acquire());
    allocation.wait(1);
    pool.recycle(std::move(held.back()));held.pop_back();
    allocation.release();retirement.wait(1);
    auto probe=std::async(std::launch::async,[&]{
      auto stats=pool.stats();auto reserve=pool.reserve_stats();
      return std::make_tuple(stats,reserve,pool.acquire());
    });
    if(probe.wait_for(std::chrono::seconds(5))!=std::future_status::ready){
      retirement.release();std::abort();
    }
    auto observed=probe.get();
    require(std::get<0>(observed).allocated==4 &&
      std::get<1>(observed).pending==1,
      "Reserve retirement held the pool mutex or escaped the physical ceiling");
    held.push_back(std::move(std::get<2>(observed)));
    require(held.back().size()==8,
      "Receiver acquisition blocked behind reserve retirement");
    retirement.release();allocation.wait(2);allocation.release();
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
    while((pool.reserve_stats().pending || !pool.reserve_stats().available) &&
        std::chrono::steady_clock::now()<deadline)std::this_thread::yield();
    require(pool.reserve_stats().available==1 && !pool.reserve_stats().pending &&
      pool.reserve_stats().lazyAllocations==0 && pool.stats().allocated==4,
      "Retired reserve was not replenished within the original bound");
    for(auto& block:held)pool.recycle(std::move(block));
  }
  // Pool shutdown joins an in-progress redundant-block retirement without
  // reacquiring the pool mutex around destruction or invalidating callers.
  {
    ReserveGate allocation;RetireGate retirement;
    auto pool=std::make_unique<Pool>(8,24,2);
    FloatCaptureBlockPoolTestAccess::allocator(*pool,ReserveGate::allocate,&allocation);
    FloatCaptureBlockPoolTestAccess::retirer(*pool,RetireGate::retire,&retirement);
    pool->prepare_reserve();std::vector<Pool::Buffer> held;
    for(unsigned i=0;i<3;++i)held.push_back(pool->acquire());
    allocation.wait(1);pool->recycle(std::move(held.back()));held.pop_back();
    allocation.release();retirement.wait(1);
    std::promise<void> started;auto ready=started.get_future();
    auto stop=std::async(std::launch::async,[owned=std::move(pool),&started]()mutable{
      started.set_value();owned.reset();
    });
    ready.wait();
    require(stop.wait_for(std::chrono::milliseconds(10))==std::future_status::timeout,
      "Pool shutdown failed to join active reserve retirement");
    retirement.release();
    if(stop.wait_for(std::chrono::seconds(5))!=std::future_status::ready)std::abort();
    stop.get();
    require(held.size()==2 && held.front().size()==8,
      "Reserve retirement shutdown invalidated caller-owned storage");
  }
  // A failed helper relinquishes its slot and does not spin. A later demand
  // can lazily succeed while triggering exactly one bounded helper retry.
  {
    ReserveGate gate;Pool pool(8,40,2);
    FloatCaptureBlockPoolTestAccess::allocator(pool,ReserveGate::allocate,&gate);
    pool.prepare_reserve();std::vector<Pool::Buffer> held;
    for(unsigned i=0;i<3;++i)held.push_back(pool.acquire());
    gate.wait(1);gate.release(true);
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
    while(!pool.reserve_stats().asyncFailures && std::chrono::steady_clock::now()<deadline)std::this_thread::yield();
    require(pool.reserve_stats().asyncFailures==1 && !pool.reserve_stats().pending &&
      pool.stats().allocated==3,"Failed reserve retained its slot or retried without demand");
    held.push_back(pool.acquire());gate.wait(2);
    require(pool.reserve_stats().lazyAllocations==1 && pool.stats().allocated==5,
      "Allocation failure disabled bounded lazy fallback or later retry");
    gate.release();
    for(auto& block:held)pool.recycle(std::move(block));
  }
  // Shutdown joins an in-flight allocation without holding the pool mutex.
  {
    ReserveGate gate;auto pool=std::make_unique<Pool>(8,24,2);
    FloatCaptureBlockPoolTestAccess::allocator(*pool,ReserveGate::allocate,&gate);
    pool->prepare_reserve();std::vector<Pool::Buffer> held;
    for(unsigned i=0;i<3;++i)
      held.push_back(pool->acquire());
    gate.wait(1);
    std::promise<void> started;auto ready=started.get_future();
    auto stop=std::async(std::launch::async,[owned=std::move(pool),&started]()mutable{
      started.set_value();owned.reset();
    });
    ready.wait();
    require(stop.wait_for(std::chrono::milliseconds(10))==std::future_status::timeout,
      "Pool destruction failed to join its in-flight allocator");
    gate.release();if(stop.wait_for(std::chrono::seconds(5))!=std::future_status::ready)std::abort();stop.get();
    require(held.size()==3 && held.front().size()==8,"Shutdown invalidated externally owned samples");
  }
}
}
int main(int argc,char** argv) {
  try {
    if(argc==2&&std::string(argv[1])=="--stream-profile-smoke"){
      stream_profile_smoke();return 0;
    }
    stream_profile_contracts();
    contiguous_double_contracts();
    contiguous_float_contracts();
    asynchronous_reserve_contracts();
    deterministic_reserve_contracts();
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
