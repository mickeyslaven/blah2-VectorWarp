#include "WienerHopf.h"
#include "BulkRotation.h"
#include "CorrelationWorker.h"
#include "gpu/GpuBlockedCpu.h"
#ifdef VECTORWARP_MIXED_FIR_BENCH
#include "gpu/OwlGpuFilter.h"
#include "gpu/GpuCompletionWorker.h"
#endif
#ifdef VECTORWARP_GPU_BLOCKED_FULL_BENCH
#include "gpu/GpuBlockedCorrelation.h"
#include "gpu/GpuOperationGuard.h"
#endif
#include "process/meta/FftLength.h"
#include <armadillo>
#include <fftw3.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <fstream>
#include <thread>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using Complex = std::complex<double>;
struct BufferFree { void operator()(Complex* p) const { fftw_free(p); } };
using Buffer = std::unique_ptr<Complex, BufferFree>;
struct PlanFree { void operator()(fftw_plan_s* p) const { if (p) fftw_destroy_plan(p); } };
using Plan = std::unique_ptr<fftw_plan_s, PlanFree>;
Buffer allocate(uint64_t count) {
  if (!count || count > std::numeric_limits<size_t>::max() / sizeof(Complex))
    throw std::invalid_argument("Clutter workspace exceeds addressable storage");
  Buffer result(static_cast<Complex*>(fftw_malloc(count * sizeof(Complex))));
  if (!result) throw std::bad_alloc();
  return result;
}
fftw_complex* fftData(const Buffer& p) { return reinterpret_cast<fftw_complex*>(p.get()); }
Plan checked(fftw_plan p) {
  if (!p) throw std::runtime_error("Could not create clutter FFT plan");
  return Plan(p);
}
uint32_t blockLength(uint64_t minimum, uint32_t floor) {
  uint32_t value = floor;
  while (value < minimum) {
    if (value > uint32_t(INT32_MAX) / 2) throw std::invalid_argument("Clutter block is too large");
    value *= 2;
  }
  return value;
}
std::mutex& plannerMutex() { static std::mutex mutex; return mutex; }
struct RestoreThreads {
  const int previous = fftw_planner_nthreads();
  ~RestoreThreads() { fftw_plan_with_nthreads(previous); }
};
unsigned fftw_plan_flags() {
  const char* mode = std::getenv("VECTORWARP_FFTW_PLAN");
  if (!mode || std::string(mode) == "estimate") return FFTW_ESTIMATE;
  if (std::string(mode) == "measure") return FFTW_MEASURE;
  throw std::invalid_argument("VECTORWARP_FFTW_PLAN must be estimate or measure");
}
uint32_t clutter_worker_slots() {
  const char* value = std::getenv("VECTORWARP_CLUTTER_WORKERS");
  if (!value || std::string(value) == "2") return 2;
  if (std::string(value) == "1") return 1;
  if (std::string(value) == "4") return 4;
  throw std::invalid_argument("VECTORWARP_CLUTTER_WORKERS must be 1, 2, or 4");
}
}

