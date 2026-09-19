#include "process/clutter/WienerHopf.h"
#include "process/clutter/CorrelationWorker.h"
#include "process/meta/FftLength.h"
#include <armadillo>
#include <fftw3.h>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>
#include <thread>

using Complex = std::complex<double>;

struct FftwBufferFree {
  void operator()(Complex* value) const { fftw_free(value); }
};
using FftwBuffer = std::unique_ptr<Complex, FftwBufferFree>;

static void require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

class ScopedPlanMode {
  const char* previous_;
  std::string saved_;
public:
  explicit ScopedPlanMode(const char* value) : previous_(std::getenv("VECTORWARP_FFTW_PLAN")) {
    if (previous_) saved_ = previous_;
    setenv("VECTORWARP_FFTW_PLAN", value, 1);
  }
  ~ScopedPlanMode() {
    if (previous_) setenv("VECTORWARP_FFTW_PLAN", saved_.c_str(), 1);
    else unsetenv("VECTORWARP_FFTW_PLAN");
  }
};

class ScopedClutterWorkers {
  const char* previous_;
  std::string saved_;
public:
  explicit ScopedClutterWorkers(const char* value)
      : previous_(std::getenv("VECTORWARP_CLUTTER_WORKERS")) {
    if (previous_) saved_ = previous_;
    setenv("VECTORWARP_CLUTTER_WORKERS", value, 1);
  }
  ~ScopedClutterWorkers() {
    if (previous_) setenv("VECTORWARP_CLUTTER_WORKERS", saved_.c_str(), 1);
    else unsetenv("VECTORWARP_CLUTTER_WORKERS");
  }
};

static bool fast(uint32_t value) {
  for (uint32_t factor : {2u, 3u, 5u, 7u})
    while (value % factor == 0) value /= factor;
  return value == 1 || value == 11 || value == 13;
}

static void require_exact(const Complex* expected, const Complex* actual,
                          uint64_t count, const char* message) {
  for (uint64_t i = 0; i < count; ++i)
    if (expected[i] != actual[i]) throw std::runtime_error(message);
}

static void require_accumulation_close(const std::vector<Complex>& expected,
                                       const std::vector<Complex>& actual) {
  require(expected.size() == actual.size(), "Correlation output size changed");
  for (size_t i = 0; i < expected.size(); ++i) {
    const double bound = 8 * std::numeric_limits<double>::epsilon() *
      std::max(1.0, std::abs(expected[i]));
    require(std::abs(expected[i] - actual[i]) <= bound,
      "Persistent correlation worker exceeded machine-epsilon accumulation error");
  }
}

static void test_correlation_worker_fft(uint32_t samples, uint32_t taps,
                                        uint32_t length) {
  const uint32_t hop = length - taps + 1;
  std::vector<Complex> x(samples), y(samples);
  for (uint32_t i = 0; i < samples; ++i) {
    x[i] = Complex(i * .125, -double(i % 5));
    y[i] = Complex(double(i % 7), i * .25);
  }
  FftwBuffer planned(static_cast<Complex*>(fftw_malloc(sizeof(Complex) * length * 3)));
  FftwBuffer expectedCaller(static_cast<Complex*>(fftw_malloc(sizeof(Complex) * length * 3)));
  FftwBuffer expectedWorker(static_cast<Complex*>(fftw_malloc(sizeof(Complex) * length * 3)));
  require(planned && expectedCaller && expectedWorker,
    "Could not allocate correlation FFT workspace");
  const int threadsBeforePlan = fftw_planner_nthreads();
  fftw_plan_with_nthreads(1);
  int n = int(length);
  fftw_plan plan = fftw_plan_many_dft(1, &n, 3,
    reinterpret_cast<fftw_complex*>(planned.get()), nullptr, 1, length,
    reinterpret_cast<fftw_complex*>(planned.get()), nullptr, 1, length,
    FFTW_FORWARD, FFTW_ESTIMATE);
  fftw_plan_with_nthreads(threadsBeforePlan);
  require(plan != nullptr, "Could not plan correlation FFT");
  try {
    vectorwarp_clutter::CorrelationWorker worker(length, planned.get());
    vectorwarp_clutter::correlation_block(x.data(), y.data(), samples, taps,
      length, hop, plan, expectedWorker.get());
    vectorwarp_clutter::correlation_block(x.data(), y.data(), samples, taps,
      length, 0, plan, expectedCaller.get());
    worker.start({x.data(), y.data(), samples, taps, length, hop, plan});
    vectorwarp_clutter::correlation_block(x.data(), y.data(), samples, taps,
      length, 0, plan, planned.get());
    require_exact(expectedCaller.get(), planned.get(), uint64_t(length) * 3,
      "Concurrent caller FFT differs from serial FFT");
    require_exact(expectedWorker.get(), worker.finish(), uint64_t(length) * 3,
      "Concurrent worker FFT differs from serial FFT");
    const uint64_t tail = ((samples - 1) / hop) * uint64_t(hop);
    vectorwarp_clutter::correlation_block(x.data(), y.data(), samples, taps,
      length, tail, plan, expectedCaller.get());
    vectorwarp_clutter::correlation_block(x.data(), y.data(), samples, taps,
      length, tail, plan, planned.get());
    require_exact(expectedCaller.get(), planned.get(), uint64_t(length) * 3,
      "Partial correlation FFT differs from serial FFT");
  } catch (...) {
    fftw_destroy_plan(plan);
    throw;
  }
  fftw_destroy_plan(plan);
}

