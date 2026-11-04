#ifndef FIXED_EXECUTOR_H
#define FIXED_EXECUTOR_H

#include <cstddef>
#include <functional>
#include <future>
#include <memory>

namespace blah2::utility
{

// Persistent bounded workers for short, repeated DSP batches. Calls are
// synchronous and exception-safe; no work survives parallel_for().
class FixedExecutor
{
public:
  explicit FixedExecutor(std::size_t workers);
  ~FixedExecutor();
  FixedExecutor(const FixedExecutor&) = delete;
  FixedExecutor& operator=(const FixedExecutor&) = delete;

  void parallel_for(std::size_t count,
    const std::function<void(std::size_t)>& work);
  std::future<void> submit(std::function<void()> work);
  std::size_t size() const;

private:
  struct State;
  std::unique_ptr<State> state_;
};

} // namespace blah2::utility

#endif
