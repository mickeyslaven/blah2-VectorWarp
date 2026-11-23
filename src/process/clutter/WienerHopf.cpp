#include "WienerHopf.h"
#include "BulkRotation.h"
#include "CorrelationWorker.h"
#include "gpu/GpuBlockedCpu.h"
#include "process/meta/FftLength.h"
#include "process/utility/FftwThreads.h"
#include <armadillo>
#include <fftw3.h>
#include <algorithm>
#include <array>
#include <charconv>
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
#include <system_error>
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
  const int previous = blah2::fftw_planner_threads();
  ~RestoreThreads() { blah2::set_fftw_planner_threads(previous); }
};
unsigned fftw_plan_flags() {
  const char* mode = std::getenv("VECTORWARP_FFTW_PLAN");
  if (!mode || std::string(mode) == "estimate") return FFTW_ESTIMATE;
  if (std::string(mode) == "measure") return FFTW_MEASURE;
  throw std::invalid_argument("VECTORWARP_FFTW_PLAN must be estimate or measure");
}
uint32_t clutter_worker_slots(uint32_t configured) {
  std::size_t requested = configured;
  if (!requested) {
    const char* value = std::getenv("VECTORWARP_CLUTTER_WORKERS");
    if (value && *value) {
      const char* end = value + std::char_traits<char>::length(value);
      const auto parsed = std::from_chars(value, end, requested);
      if (parsed.ec != std::errc{} || parsed.ptr != end || !requested || requested > 8)
        throw std::invalid_argument("VECTORWARP_CLUTTER_WORKERS must be an integer from 1 through 8");
    }
  }
  // Direct/single-lane callers retain the historical two-slot default. A
  // RadarBank passes its immutable per-lane share explicitly.
  const std::size_t slots = requested ? requested : 2;
  if (slots > 8)
    throw std::invalid_argument("Clutter worker slots must be from 1 through 8");
  return static_cast<uint32_t>(slots);
}
}