// Compare the persistent-worker path with the serial blocked transform on a
// tail block.  The worker may overlap FFTs, but additions must remain ordered.
static void test_correlation_worker() {
  constexpr uint32_t samples = 35, taps = 8, length = 16;
  constexpr uint32_t hop = length - taps + 1;
  std::vector<Complex> x(samples), y(samples);
  std::vector<Complex> serialA(length), serialB(length), threadedA(length),
      threadedB(length);
  for (uint32_t i = 0; i < samples; ++i) {
    x[i] = Complex(i * .125, -double(i % 5));
    y[i] = Complex(double(i % 7), i * .25);
  }
  FftwBuffer planned(static_cast<Complex*>(
      fftw_malloc(sizeof(Complex) * length * 3)));
  FftwBuffer serialBuffer(static_cast<Complex*>(
      fftw_malloc(sizeof(Complex) * length * 3)));
  require(planned && serialBuffer, "Could not allocate correlation FFT workspace");
  int n = length;
  // Production creates this blocked plan with one FFTW thread, then restores
  // the caller's global budget before the worker executes it.
  const int threadsBeforePlan = fftw_planner_nthreads();
  fftw_plan_with_nthreads(1);
  fftw_plan plan = fftw_plan_many_dft(1, &n, 3,
    reinterpret_cast<fftw_complex*>(planned.get()), nullptr, 1, length,
    reinterpret_cast<fftw_complex*>(planned.get()), nullptr, 1, length,
    FFTW_FORWARD, FFTW_ESTIMATE);
  fftw_plan_with_nthreads(threadsBeforePlan);
  require(plan != nullptr, "Could not plan correlation FFT");
  try {
    for (uint64_t begin = 0; begin < samples; begin += hop) {
      vectorwarp_clutter::correlation_block(x.data(), y.data(), samples, taps,
        length, begin, plan, serialBuffer.get());
      vectorwarp_clutter::accumulate_correlation(serialBuffer.get(), length,
        serialA.data(), serialB.data());
    }
    std::vector<std::unique_ptr<vectorwarp_clutter::CorrelationWorker>> workers;
    workers.push_back(std::make_unique<vectorwarp_clutter::CorrelationWorker>(
      length, planned.get()));
    vectorwarp_clutter::correlate(workers, x.data(), y.data(), samples, taps,
      length, plan, planned.get(), threadedA.data(), threadedB.data());
    // ARM GCC may fuse the inlined complex accumulation differently across
    // call sites. FFT blocks are exact (tested separately); retain a strict
    // machine-epsilon bound for their ordered accumulation.
    require_accumulation_close(serialA, threadedA);
    require_accumulation_close(serialB, threadedB);
    bool rejected = false;
    try { workers[0]->start({x.data(), y.data(), samples, taps, length - 1, 0, plan}); }
    catch (const std::invalid_argument&) { rejected = true; }
    require(rejected, "Correlation worker accepted a mismatched length");
  } catch (...) {
    fftw_destroy_plan(plan);
    throw;
  }
  fftw_destroy_plan(plan);
}

