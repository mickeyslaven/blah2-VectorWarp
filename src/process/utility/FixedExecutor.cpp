#include "process/utility/FixedExecutor.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <exception>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

namespace blah2::utility
{

struct FixedExecutor::State
{
  struct Batch
  {
    std::mutex mutex;
    std::condition_variable ready;
    std::size_t remaining = 0;
    std::exception_ptr error;
  };

  std::mutex mutex;
  std::condition_variable ready;
  std::deque<std::function<void()>> queue;
  std::vector<std::thread> workers;
  std::size_t idle = 0;
  static thread_local State* current;
  bool stopping = false;

  explicit State(std::size_t count)
  {
    if (!count) throw std::invalid_argument("Fixed executor requires workers");
    workers.reserve(count);
    idle = count;
    for (std::size_t index = 0; index < count; ++index)
      workers.emplace_back([this] {
        while (true)
        {
          std::function<void()> work;
          {
            std::unique_lock lock(mutex);
            ready.wait(lock, [this] { return stopping || !queue.empty(); });
            if (stopping && queue.empty()) return;
            work = std::move(queue.front());
            queue.pop_front();
            --idle;
          }
          current = this;
          work();
          current = nullptr;
          { std::lock_guard lock(mutex); ++idle; }
          ready.notify_all();
        }
      });
  }

  ~State()
  {
    {
      std::lock_guard lock(mutex);
      stopping = true;
    }
    ready.notify_all();
    for (auto& worker : workers) worker.join();
  }
};
thread_local FixedExecutor::State* FixedExecutor::State::current = nullptr;

FixedExecutor::FixedExecutor(std::size_t workers)
  : state_(std::make_unique<State>(workers)) {}

FixedExecutor::~FixedExecutor() = default;

std::size_t FixedExecutor::size() const { return state_->workers.size(); }

void FixedExecutor::borrow(std::size_t count,
  const std::function<void(std::size_t)>& work,
  const std::function<void()>& check)
{
  if (!work || !check) throw std::invalid_argument("Fixed executor work is empty");
  if (State::current == state_.get())
    throw std::logic_error("Cannot borrow an executor from its own worker");
  if (!count) return;
  State::Batch batch;
  std::atomic<std::size_t> next{0};
  std::atomic<bool> cancelled{false};
  std::unique_lock admission(state_->mutex);
  while (!state_->queue.empty() || !state_->idle) {
    admission.unlock(); check(); admission.lock();
    if (state_->stopping) throw std::runtime_error("Fixed executor is stopping");
    state_->ready.wait_for(admission, std::chrono::milliseconds(1));
  }
  check();
  if (state_->stopping) throw std::runtime_error("Fixed executor is stopping");
  batch.remaining = std::min(count, state_->idle);
  std::size_t enqueued = 0;
  try {
  for (; enqueued < batch.remaining; ++enqueued)
    state_->queue.emplace_back([&] {
      try {
        while (!cancelled.load(std::memory_order_relaxed)) {
          check();
          const auto index = next.fetch_add(1, std::memory_order_relaxed);
          if (index >= count) break;
          work(index);
        }
      } catch (...) {
        cancelled.store(true, std::memory_order_relaxed);
        std::lock_guard lock(batch.mutex);
        if (!batch.error) batch.error = std::current_exception();
      }
      std::lock_guard lock(batch.mutex);
      if (--batch.remaining == 0) batch.ready.notify_one();
    });
  } catch (...) {
    // Admission still owns the queue lock: no worker can retain stack refs.
    while (enqueued) { state_->queue.pop_back(); --enqueued; }
    throw;
  }
  admission.unlock(); state_->ready.notify_all();
  std::unique_lock completion(batch.mutex);
  batch.ready.wait(completion, [&] { return batch.remaining == 0; });
  if (batch.error) std::rethrow_exception(batch.error);
  check();
}

std::future<void> FixedExecutor::submit(std::function<void()> work)
{
  if (!work) throw std::invalid_argument("Fixed executor work is empty");
  auto task = std::make_shared<std::packaged_task<void()>>(std::move(work));
  auto result = task->get_future();
  {
    std::lock_guard lock(state_->mutex);
    if (state_->stopping) throw std::runtime_error("Fixed executor is stopping");
    state_->queue.emplace_back([task] { (*task)(); });
  }
  state_->ready.notify_one();
  return result;
}

void FixedExecutor::parallel_for(std::size_t count,
  const std::function<void(std::size_t)>& work)
{
  if (!work) throw std::invalid_argument("Fixed executor work is empty");
  if (!count) return;
  if (count == 1)
  {
    work(0);
    return;
  }
  State::Batch batch;
  batch.remaining = count;
  {
    std::lock_guard lock(state_->mutex);
    if (state_->stopping) throw std::runtime_error("Fixed executor is stopping");
    for (std::size_t index = 0; index < count; ++index)
      state_->queue.emplace_back([&batch, &work, index] {
        try { work(index); }
        catch (...) {
          std::lock_guard lock(batch.mutex);
          if (!batch.error) batch.error = std::current_exception();
        }
        {
          std::lock_guard lock(batch.mutex);
          if (--batch.remaining == 0) batch.ready.notify_one();
        }
      });
  }
  state_->ready.notify_all();
  std::unique_lock lock(batch.mutex);
  batch.ready.wait(lock, [&batch] { return batch.remaining == 0; });
  if (batch.error) std::rethrow_exception(batch.error);
}

} // namespace blah2::utility
