#include "WienerHopf.h"
#include "BulkRotation.h"
#include "CorrelationWorker.h"
#include "process/meta/FftLength.h"
#include <armadillo>
#include <fftw3.h>
#include <algorithm>
#include <array>
#include <iostream>
#include <limits>
#include <mutex>
#include <stdexcept>

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
}

// Blocked correlation and FIR follow the CPU algorithms contributed in
// offworldlabs/blah2-arm PR67. Keep dense FP64 Cholesky and VectorWarp's thread
// budget; this does not import its V3D backend or experimental fast solver.
struct WienerHopf::Impl {
  int32_t delayMin;
  uint32_t samples, taps, correlationLength, filterLength, lanes;
  bool blockedCorrelation, blockedFilter;
  arma::cx_mat matrix;
  arma::cx_vec a, b, weights;
  Buffer x, y, outX, outY, fullA, fullB, correlation, correlationA, correlationB;
  Buffer filterX, filterW, filterOut;
  // Declared after buffers so plan destruction precedes storage release.
  std::array<Plan, 4> fullPlans;
  std::array<Plan, 3> correlationPlans, filterPlans;

  Impl(int32_t first, int32_t last, uint32_t count) : delayMin(first), samples(count) {
    const int64_t width = int64_t(last) - first;
    if (!count || count > uint32_t(INT32_MAX) || width <= 0 || uint64_t(width) > count)
      throw std::invalid_argument("Clutter filter needs a non-empty half-open delay range no longer than the CPI");
    taps = uint32_t(width);
    correlationLength = blockLength(uint64_t(taps) * 2, 4096);
    const uint32_t filterBlock = blockLength(uint64_t(taps) * 2, 1024);
    blockedCorrelation = uint64_t(count) >= uint64_t(correlationLength) * 4;
    blockedFilter = uint64_t(count) >= uint64_t(filterBlock) * 4;
    filterLength = blockedFilter ? filterBlock : blah2::nextFastFftLength(uint64_t(count) + taps + 1);
    lanes = blockedFilter ? 4 : 1;
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
    try {
      if (blockedCorrelation) {
        fftw_plan_with_nthreads(1);
        int length = int(correlationLength);
        correlationPlans[0] = checked(fftw_plan_many_dft(1, &length, 3,
          fftData(correlation), nullptr, 1, length, fftData(correlation), nullptr, 1, length,
          FFTW_FORWARD, FFTW_ESTIMATE));
        correlationPlans[1] = checked(fftw_plan_dft_1d(length, fftData(correlationA), fftData(correlationA), FFTW_BACKWARD, FFTW_ESTIMATE));
        correlationPlans[2] = checked(fftw_plan_dft_1d(length, fftData(correlationB), fftData(correlationB), FFTW_BACKWARD, FFTW_ESTIMATE));
      } else {
        fullPlans[0] = checked(fftw_plan_dft_1d(count, fftData(x), fftData(outX), FFTW_FORWARD, FFTW_ESTIMATE));
        fullPlans[1] = checked(fftw_plan_dft_1d(count, fftData(y), fftData(outY), FFTW_FORWARD, FFTW_ESTIMATE));
        fullPlans[2] = checked(fftw_plan_dft_1d(count, fftData(fullA), fftData(fullA), FFTW_BACKWARD, FFTW_ESTIMATE));
        fullPlans[3] = checked(fftw_plan_dft_1d(count, fftData(fullB), fftData(fullB), FFTW_BACKWARD, FFTW_ESTIMATE));
      }
      fftw_plan_with_nthreads(blockedFilter ? 1 : restore.previous);
      int length = int(filterLength);
      filterPlans[0] = checked(fftw_plan_many_dft(1, &length, lanes,
        fftData(filterX), nullptr, 1, length, fftData(filterX), nullptr, 1, length, FFTW_FORWARD, FFTW_ESTIMATE));
      filterPlans[1] = checked(fftw_plan_dft_1d(length, fftData(filterW), fftData(filterW), FFTW_FORWARD, FFTW_ESTIMATE));
      filterPlans[2] = checked(fftw_plan_many_dft(1, &length, lanes,
        fftData(filterOut), nullptr, 1, length, fftData(filterOut), nullptr, 1, length, FFTW_BACKWARD, FFTW_ESTIMATE));
    } catch (...) {
      // Exceptional member cleanup must also serialize FFTW plan destruction.
      for (auto& p : fullPlans) p.reset();
      for (auto& p : correlationPlans) p.reset();
      for (auto& p : filterPlans) p.reset();
      throw;
    }
  }
  ~Impl() {
    std::lock_guard<std::mutex> lock(plannerMutex());
    for (auto& p : fullPlans) p.reset();
    for (auto& p : correlationPlans) p.reset();
    for (auto& p : filterPlans) p.reset();
  }
  bool process(IqData* reference, IqData* surveillance) {
    if (!reference || !surveillance || reference->get_length() != samples || surveillance->get_length() != samples)
      throw std::invalid_argument("Clutter input length differs from CPI");
    vectorwarp_clutter::copy_rotation(reference->view_data(), samples, delayMin, x.get());
    std::copy_n(surveillance->view_data().begin(), samples, y.get());
    if (blockedCorrelation) {
      std::fill_n(correlationA.get(), correlationLength, Complex{});
      std::fill_n(correlationB.get(), correlationLength, Complex{});
      const uint32_t hop = correlationLength - taps + 1;
      for (uint64_t begin = 0; begin < samples; begin += hop) {
        vectorwarp_clutter::correlation_block(x.get(), y.get(), samples, taps,
          correlationLength, begin, correlationPlans[0].get(), correlation.get());
        vectorwarp_clutter::accumulate_correlation(correlation.get(), correlationLength, correlationA.get(), correlationB.get());
      }
      fftw_execute(correlationPlans[1].get()); fftw_execute(correlationPlans[2].get());
      for (uint32_t i = 0; i < taps; ++i) {
        a[i] = std::conj(correlationA.get()[i]) / double(correlationLength);
        b[i] = correlationB.get()[i] / double(correlationLength);
      }
    } else {
      fftw_execute(fullPlans[0].get()); fftw_execute(fullPlans[1].get());
      for (uint32_t i = 0; i < samples; ++i) {
        fullA.get()[i] = outX.get()[i] * std::conj(outX.get()[i]);
        fullB.get()[i] = outY.get()[i] * std::conj(outX.get()[i]);
      }
      fftw_execute(fullPlans[2].get()); fftw_execute(fullPlans[3].get());
      for (uint32_t i = 0; i < taps; ++i) {
        a[i] = std::conj(fullA.get()[i]) / double(samples);
        b[i] = fullB.get()[i] / double(samples);
      }
    }
    // Zero-lag autocorrelation is sum(norm(x)), hence exactly real. The
    // blocked FFT reduction can leave a small imaginary rounding residue at
    // raw ADC amplitudes; it must not become an imaginary matrix diagonal.
    a[0] = Complex(a[0].real(), 0);
    matrix = arma::toeplitz(a);
    for (uint32_t row = 0; row < taps; ++row)
      for (uint32_t col = 0; col < row; ++col) matrix(row,col) = std::conj(matrix(row,col));
    if (!arma::chol(matrix, matrix)) {
      std::cerr << "Chol decomposition failed, skip clutter filter\n"; return false;
    }
    if (!arma::solve(weights, arma::trimatu(matrix), arma::solve(arma::trimatl(arma::trans(matrix)), b))) {
      std::cerr << "Solve failed, skip clutter filter\n"; return false;
    }
    std::copy_n(weights.memptr(), taps, filterW.get());
    std::fill(filterW.get() + taps, filterW.get() + filterLength, Complex{});
    fftw_execute(filterPlans[1].get());
    const uint32_t hop = blockedFilter ? filterLength - taps + 1 : samples;
    for (uint64_t base = 0; base < samples; base += uint64_t(lanes) * hop) {
      for (uint32_t lane = 0; lane < lanes; ++lane) {
        Complex* slot = filterX.get() + uint64_t(lane) * filterLength;
        const int64_t first = int64_t(base) + int64_t(lane) * hop - (blockedFilter ? taps - 1 : 0);
        const int64_t lower = std::max<int64_t>(0, first);
        const int64_t upper = std::min<int64_t>(samples, first + filterLength);
        if (lower >= upper) { std::fill_n(slot, filterLength, Complex{}); continue; }
        const size_t left = lower - first, count = upper - lower;
        std::fill_n(slot, left, Complex{});
        std::copy(x.get() + lower, x.get() + upper, slot + left);
        std::fill(slot + left + count, slot + filterLength, Complex{});
      }
      fftw_execute(filterPlans[0].get());
      for (uint32_t lane = 0; lane < lanes; ++lane)
        for (uint32_t i = 0; i < filterLength; ++i) {
          const uint64_t at = uint64_t(lane) * filterLength + i;
          filterOut.get()[at] = filterX.get()[at] * filterW.get()[i];
        }
      fftw_execute(filterPlans[2].get());
      for (uint32_t lane = 0; lane < lanes; ++lane) {
        const uint64_t begin = base + uint64_t(lane) * hop;
        if (begin >= samples) break;
        const uint32_t count = std::min<uint64_t>(hop, samples - begin);
        const Complex* valid = filterOut.get() + uint64_t(lane) * filterLength + (blockedFilter ? taps - 1 : 0);
        for (uint32_t i = 0; i < count; ++i) y.get()[begin+i] -= valid[i] / double(filterLength);
      }
    }
    surveillance->replace(std::deque<Complex>(y.get(), y.get() + samples));
    return true;
  }
};
WienerHopf::WienerHopf(int32_t first, int32_t last, uint32_t samples) : impl_(std::make_unique<Impl>(first,last,samples)) {}
WienerHopf::~WienerHopf() = default;
uint32_t WienerHopf::filter_fft_length() const { return impl_->filterLength; }
bool WienerHopf::process(IqData* x, IqData* y) { return impl_->process(x,y); }