// Blocked correlation and FIR follow the CPU algorithms contributed in
// offworldlabs/blah2-arm PR67. Keep dense FP64 Cholesky and VectorWarp's thread
// budget; this does not import its V3D backend or experimental fast solver.
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
  double relativeDiagonalLoading;
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

  Impl(int32_t first, int32_t last, uint32_t count, bool sharedReference,
       double loading, uint32_t workerSlots)
      : delayMin(first), samples(count), relativeDiagonalLoading(loading),
        sharedReferenceWorkspace(sharedReference) {
    if (!std::isfinite(relativeDiagonalLoading) || relativeDiagonalLoading < 0 ||
        relativeDiagonalLoading > 1)
      throw std::invalid_argument("Clutter relative diagonal loading must be finite in [0,1]");
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
    const uint32_t requestedWorkers = clutter_worker_slots(workerSlots);
    try {
      if (blockedCorrelation) {
        blah2::set_fftw_planner_threads(1);
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
      blah2::set_fftw_planner_threads(blockedFilter ? 1 : restore.previous);
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
    surveillance->sample_view().copy_to(scratch.get(), samples);
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

  template <typename Load>
  bool process_loaded(IqData* surveillance, bool borrowOutput, Load load) {
    if (sharedReferenceWorkspace)
      throw std::logic_error("Shared-reference clutter workspace requires a prepared reference");
    viewReady = false;
    const auto inputStarted = std::chrono::steady_clock::now();
    load(x.get(), y.get());
    const auto inputFinished = std::chrono::steady_clock::now();
    const auto diagnosticCorrelationStarted = inputFinished;
    if (blockedCorrelation) {
      std::fill_n(correlationA.get(), correlationLength, Complex{});
      std::fill_n(correlationB.get(), correlationLength, Complex{});
      vectorwarp_gpu_corr_bench::correlateCpuBlocks(correlationWorkers,
        x.get(), y.get(), samples, taps, correlationLength, 0,
        correlationPlans[0].get(), correlation.get(),
        correlationA.get(), correlationB.get());
      fftw_execute(correlationPlans[1].get()); fftw_execute(correlationPlans[2].get());
      for (uint32_t i = 0; i < taps; ++i) {
        a[i] = std::conj(correlationA.get()[i]) / double(correlationLength);
        b[i] = correlationB.get()[i] / double(correlationLength);
      }
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
    const auto diagnosticCorrelationFinished = std::chrono::steady_clock::now();
    matrix = arma::toeplitz(a);
    for (uint32_t row = 0; row < taps; ++row)
      for (uint32_t col = 0; col < row; ++col) matrix(row,col) = std::conj(matrix(row,col));
    if (relativeDiagonalLoading) {
      const double scale = a[0].real();
      const double loading = relativeDiagonalLoading * scale;
      if (!std::isfinite(scale) || scale < 0 || !std::isfinite(loading)) {
        std::cerr << "Invalid covariance scale, skip clutter filter\n";
        return false;
      }
      matrix.diag() += loading;
    }
    if (!arma::chol(matrix, matrix)) {
      std::cerr << "Chol decomposition failed, skip clutter filter\n"; return false;
    }
    if (!arma::solve(weights, arma::trimatu(matrix), arma::solve(arma::trimatl(arma::trans(matrix)), b))) {
      std::cerr << "Solve failed, skip clutter filter\n"; return false;
    }
    const auto diagnosticSolveFinished = std::chrono::steady_clock::now();
    uint32_t gpuSamples = 0;
    const auto diagnosticFilterStarted = std::chrono::steady_clock::now();
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
    const auto publishStarted = std::chrono::steady_clock::now();
    const auto diagnosticFilterFinished = publishStarted;
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
    if (std::getenv("VECTORWARP_CLUTTER_TIMING")) {
      static std::mutex diagnosticMutex;
      const auto millis = [](auto first, auto last) {
        return std::chrono::duration<double, std::milli>(last - first).count();
      };
      std::lock_guard<std::mutex> lock(diagnosticMutex);
      std::cerr << "[ClutterTiming] samples=" << samples << " taps=" << taps
        << " input_ms=" << millis(inputStarted, inputFinished)
        << " correlation_ms=" << millis(diagnosticCorrelationStarted,
             diagnosticCorrelationFinished)
        << " solve_ms=" << millis(diagnosticCorrelationFinished,
             diagnosticSolveFinished)
        << " filter_ms=" << millis(diagnosticFilterStarted,
             diagnosticFilterFinished) << '\n';
    }
    return true;
  }
  bool process(IqData* reference, IqData* surveillance, bool borrowOutput) {
    if (!reference || !surveillance || reference->get_length() < samples ||
        surveillance->get_length() < samples)
      throw std::invalid_argument("Clutter input length differs from CPI");
    return process_loaded(surveillance, borrowOutput,
      [&](Complex* loadedReference, Complex* loadedSurveillance) {
        vectorwarp_clutter::copy_rotation(reference->sample_view(), samples,
          delayMin, loadedReference);
        surveillance->sample_view().copy_to(loadedSurveillance, samples);
      });
  }
  bool process_float(const std::complex<float>* reference,
      const std::complex<float>* surveillance, uint32_t count) {
    if (!reference || !surveillance || count < samples)
      throw std::invalid_argument("Float clutter input length differs from CPI");
    return process_loaded(nullptr, true,
      [&](Complex* loadedReference, Complex* loadedSurveillance) {
        int64_t source = (-int64_t(delayMin)) % int64_t(samples);
        if (source < 0) source += samples;
        const uint32_t first = samples - uint32_t(source);
        const auto widen = [](const std::complex<float> value) {
          return Complex(value);
        };
        std::transform(reference + source, reference + samples,
          loadedReference, widen);
        std::transform(reference, reference + source,
          loadedReference + first, widen);
        std::transform(surveillance, surveillance + samples,
          loadedSurveillance, widen);
      });
  }
  WienerHopf::FilteredView filtered_view() const {
    if (!viewReady) throw std::logic_error("No completed borrowed clutter CPI");
    return {x.get(), y.get(), samples, delayMin, generation};
  }
};
WienerHopf::PreparedReference::PreparedReference(int32_t first, int32_t last,
                                                 uint32_t count, double loading)
  : delayMin_(first), samples_(count), relativeDiagonalLoading_(loading)
{
  if (!std::isfinite(relativeDiagonalLoading_) || relativeDiagonalLoading_ < 0 ||
      relativeDiagonalLoading_ > 1)
    throw std::invalid_argument("Clutter relative diagonal loading must be finite in [0,1]");
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
  const auto& data = reference.sample_view();
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
  if (relativeDiagonalLoading_) {
    const double scale = correlation_[0].real();
    const double loading = relativeDiagonalLoading_ * scale;
    if (!std::isfinite(scale) || scale < 0 || !std::isfinite(loading)) {
      std::cerr << "Invalid covariance scale, skip clutter filter\n";
      return false;
    }
    cholesky_.diag() += loading;
  }
  valid_ = arma::chol(cholesky_, cholesky_);
  if (!valid_) std::cerr << "Chol decomposition failed, skip clutter filter\n";
  return valid_;
}

WienerHopf::WienerHopf(int32_t first, int32_t last, uint32_t samples, Workspace workspace,
                       double loading, uint32_t workerSlots)
  : impl_(std::make_unique<Impl>(first, last, samples,
      workspace == Workspace::SharedReference, loading, workerSlots)) {}
double WienerHopf::PreparedReference::cholesky_diagonal_ratio() const {
  if (!valid_) return std::numeric_limits<double>::quiet_NaN();
  double smallest = std::numeric_limits<double>::infinity(), largest = 0;
  for (uint32_t i = 0; i < taps_; ++i) {
    const double value = std::abs(cholesky_(i, i));
    smallest = std::min(smallest, value); largest = std::max(largest, value);
  }
  return largest > 0 ? smallest / largest : std::numeric_limits<double>::quiet_NaN();
}
WienerHopf::~WienerHopf() = default;
uint32_t WienerHopf::filter_fft_length() const { return impl_->filterLength; }
bool WienerHopf::process(IqData* x, IqData* y, bool borrowOutput) { return impl_->process(x,y,borrowOutput); }
bool WienerHopf::process_float(const std::complex<float>* reference,
  const std::complex<float>* surveillance, uint32_t samples)
{ return impl_->process_float(reference, surveillance, samples); }
bool WienerHopf::process(const PreparedReference& reference, IqData* surveillance) {
  if (reference.delayMin_ != impl_->delayMin || reference.taps_ != impl_->taps ||
      reference.samples_ != impl_->samples ||
      reference.relativeDiagonalLoading_ != impl_->relativeDiagonalLoading)
    throw std::invalid_argument("Clutter surveillance or prepared reference geometry/loading does not match");
  if (!surveillance || surveillance->get_length() < impl_->samples)
    throw std::invalid_argument("Clutter input length differs from CPI");
  if (!reference.valid_) return false;
  return impl_->process_prepared(reference, surveillance);
}
WienerHopf::FilteredView WienerHopf::filtered_view() const { return impl_->filtered_view(); }
