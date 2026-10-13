#include "process/ambiguity/Acceleration.h"
#include "process/clutter/WienerHopf.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <deque>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using Complex = std::complex<double>;
using Clock = std::chrono::steady_clock;
constexpr uint32_t Fs = 2400000, Channels = 5;
constexpr int ClutterFirst = -10, ClutterLast = 200;
constexpr double Pi = 3.14159265358979323846;
struct Options {
  std::string device;
  uint32_t samples = 1200000;
  unsigned seed = 7301;
  double residual = 2e-5, color = .85, pilot = .7;
};
void require(bool ok, const char* message) {
  if (!ok) throw std::runtime_error(message);
}
double milliseconds(Clock::time_point start) {
  return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}
Options options(int argc, char** argv) {
  Options result;
  for (int i = 1; i < argc; ++i) {
    const std::string flag = argv[i];
    require(i + 1 < argc, "Every option requires a value");
    const std::string value = argv[++i];
    if (flag == "--device") result.device = value;
    else if (flag == "--cpi-ms") {
      require(value == "200" || value == "500", "CPI must be 200 or 500 ms");
      result.samples = value == "200" ? 480000 : 1200000;
    } else if (flag == "--seed") {
      size_t used = 0;
      const auto seed = std::stoul(value, &used);
      require(used == value.size() && seed <= 65535, "Seed must be 0..65535");
      result.seed = static_cast<unsigned>(seed);
    } else {
      size_t used = 0;
      const double number = std::stod(value, &used);
      require(used == value.size() && std::isfinite(number), "Invalid finite fixture parameter");
      if (flag == "--residual") {
        require(number >= 1e-6 && number <= .01, "Residual must be 1e-6..0.01");
        result.residual = number;
      } else if (flag == "--color") {
        require(number >= 0 && number <= .98, "Color must be 0..0.98");
        result.color = number;
      } else if (flag == "--pilot") {
        require(number >= 0 && number <= 2, "Pilot must be 0..2");
        result.pilot = number;
      } else throw std::runtime_error("Unknown option: " + flag);
    }
  }
  require(!result.device.empty(), "Required: --device vendor:device:index (or auto)");
  return result;
}
std::deque<Complex> referenceSignal(const Options& o) {
  std::mt19937 random(o.seed);
  std::normal_distribution<double> noise;
  // OFDM-like occupied band plus a configurable broadband floor. A nonzero
  // floor keeps this cancellation fixture full rank even at high coloredness.
  std::vector<Complex> bins(o.samples);
  double spectralPower = 0;
  for (uint32_t i = 0; i < o.samples; ++i) {
    const double frequency = double(std::min(i, o.samples - i)) / o.samples;
    const double band = frequency < .32 ? 1.0 : 1.0 - o.color;
    bins[i] = std::sqrt(band) * Complex(noise(random), noise(random));
    spectralPower += std::norm(bins[i]);
  }
  auto* raw = reinterpret_cast<fftw_complex*>(bins.data());
  const auto inverse = fftw_plan_dft_1d(int(o.samples), raw, raw, FFTW_BACKWARD, FFTW_ESTIMATE);
  require(inverse != nullptr, "Synthetic OFDM inverse FFT planning failed");
  fftw_execute(inverse); fftw_destroy_plan(inverse);
  const double normalization = 1 / std::sqrt(spectralPower);
  std::deque<Complex> result;
  for (uint32_t i = 0; i < o.samples; ++i)
    result.push_back(1e-4 * (normalization * bins[i] + o.pilot *
      (std::polar(1.0, .071 * i) + .37 * std::polar(1.0, -.193 * i))));
  return result;
}
Complex delayed(const std::deque<Complex>& source, uint32_t i, int lag) {
  const int64_t index = (int64_t(i) - lag + int64_t(source.size())) % int64_t(source.size());
  return source[static_cast<size_t>(index)];
}
std::deque<Complex> channelSignal(const Options& o, const std::deque<Complex>& source,
                                 unsigned channel, unsigned frame) {
  std::mt19937 random(o.seed + 101 * channel + 1009 * frame);
  std::normal_distribution<double> noise;
  std::deque<Complex> result;
  // Two weak moving targets stay within the requested map and outside DC clutter.
  const int targetLag = 35 + 27 * int(channel);
  const double targetHz = 62 + 38 * channel + 2 * frame;
  const auto directGain = std::polar(4.0 + .5 * channel, .37 * channel);
  const auto echoGain = std::polar(.6 - .08 * channel, -.23 * channel);
  for (uint32_t i = 0; i < o.samples; ++i) {
    const Complex direct = directGain * delayed(source, i, 2) +
      echoGain * delayed(source, i, -3) + .12 * delayed(source, i, 17);
    const Complex target = o.residual * delayed(source, i, targetLag) *
      std::polar(1.0, 2 * Pi * targetHz * double(i) / Fs);
    const Complex second = .3 * o.residual * delayed(source, i, 225) *
      std::polar(1.0, -2 * Pi * 173 * double(i) / Fs);
    result.push_back(direct + target + second +
      (1e-4 * o.residual * .1) * Complex(noise(random), noise(random)));
  }
  return result;
}
// Independent fixture conditioning diagnostic, never a relaxed qualification gate.
double referenceCondition(const std::deque<Complex>& source) {
  std::vector<Complex> values(source.begin(), source.end());
  auto* raw = reinterpret_cast<fftw_complex*>(values.data());
  const auto forward = fftw_plan_dft_1d(int(values.size()), raw, raw, FFTW_FORWARD, FFTW_ESTIMATE);
  const auto inverse = fftw_plan_dft_1d(int(values.size()), raw, raw, FFTW_BACKWARD, FFTW_ESTIMATE);
  require(forward && inverse, "Condition diagnostic FFT planning failed");
  fftw_execute(forward);
  for (auto& value : values) value = Complex(std::norm(value), 0);
  fftw_execute(inverse);
  fftw_destroy_plan(forward); fftw_destroy_plan(inverse);
  arma::cx_vec autocorrelation(ClutterLast - ClutterFirst);
  for (size_t i = 0; i < autocorrelation.n_elem; ++i)
    autocorrelation[i] = std::conj(values[i]) / double(source.size());
  autocorrelation[0] = Complex(autocorrelation[0].real(), 0);
  arma::cx_mat matrix = arma::toeplitz(autocorrelation);
  for (size_t row = 0; row < matrix.n_rows; ++row)
    for (size_t col = 0; col < row; ++col) matrix(row, col) = std::conj(matrix(row, col));
  return arma::rcond(matrix);
}
struct Error {
  double signal = 0, error = 0, peak = 0, maximum = 0;
  void add(Complex actual, Complex expected) {
    require(std::isfinite(actual.real()) && std::isfinite(actual.imag()) &&
      std::isfinite(expected.real()) && std::isfinite(expected.imag()), "Nonfinite oracle/output");
    signal += std::norm(expected); error += std::norm(actual - expected);
    peak = std::max(peak, std::abs(expected)); maximum = std::max(maximum, std::abs(actual - expected));
  }
  double rms() const { return std::sqrt(error / std::max(signal, 1e-30)); }
  double relativePeak() const { return maximum / std::max(peak, 1e-30); }
};
void status(unsigned frame, const char* stage, const blah2::AccelerationStatus& s) {
  std::cout << "stage frame=" << frame << " stage=" << stage << " active=" << s.active
    << " state=" << s.state << " cpu_ms=" << s.cpuMs << " gpu_ms=" << s.gpuMs
    << " reason=" << s.reason << '\n';
}
}
int main(int argc, char** argv) try {
  const auto started = Clock::now();
  const Options o = options(argc, argv);
  std::cout << std::setprecision(12);
  auto source = referenceSignal(o);
  IqData reference(o.samples); reference.replace(std::deque<Complex>(source));
  WienerHopf::PreparedReference prepared(ClutterFirst, ClutterLast, o.samples);
  require(prepared.prepare(reference), "CPU prepared reference rejected synthetic fixture");
  const double reciprocalCondition = referenceCondition(source);
  require(std::isfinite(reciprocalCondition) && reciprocalCondition > 0,
    "Synthetic reference conditioning diagnostic failed");
  std::cout << "fixture seed=" << o.seed << " samples=" << o.samples << " fs=" << Fs
    << " channels=" << Channels << " residual=" << o.residual << " color=" << o.color
    << " pilot=" << o.pilot << " reciprocal_condition=" << reciprocalCondition << '\n';
  std::vector<std::unique_ptr<Ambiguity>> maps;
  std::vector<std::unique_ptr<WienerHopf>> filters;
  std::vector<std::unique_ptr<IqData>> data;
  std::vector<Ambiguity*> mapPointers;
  std::vector<IqData*> dataPointers;
  for (unsigned channel = 0; channel < Channels; ++channel) {
    maps.push_back(std::make_unique<Ambiguity>(-10, 245, -800, 800, Fs, o.samples, true));
    filters.push_back(std::make_unique<WienerHopf>(ClutterFirst, ClutterLast, o.samples,
      WienerHopf::Workspace::SharedReference));
    data.push_back(std::make_unique<IqData>(o.samples));
    mapPointers.push_back(maps.back().get()); dataPointers.push_back(data.back().get());
  }
  Ambiguity oracle(-10, 245, -800, 800, Fs, o.samples, true);
  WienerHopf oracleFilter(ClutterFirst, ClutterLast, o.samples, WienerHopf::Workspace::SharedReference);
  const blah2::GpuGeometry geometry{maps[0]->get_nfft(), maps[0]->get_n_doppler_bins(),
    maps[0]->get_n_delay_bins(), Channels, -10, o.samples, ClutterLast - ClutterFirst, ClutterFirst};
  blah2::Acceleration acceleration("gpu", geometry, maps[0]->get_n_corr(), Fs,
    maps[0]->get_doppler_middle(), o.device);
  std::cout << "geometry range_fft=" << geometry.range << " doppler=" << geometry.doppler
    << " delays=" << geometry.delays << " n_corr=" << maps[0]->get_n_corr()
    << " device=" << acceleration.status().device << '\n';
  constexpr unsigned Frames = blah2::Acceleration::TotalQualificationFrames + 4;
  bool referenceChanged = false;
  for (unsigned frame = 0; frame < Frames; ++frame) {
    const auto frameStarted = Clock::now();
    if (frame == blah2::Acceleration::TotalQualificationFrames) {
      // Replace the reference after qualification without replacing the GPU
      // instance. Both its FFT and coefficient factor must belong to this frame.
      auto changed = o;
      changed.seed = o.seed ^ 1U;
      auto next = referenceSignal(changed);
      require(next != source, "Changed-reference fixture is unchanged");
      source = std::move(next);
      reference.replace(std::deque<Complex>(source));
      require(prepared.prepare(reference), "CPU rejected replacement reference");
      referenceChanged = true;
      std::cout << "reference-change frame=" << frame << " seed=" << changed.seed
        << " samples=" << source.size() << '\n';
    }
    for (unsigned channel = 0; channel < Channels; ++channel)
      data[channel]->replace(channelSignal(o, source, channel, frame));
    const auto clutterStarted = Clock::now();
    require(acceleration.processClutter(reference, dataPointers, [&] {
      bool ok = true;
      for (unsigned channel = 0; channel < Channels; ++channel)
        ok = filters[channel]->process(prepared, data[channel].get()) && ok;
      return ok;
    }), "Clutter processing rejected fixture");
    const double clutterMs = milliseconds(clutterStarted);
    status(frame, "clutter", acceleration.clutterStatus());
    // Compare each filtered sample while retaining the GPU result for composition.
    Error clutterError;
    for (unsigned channel = 0; channel < Channels; ++channel) {
      IqData truth(o.samples); truth.replace(channelSignal(o, source, channel, frame));
      require(oracleFilter.process(prepared, &truth), "FP64 oracle clutter failed");
      const auto& actual = data[channel]->view_data();
      const auto& expected = truth.view_data();
      require(actual.size() == expected.size(), "Clutter output shape differs");
      for (size_t i = 0; i < actual.size(); ++i) clutterError.add(actual[i], expected[i]);
    }
    const auto ambiguityStarted = Clock::now();
    acceleration.process(reference.view_data(), dataPointers, mapPointers, [&] {
      for (unsigned channel = 0; channel < Channels; ++channel)
        maps[channel]->process(reference.view_data(), data[channel].get());
    });
    const double ambiguityMs = milliseconds(ambiguityStarted);
    status(frame, "ambiguity", acceleration.status());
    double worstRms = 0, worstPeak = 0;
    for (unsigned channel = 0; channel < Channels; ++channel) {
      IqData truth(o.samples); truth.replace(channelSignal(o, source, channel, frame));
      require(oracleFilter.process(prepared, &truth), "FP64 composed oracle clutter failed");
      oracle.process(reference.view_data(), &truth);
      const auto& expected = oracle.result()->data;
      const auto& actual = maps[channel]->result()->data;
      require(actual.size() == expected.size(), "Map row count differs");
      Error error;
      for (size_t row = 0; row < actual.size(); ++row) {
        require(actual[row].size() == expected[row].size(), "Map column count differs");
        for (size_t col = 0; col < actual[row].size(); ++col)
          error.add(actual[row][col], expected[row][col]);
      }
      std::cout << "map frame=" << frame << " channel=" << channel << " relative_rms="
        << error.rms() << " relative_peak=" << error.relativePeak() << '\n';
      worstRms = std::max(worstRms, error.rms()); worstPeak = std::max(worstPeak, error.relativePeak());
    }
    std::cout << "frame index=" << frame << " clutter_relative_rms=" << clutterError.rms()
      << " clutter_relative_peak=" << clutterError.relativePeak() << " worst_map_rms=" << worstRms
      << " worst_map_peak=" << worstPeak << " clutter_ms=" << clutterMs
      << " ambiguity_ms=" << ambiguityMs << " including_oracle_ms=" << milliseconds(frameStarted)
      << " elapsed_ms=" << milliseconds(started) << '\n' << std::flush;
    require(worstRms <= 1e-4 && worstPeak <= 1e-4, "Composed map exceeds unchanged FP64 accuracy contract");
    require(acceleration.clutterStatus().state != "fallback", "GPU clutter fallback is a regression failure");
    require(acceleration.status().state != "fallback", "GPU ambiguity fallback is a regression failure");
    if (frame + 1 >= blah2::Acceleration::TotalQualificationFrames) {
      require(acceleration.clutterStatus().active == "vulkan" &&
        acceleration.status().active == "vulkan", "Both GPU stages must remain retained after qualification");
    }
  }
  require(referenceChanged, "Post-qualification reference replacement was not exercised");
  std::cout << "PASS production-composed frames=" << Frames << " elapsed_ms=" << milliseconds(started) << '\n';
  return 0;
} catch (const std::exception& error) {
  std::cerr << "FAIL production-composed: " << error.what() << '\n';
  return 1;
}
