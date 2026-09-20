#pragma once
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>

// Persistent one-job executor. finish() is mandatory before the next start;
// destruction drains a pending job before releasing its captured storage.
class GpuCompletionWorker {
  std::mutex mutex_;
  std::condition_variable changed_;
  std::thread thread_;
  std::function<void()> job_;
  std::exception_ptr failure_;
  uint64_t issued_ = 0, completed_ = 0;
  bool pending_ = false, stopping_ = false;
  void run() noexcept {
    std::unique_lock<std::mutex> lock(mutex_);
    for (;;) {
      changed_.wait(lock, [&] { return stopping_ || issued_ != completed_; });
      if (stopping_) return;
      const uint64_t sequence = issued_;
      auto job = std::move(job_);
      lock.unlock();
      std::exception_ptr error;
      try { job(); } catch (...) { error = std::current_exception(); }
      lock.lock();
      failure_ = error;
      completed_ = sequence;
      changed_.notify_all();
    }
  }
public:
  GpuCompletionWorker() : thread_([this] { run(); }) {}
  GpuCompletionWorker(const GpuCompletionWorker&) = delete;
  GpuCompletionWorker& operator=(const GpuCompletionWorker&) = delete;
  ~GpuCompletionWorker() {
    if (pending_) try { finish(); } catch (...) {}
    {
      std::lock_guard<std::mutex> lock(mutex_);
      stopping_ = true;
    }
    changed_.notify_all();
    thread_.join();
  }
  void start(std::function<void()> job) {
    if (!job) throw std::invalid_argument("Invalid GPU completion job");
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (pending_ || issued_ != completed_)
        throw std::logic_error("GPU completion job is still active");
      job_ = std::move(job);
      failure_ = nullptr;
      pending_ = true;
      ++issued_;
    }
    changed_.notify_one();
  }
  void finish() {
    std::exception_ptr error;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      if (!pending_) throw std::logic_error("No GPU completion job to finish");
      changed_.wait(lock, [&] { return completed_ == issued_; });
      pending_ = false;
      job_ = nullptr;
      error = failure_;
    }
    if (error) std::rethrow_exception(error);
  }
};

struct GpuCompletionGuard {
  GpuCompletionWorker* worker;
  ~GpuCompletionGuard() { if (worker) try { worker->finish(); } catch (...) {} }
  void finish() {
    auto* current = worker;
    worker = nullptr;
    if (current) current->finish();
  }
};