static void test_filter_worker() {
  constexpr uint32_t samples = 47, taps = 8, length = 16, lanes = 4;
  constexpr uint32_t hop = length - taps + 1;
  constexpr uint64_t batch = uint64_t(lanes) * hop;
  std::vector<Complex> x(samples), weights(length), expectedY(samples), actualY(samples);
  for (uint32_t i = 0; i < samples; ++i) {
    x[i] = Complex(i * .25, -double(i % 3));
    expectedY[i] = actualY[i] = Complex(double(i % 5), i * .125);
  }
  for (uint32_t i = 0; i < length; ++i)
    weights[i] = Complex(.25 + i * .01, -.5 + i * .02);
  const uint64_t values = uint64_t(length) * lanes;
  FftwBuffer correlation(static_cast<Complex*>(fftw_malloc(sizeof(Complex) * length * 3)));
  FftwBuffer plannedInput(static_cast<Complex*>(fftw_malloc(sizeof(Complex) * values)));
  FftwBuffer plannedOutput(static_cast<Complex*>(fftw_malloc(sizeof(Complex) * values)));
  FftwBuffer serialInput(static_cast<Complex*>(fftw_malloc(sizeof(Complex) * values)));
  FftwBuffer serialOutput(static_cast<Complex*>(fftw_malloc(sizeof(Complex) * values)));
  FftwBuffer expectedInput(static_cast<Complex*>(fftw_malloc(sizeof(Complex) * values)));
  FftwBuffer expectedOutput(static_cast<Complex*>(fftw_malloc(sizeof(Complex) * values)));
  require(correlation && plannedInput && plannedOutput && serialInput && serialOutput &&
    expectedInput && expectedOutput, "Could not allocate filter FFT workspace");
  const int threadsBeforePlan = fftw_planner_nthreads();
  fftw_plan_with_nthreads(1);
  int n = length;
  fftw_plan forward = fftw_plan_many_dft(1, &n, lanes,
    reinterpret_cast<fftw_complex*>(plannedInput.get()), nullptr, 1, length,
    reinterpret_cast<fftw_complex*>(plannedInput.get()), nullptr, 1, length,
    FFTW_FORWARD, FFTW_ESTIMATE);
  fftw_plan inverse = fftw_plan_many_dft(1, &n, lanes,
    reinterpret_cast<fftw_complex*>(plannedOutput.get()), nullptr, 1, length,
    reinterpret_cast<fftw_complex*>(plannedOutput.get()), nullptr, 1, length,
    FFTW_BACKWARD, FFTW_ESTIMATE);
  fftw_plan_with_nthreads(threadsBeforePlan);
  require(forward && inverse, "Could not plan filter FFT");
  try {
    const vectorwarp_clutter::FilterJob first{x.data(), weights.data(), samples,
      taps, length, lanes, 0, forward, inverse};
    const vectorwarp_clutter::FilterJob tail{x.data(), weights.data(), samples,
      taps, length, lanes, batch, forward, inverse};
    vectorwarp_clutter::filter_block(first, serialInput.get(), serialOutput.get());
    vectorwarp_clutter::filter_block(tail, expectedInput.get(), expectedOutput.get());
    vectorwarp_clutter::CorrelationWorker worker(length, correlation.get(), length,
      lanes, plannedInput.get(), plannedOutput.get());
    worker.start(tail);
    vectorwarp_clutter::filter_block(first, plannedInput.get(), plannedOutput.get());
    require_exact(serialOutput.get(), plannedOutput.get(), values,
      "Concurrent caller FIR differs from serial FIR");
    const Complex* workerOutput = worker.finish();
    require_exact(expectedOutput.get(), workerOutput, values,
      "Concurrent worker FIR differs from serial FIR");
    vectorwarp_clutter::subtract_filter_block(first, serialOutput.get(), expectedY.data());
    vectorwarp_clutter::subtract_filter_block(tail, expectedOutput.get(), expectedY.data());
    vectorwarp_clutter::subtract_filter_block(first, plannedOutput.get(), actualY.data());
    vectorwarp_clutter::subtract_filter_block(tail, workerOutput, actualY.data());
    require_exact(expectedY.data(), actualY.data(), samples,
      "Concurrent FIR changed ordered partial-tail output");
  } catch (...) {
    fftw_destroy_plan(forward);
    fftw_destroy_plan(inverse);
    throw;
  }
  fftw_destroy_plan(forward);
  fftw_destroy_plan(inverse);
}

