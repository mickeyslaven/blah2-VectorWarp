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
  if (correlation > geometry.range)
    throw std::invalid_argument("Correlation input exceeds the GPU FFT length");
  if (!correlation || !geometry.delays ||
      geometry.delayMin <= -static_cast<int64_t>(correlation) ||
      static_cast<int64_t>(geometry.delayMin) + geometry.delays - 1 >= correlation)
    throw std::invalid_argument("Delay limits exceed the correlation block; reduce the delay range or narrow the Doppler span");
  status_.requested = mode;
  clutterStatus_.requested = mode;
  if (!geometry_.clutterSamples) {
    clutterStatus_.state = "disabled";
    clutterStatus_.reason = "GPU clutter geometry was not configured";
  }
  if (mode == "cpu") {
    status_.state = "selected";
    if (geometry_.clutterSamples) clutterStatus_.state = "selected";
    return;
  }
  try {
    if (factory) gpu_ = factory(geometry_, device);
    else gpu_ = createGpuProcess(geometry_, device);
    if (!gpu_) throw std::runtime_error("GPU module did not create a processor");
    status_.device = gpu_->device().name;
    status_.state = "checking";
    status_.reason = "Checking GPU accuracy and speed";
    auto* clutter = dynamic_cast<GpuClutterFrameBackend*>(gpu_.get());
    if (geometry_.clutterSamples && clutter && clutter->clutterAvailable()) {
      clutterStatus_.device = status_.device;
      clutterStatus_.state = "checking";
      clutterStatus_.reason = "Checking GPU clutter accuracy and speed";
    } else if (geometry_.clutterSamples) {
      clutterDisabled_ = true;
      clutterStatus_.state = "fallback";
      clutterStatus_.reason = "GPU clutter processing is unavailable; using CPU clutter";
    }
  } catch (const std::exception& error) { fallback(error.what()); }
}
Acceleration::~Acceleration() = default;
double Acceleration::now_ms() const { return timeSource_ ? timeSource_() : monotonic_ms(); }
void Acceleration::fallback(const std::string& message) {
  gpu_.reset();
  status_.active = "cpu";
  status_.state = "fallback";
  status_.reason = message;
  if (geometry_.clutterSamples) {
    clutterDisabled_ = true;
    clutterStatus_.active = "cpu";
    clutterStatus_.state = "fallback";
    clutterStatus_.reason = message;
  }
  reference_.clear(); surveillance_.clear(); output_.clear();
  std::cerr << "Acceleration: " << message << '\n';
}
void Acceleration::disableAmbiguity(const std::string& message) {
  ambiguityDisabled_ = true;
  status_.active = "cpu";
  status_.state = "fallback";
  status_.reason = message;
  reference_.clear(); surveillance_.clear(); output_.clear();
  std::cerr << "Acceleration: " << message << '\n';
}
bool Acceleration::processClutter(const IqData& reference,
    const std::vector<IqData*>& surveillance, const ClutterCpuRun& cpuRun) {
  if (combinedPending_)
    throw std::logic_error("GPU clutter frame was not followed by ambiguity processing");
  clutterTiming_ = {};
  const auto runCpu = [&] {
    clutterTiming_.cpuExecuted = true;
    return cpuRun();
  };
  if (!geometry_.clutterSamples || !gpu_ || clutterDisabled_) return runCpu();
  auto* backend = dynamic_cast<GpuClutterFrameBackend*>(gpu_.get());
  if (!backend || !backend->clutterAvailable()) {
    clutterDisabled_ = true;
    clutterStatus_.state = "fallback";
    clutterStatus_.reason = "GPU clutter processing is unavailable; using CPU clutter";
    return runCpu();
  }
  const size_t samples = geometry_.clutterSamples;
  if (reference.view_data().size() < samples || surveillance.size() != geometry_.channels)
    throw std::invalid_argument("Incomplete input for GPU clutter processing");
  for (const auto* channel : surveillance)
    if (!channel || channel->view_data().size() < samples)
      throw std::invalid_argument("Surveillance CPI is shorter than GPU clutter input");
  const double gpuStart = now_ms();
  try {
    auto frame = backend->clutterBuffers();
    if (!frame.reference || !frame.surveillance || !frame.output ||
        frame.referenceCount != samples ||
        frame.surveillanceCount != samples * geometry_.channels ||
        frame.outputCount != samples * geometry_.channels)
      throw std::runtime_error("GPU clutter shared-frame dimensions changed; using CPU");
    const auto& source = reference.view_data();
    for (size_t i = 0; i < samples; ++i) {
      const int64_t shifted = (int64_t(i) - geometry_.clutterDelayMin) % int64_t(samples);
      frame.reference[i] = source[shifted < 0 ? shifted + samples : shifted];
    }
    for (size_t channel = 0; channel < surveillance.size(); ++channel) {
      const auto& input = surveillance[channel]->view_data();
      for (size_t i = 0; i < samples; ++i)
        frame.surveillance[channel * samples + i] = input[i];
    }
    const double dispatchStart = now_ms();
    clutterTiming_.prepareMs = dispatchStart - gpuStart;
    if (!backend->processClutterFrame()) {
      clutterDisabled_ = true;
      clutterStatus_.active = "cpu";
      clutterStatus_.state = "fallback";
      clutterStatus_.reason = "GPU clutter precision guard rejected the frame; using CPU clutter";
      return runCpu();
    }
    const double acceptStart = now_ms();
    clutterTiming_.dispatchMs = acceptStart - dispatchStart;
    clutterTiming_.gpuExecuted = true;
    if (!std::all_of(frame.output, frame.output + frame.outputCount, [](auto value) {
          return std::isfinite(value.real()) && std::isfinite(value.imag());
        })) throw std::runtime_error("GPU returned invalid clutter data; using CPU");
    std::vector<std::deque<std::complex<double>>> candidate(surveillance.size());
    for (size_t channel = 0; channel < surveillance.size(); ++channel) {
      const auto& original = surveillance[channel]->view_data();
      for (size_t i = 0; i < samples; ++i)
        candidate[channel].push_back(original[i] -
          std::complex<double>(frame.output[channel * samples + i]));
    }
    clutterTiming_.acceptMs = now_ms() - acceptStart;
    const double gpuMs = now_ms() - gpuStart;
    const bool qualifying = clutterChecks_ < ClutterQualificationFrames;
    const bool combinedQualifying = !qualifying &&
      combinedChecks_ < CombinedQualificationFrames;
    // Full CPU comparisons are startup qualification for this instance's
    // device and geometry, never recurring work on accepted steady frames.
    if (qualifying || combinedQualifying) {
      const double cpuStart = now_ms();
      const bool success = runCpu();
      const double cpuMs = now_ms() - cpuStart;
      if (!success) return false;
      bool invalid = false;
      double worstInputRms = 0, worstFilteredRms = 0;
      for (size_t channel = 0; channel < surveillance.size(); ++channel) {
        double error = 0, filtered = 0, input = 0;
        double maxError = 0, maxFiltered = 0, maxInput = 0;
        const auto& expected = surveillance[channel]->view_data();
        for (size_t i = 0; i < samples; ++i) {
          const std::complex<double> actual = candidate[channel][i];
          const double delta = std::norm(actual - expected[i]);
          error += delta; filtered += std::norm(expected[i]);
          input += std::norm(frame.surveillance[channel * samples + i]);
          maxError = std::max(maxError, delta);
          maxFiltered = std::max(maxFiltered, std::norm(expected[i]));
          maxInput = std::max(maxInput,
            double(std::norm(frame.surveillance[channel * samples + i])));
        }
        const double filteredFloor = std::max(filtered, input * 1e-8);
        const double peakFloor = std::max(maxFiltered, maxInput * 1e-8);
        invalid = invalid || error > std::max(input * 1e-8, 1e-20) ||
          error > std::max(filteredFloor * 1e-8, 1e-20) ||
          maxError > std::max(maxInput * 1e-8, 1e-20) ||
          maxError > std::max(peakFloor * 1e-8, 1e-20);
        worstInputRms = std::max(worstInputRms,
          std::sqrt(error / std::max(input, 1e-30)));
        worstFilteredRms = std::max(worstFilteredRms,
          std::sqrt(error / std::max(filteredFloor, 1e-30)));
      }
      if (invalid) {
        clutterDisabled_ = true;
        clutterStatus_.active = "cpu";
        clutterStatus_.state = "fallback";
        clutterStatus_.reason = "GPU clutter accuracy check failed; using CPU clutter";
        std::cerr << "GPU clutter validation worst_channel_relative_input_rms="
          << worstInputRms << " worst_channel_relative_filtered_rms=" << worstFilteredRms
          << '\n';
        return true;
      }
      clutterCpuTimes_.push_back(cpuMs); clutterGpuTimes_.push_back(gpuMs);
      clutterStatus_.cpuMs = median(clutterCpuTimes_);
      clutterStatus_.gpuMs = median(clutterGpuTimes_);
      if (qualifying && ++clutterChecks_ == ClutterQualificationFrames) {
        if (clutterStatus_.requested == "auto" &&
            clutterStatus_.gpuMs >= clutterStatus_.cpuMs * .95) {
          clutterDisabled_ = true;
          clutterStatus_.active = "cpu";
          clutterStatus_.state = "fallback";
          clutterStatus_.reason = "CPU clutter is faster for these settings";
        } else {
          clutterStatus_.state = "checking";
          clutterStatus_.reason = "Checking composed GPU clutter and ambiguity output";
        }
      }
      if (!qualifying && !clutterDisabled_) {
        cpuClutter_.clear(); cpuClutter_.reserve(surveillance.size());
        for (const auto* channel : surveillance)
          cpuClutter_.emplace_back(channel->view_data());
        for (size_t channel = 0; channel < surveillance.size(); ++channel)
          surveillance[channel]->replace(std::move(candidate[channel]));
        combinedPending_ = true;
      }
      return true;
    }
    for (size_t channel = 0; channel < surveillance.size(); ++channel)
      surveillance[channel]->replace(std::move(candidate[channel]));
    clutterStatus_.gpuMs = gpuMs;
    return true;
  } catch (const std::exception& error) {
    // A startup CPU oracle may have already mutated part of the
    // frame before throwing. It is not safe to run that callback a second time.
    if (clutterTiming_.cpuExecuted) throw;
    fallback(error.what());
    return runCpu();
  }
}
void Acceleration::process(const std::deque<std::complex<double>>& reference,
  const std::vector<IqData*>& surveillance, const std::vector<Ambiguity*>& cpu,
  const CpuRun& cpuRun) {
  if (!gpu_) { cpuRun(); return; }
  if (ambiguityDisabled_) {
    if (!combinedPending_) { cpuRun(); return; }
    if (cpuClutter_.size() != surveillance.size() || cpu.size() != surveillance.size())
      throw std::runtime_error("CPU composed clutter oracle dimensions changed");
    // With CPU ambiguity selected, qualify GPU clutter composition by running
    // the same CPU ambiguity first on GPU-filtered IQ, then on the saved FP64
    // CPU-filtered IQ. The second result owns the frame delivered downstream.
    cpuRun();
    std::vector<std::vector<std::vector<std::complex<double>>>> candidateMaps;
    candidateMaps.reserve(cpu.size());
    for (const auto* processor : cpu) candidateMaps.push_back(processor->result()->data);
    for (size_t channel = 0; channel < surveillance.size(); ++channel)
      surveillance[channel]->replace(std::move(cpuClutter_[channel]));
    cpuClutter_.clear();
    combinedPending_ = false;
    cpuRun();
    bool invalid = false;
    double worstRms = 0, worstPeak = 0;
    for (size_t channel = 0; channel < cpu.size(); ++channel) {
      const auto& expected = cpu[channel]->result()->data;
      const auto& actual = candidateMaps[channel];
      if (actual.size() != expected.size()) throw std::runtime_error("CPU composed map rows changed");
      double signal = 0, error = 0, peak = 0, maxError = 0;
      for (size_t row = 0; row < expected.size(); ++row) {
        if (actual[row].size() != expected[row].size())
          throw std::runtime_error("CPU composed map columns changed");
        for (size_t column = 0; column < expected[row].size(); ++column) {
          const double delta = std::norm(actual[row][column] - expected[row][column]);
          error += delta; signal += std::norm(expected[row][column]);
          maxError = std::max(maxError, delta);
          peak = std::max(peak, std::norm(expected[row][column]));
        }
      }
      invalid = invalid || error > std::max(signal * 1e-8, 1e-20) ||
        maxError > std::max(peak * 1e-8, 1e-20);
      worstRms = std::max(worstRms, std::sqrt(error / std::max(signal, 1e-30)));
      worstPeak = std::max(worstPeak, std::sqrt(maxError / std::max(peak, 1e-30)));
    }
    if (invalid) {
      clutterDisabled_ = true;
      clutterStatus_.active = "cpu";
      clutterStatus_.state = "fallback";
      clutterStatus_.reason = "GPU clutter with CPU ambiguity accuracy check failed; using CPU clutter";
      std::cerr << "GPU clutter/CPU ambiguity validation relative_rms=" << worstRms
        << " relative_peak=" << worstPeak << '\n';
      return;
    }
    if (++combinedChecks_ == CombinedQualificationFrames) {
      if (clutterStatus_.requested == "auto" &&
          clutterStatus_.gpuMs >= clutterStatus_.cpuMs * .95) {
        clutterDisabled_ = true;
        clutterStatus_.active = "cpu";
        clutterStatus_.state = "fallback";
        clutterStatus_.reason = "CPU clutter is faster for these settings";
      } else {
        clutterStatus_.active = "vulkan";
        clutterStatus_.state = "ready";
        clutterStatus_.reason.clear();
      }
    }
    return;
  }
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
    const size_t surveillanceElements = rangeElements * geometry_.channels;
    const size_t outputElements = size_t(geometry_.doppler) * geometry_.delays * geometry_.channels;
    auto* frameBackend = dynamic_cast<GpuFrameBackend*>(gpu_.get());
    GpuFrameBuffers frame;
    std::complex<float>* referenceData;
    std::complex<float>* surveillanceData;
    if (frameBackend) {
      frame = frameBackend->frameBuffers();
      if (!frame.reference || !frame.surveillance || !frame.output ||
          frame.referenceCount != rangeElements ||
          frame.surveillanceCount != surveillanceElements ||
          frame.outputCount != outputElements)
        throw std::runtime_error("GPU shared-frame dimensions changed; using CPU");
      referenceData = frame.reference;
      surveillanceData = frame.surveillance;
    } else {
      reference_.resize(rangeElements);
      surveillance_.resize(surveillanceElements);
      referenceData = reference_.data();
      surveillanceData = surveillance_.data();
    }
    // Fill each channel sequentially and zero only its FFT padding. The old
    // producer zeroed live samples before immediately overwriting them and
    // interleaved writes across every surveillance channel.
    auto referenceSample = reference.begin();
    for (uint32_t d = 0; d < geometry_.doppler; ++d) {
      const size_t offset = size_t(d) * geometry_.range;
      for (uint32_t r = 0; r < correlation_; ++r) {
        const size_t sample = size_t(d) * correlation_ + r;
        auto value = *referenceSample++;
        if (middle_ != 0)
          value *= std::polar(1.0, 2 * std::acos(-1.0) * middle_ * sample / sampleRate_);
        referenceData[offset + r] = value;
      }
      std::fill(referenceData + offset + correlation_,
        referenceData + offset + geometry_.range, std::complex<float>{});
    }
    for (size_t channel = 0; channel < surveillance.size(); ++channel) {
      auto source = surveillance[channel]->view_data().begin();
      auto* destination = surveillanceData + channel * rangeElements;
      for (uint32_t d = 0; d < geometry_.doppler; ++d) {
        const size_t offset = size_t(d) * geometry_.range;
        std::copy_n(source, correlation_, destination + offset);
        source += correlation_;
        std::fill(destination + offset + correlation_,
          destination + offset + geometry_.range, std::complex<float>{});
      }
    }
    const std::complex<float>* outputData;
    size_t outputCount;
    if (frameBackend) {
      frameBackend->processFrame();
      outputData = frame.output;
      outputCount = frame.outputCount;
    } else {
      gpu_->process(reference_, surveillance_, output_);
      outputData = output_.data();
      outputCount = output_.size();
    }
    const size_t perChannel = size_t(geometry_.doppler) * geometry_.delays;
    if (outputCount != perChannel * geometry_.channels ||
        !std::all_of(outputData, outputData + outputCount, [](auto value) {
          return std::isfinite(value.real()) && std::isfinite(value.imag());
        })) throw std::runtime_error("GPU returned invalid radar data; using CPU");
    // Include conversion into the normal map representation in the comparison.
    for (size_t channel = 0; channel < cpu.size(); ++channel)
      cpu[channel]->import_gpu(outputData + channel * perChannel);
    const double gpuMs = now_ms() - gpuStart;
    if (combinedPending_) {
      if (cpuClutter_.size() != surveillance.size())
        throw std::runtime_error("CPU clutter oracle dimensions changed");
      for (size_t channel = 0; channel < surveillance.size(); ++channel)
        surveillance[channel]->replace(std::move(cpuClutter_[channel]));
      cpuClutter_.clear();
      combinedPending_ = false;
      cpuStarted = true;
      cpuRun();
      bool invalid = false;
      double worstRms = 0, worstPeak = 0;
      for (size_t channel = 0; channel < cpu.size(); ++channel) {
        double signal = 0, error = 0, peak = 0, maxError = 0;
        for (uint32_t d = 0; d < geometry_.doppler; ++d) {
          const auto row = cpu[channel]->result()->get_row(d);
          for (uint32_t r = 0; r < geometry_.delays; ++r) {
            const std::complex<double> value = outputData[channel * perChannel +
              size_t(r) * geometry_.doppler +
              (d + geometry_.doppler / 2 + 1) % geometry_.doppler];
            const double delta = std::norm(value - row[r]);
            signal += std::norm(row[r]); error += delta;
            peak = std::max(peak, std::norm(row[r]));
            maxError = std::max(maxError, delta);
          }
        }
        invalid = invalid || error > std::max(signal * 1e-8, 1e-20) ||
          maxError > std::max(peak * 1e-8, 1e-20);
        worstRms = std::max(worstRms,
          std::sqrt(error / std::max(signal, 1e-30)));
        worstPeak = std::max(worstPeak,
          std::sqrt(maxError / std::max(peak, 1e-30)));
      }
      if (invalid) {
        clutterDisabled_ = true;
        clutterStatus_.active = "cpu";
        clutterStatus_.state = "fallback";
        clutterStatus_.reason = "Composed GPU clutter/ambiguity accuracy check failed; using CPU clutter";
        std::cerr << "GPU composed validation relative_rms=" << worstRms
          << " relative_peak=" << worstPeak << '\n';
        return;
      }
      if (++combinedChecks_ == CombinedQualificationFrames) {
        if (clutterStatus_.requested == "auto" &&
            clutterStatus_.gpuMs >= clutterStatus_.cpuMs * .95) {
          clutterDisabled_ = true;
          clutterStatus_.active = "cpu";
          clutterStatus_.state = "fallback";
          clutterStatus_.reason = "CPU clutter is faster for these settings";
        } else {
          clutterStatus_.active = "vulkan";
          clutterStatus_.state = "ready";
          clutterStatus_.reason.clear();
        }
      }
      return;
    }
    if (checks_ < AmbiguityQualificationFrames) {
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
            const std::complex<double> value = outputData[channel * perChannel +
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
          disableAmbiguity("GPU accuracy check failed; using CPU"); return;
        }
      }
      cpuTimes_.push_back(cpuMs); gpuTimes_.push_back(gpuMs);
      status_.cpuMs = median(cpuTimes_); status_.gpuMs = median(gpuTimes_);
      if (++checks_ == AmbiguityQualificationFrames) {
        if (status_.requested == "auto" && status_.gpuMs >= status_.cpuMs * .95) {
          disableAmbiguity("CPU is faster for these settings"); return;
        }
        status_.active = "vulkan"; status_.state = "ready"; status_.reason.clear();
        std::cout << "Acceleration: Vulkan / " << status_.device << '\n';
      }
      return;
    }
    inputRetired = true; // No CPU retry once any input retirement has begun.
    for (auto* input : surveillance)
      input->discard_front(static_cast<uint32_t>(samples));
    // Qualification measures GPU transform/import before retirement because
    // CPU owns those frames for accuracy comparison. AUTO additionally checks
    // the complete accepted GPU path below; it is not a whole-pipeline tuner.
    // The frozen recorded-IQ benchmark campaign predates this safeguard.
    if (status_.requested == "auto") {
      sustainedGpuTimes_.push_back(now_ms() - gpuStart);
      status_.gpuMs = median(sustainedGpuTimes_);
      if (sustainedGpuTimes_.size() == 5) {
        if (status_.gpuMs >= status_.cpuMs * .95)
          disableAmbiguity("CPU was faster than sustained GPU processing");
        else
          sustainedGpuTimes_.clear();
      }
    }
  } catch (const std::exception& error) {
    if (combinedPending_) {
      for (size_t channel = 0; channel < surveillance.size() &&
          channel < cpuClutter_.size(); ++channel)
        surveillance[channel]->replace(std::move(cpuClutter_[channel]));
      cpuClutter_.clear(); combinedPending_ = false;
    }
    if (cpuStarted || inputRetired) throw; // Never rerun an already-consumed frame.
    // GPU never consumes input: this exact frame can safely be rerun on CPU.
    fallback(error.what());
    cpuRun();
  }
}
}
