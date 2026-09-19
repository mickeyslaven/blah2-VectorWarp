#pragma once

#include <algorithm>
#include <complex>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>
#include <fftw3.h>

#ifdef __linux__
#include <pthread.h>
#endif

namespace vectorwarp_clutter {
using Complex = std::complex<double>;

// Fill the three FFT lanes for one circular-correlation overlap-save block.
inline void correlation_block(const Complex* x, const Complex* y,
                              uint32_t samples, uint32_t taps,
                              uint32_t length, uint64_t begin,
                              fftw_plan plan, Complex* buffer)
{
  if (!x || !y || !buffer || !plan || !samples || !taps || taps > samples ||
      taps > length || begin >= samples)
    throw std::invalid_argument("Invalid clutter correlation block");
  const uint32_t hop = length - taps + 1;
  const uint32_t valid = uint32_t(std::min<uint64_t>(hop, samples - begin));
  Complex* history = buffer;
  Complex* currentX = history + length;
  Complex* currentY = currentX + length;
  uint32_t position = uint32_t((begin + samples - (taps - 1)) % samples);
  for (uint32_t copied = 0; copied < length;) {
    const uint32_t count = std::min(length - copied, samples - position);
    std::copy(x + position, x + position + count, history + copied);
    copied += count;
    position = 0;
  }
  std::fill(currentX, currentX + taps - 1, Complex{});
  std::fill(currentY, currentY + taps - 1, Complex{});
  std::copy(x + begin, x + begin + valid, currentX + taps - 1);
  std::copy(y + begin, y + begin + valid, currentY + taps - 1);
  std::fill(currentX + taps - 1 + valid, currentX + length, Complex{});
  std::fill(currentY + taps - 1 + valid, currentY + length, Complex{});
  fftw_execute_dft(plan, reinterpret_cast<fftw_complex*>(buffer),
                   reinterpret_cast<fftw_complex*>(buffer));
}

inline void accumulate_correlation(const Complex* buffer, uint32_t length,
                                   Complex* autocorrelation,
                                   Complex* crosscorrelation)
{
  if (!buffer || !autocorrelation || !crosscorrelation || !length)
    throw std::invalid_argument("Invalid clutter correlation accumulation");
  const Complex* history = buffer;
  const Complex* currentX = history + length;
  const Complex* currentY = currentX + length;
  for (uint32_t i = 0; i < length; ++i) {
    const Complex reference = std::conj(history[i]);
    autocorrelation[i] += currentX[i] * reference;
    crosscorrelation[i] += currentY[i] * reference;
  }
}

struct CorrelationJob {
  const Complex* x;
  const Complex* y;
  uint32_t samples;
  uint32_t taps;
  uint32_t length;
  uint64_t begin;
  fftw_plan plan;
};

struct FilterJob {
  const Complex* x;
  const Complex* weights;
  uint32_t samples;
  uint32_t taps;
  uint32_t length;
  uint32_t lanes;
  uint64_t base;
  fftw_plan forward;
  fftw_plan inverse;
  bool overlapSave = true;
};

inline void validate_correlation_job(const CorrelationJob& job)
{
  if (!job.x || !job.y || !job.samples || !job.taps ||
      job.taps > job.samples || job.taps > job.length ||
      job.begin >= job.samples || !job.plan)
    throw std::invalid_argument("Invalid clutter correlation job");
}

inline void validate_filter_job(const FilterJob& job)
{
  if (!job.x || !job.weights || !job.samples || !job.taps ||
      job.taps > job.samples || job.taps > job.length || !job.lanes ||
      job.base >= job.samples || !job.forward || !job.inverse)
    throw std::invalid_argument("Invalid clutter filter job");
}

inline void filter_block(const FilterJob& job, Complex* input, Complex* output)
{
  validate_filter_job(job);
  if (!input || !output) throw std::invalid_argument("Invalid clutter filter storage");
  const uint32_t hop = job.overlapSave ? job.length - job.taps + 1 : job.samples;
  for (uint32_t lane = 0; lane < job.lanes; ++lane) {
    Complex* slot = input + uint64_t(lane) * job.length;
    const int64_t first = int64_t(job.base) + int64_t(lane) * hop -
      (job.overlapSave ? job.taps - 1 : 0);
    const int64_t lower = std::max<int64_t>(0, first);
    const int64_t upper = std::min<int64_t>(job.samples, first + job.length);
    if (lower >= upper) { std::fill_n(slot, job.length, Complex{}); continue; }
    const size_t left = lower - first, count = upper - lower;
    std::fill_n(slot, left, Complex{});
    std::copy(job.x + lower, job.x + upper, slot + left);
    std::fill(slot + left + count, slot + job.length, Complex{});
  }
  fftw_execute_dft(job.forward, reinterpret_cast<fftw_complex*>(input),
                   reinterpret_cast<fftw_complex*>(input));
  for (uint32_t lane = 0; lane < job.lanes; ++lane)
    for (uint32_t i = 0; i < job.length; ++i) {
      const uint64_t at = uint64_t(lane) * job.length + i;
      output[at] = input[at] * job.weights[i];
    }
  fftw_execute_dft(job.inverse, reinterpret_cast<fftw_complex*>(output),
                   reinterpret_cast<fftw_complex*>(output));
}

inline void subtract_filter_block(const FilterJob& job, const Complex* output,
                                  Complex* surveillance)
{
  validate_filter_job(job);
  if (!output || !surveillance)
    throw std::invalid_argument("Invalid clutter filter output");
  const uint32_t hop = job.overlapSave ? job.length - job.taps + 1 : job.samples;
  for (uint32_t lane = 0; lane < job.lanes; ++lane) {
    const uint64_t begin = job.base + uint64_t(lane) * hop;
    if (begin >= job.samples) break;
    const uint32_t count = std::min<uint64_t>(hop, job.samples - begin);
    const Complex* valid = output + uint64_t(lane) * job.length +
      (job.overlapSave ? job.taps - 1 : 0);
    for (uint32_t i = 0; i < count; ++i)
      surveillance[begin + i] -= valid[i] / double(job.length);
  }
}

// One reusable FFT workspace and thread.  The plan is created with one FFTW
// thread, so the caller can execute it simultaneously on this aligned buffer.
class CorrelationWorker {
  struct FftwFree {
    void operator()(Complex* value) const { fftw_free(value); }
  };