static void run(unsigned samples, int first, int last, double scale = 1) {
  const unsigned taps = last - first;
  IqData reference(samples + 3), surveillance(samples + 3);
  const int threadsBefore = fftw_planner_nthreads();
  WienerHopf filter(first, last, samples);
  require(fftw_planner_nthreads() == threadsBefore, "Clutter changed the global FFT thread budget");
  uint32_t block = 1024;
  while (block < uint64_t(taps) * 2) block *= 2;
  const uint32_t expectedLength = samples >= uint64_t(block) * 4 ? block :
    blah2::nextFastFftLength(samples + taps + 1);
  require(filter.filter_fft_length() == expectedLength,
    "Filter did not use the selected padded length");
  std::mt19937 rng(9211);
  std::normal_distribution<double> random;
  for (int repeat = 0; repeat < 3; ++repeat) {
    reference.clear();
    surveillance.clear();
    arma::cx_vec x(samples), y(samples);
    for (unsigned i = 0; i < samples; ++i)
      reference.push_back({scale * random(rng), scale * random(rng)});
    const auto originalReference = reference.view_data();
    for (unsigned i = 0; i < samples; ++i) {
      const int64_t shifted = (int64_t(i) - first) % samples;
      x[i] = originalReference[shifted < 0 ? shifted + samples : shifted];
      y[i] = x[i] * Complex(.7, .2) + scale * Complex(.2 * random(rng), .2 * random(rng));
      surveillance.push_back(y[i]);
    }
    // The old filter processes the first CPI even if capture has already
    // appended a tail. That tail remains on reference and is trimmed on y.
    for (unsigned i = 0; i < 3; ++i) {
      reference.push_back({100. + i, -20.});
      surveillance.push_back({-300. - i, 90.});
    }
    const auto fullReference = reference.view_data();

    // Independent circular correlations and direct linear convolution, not
    // another FFT implementation. Reuse the filter on three different CPIs.
    arma::cx_vec a(taps, arma::fill::zeros), b(taps, arma::fill::zeros);
    for (unsigned lag = 0; lag < taps; ++lag)
      for (unsigned i = 0; i < samples; ++i) {
        a[lag] += std::conj(x[(i + lag) % samples]) * x[i];
        b[lag] += y[(i + lag) % samples] * std::conj(x[i]);
      }
    arma::cx_mat matrix = arma::toeplitz(a);
    for (unsigned row = 0; row < taps; ++row)
      for (unsigned col = 0; col < row; ++col)
        matrix(row, col) = std::conj(matrix(row, col));
    const arma::cx_vec weights = arma::solve(matrix, b);
    require(filter.process(&reference, &surveillance), "Full-rank fixture rejected");
    require(surveillance.get_length() == samples, "Output sample count changed");
    for (unsigned i = 0; i < samples; ++i) {
      Complex expected = y[i];
      for (unsigned tap = 0; tap < taps && tap <= i; ++tap)
        expected -= weights[tap] * x[i - tap];
      require(std::abs(expected - surveillance.view_data()[i]) < scale * 1e-9,
        "Padded clutter differs from direct convolution");
    }
    require(reference.view_data() == fullReference, "Reference or its tail mutated");
  }
  std::cout << "PASS samples=" << samples << " taps=" << taps
            << " first=" << first << " fft=" << filter.filter_fft_length() << '\n';
}