// Blocked correlation and FIR follow the CPU algorithms contributed in
// offworldlabs/blah2-arm PR67. Keep dense FP64 Cholesky and VectorWarp's thread
// budget; this does not import its V3D backend or experimental fast solver.
#ifdef VECTORWARP_MIXED_FIR_BENCH
struct GpuCycle {
  OwlGpuFilter* gpu;
  ~GpuCycle() { if (gpu) gpu->abort(); }
};
#endif
struct WienerHopf::Impl {
  // The shared-reference API deliberately has its own full-CPI workspace.
  // It is lazy so the Pi borrowed path keeps its bounded allocation profile.
  struct PreparedWorkspace {
    Buffer scratch;
    std::array<Plan, 4> plans;
    PreparedWorkspace(uint32_t samples, uint32_t filterLength) : scratch(allocate(filterLength)) {
      std::lock_guard<std::mutex> lock(plannerMutex());
      const unsigned flags = fftw_plan_flags();
      plans[0] = checked(fftw_plan_dft_1d(samples, fftData(scratch), fftData(scratch), FFTW_FORWARD, flags));
      plans[1] = checked(fftw_plan_dft_1d(samples, fftData(scratch), fftData(scratch), FFTW_BACKWARD, flags));
      plans[2] = checked(fftw_plan_dft_1d(filterLength, fftData(scratch), fftData(scratch), FFTW_FORWARD, flags));
      plans[3] = checked(fftw_plan_dft_1d(filterLength, fftData(scratch), fftData(scratch), FFTW_BACKWARD, flags));
    }
    ~PreparedWorkspace() {
      std::lock_guard<std::mutex> lock(plannerMutex());
      for (auto& plan : plans) plan.reset();
    }
  };
  int32_t delayMin;
  uint32_t samples, taps, correlationLength, filterLength, lanes;
  bool sharedReferenceWorkspace;
  bool blockedCorrelation, blockedFilter;
  bool viewReady = false;
  uint64_t generation = 0;
  std::ofstream contiguousLog;
  uint64_t contiguousFrame = 0;
  arma::cx_mat matrix;
  arma::cx_vec a, b, weights;
  Buffer x, y, outX, outY, fullA, fullB, correlation, correlationA, correlationB;
  Buffer filterX, filterW, filterOut;
  // Declared after buffers so plan destruction precedes storage release.
  std::array<Plan, 4> fullPlans;
  std::array<Plan, 3> correlationPlans, filterPlans;
  std::unique_ptr<PreparedWorkspace> preparedWorkspace;
  // At most three persistent workers supplement the caller's FFT slot.
  std::vector<std::unique_ptr<vectorwarp_clutter::CorrelationWorker>> correlationWorkers;
#ifdef VECTORWARP_MIXED_FIR_BENCH
  std::unique_ptr<OwlGpuFilter> gpu;
  std::unique_ptr<GpuCompletionWorker> gpuCompletion;
  bool asyncGpuFinish = false, earlyGpuReference = false;
  std::ofstream overlapLog;
  uint64_t overlapFrame = 0;
#endif
#ifdef VECTORWARP_GPU_BLOCKED_FULL_BENCH
  std::unique_ptr<GpuBlockedCorrelation> gpuCorrelation;
  std::ofstream gpuCorrelationLog;
  uint64_t gpuCorrelationFrame = 0;
#endif