  const uint32_t length_;
  const uint32_t filterLength_;
  const uint32_t filterLanes_;
  std::unique_ptr<Complex, FftwFree> buffer_;
  std::unique_ptr<Complex, FftwFree> filterInput_;
  std::unique_ptr<Complex, FftwFree> filterOutput_;
  std::mutex mutex_;
  std::condition_variable condition_;
  bool stopping_ = false;
  bool pending_ = false;
  bool done_ = false;
  bool busy_ = false;
  bool isFilterJob_ = false;
  CorrelationJob job_{};
  FilterJob filterJob_{};
  std::exception_ptr error_;
  std::thread thread_;

  void run()
  {
#ifdef __linux__
    pthread_setname_np(pthread_self(), "vw-clutter-fft");
#endif
    std::unique_lock<std::mutex> lock(mutex_);
    for (;;) {
      condition_.wait(lock, [&] { return stopping_ || pending_; });
      if (stopping_ && !pending_) return;
      const bool isFilterJob = isFilterJob_;
      const CorrelationJob job = job_;
      const FilterJob filterJob = filterJob_;
      pending_ = false;
      lock.unlock();
      std::exception_ptr error;
      try {
        if (isFilterJob)
          filter_block(filterJob, filterInput_.get(), filterOutput_.get());
        else
          correlation_block(job.x, job.y, job.samples, job.taps, job.length,
                            job.begin, job.plan, buffer_.get());
      } catch (...) {
        error = std::current_exception();
      }
      lock.lock();
      error_ = error;
      done_ = true;
      condition_.notify_all();
    }
  }

public:
  CorrelationWorker(uint32_t length, Complex* plannedBuffer)
      : length_(length), filterLength_(0), filterLanes_(0),
        buffer_(static_cast<Complex*>(
            fftw_malloc(sizeof(Complex) * uint64_t(length) * 3)))
  {
    if (!length || !plannedBuffer)
      throw std::invalid_argument("Invalid clutter correlation worker geometry");
    if (!buffer_) throw std::bad_alloc();
    // fftw_execute_dft requires new arrays to have the plan's alignment.
    if (fftw_alignment_of(reinterpret_cast<double*>(buffer_.get())) !=
        fftw_alignment_of(reinterpret_cast<double*>(plannedBuffer)))
      throw std::runtime_error("Clutter worker FFT alignment differs from plan");
    thread_ = std::thread([this] { run(); });
  }

