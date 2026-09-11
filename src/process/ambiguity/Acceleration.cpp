#include "Acceleration.h"
#include "GpuProcess.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <utility>

namespace blah2 {
namespace {
using Clock = std::chrono::steady_clock;
double monotonic_ms() {
  return std::chrono::duration<double, std::milli>(Clock::now().time_since_epoch()).count();
}
double median(std::vector<double> values) {
  std::sort(values.begin(), values.end());
  return values[values.size() / 2];
}
}
Acceleration::Acceleration(std::string mode, GpuGeometry geometry,
  uint32_t correlation, uint32_t sampleRate, double middle, std::string device, BackendFactory factory,
  TimeSource timeSource)
  : geometry_(geometry), correlation_(correlation), sampleRate_(sampleRate), middle_(middle),
    timeSource_(std::move(timeSource)) {
  if (mode != "auto" && mode != "cpu" && mode != "gpu")
    throw std::invalid_argument("Acceleration must be auto, cpu or gpu");
  if (!correlation || !geometry.delays ||
      geometry.delayMin <= -static_cast<int64_t>(correlation) ||
      static_cast<int64_t>(geometry.delayMin) + geometry.delays - 1 >= correlation)
    throw std::invalid_argument("Delay limits exceed the correlation block; reduce the delay range or narrow the Doppler span");
  status_.requested = mode;
  if (mode == "cpu") { status_.state = "selected"; return; }
  try {
    if (factory) gpu_ = factory(geometry_, device);
    else gpu_ = createGpuProcess(geometry_, device);
    if (!gpu_) throw std::runtime_error("GPU module did not create a processor");
    status_.device = gpu_->device().name;
    status_.state = "checking";
    status_.reason = "Checking GPU accuracy and speed";
  } catch (const std::exception& error) { fallback(error.what()); }
}
Acceleration::~Acceleration() = default;
double Acceleration::now_ms() const { return timeSource_ ? timeSource_() : monotonic_ms(); }
void Acceleration::fallback(const std::string& message) {
  gpu_.reset();
  status_.active = "cpu";
  status_.state = "fallback";
  status_.reason = message;
  reference_.clear(); surveillance_.clear(); output_.clear();
  std::cerr << "Acceleration: " << message << '\n';
}
void Acceleration::process(const std::deque<std::complex<double>>& reference,
  const std::vector<IqData*>& surveillance, const std::vector<Ambiguity*>& cpu,
  const CpuRun& cpuRun) {
  if (!gpu_) { cpuRun(); return; }
  const uint64_t samples = uint64_t(correlation_) * geometry_.doppler;
  if (surveillance.size() != geometry_.channels || cpu.size() != geometry_.channels ||
      reference.size() < samples)
    throw std::invalid_argument("Incomplete input for GPU radar processing");
  for (const auto* input : surveillance)
    if (input->view_data().size() < samples)
      throw std::invalid_argument("Surveillance CPI is shorter than ambiguity input");
  const double gpuStart = now_ms();
  bool cpuStarted = false;
  bool inputRetired = false;
  try {
    const size_t rangeElements = size_t(geometry_.range) * geometry_.doppler;
    reference_.assign(rangeElements, {});
    surveillance_.assign(rangeElements * geometry_.channels, {});
    for (uint32_t d = 0; d < geometry_.doppler; ++d)
      for (uint32_t r = 0; r < correlation_; ++r) {
        const size_t sample = size_t(d) * correlation_ + r;
        const size_t target = size_t(d) * geometry_.range + r;
        auto value = reference[sample];
        if (middle_ != 0)
          value *= std::polar(1.0, 2 * std::acos(-1.0) * middle_ * sample / sampleRate_);
        reference_[target] = value;
        for (size_t channel = 0; channel < surveillance.size(); ++channel)
          surveillance_[channel * rangeElements + target] = surveillance[channel]->view_data()[sample];
      }
    gpu_->process(reference_, surveillance_, output_);
    const size_t perChannel = size_t(geometry_.doppler) * geometry_.delays;
    if (output_.size() != perChannel * geometry_.channels ||
        !std::all_of(output_.begin(), output_.end(), [](auto value) {
          return std::isfinite(value.real()) && std::isfinite(value.imag());
        })) throw std::runtime_error("GPU returned invalid radar data; using CPU");
    // Include conversion into the normal map representation in the comparison.
    for (size_t channel = 0; channel < cpu.size(); ++channel)
      cpu[channel]->import_gpu(output_.data() + channel * perChannel);
    const double gpuMs = now_ms() - gpuStart;
    if (checks_ < 3) {
      // CPU owns these initial frames. Its independent FP64 result checks every
      // complex bin on every channel before any GPU result can reach detection.
      const double cpuStart = now_ms();
      cpuStarted = true;
      cpuRun();
      const double cpuMs = now_ms() - cpuStart;
      for (size_t channel = 0; channel < cpu.size(); ++channel) {
        double signal = 0, error = 0, peak = 0, maxError = 0;
        for (uint32_t d = 0; d < geometry_.doppler; ++d) {
          const auto row = cpu[channel]->result()->get_row(d);
          for (uint32_t r = 0; r < geometry_.delays; ++r) {
            const std::complex<double> value = output_[channel * perChannel +
              size_t(r) * geometry_.doppler + (d + geometry_.doppler / 2 + 1) % geometry_.doppler];
            const double delta = std::norm(value - row[r]);
            signal += std::norm(row[r]); error += delta;
            peak = std::max(peak, std::norm(row[r])); maxError = std::max(maxError, delta);
          }
        }
        if (error > std::max(signal * 1e-8, 1e-20) ||
            maxError > std::max(peak * 1e-8, 1e-20)) {
          std::cerr << "GPU validation channel=" << channel << " relative_rms="
            << std::sqrt(error / std::max(signal, 1e-30)) << " relative_peak="
            << std::sqrt(maxError / std::max(peak, 1e-30)) << '\n';
          fallback("GPU accuracy check failed; using CPU"); return;
        }
      }
      cpuTimes_.push_back(cpuMs); gpuTimes_.push_back(gpuMs);
      status_.cpuMs = median(cpuTimes_); status_.gpuMs = median(gpuTimes_);
      if (++checks_ == 3) {
        if (status_.requested == "auto" && status_.gpuMs >= status_.cpuMs * .95) {
          fallback("CPU is faster for these settings"); return;
        }
        status_.active = "vulkan"; status_.state = "ready"; status_.reason.clear();
        std::cout << "Acceleration: Vulkan / " << status_.device << '\n';
      }
      return;
    }
    inputRetired = true; // No CPU retry once any input retirement has begun.
    for (auto* input : surveillance)
      for (uint64_t n = 0; n < samples; ++n) input->pop_front();
    // Qualification measures GPU transform/import before retirement because
    // CPU owns those frames for accuracy comparison. AUTO additionally checks
    // the complete accepted GPU path below; it is not a whole-pipeline tuner.
    // The frozen recorded-IQ benchmark campaign predates this safeguard.
    if (status_.requested == "auto") {
      sustainedGpuTimes_.push_back(now_ms() - gpuStart);
      status_.gpuMs = median(sustainedGpuTimes_);
      if (sustainedGpuTimes_.size() == 5) {
        if (status_.gpuMs >= status_.cpuMs * .95)
          fallback("CPU was faster than sustained GPU processing");
        else
          sustainedGpuTimes_.clear();
      }
    }
  } catch (const std::exception& error) {
    if (cpuStarted || inputRetired) throw; // Never rerun an already-consumed frame.
    // GPU never consumes input: this exact frame can safely be rerun on CPU.
    fallback(error.what());
    cpuRun();
  }
}
}