  Impl(int32_t first, int32_t last, uint32_t count, bool sharedReference)
      : delayMin(first), samples(count), sharedReferenceWorkspace(sharedReference) {
    if (const char* path = std::getenv("VECTORWARP_BENCH_CONTIGUOUS_LOG")) {
      contiguousLog.open(path, std::ios::app);
      if (!contiguousLog) throw std::runtime_error("Contiguous CPI timing log open failed");
    }
    const int64_t width = int64_t(last) - first;
    if (!count || count > uint32_t(INT32_MAX) || width <= 0 || uint64_t(width) > count)
      throw std::invalid_argument("Clutter filter needs a non-empty half-open delay range no longer than the CPI");
    taps = uint32_t(width);
    correlationLength = blockLength(uint64_t(taps) * 2, 4096);
    const uint32_t filterBlock = blockLength(uint64_t(taps) * 2, 1024);
    blockedCorrelation = uint64_t(count) >= uint64_t(correlationLength) * 4;
    blockedFilter = uint64_t(count) >= uint64_t(filterBlock) * 4;
    filterLength = sharedReferenceWorkspace ? blah2::nextFastFftLength(uint64_t(count) + taps + 1) :
      (blockedFilter ? filterBlock : blah2::nextFastFftLength(uint64_t(count) + taps + 1));
    lanes = blockedFilter ? 4 : 1;
    if (sharedReferenceWorkspace) {
      preparedWorkspace = std::make_unique<PreparedWorkspace>(samples, filterLength);
      b.set_size(taps); weights.set_size(taps);
      return;
    }
    matrix.set_size(taps, taps); a.set_size(taps); b.set_size(taps); weights.set_size(taps);
    x = allocate(count); y = allocate(count);
    filterX = allocate(uint64_t(filterLength) * lanes);
    filterW = allocate(filterLength);
    filterOut = allocate(uint64_t(filterLength) * lanes);
    if (blockedCorrelation) {
      correlation = allocate(uint64_t(correlationLength) * 3);
      correlationA = allocate(correlationLength); correlationB = allocate(correlationLength);
    } else {
      outX = allocate(count); outY = allocate(count);
      fullA = allocate(count); fullB = allocate(count);
    }
#ifdef VECTORWARP_MIXED_FIR_BENCH
    const char* percentSetting = std::getenv("VECTORWARP_GPU_FIR_PERCENT");
    const std::string percent = percentSetting ? percentSetting : "0";
    const char* fftSetting = std::getenv("VECTORWARP_GPU_FIR_FFT");
    const std::string fft = fftSetting ? fftSetting : "2048";
    if (percent != "0" && percent != "25" && percent != "50" &&
        percent != "75" && percent != "100")
      throw std::invalid_argument("VECTORWARP_GPU_FIR_PERCENT must be 0, 25, 50, 75, or 100");
    if (fft != "1024" && fft != "2048" && fft != "4096")
      throw std::invalid_argument("VECTORWARP_GPU_FIR_FFT must be 1024, 2048, or 4096");
    auto readToggle = [](const char* name) {
      const char* value = std::getenv(name);
      if (!value || std::string(value) == "0") return false;
      if (std::string(value) == "1") return true;
      throw std::invalid_argument(std::string(name) + " must be 0 or 1");
    };
    earlyGpuReference = readToggle("VECTORWARP_GPU_FIR_EARLY_REFERENCE");
    asyncGpuFinish = readToggle("VECTORWARP_GPU_FIR_ASYNC_FINISH");
    if (percent != "0") {
      const uint32_t gpuFft = static_cast<uint32_t>(std::stoul(fft));
      if (!blockedFilter || taps > gpuFft)
        throw std::runtime_error("Mixed FIR benchmark needs blocked filter and taps <= GPU FFT");
      gpu = std::make_unique<OwlGpuFilter>(samples, taps, gpuFft,
        static_cast<uint32_t>(std::stoul(percent)), earlyGpuReference);
      if (asyncGpuFinish) gpuCompletion = std::make_unique<GpuCompletionWorker>();
      if (const char* path = std::getenv("VECTORWARP_GPU_FIR_OVERLAP_LOG")) {
        overlapLog.open(path, std::ios::app);
        if (!overlapLog) throw std::runtime_error("Mixed FIR overlap log open failed");
      }
    }
#endif
#ifdef VECTORWARP_GPU_BLOCKED_FULL_BENCH
    const char* correlationSetting = std::getenv("VECTORWARP_GPU_BLOCKED_CORRELATION");
    const std::string correlationMode = correlationSetting ? correlationSetting : "0";
    if (correlationMode != "0" && correlationMode != "1")
      throw std::invalid_argument("VECTORWARP_GPU_BLOCKED_CORRELATION must be 0 or 1");
    if (correlationMode == "1") {
      if (!blockedCorrelation || samples != 1000000 || taps != 410 || correlationLength != 4096)
        throw std::runtime_error("GPU blocked correlation requires exact 1M/410/4096 blocked geometry");
      gpuCorrelation = std::make_unique<GpuBlockedCorrelation>(samples, taps, correlationLength);
      if (const char* path = std::getenv("VECTORWARP_GPU_BLOCKED_CORRELATION_LOG")) {
        gpuCorrelationLog.open(path, std::ios::app);
        if (!gpuCorrelationLog)
          throw std::runtime_error("GPU blocked correlation log open failed");
      }
    }
#endif
    std::lock_guard<std::mutex> lock(plannerMutex());
    static std::once_flag initialized;
    static bool ready = false;
    std::call_once(initialized, [] { ready = fftw_init_threads() != 0; });
    if (!ready) throw std::runtime_error("FFTW thread initialization failed");
    RestoreThreads restore;
    unsigned planFlags = fftw_plan_flags();
    if (count == 1000000 &&
        first == -10 && taps == 410)
      planFlags = FFTW_MEASURE;
    const uint32_t requestedWorkers = clutter_worker_slots();
    try {
      if (blockedCorrelation) {
        fftw_plan_with_nthreads(1);
        int length = int(correlationLength);
        correlationPlans[0] = checked(fftw_plan_many_dft(1, &length, 3,
          fftData(correlation), nullptr, 1, length, fftData(correlation), nullptr, 1, length,
          FFTW_FORWARD, planFlags));
        correlationPlans[1] = checked(fftw_plan_dft_1d(length, fftData(correlationA), fftData(correlationA), FFTW_BACKWARD, planFlags));
        correlationPlans[2] = checked(fftw_plan_dft_1d(length, fftData(correlationB), fftData(correlationB), FFTW_BACKWARD, planFlags));
      } else {
        fullPlans[0] = checked(fftw_plan_dft_1d(count, fftData(x), fftData(outX), FFTW_FORWARD, planFlags));
        fullPlans[1] = checked(fftw_plan_dft_1d(count, fftData(y), fftData(outY), FFTW_FORWARD, planFlags));
        fullPlans[2] = checked(fftw_plan_dft_1d(count, fftData(fullA), fftData(fullA), FFTW_BACKWARD, planFlags));
        fullPlans[3] = checked(fftw_plan_dft_1d(count, fftData(fullB), fftData(fullB), FFTW_BACKWARD, planFlags));
      }
      fftw_plan_with_nthreads(blockedFilter ? 1 : restore.previous);
      int length = int(filterLength);
      filterPlans[0] = checked(fftw_plan_many_dft(1, &length, lanes,
        fftData(filterX), nullptr, 1, length, fftData(filterX), nullptr, 1, length, FFTW_FORWARD, planFlags));
      filterPlans[1] = checked(fftw_plan_dft_1d(length, fftData(filterW), fftData(filterW), FFTW_FORWARD, planFlags));
      filterPlans[2] = checked(fftw_plan_many_dft(1, &length, lanes,
        fftData(filterOut), nullptr, 1, length, fftData(filterOut), nullptr, 1, length, FFTW_BACKWARD, planFlags));
      const uint32_t slots = std::min(requestedWorkers,
        static_cast<uint32_t>(std::max(restore.previous, 1)));
      if (blockedCorrelation)
        for (uint32_t worker = 1; worker < slots; ++worker)
          correlationWorkers.push_back(
            std::make_unique<vectorwarp_clutter::CorrelationWorker>(
              correlationLength, correlation.get(), filterLength, lanes,
              filterX.get(), filterOut.get()));
    } catch (...) {
      // Exceptional member cleanup must also serialize FFTW plan destruction.
      for (auto& p : fullPlans) p.reset();
      for (auto& p : correlationPlans) p.reset();
      for (auto& p : filterPlans) p.reset();
      throw;
    }
  }
  ~Impl() {
#ifdef VECTORWARP_MIXED_FIR_BENCH
    gpuCompletion.reset();
#endif
    // A worker may be executing correlationPlans[0]; join it before either its
    // FFT plan or buffers are released.
    correlationWorkers.clear();
    preparedWorkspace.reset();
    std::lock_guard<std::mutex> lock(plannerMutex());
    for (auto& p : fullPlans) p.reset();
    for (auto& p : correlationPlans) p.reset();
    for (auto& p : filterPlans) p.reset();
  }