  CorrelationWorker(uint32_t length, Complex* plannedBuffer,
                    uint32_t filterLength, uint32_t filterLanes,
                    Complex* plannedFilterInput, Complex* plannedFilterOutput)
      : length_(length), filterLength_(filterLength), filterLanes_(filterLanes),
        buffer_(static_cast<Complex*>(fftw_malloc(sizeof(Complex) * uint64_t(length) * 3))),
        filterInput_(static_cast<Complex*>(fftw_malloc(
            sizeof(Complex) * uint64_t(filterLength) * filterLanes))),
        filterOutput_(static_cast<Complex*>(fftw_malloc(
            sizeof(Complex) * uint64_t(filterLength) * filterLanes)))
  {
    if (!length || !plannedBuffer || !filterLength || !filterLanes ||
        !plannedFilterInput || !plannedFilterOutput)
      throw std::invalid_argument("Invalid clutter worker geometry");
    if (!buffer_ || !filterInput_ || !filterOutput_) throw std::bad_alloc();
    if (fftw_alignment_of(reinterpret_cast<double*>(buffer_.get())) !=
        fftw_alignment_of(reinterpret_cast<double*>(plannedBuffer)) ||
        fftw_alignment_of(reinterpret_cast<double*>(filterInput_.get())) !=
        fftw_alignment_of(reinterpret_cast<double*>(plannedFilterInput)) ||
        fftw_alignment_of(reinterpret_cast<double*>(filterOutput_.get())) !=
        fftw_alignment_of(reinterpret_cast<double*>(plannedFilterOutput)))
      throw std::runtime_error("Clutter worker FFT alignment differs from plan");
    thread_ = std::thread([this] { run(); });
  }

  ~CorrelationWorker()
  {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      stopping_ = true;
    }
    condition_.notify_all();
    if (thread_.joinable()) thread_.join();
  }

  void start(const CorrelationJob& job)
  {
    validate_correlation_job(job);
    if (job.length != length_)
      throw std::invalid_argument("Clutter worker length mismatch");
    std::lock_guard<std::mutex> lock(mutex_);
    if (busy_ || stopping_)
      throw std::logic_error("Clutter worker job not consumed");
    job_ = job;
    isFilterJob_ = false;
    pending_ = true;
    done_ = false;
    busy_ = true;
    error_ = nullptr;
    condition_.notify_all();
  }

  void start(const FilterJob& job)
  {
    validate_filter_job(job);
    if (job.length != filterLength_ || job.lanes != filterLanes_)
      throw std::invalid_argument("Clutter worker filter geometry mismatch");
    std::lock_guard<std::mutex> lock(mutex_);
    if (busy_ || stopping_)
      throw std::logic_error("Clutter worker job not consumed");
    filterJob_ = job;
    isFilterJob_ = true;
    pending_ = true;
    done_ = false;
    busy_ = true;
    error_ = nullptr;
    condition_.notify_all();
  }

  const Complex* finish()
  {
    std::unique_lock<std::mutex> lock(mutex_);
    if (!busy_) throw std::logic_error("No clutter worker job pending");
    condition_.wait(lock, [&] { return done_; });
    done_ = false;
    busy_ = false;
    if (error_) std::rethrow_exception(error_);
    return isFilterJob_ ? filterOutput_.get() : buffer_.get();
  }

  CorrelationWorker(const CorrelationWorker&) = delete;
  CorrelationWorker& operator=(const CorrelationWorker&) = delete;
};

// Preserve block-order accumulation.  Only the independent forward FFT runs
// concurrently, yielding the same floating-point additions as the serial path.
inline void drain_workers(std::vector<std::unique_ptr<CorrelationWorker>>& workers,
                          size_t count) noexcept
{
  for (size_t i = 0; i < count; ++i)
    try { (void)workers[i]->finish(); } catch (...) {}
}

inline void correlate(std::vector<std::unique_ptr<CorrelationWorker>>& workers,
                      const Complex* x,
                      const Complex* y, uint32_t samples, uint32_t taps,
                      uint32_t length, fftw_plan plan, Complex* callerBuffer,
                      Complex* autocorrelation, Complex* crosscorrelation)
{
  validate_correlation_job({x, y, samples, taps, length, 0, plan});
  if (workers.empty() || !callerBuffer || !autocorrelation || !crosscorrelation)
    throw std::invalid_argument("Invalid clutter correlation storage");
  const uint32_t hop = length - taps + 1;
  const uint64_t stride = uint64_t(hop) * (workers.size() + 1);
  for (uint64_t begin = 0; begin < samples; begin += stride) {
    size_t submitted = 0;
    try {
      for (; submitted < workers.size(); ++submitted) {
        const uint64_t next = begin + uint64_t(submitted + 1) * hop;
        if (next >= samples) break;
        workers[submitted]->start({x, y, samples, taps, length, next, plan});
      }
      correlation_block(x, y, samples, taps, length, begin, plan, callerBuffer);
      accumulate_correlation(callerBuffer, length, autocorrelation,
                             crosscorrelation);
    } catch (...) {
      drain_workers(workers, submitted);
      throw;
    }
    for (size_t worker = 0; worker < submitted; ++worker) {
      try {
        accumulate_correlation(workers[worker]->finish(), length,
                               autocorrelation, crosscorrelation);
      } catch (...) {
        for (size_t remaining = worker + 1; remaining < submitted; ++remaining)
          try { (void)workers[remaining]->finish(); } catch (...) {}
        throw;
      }
    }
  }
}
}  // namespace vectorwarp_clutter