static void shared_reference(unsigned samples) {
  const unsigned paths = 5;
  IqData reference(samples);
  WienerHopf::PreparedReference prepared(-3, 5, samples);
  std::vector<std::unique_ptr<WienerHopf>> filters;
  for (unsigned path = 0; path < paths; ++path)
    filters.push_back(std::make_unique<WienerHopf>(-3, 5, samples,
      WienerHopf::Workspace::SharedReference));
  std::mt19937 rng(713);
  std::normal_distribution<double> random;
  for (unsigned frame = 0; frame < 5; ++frame) {
    reference.clear();
    for (unsigned i = 0; i < samples; ++i)
      reference.push_back({random(rng), random(rng)});
    require(prepared.prepare(reference), "Changing shared reference rejected");
    std::vector<std::unique_ptr<IqData>> observed, control;
    for (unsigned path = 0; path < paths; ++path) {
      observed.push_back(std::make_unique<IqData>(samples));
      control.push_back(std::make_unique<IqData>(samples));
      for (unsigned i = 0; i < samples; ++i) {
        const Complex value(random(rng) + .3 * path, random(rng));
        observed.back()->push_back(value);
        control.back()->push_back(value);
      }
    }
    std::vector<uint8_t> accepted(paths);
    std::vector<std::thread> workers;
    for (unsigned path = 0; path < paths; ++path)
      workers.emplace_back([&, path] {
        accepted[path] = filters[path]->process(prepared, observed[path].get());
      });
    for (auto& worker : workers) worker.join();
    for (unsigned path = 0; path < paths; ++path) {
      WienerHopf independent(-3, 5, samples);
      require(accepted[path] && independent.process(&reference, control[path].get()),
        "Shared reference path rejected a full-rank fixture");
      for (unsigned i = 0; i < samples; ++i)
        require(std::abs(observed[path]->view_data()[i] -
                         control[path]->view_data()[i]) < 1e-9,
          "Shared reference changed the filter output");
    }
  }
  IqData singular(samples), unchanged(samples), shortInput(samples);
  for (unsigned i = 0; i < samples; ++i) {
    singular.push_back({0, 0}); unchanged.push_back({1, 0});
  }
  const auto original = unchanged.view_data();
  require(!prepared.prepare(singular) &&
          !filters.front()->process(prepared, &unchanged) &&
          unchanged.view_data() == original,
    "A failed shared preparation published surveillance data");
  bool rejected = false;
  try { prepared.prepare(shortInput); }
  catch (const std::invalid_argument&) { rejected = true; }
  require(rejected && !filters.front()->process(prepared, &unchanged),
    "Invalid preparation retained the previous reference");
  rejected = false;
  try { filters.front()->process(prepared, &shortInput); }
  catch (const std::invalid_argument&) { rejected = true; }
  require(rejected, "Short surveillance was accepted");
  WienerHopf wrongGeometry(-2, 6, samples);
  rejected = false;
  try { wrongGeometry.process(prepared, &unchanged); }
  catch (const std::invalid_argument&) { rejected = true; }
  require(rejected, "Mismatched prepared-reference shift was accepted");
  reference.clear();
  for (unsigned i = 0; i < samples; ++i)
    reference.push_back({random(rng), random(rng)});
  require(prepared.prepare(reference) && filters.front()->process(prepared, &unchanged),
    "Shared reference did not recover after failed preparation");
  rejected = false;
  WienerHopf compatibility(-3, 5, samples);
  try { compatibility.process(&shortInput, &unchanged); }
  catch (const std::invalid_argument&) { rejected = true; }
  require(rejected, "Compatibility filter accepted a short reference");

  IqData overReference(samples + 2), overSurveillance(samples + 2);
  for (unsigned i = 0; i < samples + 2; ++i) {
    overReference.push_back({random(rng), random(rng)});
    overSurveillance.push_back({random(rng), random(rng)});
  }
  const auto completeReference = overReference.view_data();
  require(prepared.prepare(overReference) &&
          filters.front()->process(prepared, &overSurveillance) &&
          overSurveillance.get_length() == samples &&
          overReference.view_data() == completeReference,
    "Shared filter did not consume only the overlong input prefix");
}

