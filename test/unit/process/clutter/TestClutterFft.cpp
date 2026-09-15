// The padded clutter convolution.
//
// Two things need holding down. That the padded length is always at least the
// alias-free convolution length, since anything shorter wraps and corrupts the
// start of the output. And that the filter still computes the right answer,
// checked against an independent direct convolution rather than against another
// FFT, so a fault in the transform path cannot hide behind itself.
#include "process/clutter/WienerHopf.h"
#include "process/meta/FftLength.h"

#include <algorithm>
#include <complex>
#include <cstdint>
#include <iostream>
#include <random>
#include <stdexcept>
#include <vector>

using Complex = std::complex<double>;

static void require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

static void run(unsigned samples, int first, int last) {
  const unsigned taps = last - first;
  IqData reference(samples), surveillance(samples);
  WienerHopf filter(first, last, samples);

  // The only property the convolution depends on: long enough to be alias-free.
  const uint64_t minimum = uint64_t(samples) + taps + 1;
  require(filter.filter_fft_length() >= minimum,
    "Filter FFT length is below the alias-free convolution length");

  std::mt19937 rng(9211);
  std::normal_distribution<double> random;
  for (int repeat = 0; repeat < 3; ++repeat) {
    reference.clear();
    surveillance.clear();
    arma::cx_vec x(samples), y(samples);
    for (unsigned i = 0; i < samples; ++i)
      reference.push_back({random(rng), random(rng)});
    const auto originalReference = reference.get_data();
    for (unsigned i = 0; i < samples; ++i) {
      const int64_t shifted = (int64_t(i) - first) % samples;
      x[i] = originalReference[shifted < 0 ? shifted + samples : shifted];
      y[i] = x[i] * Complex(.7, .2) + Complex(.2 * random(rng), .2 * random(rng));
      surveillance.push_back(y[i]);
    }

    // Independent circular correlations and a direct linear convolution, not
    // another FFT implementation. The filter is reused across three CPIs.
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
    const auto filtered = surveillance.get_data();
    for (unsigned i = 0; i < samples; ++i) {
      Complex expected = y[i];
      for (unsigned tap = 0; tap < taps && tap <= i; ++tap)
        expected -= weights[tap] * x[i - tap];
      require(std::abs(expected - filtered[i]) < 1e-9,
        "Padded clutter differs from direct convolution");
    }
    require(reference.get_data() == originalReference, "Reference mutated");
  }
  std::cout << "PASS samples=" << samples << " taps=" << taps
            << " first=" << first << " fft=" << filter.filter_fft_length() << '\n';
}

int main() {
  try {
    // The shipped geometry is the one the constant was measured for: 2 MS/s,
    // 0.5 s CPI, clutter delay -10..400, so 1000000 + 410 + 1 points.
    const uint64_t shipped = 1000411;
    require(blah2::clutterFftLength(shipped) == blah2::kClutterFftLength,
      "Shipped geometry no longer uses the measured length");
    require(blah2::kClutterFftLength >= shipped,
      "Measured length is shorter than the shipped geometry needs");
    std::cout << "PASS shipped geometry uses " << blah2::kClutterFftLength << '\n';

    // The guard. The constant is only right for the geometry it was measured
    // at, so anything it does not cover falls back to the unpadded length,
    // which is always alias-free and is what the code did before it existed.
    require(blah2::clutterFftLength(blah2::kClutterFftLength) == blah2::kClutterFftLength,
      "Exact fit rejected");
    require(blah2::clutterFftLength(blah2::kClutterFftLength + 1)
              == blah2::kClutterFftLength + 1,
      "Geometry above the constant must not use it, that would alias");
    require(blah2::clutterFftLength(200411) == 200411,
      "A much smaller geometry must not be padded fivefold");
    require(blah2::clutterFftLength(2000411) == 2000411,
      "A larger geometry must fall back to unpadded");
    for (uint64_t minimum : {uint64_t(1), uint64_t(97), uint64_t(200411),
                             uint64_t(999999), shipped, uint64_t(2000411)})
      require(blah2::clutterFftLength(minimum) >= minimum,
        "Padded length below the alias-free minimum");
    std::cout << "PASS geometry guard\n";

    // A length outside what FFTW can index is a programming error, not a
    // silently truncated transform.
    for (uint64_t invalid : {uint64_t(0), uint64_t(INT32_MAX) + 1, UINT64_MAX}) {
      bool rejected = false;
      try { (void)blah2::clutterFftLength(invalid); }
      catch (const std::invalid_argument&) { rejected = true; }
      require(rejected, "Unrepresentable FFT geometry accepted");
    }
    std::cout << "PASS invalid geometry rejected\n";

    // Correctness across a spread of geometries, including positive delayMin,
    // which used to read the wrong sample: `i - delayMin` promoted to unsigned
    // and wrapped at 2^32. Negative and zero cancelled exactly, so the shipped
    // -10 was unaffected and the fault stayed hidden.
    for (unsigned samples : {64u, 127u, 257u})
      for (int first : {-3, 0, 2}) run(samples, first, first + 8);
    run(128, -2, -1);
    run(31, -2, 2);
    run(32, 0, 32);  // tap count equal to the CPI sample count

    for (const auto& bounds : {std::pair<int, int>{0, 0}, {5, 2}, {0, 65},
                              {INT32_MIN, INT32_MAX}}) {
      bool rejected = false;
      try { WienerHopf reject(bounds.first, bounds.second, 64); }
      catch (const std::invalid_argument&) { rejected = true; }
      require(rejected, "Degenerate delay range accepted");
    }
    std::cout << "PASS degenerate delay ranges rejected\n";

    std::cout << "All clutter FFT checks passed\n";
  } catch (const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
  }
  return 0;
}
