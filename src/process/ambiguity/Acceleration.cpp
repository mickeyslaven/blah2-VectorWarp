#include "Acceleration.h"
#include "GpuProcess.h"
#include "GpuClutterBatch.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <exception>
#include <iostream>
#include <sstream>
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
  TimeSource timeSource, utility::FixedExecutor* executor)
  : geometry_(geometry), correlation_(correlation), sampleRate_(sampleRate), middle_(middle),
    backendFactory_(std::move(factory)), deviceSelector_(std::move(device)),
    timeSource_(std::move(timeSource)), executor_(executor) {
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
    if (backendFactory_) gpu_ = backendFactory_(geometry_, deviceSelector_);
    else gpu_ = createGpuProcess(geometry_, deviceSelector_);
    if (!gpu_) throw std::runtime_error("GPU module did not create a processor");
    status_.device = gpu_->device().id;
    status_.state = "checking";
    status_.reason = "Checking GPU accuracy";
    auto* clutter = dynamic_cast<GpuClutterFrameBackend*>(gpu_.get());
    if (geometry_.clutterSamples && clutter && clutter->clutterAvailable()) {
      clutterStatus_.device = status_.device;
      clutterStatus_.state = "checking";
      clutterStatus_.reason = "Checking GPU clutter accuracy";
    } else if (geometry_.clutterSamples) {
      clutterDisabled_ = true;
      clutterStatus_.state = "fallback";
      clutterStatus_.reason = "GPU clutter processing is unavailable; using CPU clutter";
    }
  } catch (const std::exception& error) { fallback(error.what()); }
}
Acceleration::~Acceleration() = default;
bool Acceleration::startupPending() const {
  return gpu_ && (combinedPending_ ||
    (!ambiguityDisabled_ && checks_ < AmbiguityQualificationFrames) ||
    (geometry_.clutterSamples && !clutterDisabled_ &&
      (clutterChecks_ < ClutterQualificationFrames ||
       combinedChecks_ < CombinedQualificationFrames)));
}
void Acceleration::prepareStartup(
    const std::function<void(unsigned)>& processFrame) {
  for (unsigned frame = 0; startupPending(); ++frame) {
    if (frame >= TotalQualificationFrames)
      throw std::runtime_error("Acceleration startup qualification did not complete");
    processFrame(frame);
  }
}
void Acceleration::retryClutterQualificationAfterStartup() {
  if (!gpu_ || !geometry_.clutterSamples || !clutterDisabled_) return;
  auto* backend = dynamic_cast<GpuClutterFrameBackend*>(gpu_.get());
  if (!backend || !backend->clutterAvailable()) return;
  if (requalifications_ != UINT64_MAX) ++requalifications_;
  clutterDisabled_ = false;
  clutterChecks_ = 0;
  combinedChecks_ = 0;
  clutterCpuTimes_.clear();
  clutterGpuTimes_.clear();
  clutterStatus_.active = "cpu";
  clutterStatus_.state = "checking";
  clutterStatus_.reason =
    "Startup fixture rejected GPU clutter; checking live RF before admission";
}
void Acceleration::beginFrame() { recoverWorkerIfPending(); }
double Acceleration::now_ms() const { return timeSource_ ? timeSource_() : monotonic_ms(); }
namespace {
template<class T> void release_vector(std::vector<T>& value) {
  std::vector<T>().swap(value);
}
}
void Acceleration::fallback(const std::string& message) {
  const bool lostWorker = bool(gpu_) && recoverableWorkerFailure(message);
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
  release_vector(reference_);
  release_vector(surveillance_);
  release_vector(output_);
  release_vector(cpuClutter_);
  combinedPending_ = false;
  recoveryPending_ = lostWorker;
  recoveryCooldownFrames_ = 0;
  std::cerr << "Acceleration: " << message << '\n';
}
bool Acceleration::recoverableWorkerFailure(const std::string& message) {
  return message.find("GPU worker ") != std::string::npos &&
    (message.find("timed out") != std::string::npos ||
     message.find("stopped unexpectedly") != std::string::npos ||
     message.find("connection failed") != std::string::npos ||
     message.find("communication failed") != std::string::npos ||
     message.find("invalid response") != std::string::npos ||
     message.find("wrong frame") != std::string::npos ||
     message.find("no longer available") != std::string::npos);
}
void Acceleration::recoverWorkerIfPending() {
  if (!recoveryPending_ || gpu_) return;
  if (recoveryCooldownFrames_) { --recoveryCooldownFrames_; return; }
  if (requalifications_ != UINT64_MAX) ++requalifications_;
  try {
    auto replacement = backendFactory_ ? backendFactory_(geometry_, deviceSelector_) :
      createGpuProcess(geometry_, deviceSelector_);
    if (!replacement) throw std::runtime_error("GPU module did not create a processor");
    auto* clutter = dynamic_cast<GpuClutterFrameBackend*>(replacement.get());
    const bool clutterReady = !geometry_.clutterSamples ||
      (clutter && clutter->clutterAvailable());
    status_.device = replacement->device().id;
    status_.active = "cpu";
    status_.state = "checking";
    status_.reason = "Numerically checking recovered GPU worker";
    if (!ambiguityDisabled_ && checks_ >= AmbiguityQualificationFrames)
      checks_ = AmbiguityQualificationFrames - 1;
    if (geometry_.clutterSamples) {
      clutterDisabled_ = !clutterReady;
      clutterStatus_.device = status_.device;
      clutterStatus_.active = "cpu";
      clutterStatus_.state = clutterReady ? "checking" : "fallback";
      clutterStatus_.reason = clutterReady ?
        "Numerically checking recovered GPU clutter worker" :
        "Recovered worker has no GPU clutter capability; using CPU clutter";
      if (clutterReady && clutterChecks_ >= ClutterQualificationFrames)
        clutterChecks_ = ClutterQualificationFrames - 1;
    }
    gpu_ = std::move(replacement);
    recoveryPending_ = false;
    recoveryFailures_ = 0;
    recoveryCooldownFrames_ = 0;
    std::cerr << "Acceleration: recreated GPU worker " << status_.device
      << "; numerical requalification required\n";
  } catch (const std::exception& error) {
    ++recoveryFailures_;
    recoveryCooldownFrames_ = 1u << std::min(recoveryFailures_, 4u);
    status_.reason = std::string("GPU worker recovery failed: ") + error.what();
    if (geometry_.clutterSamples) clutterStatus_.reason = status_.reason;
    std::cerr << "Acceleration: " << status_.reason << "; retry in "
      << recoveryCooldownFrames_ << " frames\n";
  }
}
void Acceleration::disableAmbiguity(const std::string& message) {
  ambiguityDisabled_ = true;
  status_.active = "cpu";
  status_.state = "fallback";
  status_.reason = message;
  release_vector(reference_);
  release_vector(surveillance_);
  release_vector(output_);
  if (!combinedPending_) release_vector(cpuClutter_);
  // A worker with no remaining eligible stage can release its
  // process and device allocations immediately instead of retaining idle GPU
  // memory for the rest of the capture session.
  if (!geometry_.clutterSamples || clutterDisabled_) gpu_.reset();
  std::cerr << "Acceleration: " << message << '\n';
}
bool Acceleration::processClutter(const IqData& reference,
    const std::vector<IqData*>& surveillance, const ClutterCpuRun& cpuRun) {
  return processClutter(reference, surveillance, cpuRun, {});
}
bool Acceleration::processClutter(const IqData& reference,
    const std::vector<IqData*>& surveillance, const ClutterCpuRun& cpuRun,
    const CpuRun& hostWork) {
  recoverWorkerIfPending();
  if (combinedPending_)
    throw std::logic_error("GPU clutter frame was not followed by ambiguity processing");
  clutterTiming_ = {};
  clutterStatus_.gpuExecutionBackend.clear();
  bool hostWorkAttempted = false;
  std::exception_ptr hostWorkFailure;
  const auto runHostWork = [&] {
    if (hostWorkAttempted) return;
    hostWorkAttempted = true;
    try { if (hostWork) hostWork(); }
    catch (...) { hostWorkFailure = std::current_exception(); }
  };
  const auto requireHostWork = [&] {
    runHostWork();
    if (hostWorkFailure) std::rethrow_exception(hostWorkFailure);
  };
  const auto runCpu = [&] {
    requireHostWork();
    clutterTiming_.cpuExecuted = true;
    return cpuRun();
  };
  if (!geometry_.clutterSamples || !gpu_ || clutterDisabled_) return runCpu();
  auto* backend = dynamic_cast<GpuClutterFrameBackend*>(gpu_.get());
  if (!backend || !backend->clutterAvailable()) {
    clutterDisabled_ = true;
    clutterStatus_.active = "cpu";
    clutterStatus_.state = "fallback";
    clutterStatus_.reason = "GPU clutter processing is unavailable; using CPU clutter";
    return runCpu();
  }
  const size_t samples = geometry_.clutterSamples;
  if (reference.sample_view().size() < samples || surveillance.size() != geometry_.channels)
    throw std::invalid_argument("Incomplete input for GPU clutter processing");
  for (const auto* channel : surveillance)
    if (!channel || channel->sample_view().size() < samples)
      throw std::invalid_argument("Surveillance CPI is shorter than GPU clutter input");
  const double gpuStart = now_ms();
  bool inputCommitted = false;
  try {
    auto frame = backend->clutterBuffers();
    if (!frame.reference || !frame.surveillance || !frame.output ||
        frame.referenceCount != samples ||
        frame.surveillanceCount != samples * geometry_.channels ||
        frame.outputCount != samples * geometry_.channels)
      throw std::runtime_error("GPU clutter shared-frame dimensions changed; using CPU");
    const auto& source = reference.sample_view();
    copyClutterReference(source, frame.reference, samples, geometry_.clutterDelayMin);
    for (size_t channel = 0; channel < surveillance.size(); ++channel) {
      const auto& input = surveillance[channel]->sample_view();
      input.copy_to(frame.surveillance + channel * samples, samples);
    }
    const double dispatchStart = now_ms();
    clutterTiming_.prepareMs = dispatchStart - gpuStart;
    auto* overlap = dynamic_cast<GpuClutterHostWorkBackend*>(gpu_.get());
    // The worker sees copied buffers. Host work reads the unchanged FP64 IQ;
    // even a failed callback is drained before any result or fallback commits.
    bool clutterAccepted;
    if (hostWork && overlap)
      clutterAccepted = overlap->processClutterFrameWithHostWork(runHostWork);
    else {
      requireHostWork();
      clutterAccepted = backend->processClutterFrame();
    }
    // A normal clutterRejected reply is still completed GPU work; execution
    // telemetry describes work performed, independently of output admission.
    clutterTiming_.gpuExecuted = true;
    clutterStatus_.gpuExecutionBackend = "vulkan";
    requireHostWork();
    if (!clutterAccepted) {
      clutterDisabled_ = true;
      clutterStatus_.active = "cpu";
      clutterStatus_.state = "fallback";
      clutterStatus_.reason = "GPU clutter unavailable or precision guard rejected the frame; using CPU clutter";
      return runCpu();
    }
    const double acceptStart = now_ms();
    clutterTiming_.dispatchMs = acceptStart - dispatchStart;
    if (!std::all_of(frame.output, frame.output + frame.outputCount, [](auto value) {
          return std::isfinite(value.real()) && std::isfinite(value.imag());
        })) throw std::runtime_error("GPU returned invalid clutter data; using CPU");
    const bool qualifying = clutterChecks_ < ClutterQualificationFrames;
    const bool combinedQualifying = !qualifying &&
      combinedChecks_ < CombinedQualificationFrames;
    if (!qualifying && !combinedQualifying) {
      // The complete estimate and every input shape were checked above. Keep
      // FP64 cancellation, but reuse owned IQ storage instead of constructing
      // another full-CPI deque solely to swap it into place.
      inputCommitted = true;
      for (size_t channel = 0; channel < surveillance.size(); ++channel)
        surveillance[channel]->subtract_clutter(frame.output + channel * samples,
          static_cast<uint32_t>(samples));
      clutterTiming_.acceptMs = now_ms() - acceptStart;
      clutterStatus_.gpuMs = now_ms() - gpuStart;
      return true;
    }
    std::vector<std::deque<std::complex<double>>> candidate(surveillance.size());
    for (size_t channel = 0; channel < surveillance.size(); ++channel) {
      const auto& original = surveillance[channel]->sample_view();
      for (size_t i = 0; i < samples; ++i)
        candidate[channel].push_back(original[i] -
          std::complex<double>(frame.output[channel * samples + i]));
    }
    clutterTiming_.acceptMs = now_ms() - acceptStart;
    const double gpuMs = now_ms() - gpuStart;
    // Full CPU comparisons are startup qualification for this instance's
    // device and geometry, never recurring work on accepted steady frames.
    if (qualifying || combinedQualifying) {
      const double cpuStart = now_ms();
      const bool success = runCpu();
      const double cpuMs = now_ms() - cpuStart;
      if (!success) return false;
      bool invalid = false;
      double worstInputRms = 0, worstFilteredRms = 0;
      std::ostringstream failures;
      for (size_t channel = 0; channel < surveillance.size(); ++channel) {
        double error = 0, filtered = 0, input = 0;
        double maxError = 0, maxFiltered = 0, maxInput = 0;
        size_t maxErrorSample = 0;
        const auto& expected = surveillance[channel]->sample_view();
        for (size_t i = 0; i < samples; ++i) {
          const std::complex<double> actual = candidate[channel][i];
          const double delta = std::norm(actual - expected[i]);
          error += delta; filtered += std::norm(expected[i]);
          input += std::norm(frame.surveillance[channel * samples + i]);
          if (delta > maxError) { maxError = delta; maxErrorSample = i; }
          maxFiltered = std::max(maxFiltered, std::norm(expected[i]));
          maxInput = std::max(maxInput,
            double(std::norm(frame.surveillance[channel * samples + i])));
        }
        const double filteredFloor = std::max(filtered, input * 1e-8);
        const double peakFloor = std::max(maxFiltered, maxInput * 1e-8);
        const bool channelInvalid = error > std::max(input * 1e-8, 1e-20) ||
          error > std::max(filteredFloor * 1e-8, 1e-20) ||
          maxError > std::max(maxInput * 1e-8, 1e-20) ||
          maxError > std::max(peakFloor * 1e-8, 1e-20);
        invalid = invalid || channelInvalid;
        if (channelInvalid) {
          failures << " [channel=" << channel
            << " relative_input_rms=" << std::sqrt(error / std::max(input, 1e-30))
            << " relative_filtered_rms=" << std::sqrt(error / std::max(filteredFloor, 1e-30))
            << " relative_input_peak=" << std::sqrt(maxError / std::max(maxInput, 1e-30))
            << " relative_filtered_peak=" << std::sqrt(maxError / std::max(peakFloor, 1e-30))
            << " peak_sample=" << maxErrorSample
            << " input_rms=" << std::sqrt(input / samples)
            << " filtered_rms=" << std::sqrt(filtered / samples) << ']';
        }
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
          << " samples=" << samples << " bins=" << geometry_.clutterBins
          << " qualification_frame=" << clutterChecks_ + combinedChecks_ + 1
          << failures.str() << '\n';
        return true;
      }
      clutterCpuTimes_.push_back(cpuMs); clutterGpuTimes_.push_back(gpuMs);
      clutterStatus_.cpuMs = median(clutterCpuTimes_);
      clutterStatus_.gpuMs = median(clutterGpuTimes_);
      if (qualifying && ++clutterChecks_ == ClutterQualificationFrames) {
        if (combinedChecks_ >= CombinedQualificationFrames) {
          clutterStatus_.active = "vulkan";
          clutterStatus_.state = "ready";
          clutterStatus_.reason.clear();
        } else {
          clutterStatus_.state = "checking";
          clutterStatus_.reason = "Checking composed GPU clutter and ambiguity output";
        }
      }
      if (!qualifying && !clutterDisabled_) {
        cpuClutter_.clear(); cpuClutter_.reserve(surveillance.size());
        for (const auto* channel : surveillance) {
          const auto source = channel->sample_view();
          cpuClutter_.emplace_back(source.begin(), source.end());
        }
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
    if (hostWorkFailure) std::rethrow_exception(hostWorkFailure);
    // A startup CPU oracle may have already mutated part of the
    // frame before throwing. It is not safe to run that callback a second time.
    if (clutterTiming_.cpuExecuted || inputCommitted) throw;
    fallback(error.what());
    return runCpu();
  }
}
void Acceleration::process(const std::deque<std::complex<double>>& reference,
  const std::vector<IqData*>& surveillance, const std::vector<Ambiguity*>& cpu,
  const CpuRun& cpuRun) {
  process(IqSampleView(reference), surveillance, cpu, cpuRun);
}
void Acceleration::process(IqSampleView reference,
  const std::vector<IqData*>& surveillance, const std::vector<Ambiguity*>& cpu,
  const CpuRun& cpuRun) {
  processInput(reference, surveillance, nullptr, {}, 0, 0, cpu, cpuRun);
}
void Acceleration::processBorrowed(
    const std::complex<double>* rotatedReference,
    const std::vector<const std::complex<double>*>& filteredSurveillance,
    uint32_t fullSamples, int32_t referenceRotation,
    const std::vector<Ambiguity*>& cpu, const CpuRun& cpuRun) {
  static const std::deque<std::complex<double>> emptyReference;
  std::vector<IqData*> placeholders(filteredSurveillance.size(), nullptr);
  processInput(emptyReference, placeholders, rotatedReference,
    filteredSurveillance, fullSamples, referenceRotation, cpu, cpuRun);
}
void Acceleration::processInput(
    IqSampleView reference,
    const std::vector<IqData*>& surveillance,
    const std::complex<double>* borrowedReference,
    const std::vector<const std::complex<double>*>& borrowedSurveillance,
    uint32_t borrowedSamples, int32_t borrowedRotation,
    const std::vector<Ambiguity*>& cpu, const CpuRun& cpuRun) {
  if (!geometry_.clutterSamples) recoverWorkerIfPending();
  const bool borrowed = borrowedReference != nullptr;
  if (borrowed && (clutterAvailable() || combinedPending_))
    throw std::invalid_argument(
      "Borrowed ambiguity input requires rejected or unavailable GPU clutter");
  ambiguityGpuExecuted_ = false;
  status_.gpuExecutionBackend.clear();
  ambiguityCpuExecuted_ = false;
  const auto runCpu = [&] {
    ambiguityCpuExecuted_ = true;
    cpuRun();
  };
  if (!gpu_) { runCpu(); return; }
  if (ambiguityDisabled_) {
    if (!combinedPending_) { runCpu(); return; }
    if (cpuClutter_.size() != surveillance.size() || cpu.size() != surveillance.size())
      throw std::runtime_error("CPU composed clutter oracle dimensions changed");
    // With CPU ambiguity selected, qualify GPU clutter composition by running
    // the same CPU ambiguity first on GPU-filtered IQ, then on the saved FP64
    // CPU-filtered IQ. The second result owns the frame delivered downstream.
    runCpu();
    std::vector<std::vector<std::vector<std::complex<double>>>> candidateMaps;
    candidateMaps.reserve(cpu.size());
    for (const auto* processor : cpu) candidateMaps.push_back(processor->result()->data);
    for (size_t channel = 0; channel < surveillance.size(); ++channel)
      surveillance[channel]->replace(std::move(cpuClutter_[channel]));
    cpuClutter_.clear();
    combinedPending_ = false;
    runCpu();
    bool invalid = false;
    double worstRms = 0, worstPeak = 0;
    std::ostringstream failures;
    for (size_t channel = 0; channel < cpu.size(); ++channel) {
      const auto& expected = cpu[channel]->result()->data;
      const auto& actual = candidateMaps[channel];
      if (actual.size() != expected.size()) throw std::runtime_error("CPU composed map rows changed");
      double signal = 0, error = 0, peak = 0, maxError = 0;
      size_t maxErrorRow = 0, maxErrorColumn = 0;
      for (size_t row = 0; row < expected.size(); ++row) {
        if (actual[row].size() != expected[row].size())
          throw std::runtime_error("CPU composed map columns changed");
        for (size_t column = 0; column < expected[row].size(); ++column) {
          const double delta = std::norm(actual[row][column] - expected[row][column]);
          error += delta; signal += std::norm(expected[row][column]);
          if (delta > maxError) {
            maxError = delta; maxErrorRow = row; maxErrorColumn = column;
          }
          peak = std::max(peak, std::norm(expected[row][column]));
        }
      }
      const bool channelInvalid = error > std::max(signal * 1e-8, 1e-20) ||
        maxError > std::max(peak * 1e-8, 1e-20);
      invalid = invalid || channelInvalid;
      if (channelInvalid) {
        failures << " [channel=" << channel
          << " relative_rms=" << std::sqrt(error / std::max(signal, 1e-30))
          << " relative_peak=" << std::sqrt(maxError / std::max(peak, 1e-30))
          << " peak_doppler_row=" << maxErrorRow
          << " peak_delay_column=" << maxErrorColumn << ']';
      }
      worstRms = std::max(worstRms, std::sqrt(error / std::max(signal, 1e-30)));
      worstPeak = std::max(worstPeak, std::sqrt(maxError / std::max(peak, 1e-30)));
    }
    if (invalid) {
      clutterDisabled_ = true;
      clutterStatus_.active = "cpu";
      clutterStatus_.state = "fallback";
      clutterStatus_.reason = "GPU clutter with CPU ambiguity accuracy check failed; using CPU clutter";
      std::cerr << "GPU clutter/CPU ambiguity validation relative_rms=" << worstRms
        << " relative_peak=" << worstPeak
        << " samples=" << geometry_.clutterSamples << " bins=" << geometry_.clutterBins
        << " qualification_frame=" << combinedChecks_ + 1 << failures.str() << '\n';
      return;
    }
    if (++combinedChecks_ == CombinedQualificationFrames) {
      clutterStatus_.active = "vulkan";
      clutterStatus_.state = "ready";
      clutterStatus_.reason.clear();
    }
    return;
  }
  const uint64_t samples = uint64_t(correlation_) * geometry_.doppler;
  if (surveillance.size() != geometry_.channels || cpu.size() != geometry_.channels ||
      (borrowed ? borrowedSamples < samples : reference.size() < samples) ||
      (borrowed && borrowedSurveillance.size() != geometry_.channels))
    throw std::invalid_argument("Incomplete input for GPU radar processing");
  for (size_t channel = 0; channel < surveillance.size(); ++channel)
    if (borrowed ? borrowedSurveillance[channel] == nullptr :
        (!surveillance[channel] ||
          surveillance[channel]->sample_view().size() < samples))
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
    // Reference and surveillance slices have disjoint destinations. Pack them
    // concurrently through the existing bounded process pool; this shortens
    // the host-to-GPU critical path without changing sample order or values.
    const auto pack = [&](size_t job) {
      if (job == 0) {
        auto referenceSample = reference.begin();
        uint32_t borrowedReferenceIndex = 0;
        if (borrowed) {
          const int64_t start = int64_t(borrowedRotation) % borrowedSamples;
          borrowedReferenceIndex = uint32_t(start < 0 ? start + borrowedSamples : start);
        }
        for (uint32_t d = 0; d < geometry_.doppler; ++d) {
          const size_t offset = size_t(d) * geometry_.range;
          for (uint32_t r = 0; r < correlation_; ++r) {
            const size_t sample = size_t(d) * correlation_ + r;
            auto value = borrowed ? borrowedReference[borrowedReferenceIndex] :
              *referenceSample++;
            if (borrowed && ++borrowedReferenceIndex == borrowedSamples)
              borrowedReferenceIndex = 0;
            if (middle_ != 0)
              value *= std::polar(1.0, 2 * std::acos(-1.0) * middle_ * sample / sampleRate_);
            referenceData[offset + r] = value;
          }
          std::fill(referenceData + offset + correlation_,
            referenceData + offset + geometry_.range, std::complex<float>{});
        }
        return;
      }
      const size_t channel = job - 1;
      auto* destination = surveillanceData + channel * rangeElements;
      if (borrowed) {
        const auto* source = borrowedSurveillance[channel];
        for (uint32_t d = 0; d < geometry_.doppler; ++d) {
          const size_t offset = size_t(d) * geometry_.range;
          std::copy_n(source, correlation_, destination + offset);
          source += correlation_;
          std::fill(destination + offset + correlation_,
            destination + offset + geometry_.range, std::complex<float>{});
        }
      } else {
        const auto source = surveillance[channel]->sample_view();
        for (uint32_t d = 0; d < geometry_.doppler; ++d) {
          const size_t offset = size_t(d) * geometry_.range;
          source.copy_to(destination + offset, correlation_, size_t(d) * correlation_);
          std::fill(destination + offset + correlation_,
            destination + offset + geometry_.range, std::complex<float>{});
        }
      }
    };
    const size_t packingJobs = surveillance.size() + 1;
    if (executor_ && packingJobs > 1) executor_->parallel_for(packingJobs, pack);
    else for (size_t job = 0; job < packingJobs; ++job) pack(job);
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
    // A returned frame completed on the GPU even if the numerical/output
    // admission checks below reject it and recompute on CPU.
    ambiguityGpuExecuted_ = true;
    status_.gpuExecutionBackend = "vulkan";
    const size_t perChannel = size_t(geometry_.doppler) * geometry_.delays;
    if (outputCount != perChannel * geometry_.channels ||
        !std::all_of(outputData, outputData + outputCount, [](auto value) {
          return std::isfinite(value.real()) && std::isfinite(value.imag());
        })) throw std::runtime_error("GPU returned invalid radar data; using CPU");
    // Include conversion into the normal map representation in the comparison.
    const auto import = [&](size_t channel) {
      cpu[channel]->import_gpu(outputData + channel * perChannel);
    };
    if (executor_ && cpu.size() > 1) executor_->parallel_for(cpu.size(), import);
    else for (size_t channel = 0; channel < cpu.size(); ++channel) import(channel);
    const double gpuMs = now_ms() - gpuStart;
    if (combinedPending_) {
      if (cpuClutter_.size() != surveillance.size())
        throw std::runtime_error("CPU clutter oracle dimensions changed");
      for (size_t channel = 0; channel < surveillance.size(); ++channel)
        surveillance[channel]->replace(std::move(cpuClutter_[channel]));
      cpuClutter_.clear();
      combinedPending_ = false;
      cpuStarted = true;
      runCpu();
      bool invalid = false;
      double worstRms = 0, worstPeak = 0;
      std::ostringstream failures;
      for (size_t channel = 0; channel < cpu.size(); ++channel) {
        double signal = 0, error = 0, peak = 0, maxError = 0;
        uint32_t maxErrorDoppler = 0, maxErrorDelay = 0;
        for (uint32_t d = 0; d < geometry_.doppler; ++d) {
          const auto row = cpu[channel]->result()->get_row(d);
          for (uint32_t r = 0; r < geometry_.delays; ++r) {
            const std::complex<double> value = outputData[channel * perChannel +
              size_t(r) * geometry_.doppler +
              (d + geometry_.doppler / 2 + 1) % geometry_.doppler];
            const double delta = std::norm(value - row[r]);
            signal += std::norm(row[r]); error += delta;
            peak = std::max(peak, std::norm(row[r]));
            if (delta > maxError) {
              maxError = delta; maxErrorDoppler = d; maxErrorDelay = r;
            }
          }
        }
        const bool channelInvalid = error > std::max(signal * 1e-8, 1e-20) ||
          maxError > std::max(peak * 1e-8, 1e-20);
        invalid = invalid || channelInvalid;
        if (channelInvalid) {
          failures << " [channel=" << channel
            << " relative_rms=" << std::sqrt(error / std::max(signal, 1e-30))
            << " relative_peak=" << std::sqrt(maxError / std::max(peak, 1e-30))
            << " peak_doppler_row=" << maxErrorDoppler
            << " peak_delay_column=" << maxErrorDelay << ']';
        }
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
          << " relative_peak=" << worstPeak
          << " samples=" << geometry_.clutterSamples << " bins=" << geometry_.clutterBins
          << " qualification_frame=" << combinedChecks_ + 1 << failures.str() << '\n';
        return;
      }
      if (++combinedChecks_ == CombinedQualificationFrames) {
        clutterStatus_.active = "vulkan";
        clutterStatus_.state = "ready";
        clutterStatus_.reason.clear();
      }
      return;
    }
    if (checks_ < AmbiguityQualificationFrames) {
      // CPU owns these initial frames. Its independent FP64 result checks every
      // complex bin on every channel before any GPU result can reach detection.
      const double cpuStart = now_ms();
      cpuStarted = true;
      runCpu();
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
        // AUTO retains correctness-qualified GPU work. Timings are diagnostic,
        // not a noisy short-window admission policy or a steady-state demotion.
        status_.active = "vulkan"; status_.state = "ready"; status_.reason.clear();
        std::cout << "Acceleration: Vulkan / " << status_.device << '\n';
      }
      return;
    }
    inputRetired = true; // No CPU retry once any input retirement has begun.
    if (!borrowed)
      for (auto* input : surveillance)
        input->discard_front(static_cast<uint32_t>(samples));
    // Startup qualification compares the same ambiguity work on CPU and GPU.
    // Do not revisit that decision using loaded live-call latency after
    // capture is admitted: worker scheduling and load phase can bias that
    // wall time, while status_.cpuMs is a frozen pre-capture baseline.  The
    // mismatched phases can demote a proven GPU path to a CPU path that cannot
    // sustain the receiver. Runtime GPU faults and invalid results still take
    // the exception fallback above.
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
    runCpu();
  }
}
}
