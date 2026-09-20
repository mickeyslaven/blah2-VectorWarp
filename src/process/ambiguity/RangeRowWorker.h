#pragma once
#include <fftw3.h>
#include "process/utility/FftwThreads.h"
#include <algorithm>
#include <cmath>
#include <complex>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <exception>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

class RangeRowWorker {
public:
  using Complex = std::complex<double>;
  struct Job {
    const std::deque<Complex>* x = nullptr;
    const std::deque<Complex>* y = nullptr;
    std::vector<Complex>* rows = nullptr;
    uint32_t first = 0, last = 0, corr = 0, fft = 0, delays = 0, fs = 0;
    int32_t delayMin = 0;
    double dopplerMiddle = 0;
    const Complex* rotatedX = nullptr;
    const Complex* filteredY = nullptr;
    uint32_t fullSamples = 0;
    int32_t rotation = 0;
  };
private:
  const uint32_t fftLength_;
  std::vector<Complex> input_, product_;
  fftw_plan forward_ = nullptr, inverse_ = nullptr;
  std::mutex& planner_;
  std::mutex mutex_;
  std::condition_variable condition_;
  std::thread thread_;
  Job job_{};
  std::exception_ptr error_;
  bool pending_ = false, busy_ = false, done_ = false, stopping_ = false;

  void execute(const Job& job) {
    if (!job.x || !job.y || !job.rows || job.fft != fftLength_ ||
        !job.corr || !job.delays ||
        job.first >= job.last || job.corr > job.fft || !job.fs ||
        job.delayMin <= -int64_t(job.fft) ||
        int64_t(job.delayMin)+job.delays-1 >= job.fft ||
        job.last * uint64_t(job.corr) > job.x->size() ||
        (!job.filteredY && job.last * uint64_t(job.corr) > job.y->size()) ||
        ((job.rotatedX || job.filteredY) &&
          (!job.fullSamples || job.last * uint64_t(job.corr) > job.fullSamples)) ||
        job.last * uint64_t(job.delays) > job.rows->size())
      throw std::invalid_argument("Invalid ambiguity range-row job");
    const Complex imaginary{0,1};
    for (uint32_t row = job.first; row < job.last; ++row) {
      const uint64_t base = uint64_t(row) * job.corr;
      uint32_t rotatedIndex = 0;
      if (job.rotatedX) {
        const int64_t at = (int64_t(base) + job.rotation) % job.fullSamples;
        rotatedIndex = uint32_t(at < 0 ? at + job.fullSamples : at);
      }
      for (uint32_t j = 0; j < job.corr; ++j) {
        input_[j] = job.rotatedX ? job.rotatedX[rotatedIndex] : (*job.x)[base+j];
        if (job.rotatedX && ++rotatedIndex == job.fullSamples) rotatedIndex = 0;
        if (job.dopplerMiddle != 0)
          input_[j] *= std::exp(imaginary * 2.0 * M_PI * job.dopplerMiddle *
            (static_cast<double>(base+j) / job.fs));
        input_[job.fft+j] = job.filteredY ? job.filteredY[base+j] : (*job.y)[base+j];
      }
      std::fill(input_.begin()+job.corr, input_.begin()+job.fft, Complex{});
      std::fill(input_.begin()+job.fft+job.corr, input_.end(), Complex{});
      fftw_execute(forward_);
      for (uint32_t j = 0; j < job.fft; ++j)
        product_[j] = input_[job.fft+j] * std::conj(input_[j]) / double(job.fft);
      fftw_execute(inverse_);
      for (uint32_t j = 0; j < job.delays; ++j) {
        const int32_t lag = job.delayMin + int32_t(j);
        const uint32_t index = lag < 0 ? uint32_t(int64_t(job.fft)+lag) : uint32_t(lag);
        (*job.rows)[uint64_t(row)*job.delays+j] = product_[index];
      }
    }
  }
  void run() noexcept {
    std::unique_lock<std::mutex> lock(mutex_);
    for (;;) {
      condition_.wait(lock,[&]{return stopping_ || pending_;});
      if (stopping_ && !pending_) return;
      const Job job = job_;
      pending_ = false;
      lock.unlock();
      std::exception_ptr error;
      try { execute(job); } catch (...) { error = std::current_exception(); }
      lock.lock();
      error_ = error;
      done_ = true;
      condition_.notify_all();
    }
  }
public:
  RangeRowWorker(uint32_t fft, unsigned flags, std::mutex& planner, bool threaded)
      : fftLength_(fft), input_(uint64_t(fft)*2), product_(fft), planner_(planner) {
    if (!fft) throw std::invalid_argument("Invalid ambiguity row FFT");
    {
      std::lock_guard<std::mutex> lock(planner_);
      const int saved = blah2::fftw_planner_threads();
      blah2::set_fftw_planner_threads(1);
      int length = int(fft);
      forward_ = fftw_plan_many_dft(1,&length,2,
        reinterpret_cast<fftw_complex*>(input_.data()),nullptr,1,length,
        reinterpret_cast<fftw_complex*>(input_.data()),nullptr,1,length,
        FFTW_FORWARD,flags);
      inverse_ = fftw_plan_dft_1d(length,
        reinterpret_cast<fftw_complex*>(product_.data()),
        reinterpret_cast<fftw_complex*>(product_.data()),FFTW_BACKWARD,flags);
      blah2::set_fftw_planner_threads(saved);
    }
    if (!forward_ || !inverse_) {
      std::lock_guard<std::mutex> lock(planner_);
      if (forward_) fftw_destroy_plan(forward_);
      if (inverse_) fftw_destroy_plan(inverse_);
      forward_=inverse_=nullptr;
      throw std::runtime_error("Could not create ambiguity row FFT plan");
    }
    if (threaded) {
      try { thread_ = std::thread([this]{run();}); }
      catch (...) {
        std::lock_guard<std::mutex> lock(planner_);
        fftw_destroy_plan(forward_);
        fftw_destroy_plan(inverse_);
        forward_=inverse_=nullptr;
        throw;
      }
    }
  }
  ~RangeRowWorker() {
    if (thread_.joinable()) {
      { std::lock_guard<std::mutex> lock(mutex_); stopping_=true; }
      condition_.notify_all();
      thread_.join();
    }
    std::lock_guard<std::mutex> lock(planner_);
    if (forward_) fftw_destroy_plan(forward_);
    if (inverse_) fftw_destroy_plan(inverse_);
  }
  RangeRowWorker(const RangeRowWorker&)=delete;
  RangeRowWorker& operator=(const RangeRowWorker&)=delete;
  void executeCaller(const Job& job) { if (thread_.joinable()) throw std::logic_error("Threaded row worker cannot run in caller"); execute(job); }
  void start(const Job& job) {
    if (!thread_.joinable()) throw std::logic_error("Row worker has no thread");
    std::lock_guard<std::mutex> lock(mutex_);
    if (busy_ || stopping_) throw std::logic_error("Ambiguity row worker is busy");
    job_=job; error_=nullptr; done_=false; busy_=pending_=true;
    condition_.notify_all();
  }
  void finish() {
    std::unique_lock<std::mutex> lock(mutex_);
    if (!busy_) throw std::logic_error("Ambiguity row worker has no job");
    condition_.wait(lock,[&]{return done_;});
    busy_=false; done_=false;
    if (error_) std::rethrow_exception(error_);
  }
};