  bool process_prepared(const WienerHopf::PreparedReference& reference,
                        IqData* surveillance) {
    viewReady = false;
    if (!preparedWorkspace)
      preparedWorkspace = std::make_unique<PreparedWorkspace>(samples, reference.filterLength_);
    auto& scratch = preparedWorkspace->scratch;
    std::copy_n(surveillance->view_data().begin(), samples, scratch.get());
    fftw_execute(preparedWorkspace->plans[0].get());
    for (uint32_t i = 0; i < samples; ++i)
      scratch.get()[i] *= std::conj(reference.spectrum_[i]);
    fftw_execute(preparedWorkspace->plans[1].get());
    for (uint32_t i = 0; i < taps; ++i)
      b[i] = scratch.get()[i] / double(samples);
    if (!arma::solve(weights, arma::trimatu(reference.cholesky_),
                     arma::solve(arma::trimatl(arma::trans(reference.cholesky_)), b))) {
      std::cerr << "Solve failed, skip clutter filter\n";
      return false;
    }
    std::copy_n(weights.memptr(), taps, scratch.get());
    std::fill(scratch.get() + taps, scratch.get() + reference.filterLength_, Complex{});
    fftw_execute(preparedWorkspace->plans[2].get());
    for (uint32_t i = 0; i < reference.filterLength_; ++i)
      scratch.get()[i] *= reference.paddedSpectrum_[i];
    fftw_execute(preparedWorkspace->plans[3].get());
    surveillance->subtract_clutter(scratch.get(), samples, reference.filterLength_);
    return true;
  }

