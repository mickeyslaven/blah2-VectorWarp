// Mechanism/negative-control regression for the isolated intra-CPI experiment.
#include "process/integration/RangeMigrationReplay.h"
#include "process/ambiguity/Ambiguity.h"
#include <iostream>
#include <random>

using Migration = blah2::integration::RangeMigrationReplay;
using Complex = Migration::Complex;
namespace {
constexpr double tau = 6.283185307179586476925286766559;
void check(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}
template<class F> void rejects(F work) {
  bool rejected = false;
  try { work(); } catch (const std::invalid_argument&) { rejected = true; }
  check(rejected, "Invalid migration contract accepted");
}
Map<Complex> map(uint32_t rows = 65, uint32_t columns = 81) {
  Map<Complex> value(rows, columns);
  for (auto& row : value.data) std::fill(row.begin(), row.end(), Complex{});
  for (uint32_t c = 0; c < columns; ++c) value.delay.push_back(int(c) - int(columns / 2));
  for (uint32_t r = 0; r < rows; ++r) value.doppler.push_back((double(r) - rows / 2) / (rows / 64.0));
  return value;
}
double peak(const Migration::Bank& bank) {
  double result = 0;
  for (auto value : bank.cells) result = std::max(result, std::norm(value) / bank.whiteNoiseGain);
  return result;
}
Map<Complex> echo(double rate, double delay, double frequencyBins) {
  auto value = map();
  // Independent analytic range profile: continuous bandlimited pulse; no
  // use of the candidate's interpolation kernel to generate the target.
  constexpr uint32_t rows = 65, columns = 81;
  for (uint32_t time = 0; time < rows; ++time) {
    const double position = delay + rate * (double(time) - 32) / 64;
    const auto phasor = std::polar(1.0, tau * frequencyBins * time / rows);
    for (uint32_t col = 0; col < columns; ++col) {
      const double delta = value.delay[col] - position;
      const double pulse = std::abs(delta) < 1e-12 ? 1 : std::sin(.65 * std::acos(-1.) * delta) / (.65 * std::acos(-1.) * delta);
      for (uint32_t row = 0; row < rows; ++row)
        value.data[row][col] += pulse * phasor * std::polar(1.0,
          -tau * (double(row) - 32) * time / rows);
    }
  }
  return value;
}
void analyticEchoes() {
  for (const double rate : {-12., 12.}) for (const double delay : {0., .35})
    for (const double doppler : {-5., 5.25}) {
      const auto value = echo(rate, delay, doppler);
      const auto result = Migration::process(value, 8192, 128, {0, rate, -rate});
      const double gain = peak(result.banks[1]) / peak(result.banks[0]);
      check(gain > 5, "Uncompensated migrating echo did not smear");
      check(peak(result.banks[1]) > 3 * peak(result.banks[2]), "Delay-rate sign reversed");
      const auto strongest = std::max_element(result.banks[1].cells.begin(), result.banks[1].cells.end(),
        [](auto a, auto b) { return std::norm(a) < std::norm(b); });
      const auto index = size_t(strongest - result.banks[1].cells.begin());
      check(std::abs(double(index / result.columns) - (32 + doppler)) <= .5,
        "Migration changed Doppler sign/bin units");
      check(value.delay[result.firstColumn + index % result.columns] == 0,
        "Corrected delay does not refer to block midpoint");
      const auto stationary = Migration::process(echo(0, delay, doppler), 8192, 128, {0, rate});
      const double recovery = peak(result.banks[1]) / peak(stationary.banks[0]);
      check(recovery > .90 && recovery < 1.15, "Correction invents or loses coherent SNR");
      std::cout << "analytic rate=" << rate << " delay=" << delay << " doppler_bins=" << doppler
        << " normalized_peak_gain=" << gain << " stationary_recovery=" << recovery << '\n';
    }
}
void canonicalIq() {
  constexpr uint32_t fs = 8192, samples = 8320;
  std::mt19937 generator(245); std::uniform_real_distribution<double> uniform(-.5, .5);
  std::array<double, 48> frequencies{}, phases{};
  for (size_t i = 0; i < frequencies.size(); ++i) { frequencies[i] = uniform(generator) * .75; phases[i] = uniform(generator) * tau; }
  const auto referenceAt = [&](double n) {
    Complex value{};
    for (size_t i = 0; i < frequencies.size(); ++i)
      value += std::polar(1.0, tau * frequencies[i] * n + phases[i]);
    return value / std::sqrt(double(frequencies.size()));
  };
  for (const double rate : {-12., 12.}) for (const bool physical : {false, true}) {
    // Physical case uses explicit synthetic carrier fc=256*fs, hence
    // f_D=-fc*delayRate/fs. Nonzero centers also check reference heterodyning.
    const int middle = physical ? int(-256 * rate) : 0;
    Ambiguity ambiguity(-40, 40, middle - 32, middle + 32, fs, samples);
    check(ambiguity.get_n_corr() == 128 && ambiguity.get_n_doppler_bins() == 65, "Unexpected canonical CAF geometry");
    const double doppler = physical ? double(middle) : 5.25 / ambiguity.get_cpi();
    std::deque<Complex> reference; IqData surveillance(samples);
    for (uint32_t n = 0; n < samples; ++n) {
      reference.push_back(referenceAt(n));
      const double delay = .35 + rate * (double(n) - double(samples - 1) / 2) / fs;
      surveillance.push_back(referenceAt(n - delay) * std::polar(1.0, tau * doppler * n / fs));
    }
    const auto caf = ambiguity.process(reference, &surveillance);
    const auto result = Migration::process(*caf, fs, 128, {0, rate, -rate});
    check(peak(result.banks[1]) > 3 * peak(result.banks[0]), "Canonical IQ motion did not recover");
    check(peak(result.banks[1]) > 2 * peak(result.banks[2]), "Canonical IQ delay sign differs");
    const auto winner = std::max_element(result.banks[1].cells.begin(), result.banks[1].cells.end(),
      [](auto a, auto b) { return std::norm(a) < std::norm(b); });
    check(size_t(winner - result.banks[1].cells.begin()) / result.columns == (physical ? 32u : 37u),
      "Canonical IQ positive Doppler sign differs");
    std::cout << "canonical_iq rate=" << rate << " physical=" << physical << " normalized_peak_gain="
      << peak(result.banks[1]) / peak(result.banks[0]) << '\n';
  }
}
void invariantsAndNoise() {
  auto value = echo(0, .35, 5.25);
  const auto result = Migration::process(value, 8192, 128, {0, 12});
  for (uint32_t row = 0; row < result.rows; ++row)
    for (uint32_t col = 0; col < result.columns; ++col)
      check(result.banks[0].cells[row * result.columns + col] == value.data[row][col + result.firstColumn],
        "Zero-rate baseline changed canonical amplitude");
  const Complex scale(2, -1);
  for (auto& row : value.data) for (auto& cell : row) cell *= scale;
  const auto scaled = Migration::process(value, 8192, 128, {0, 12});
  for (size_t index = 0; index < result.banks[1].cells.size(); ++index)
    check(std::abs(scaled.banks[1].cells[index] - scale * result.banks[1].cells[index]) < 1e-10,
      "Migration violates complex amplitude linearity");
  check(std::abs(peak(scaled.banks[1]) / peak(result.banks[1]) - 5) < 1e-10,
    "Migration violates power scaling");
  std::mt19937 generator(719); std::normal_distribution<double> normal(0, std::sqrt(.5));
  std::array<double, 3> powers{}; double selectedPower = 0; size_t cells = 0;
  for (unsigned realization = 0; realization < 12; ++realization) {
    auto noise = map(33, 65);
    for (auto& row : noise.data) for (auto& cell : row) cell = {normal(generator), normal(generator)};
    const auto banks = Migration::process(noise, 8192, 128, {0, 12, -12});
    check(banks.evaluatedCells == 3 * banks.rows * banks.columns && banks.banks.size() == 3,
      "Migration did not charge all baseline and rate trials");
    for (size_t index = 0; index < banks.banks[0].cells.size(); ++index) {
      double selected = 0;
      for (size_t trial = 0; trial < powers.size(); ++trial) {
        const auto& bank = banks.banks[trial];
        const double power = std::norm(bank.cells[index]) / bank.whiteNoiseGain;
        powers[trial] += power; selected = std::max(selected, power);
      }
      selectedPower += selected; ++cells;
    }
  }
  for (const auto power : powers)
    check(std::abs(power / cells - 1) < .08, "Fixed-trial normalized white-noise power changed");
  check(selectedPower / cells > 1.4, "Search maximum was treated as one uncharged noise trial");
  std::cout << "noise fixed_trial_means=" << powers[0] / cells << ',' << powers[1] / cells
    << ',' << powers[2] / cells << " searched_mean=" << selectedPower / cells << '\n';
}
void refusal() {
  auto value = map();
  const auto run = [&](const std::vector<double>& rates) { return Migration::process(value, 8192, 128, rates); };
  for (const auto& bank : run({0, 12, -12}).banks)
    for (const auto sample : bank.cells) check(sample == Complex{}, "Zero-input control created power");
  rejects([&] { run({12}); }); rejects([&] { run({0, 0}); });
  rejects([&] { run({0, 16.01}); }); rejects([&] { run({0, NAN}); });
  rejects([&] { run({0, INFINITY}); }); rejects([&] { run({}); });
  rejects([&] { run({0,1,2,3,4,5,6,7,8,9}); });
  rejects([&] { Migration::process(value, 0, 128, {0}); });
  rejects([&] { Migration::process(value, 8192, 0, {0}); });
  value.doppler.pop_back(); rejects([&] { run({0}); }); value = map();
  value.doppler[4] += 1; rejects([&] { run({0}); }); value = map();
  value.delay[2] += 1; rejects([&] { run({0}); }); value = map();
  value.data[0].pop_back(); rejects([&] { run({0}); }); value = map();
  value.data[0][0] = {NAN, 0}; rejects([&] { run({0}); }); value = map(65, 21);
  rejects([&] { run({0, 12}); }); value = map(511, 127);
  rejects([&] { run({0,.001,.002,.003,.004,.005,.006,.007,.008}); }); // Operation bound.
  value = map(513, 81); rejects([&] { run({0}); });
}
void complexCoordinate() {
  constexpr uint32_t columns = 64, fs = 8192;
  std::vector<Complex> profiles(2 * columns);
  const auto sinc = [](double x) { return x == 0 ? 1. : std::sin(std::acos(-1.) * x) / (std::acos(-1.) * x); };
  for (double center : {-2300., 0., 1700.}) for (double base : {0., .07}) {
    const Complex scale(.7, -1.1);
    const auto signal = [&](double position) { return scale * std::polar(1., tau * (base + center / fs) * position); };
    for (uint32_t col = 0; col < columns; ++col) profiles[columns + col] = signal(col);
    for (double column : {8., 8.25, 17.5, 23.875, 55.75}) {
      // Independent scalar formula. Inputs are continuous complex tones, not
      // generated by the implementation's kernel or nearest-cell sampler.
      Complex expected{}; double normalization = 0;
      for (int source = int(std::floor(column)) - 7; source <= int(std::floor(column)) + 8; ++source) {
        const double distance = column - source;
        const double weight = sinc(distance) * sinc(distance / 8.);
        normalization += weight;
        expected += profiles[columns + source] * weight * std::polar(1., tau * center * distance / fs);
      }
      expected /= normalization;
      const auto actual = Migration::sampleProfile(profiles, columns, 1, column, center, fs);
      check(std::abs(actual - expected) < 2e-12, "Complex coordinate differs from scalar Lanczos/phase oracle");
      check(std::abs(actual - signal(column)) < (base == 0 ? 2e-12 : .004), "Continuous complex phase/amplitude not preserved");
      if (column == 8.) check(actual == profiles[columns + 8], "Integer complex coordinate changed");
    }
    for (double shift : {-1.4, -.4, 0., .4, 1.4}) {
      const auto kernel = Migration::prepareProfile(shift, center, fs);
      check(kernel.firstOffset() == int64_t(std::floor(shift)) - 7 &&
        kernel.lastOffset() == int64_t(std::floor(shift)) + 8, "Prepared support differs");
      double sum = 0, squares = 0;
      for (int64_t tap = kernel.firstOffset(); tap <= kernel.lastOffset(); ++tap) {
        const double weight = sinc(shift - tap) * sinc((shift - tap) / 8.);
        sum += weight; squares += weight * weight;
      }
      check(std::abs(kernel.whiteNoiseGain() - squares / (sum * sum)) < 2e-15,
        "Prepared white-noise gain differs from independent scalar sum");
      for (uint32_t col : {10u, 24u, 52u}) {
        const auto actual = Migration::sampleProfile(profiles, columns, 1, col, kernel);
        check(std::abs(actual - Migration::sampleProfile(profiles, columns, 1,
          col + shift, center, fs)) < 2e-12, "Prepared/direct complex sampling differs");
        // Older CPI is sampled at c+shift, but newest CPI's range gauge is c.
        // This independent continuous oracle catches a missing/doubled ramp.
        const auto newestGauge = actual * std::polar(1., -tau * center * shift / fs);
        const auto expected = scale * std::polar(1., tau * (base * (col + shift) + center * col / fs));
        check(std::abs(newestGauge - expected) < (base == 0 ? 2e-12 : .004),
          "Nonzero-center inter-CPI output-coordinate gauge differs");
      }
    }
  }
  const auto run = [&](uint32_t width, uint32_t row, double column, double center, uint32_t fs) {
    return Migration::sampleProfile(profiles, width, row, column, center, fs);
  };
  rejects([&] { run(0, 0, 8., 0, fs); }); rejects([&] { run(columns, 0, 8., 0, 0); });
  rejects([&] { run(columns, 2, 8., 0, fs); }); rejects([&] { run(columns, UINT32_MAX, 8., 0, fs); });
  rejects([&] { run(columns, 1, 7.999, 0, fs); }); rejects([&] { run(columns, 1, 56., 0, fs); });
  rejects([&] { run(columns, 1, NAN, 0, fs); }); rejects([&] { run(columns, 1, 8., INFINITY, fs); });
  rejects([&] { run(columns, 1, 1e100, 0, fs); });
  rejects([&] { Migration::prepareProfile(INFINITY, 0, fs); });
  rejects([&] { Migration::prepareProfile(-1e100, 0, fs); });
  rejects([&] { Migration::prepareProfile(0, 1e308, 1); });
  const auto boundary = Migration::prepareProfile(-.4, 1700, fs);
  rejects([&] { Migration::sampleProfile(profiles, columns, 1, 8u, boundary); });
  rejects([&] { Migration::sampleProfile(profiles, columns, 1, UINT32_MAX, boundary); });
  profiles.pop_back(); rejects([&] { run(columns, 0, 8., 0, fs); }); profiles.push_back(0.);
  profiles[columns + 8] = {NAN, 0}; rejects([&] { run(columns, 1, 8., 0, fs); });
  std::cout << "complex_coordinate scalar_phase_oracle=passed boundary_refusal=passed\n";
}
}
int main() try {
  analyticEchoes(); canonicalIq(); invariantsAndNoise(); refusal(); complexCoordinate();
  std::cout << "PASS range migration replay numerical contract (not radar qualification)\n";
  return 0;
} catch (const std::exception& error) {
  std::cerr << "FAIL range migration replay: " << error.what() << '\n'; return 1;
}