int main() {
  try {
    require(fftw_init_threads() != 0, "FFTW thread initialization failed");
    fftw_plan_with_nthreads(2);
    {
      ScopedPlanMode invalid("invalid");
      bool rejected = false;
      try { WienerHopf filter(0, 8, 64); }
      catch (const std::invalid_argument&) { rejected = true; }
      require(rejected, "Invalid FFTW plan mode accepted");
    }
    {
      ScopedClutterWorkers invalid("3");
      bool rejected = false;
      try { WienerHopf filter(0, 8, 64); }
      catch (const std::invalid_argument&) { rejected = true; }
      require(rejected, "Invalid clutter worker count accepted");
    }
    {
      ScopedPlanMode measure("measure");
      run(64, -3, 5);
    }
    test_correlation_worker_fft(35, 8, 16);
    test_correlation_worker_fft(20000, 410, 4096);
    test_correlation_worker();
    test_filter_worker();
    // Independently scan a bounded range to check both admissibility and
    // minimality, including lengths containing repeated factors of 11/13.
    for (uint32_t minimum = 1; minimum <= 4096; ++minimum) {
      uint32_t expected = minimum;
      while (!fast(expected)) ++expected;
      require(blah2::nextFastFftLength(minimum) == expected,
        "Selected FFT length is not the smallest allowed size");
    }
    require(blah2::nextFastFftLength(480211) == 481140,
      "200 ms convolution geometry changed");
    require(blah2::nextFastFftLength(1200211) == 1200500,
      "500 ms convolution geometry changed");
    for (uint64_t invalid : {uint64_t(0), uint64_t(INT32_MAX),
                             uint64_t(INT32_MAX) + 1, UINT64_MAX}) {
      bool rejected = false;
      try { (void)blah2::nextFastFftLength(invalid); }
      catch (const std::invalid_argument&) { rejected = true; }
      require(rejected, "Unrepresentable FFT geometry accepted");
    }
    for (unsigned samples : {64u, 127u, 257u})
      for (int first : {-3, 0, 2}) run(samples, first, first + 8);
    run(128, -2, -1);
    run(31, -2, 2); // Already-fast convolution length (36).
    run(32, 0, 32); // Tap count equal to CPI sample count.
    run(5000, 3, 34); // Bounded overlap-save FIR, including a partial tail.
    run(16384, 3, 34); // Exact blocked-correlation threshold.
    run(20000, -3, 28); // Bounded circular correlation and FIR partial blocks.
    run(20000, -10, 400, 8192); // Realistic ADC units and the Pi's tap count.
    // Requested slots cap at the caller FFT budget; each run reuses workers
    // over three partial-tail CPIs and checks the direct convolution oracle.
    fftw_plan_with_nthreads(4);
    { ScopedClutterWorkers one("1"); run(20000, -10, 400, 8192); }
    fftw_plan_with_nthreads(1);
    { ScopedClutterWorkers four("4"); run(20000, -10, 400, 8192); }
    require(fftw_planner_nthreads() == 1, "Serial clutter changed FFT budget");
    fftw_plan_with_nthreads(2);
    { ScopedClutterWorkers four("4"); run(20000, -10, 400, 8192); }
    require(fftw_planner_nthreads() == 2, "Threaded clutter changed FFT budget");
    fftw_plan_with_nthreads(4);
    { ScopedClutterWorkers four("4"); run(20000, -10, 400, 8192); }
    require(fftw_planner_nthreads() == 4, "Four-slot clutter changed FFT budget");
    shared_reference(127);
    // 16384 is the smallest geometry selecting the Pi blocked-correlation
    // path. Prepared references must still use their cached full CPI data.
    shared_reference(16384);
    for (const auto& bounds : {std::pair<int, int>{0, 0}, {5, 2}, {0, 65},
                              {INT32_MIN, INT32_MAX}}) {
      bool rejected = false;
      try { WienerHopf invalid(bounds.first, bounds.second, 64); }
      catch (const std::invalid_argument&) { rejected = true; }
      require(rejected, "Invalid clutter tap range accepted");
    }
    bool rejected = false;
    try { WienerHopf invalid(0, 1, 0); }
    catch (const std::invalid_argument&) { rejected = true; }
    require(rejected, "Empty CPI accepted");
    IqData x(64), y(64);
    for (unsigned i = 0; i < 64; ++i) { x.push_back({0, 0}); y.push_back({1, 0}); }
    const auto original = y.view_data();
    WienerHopf filter(0, 8, 64);
    bool nullRejected = false;
    try { filter.process(nullptr, &y); } catch (const std::invalid_argument&) { nullRejected = true; }
    require(nullRejected, "Null clutter input accepted");
    IqData shortInput(1); shortInput.push_back({1, 0});
    bool shortRejected = false;
    try { filter.process(&shortInput, &y); } catch (const std::invalid_argument&) { shortRejected = true; }
    require(shortRejected && y.view_data() == original, "Short clutter input accepted or output changed");
    require(!filter.process(&x, &y) && y.view_data() == original,
      "Singular clutter input was accepted or changed");
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