  bool process(IqData* reference, IqData* surveillance, bool borrowOutput) {
    if (sharedReferenceWorkspace)
      throw std::logic_error("Shared-reference clutter workspace requires a prepared reference");
    viewReady = false;
    if (!reference || !surveillance || reference->get_length() < samples || surveillance->get_length() < samples)
      throw std::invalid_argument("Clutter input length differs from CPI");
    const auto inputStarted = std::chrono::steady_clock::now();
    vectorwarp_clutter::copy_rotation(reference->view_data(), samples, delayMin, x.get());
    std::copy_n(surveillance->view_data().begin(), samples, y.get());
    const auto inputFinished = std::chrono::steady_clock::now();
#ifdef VECTORWARP_MIXED_FIR_BENCH
#ifdef VECTORWARP_GPU_BLOCKED_FULL_BENCH
    if (gpu && !gpuCorrelation) gpu->reference(x.get());
#else
    if (gpu) gpu->reference(x.get());
#endif
    GpuCycle gpuCycle{gpu.get()};
#endif
#ifdef VECTORWARP_GPU_BLOCKED_FULL_BENCH
    GpuOperationGuard<GpuBlockedCorrelation> gpuCorrelationCycle{gpuCorrelation.get()};
    GpuBlockedCorrelationTiming gpuCorrelationTiming{};
    double cpuCorrelationBlocksMs = 0, cpuCorrelationInverseMs = 0, solveMs = 0;
    if (gpuCorrelation) gpuCorrelation->start(x.get(), y.get());
#endif
    if (blockedCorrelation) {
      std::fill_n(correlationA.get(), correlationLength, Complex{});
      std::fill_n(correlationB.get(), correlationLength, Complex{});
#ifdef VECTORWARP_GPU_BLOCKED_FULL_BENCH
      const auto cpuBlocksStarted = std::chrono::steady_clock::now();
      vectorwarp_gpu_corr_bench::correlateCpuBlocks(correlationWorkers,
        x.get(), y.get(), samples, taps, correlationLength,
        gpuCorrelation ? GpuBlockedCorrelation::gpu_blocks() : 0,
        correlationPlans[0].get(), correlation.get(),
        correlationA.get(), correlationB.get());
      const auto cpuBlocksFinished = std::chrono::steady_clock::now();
#else
      vectorwarp_gpu_corr_bench::correlateCpuBlocks(correlationWorkers,
        x.get(), y.get(), samples, taps, correlationLength, 0,
        correlationPlans[0].get(), correlation.get(),
        correlationA.get(), correlationB.get());
#endif
      fftw_execute(correlationPlans[1].get()); fftw_execute(correlationPlans[2].get());
#ifdef VECTORWARP_GPU_BLOCKED_FULL_BENCH
      const auto cpuInverseFinished = std::chrono::steady_clock::now();
      cpuCorrelationBlocksMs = std::chrono::duration<double,std::milli>(
        cpuBlocksFinished-cpuBlocksStarted).count();
      cpuCorrelationInverseMs = std::chrono::duration<double,std::milli>(
        cpuInverseFinished-cpuBlocksFinished).count();
#endif
      for (uint32_t i = 0; i < taps; ++i) {
        a[i] = std::conj(correlationA.get()[i]) / double(correlationLength);
        b[i] = correlationB.get()[i] / double(correlationLength);
      }
#ifdef VECTORWARP_GPU_BLOCKED_FULL_BENCH
      if (gpuCorrelation)
        gpuCorrelationTiming = gpuCorrelation->finish(a.memptr(), b.memptr());
#endif
    } else {
      fftw_execute(fullPlans[1].get());
      fftw_execute(fullPlans[0].get());
      for (uint32_t i = 0; i < samples; ++i) {
        fullA.get()[i] = outX.get()[i] * std::conj(outX.get()[i]);
        fullB.get()[i] = outY.get()[i] * std::conj(outX.get()[i]);
      }
      fftw_execute(fullPlans[2].get());
      fftw_execute(fullPlans[3].get());
      for (uint32_t i = 0; i < taps; ++i) {
        a[i] = std::conj(fullA.get()[i]) / double(samples);
        b[i] = fullB.get()[i] / double(samples);
      }
    }
    // Zero-lag autocorrelation is sum(norm(x)), hence exactly real. The
    // blocked FFT reduction can leave a small imaginary rounding residue at
    // raw ADC amplitudes; it must not become an imaginary matrix diagonal.
    a[0] = Complex(a[0].real(), 0);
#ifdef VECTORWARP_MIXED_FIR_BENCH
#ifdef VECTORWARP_GPU_BLOCKED_FULL_BENCH
    if (gpu && gpuCorrelation) gpu->reference(x.get());
#endif
#endif
#ifdef VECTORWARP_GPU_BLOCKED_FULL_BENCH
    const auto solveStarted = std::chrono::steady_clock::now();
#endif
    matrix = arma::toeplitz(a);
    for (uint32_t row = 0; row < taps; ++row)
      for (uint32_t col = 0; col < row; ++col) matrix(row,col) = std::conj(matrix(row,col));
    if (!arma::chol(matrix, matrix)) {
      std::cerr << "Chol decomposition failed, skip clutter filter\n"; return false;
    }
    if (!arma::solve(weights, arma::trimatu(matrix), arma::solve(arma::trimatl(arma::trans(matrix)), b))) {
      std::cerr << "Solve failed, skip clutter filter\n"; return false;
    }
#ifdef VECTORWARP_GPU_BLOCKED_FULL_BENCH
    solveMs = std::chrono::duration<double,std::milli>(
      std::chrono::steady_clock::now()-solveStarted).count();
    if (gpuCorrelation && gpuCorrelationLog) {
      gpuCorrelationLog << "{\"frame\":" << ++gpuCorrelationFrame
        << ",\"gpu_blocks\":" << gpuCorrelationTiming.gpuBlocks
        << ",\"cpu_blocks\":" << gpuCorrelationTiming.cpuBlocks
        << ",\"total_blocks\":" << GpuBlockedCorrelation::total_blocks()
        << ",\"convert_ms\":" << gpuCorrelationTiming.convertMs
        << ",\"flush_ms\":" << gpuCorrelationTiming.flushMs
        << ",\"submit_ms\":" << gpuCorrelationTiming.submitMs
        << ",\"cpu_blocks_ms\":" << cpuCorrelationBlocksMs
        << ",\"cpu_inverse_ms\":" << cpuCorrelationInverseMs
        << ",\"submit_to_fence_ms\":" << gpuCorrelationTiming.submitToFenceMs
        << ",\"fence_wait_ms\":" << gpuCorrelationTiming.fenceWaitMs
        << ",\"invalidate_ms\":" << gpuCorrelationTiming.invalidateMs
        << ",\"mapped_copy_ms\":" << gpuCorrelationTiming.mappedCopyMs
        << ",\"finite_check_ms\":" << gpuCorrelationTiming.finiteCheckMs
        << ",\"fp64_merge_ms\":" << gpuCorrelationTiming.fp64MergeMs
        << ",\"solve_ms\":" << solveMs << "}\n" << std::flush;
    }
#endif
    uint32_t gpuSamples = 0;
#ifdef VECTORWARP_MIXED_FIR_BENCH
    if (gpu) gpuSamples = gpu->start(x.get(), weights.memptr());
    if (gpuCompletion) gpuCompletion->start([this] { gpu->finish(y.get()); });
    GpuCompletionGuard completionGuard{gpuCompletion.get()};
    const auto cpuSuffixStart = std::chrono::steady_clock::now();
#endif
    if (gpuSamples < samples) {
      std::copy_n(weights.memptr(), taps, filterW.get());
      std::fill(filterW.get() + taps, filterW.get() + filterLength, Complex{});
      fftw_execute(filterPlans[1].get());
    }
    const uint32_t hop = blockedFilter ? filterLength - taps + 1 : samples;
    const uint64_t batch = uint64_t(lanes) * hop;
    auto filterJob = [&](uint64_t base) {
      return vectorwarp_clutter::FilterJob{x.get(), filterW.get(), samples,
        taps, filterLength, lanes, base, filterPlans[0].get(),
        filterPlans[2].get(), blockedFilter};
    };
    if (!correlationWorkers.empty() && blockedFilter && samples > batch) {
      // Both batches read x and weights only; subtract their disjoint outputs
      // in base order after each transform completes.
      const uint64_t stride = batch * (correlationWorkers.size() + 1);
      for (uint64_t base = gpuSamples; base < samples; base += stride) {
        size_t submitted = 0;
        try {
          for (; submitted < correlationWorkers.size(); ++submitted) {
            const uint64_t next = base + uint64_t(submitted + 1) * batch;
            if (next >= samples) break;
            correlationWorkers[submitted]->start(filterJob(next));
          }
          const auto current = filterJob(base);
          vectorwarp_clutter::filter_block(current, filterX.get(), filterOut.get());
          vectorwarp_clutter::subtract_filter_block(current, filterOut.get(), y.get());
        } catch (...) {
          vectorwarp_clutter::drain_workers(correlationWorkers, submitted);
          throw;
        }
        for (size_t worker = 0; worker < submitted; ++worker) {
          const uint64_t next = base + uint64_t(worker + 1) * batch;
          try {
            vectorwarp_clutter::subtract_filter_block(filterJob(next),
              correlationWorkers[worker]->finish(), y.get());
          } catch (...) {
            for (size_t remaining = worker + 1; remaining < submitted; ++remaining)
              try { (void)correlationWorkers[remaining]->finish(); } catch (...) {}
            throw;
          }
        }
      }
    } else {
      for (uint64_t base = gpuSamples; base < samples; base += batch) {
        const auto current = filterJob(base);
        vectorwarp_clutter::filter_block(current, filterX.get(), filterOut.get());
        vectorwarp_clutter::subtract_filter_block(current, filterOut.get(), y.get());
      }
    }
#ifdef VECTORWARP_MIXED_FIR_BENCH
    const auto cpuSuffixEnd = std::chrono::steady_clock::now();
    const auto joinStart = cpuSuffixEnd;
    if (gpuCompletion) completionGuard.finish();
    else if (gpu) gpu->finish(y.get());
    const auto joinEnd = std::chrono::steady_clock::now();
    if (gpu && overlapLog) {
      auto millis = [](auto first, auto last) {
        return std::chrono::duration<double, std::milli>(last-first).count();
      };
      overlapLog << "{\"frame\":" << ++overlapFrame
        << ",\"early_reference\":" << (earlyGpuReference ? "true" : "false")
        << ",\"async_finish\":" << (asyncGpuFinish ? "true" : "false")
        << ",\"gpu_samples\":" << gpuSamples
        << ",\"cpu_samples\":" << (samples-gpuSamples)
        << ",\"cpu_suffix_ms\":" << millis(cpuSuffixStart,cpuSuffixEnd)
        << ",\"join_wait_ms\":" << millis(joinStart,joinEnd) << "}\n" << std::flush;
    }
#endif
    const auto publishStarted = std::chrono::steady_clock::now();
    if (!borrowOutput) surveillance->assign_complex(y.get(), samples);
    else { ++generation; viewReady = true; }
    if (contiguousLog) {
      const auto publishFinished = std::chrono::steady_clock::now();
      auto ms=[](auto a,auto b){return std::chrono::duration<double,std::milli>(b-a).count();};
      contiguousLog << "{\"frame\":" << ++contiguousFrame
        << ",\"borrowed\":" << (borrowOutput?"true":"false")
        << ",\"input_copy_ms\":" << ms(inputStarted,inputFinished)
        << ",\"publish_copy_ms\":" << ms(publishStarted,publishFinished)
        << "}\n" << std::flush;
    }
    return true;
  }
  WienerHopf::FilteredView filtered_view() const {
    if (!viewReady) throw std::logic_error("No completed borrowed clutter CPI");
    return {x.get(), y.get(), samples, delayMin, generation};
  }
};
WienerHopf::PreparedReference::PreparedReference(int32_t first, int32_t last,
                                                 uint32_t count)
  : delayMin_(first), samples_(count)
{
  const int64_t width = int64_t(last) - first;
  if (!count || width <= 0 || uint64_t(width) > count)
    throw std::invalid_argument("Clutter filter needs a non-empty half-open delay range no longer than the CPI");
  taps_ = static_cast<uint32_t>(width);
  filterLength_ = blah2::nextFastFftLength(uint64_t(count) + taps_ + 1);
  rotated_.resize(samples_); spectrum_.resize(samples_); paddedSpectrum_.resize(filterLength_);
  correlation_.set_size(taps_); cholesky_.set_size(taps_, taps_);
  std::lock_guard<std::mutex> lock(plannerMutex());
  referencePlan_ = fftw_plan_dft_1d(samples_, reinterpret_cast<fftw_complex*>(rotated_.data()),
    reinterpret_cast<fftw_complex*>(spectrum_.data()), FFTW_FORWARD, fftw_plan_flags());
  correlationPlan_ = fftw_plan_dft_1d(samples_, reinterpret_cast<fftw_complex*>(rotated_.data()),
    reinterpret_cast<fftw_complex*>(rotated_.data()), FFTW_BACKWARD, fftw_plan_flags());
  paddedPlan_ = fftw_plan_dft_1d(filterLength_, reinterpret_cast<fftw_complex*>(paddedSpectrum_.data()),
    reinterpret_cast<fftw_complex*>(paddedSpectrum_.data()), FFTW_FORWARD, fftw_plan_flags());
  if (!referencePlan_ || !correlationPlan_ || !paddedPlan_) {
    if (referencePlan_) fftw_destroy_plan(referencePlan_);
    if (correlationPlan_) fftw_destroy_plan(correlationPlan_);
    if (paddedPlan_) fftw_destroy_plan(paddedPlan_);
    throw std::runtime_error("Could not plan clutter reference FFT");
  }
}

