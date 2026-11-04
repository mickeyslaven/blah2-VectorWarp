#include "process/utility/FixedExecutor.h"

#include <algorithm>
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
  bool stopping = false;

  explicit State(std::size_t count)
  {
    if (!count) throw std::invalid_argument("Fixed executor requires workers");
    workers.reserve(count);
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
          }
          work();
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

FixedExecutor::FixedExecutor(std::size_t workers)
  : state_(std::make_unique<State>(workers)) {}

FixedExecutor::~FixedExecutor() = default;

std::size_t FixedExecutor::size() const { return state_->workers.size(); }

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