WienerHopf::PreparedReference::~PreparedReference()
{
  std::lock_guard<std::mutex> lock(plannerMutex());
  fftw_destroy_plan(referencePlan_);
  fftw_destroy_plan(correlationPlan_);
  fftw_destroy_plan(paddedPlan_);
}

bool WienerHopf::PreparedReference::prepare(const IqData& reference)
{
  valid_ = false;
  const auto& data = reference.view_data();
  if (data.size() < samples_)
    throw std::invalid_argument("Clutter reference is shorter than CPI");
  for (uint32_t i = 0; i < samples_; ++i) {
    const int64_t shifted = (int64_t(i) - delayMin_) % int64_t(samples_);
    rotated_[i] = data[shifted < 0 ? shifted + samples_ : shifted];
    paddedSpectrum_[i] = rotated_[i];
  }
  std::fill(paddedSpectrum_.begin() + samples_, paddedSpectrum_.end(), Complex{});
  fftw_execute(paddedPlan_);
  fftw_execute(referencePlan_);
  for (uint32_t i = 0; i < samples_; ++i)
    rotated_[i] = spectrum_[i] * std::conj(spectrum_[i]);
  fftw_execute(correlationPlan_);
  for (uint32_t i = 0; i < taps_; ++i)
    correlation_[i] = std::conj(rotated_[i]) / double(samples_);
  correlation_[0] = Complex(correlation_[0].real(), 0);
  cholesky_ = arma::toeplitz(correlation_);
  for (uint32_t row = 0; row < taps_; ++row)
    for (uint32_t col = 0; col < row; ++col)
      cholesky_(row, col) = std::conj(cholesky_(row, col));
  valid_ = arma::chol(cholesky_, cholesky_);
  if (!valid_) std::cerr << "Chol decomposition failed, skip clutter filter\n";
  return valid_;
}

WienerHopf::WienerHopf(int32_t first, int32_t last, uint32_t samples, Workspace workspace)
  : impl_(std::make_unique<Impl>(first, last, samples, workspace == Workspace::SharedReference)) {}
WienerHopf::~WienerHopf() = default;
uint32_t WienerHopf::filter_fft_length() const { return impl_->filterLength; }
bool WienerHopf::process(IqData* x, IqData* y, bool borrowOutput) { return impl_->process(x,y,borrowOutput); }
bool WienerHopf::process(const PreparedReference& reference, IqData* surveillance) {
  if (reference.delayMin_ != impl_->delayMin || reference.taps_ != impl_->taps ||
      reference.samples_ != impl_->samples)
    throw std::invalid_argument("Clutter surveillance or prepared reference geometry does not match");
  if (!surveillance || surveillance->get_length() < impl_->samples)
    throw std::invalid_argument("Clutter input length differs from CPI");
  if (!reference.valid_) return false;
  return impl_->process_prepared(reference, surveillance);
}
WienerHopf::FilteredView WienerHopf::filtered_view() const { return impl_->filtered_view(); }
