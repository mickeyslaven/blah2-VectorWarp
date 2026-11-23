#pragma once
#include "Ambiguity.h"
#include "GpuBackend.h"
#include "AccelerationStatus.h"
#include "process/utility/FixedExecutor.h"
#include <functional>

namespace blah2 {
struct ClutterTiming {
  double prepareMs = 0, dispatchMs = 0, acceptMs = 0;
  bool gpuExecuted = false, cpuExecuted = false;
};
class Acceleration {
public:
  static constexpr unsigned AmbiguityQualificationFrames = 3;
  static constexpr unsigned ClutterQualificationFrames = 3;
  static constexpr unsigned CombinedQualificationFrames = 5;
  static constexpr unsigned TotalQualificationFrames =
    AmbiguityQualificationFrames + CombinedQualificationFrames;
  using CpuRun = std::function<void()>;
  using ClutterCpuRun = std::function<bool()>;
  using BackendFactory = std::function<std::unique_ptr<GpuBackend>(const GpuGeometry&, const std::string&)>;
  using TimeSource = std::function<double()>;
  Acceleration(std::string mode, GpuGeometry geometry, uint32_t correlation,
    uint32_t sampleRate, double middle, std::string device = "auto", BackendFactory factory = {},
    TimeSource timeSource = {}, utility::FixedExecutor* executor = nullptr);
  ~Acceleration();
  // Run the existing numerical qualification before admitting capture. The
  // caller supplies private nonzero IQ and must not publish these frames.
  bool startupPending() const;
  void prepareStartup(const std::function<void(unsigned)>& processFrame);
  // A private synthetic fixture can be numerically unsuitable for clutter
  // while the real RF stream is valid. Re-open only the still-live backend;
  // live frames remain CPU-owned until the ordinary oracle checks complete.
  void retryClutterQualificationAfterStartup();
  // Poll transient worker recovery before the caller selects CPU/GPU stage
  // ownership for a new CPI. This must happen outside processClutter(): after
  // a clutter-worker fault the safe CPU borrowed path intentionally stops
  // calling that method, but still needs a route back to the GPU.
  void beginFrame();
  void process(const std::deque<std::complex<double>>& reference,
    const std::vector<IqData*>& surveillance, const std::vector<Ambiguity*>& cpu,
    const CpuRun& cpuRun);
  void process(IqSampleView reference, const std::vector<IqData*>& surveillance,
    const std::vector<Ambiguity*>& cpu, const CpuRun& cpuRun);
  void processBorrowed(const std::complex<double>* rotatedReference,
    const std::vector<const std::complex<double>*>& filteredSurveillance,
    uint32_t fullSamples, int32_t referenceRotation,
    const std::vector<Ambiguity*>& cpu, const CpuRun& cpuRun);
  bool processClutter(const IqData& reference, const std::vector<IqData*>& surveillance,
    const ClutterCpuRun& cpuRun);
  bool processClutter(const IqData& reference, const std::vector<IqData*>& surveillance,
    const ClutterCpuRun& cpuRun, const CpuRun& hostWork);
  const AccelerationStatus& status() const { return status_; }
  const AccelerationStatus& clutterStatus() const { return clutterStatus_; }
  const ClutterTiming& clutterTiming() const { return clutterTiming_; }
  bool available() const { return gpu_ != nullptr; }
  // True while clutter is qualifying or admitted. A rejected clutter stage
  // can retain its independently qualified ambiguity worker.
  bool clutterAvailable() const {
    return gpu_ && geometry_.clutterSamples && !clutterDisabled_;
  }
  bool ambiguityGpuExecuted() const { return ambiguityGpuExecuted_; }
  bool ambiguityCpuExecuted() const { return ambiguityCpuExecuted_; }
  // Diagnostic ownership only; proves a retired backend does not keep large
  // staging allocations resident for the remainder of a live session.
  std::size_t retainedScratchBytes() const {
    return (reference_.capacity() + surveillance_.capacity() + output_.capacity()) *
      sizeof(std::complex<float>) + cpuClutter_.capacity() * sizeof(cpuClutter_[0]);
  }
private:
  void fallback(const std::string& message);
  void recoverWorkerIfPending();
  static bool recoverableWorkerFailure(const std::string& message);
  void disableAmbiguity(const std::string& message);
  void processInput(IqSampleView reference,
    const std::vector<IqData*>& surveillance,
    const std::complex<double>* borrowedReference,
    const std::vector<const std::complex<double>*>& borrowedSurveillance,
    uint32_t borrowedSamples, int32_t borrowedRotation,
    const std::vector<Ambiguity*>& cpu, const CpuRun& cpuRun);
  double now_ms() const;
  GpuGeometry geometry_;
  uint32_t correlation_, sampleRate_;
  double middle_;
  std::unique_ptr<GpuBackend> gpu_;
  BackendFactory backendFactory_;
  std::string deviceSelector_;
  AccelerationStatus status_;
  AccelerationStatus clutterStatus_;
  unsigned checks_ = 0;
  std::vector<double> cpuTimes_, gpuTimes_;
  TimeSource timeSource_;
  // Borrow the canonical process pool. Packing/import jobs are independent
  // channel slices and never outlive processInput(); no private thread pool or
  // receiver/OS-specific scheduling policy is introduced here.
  utility::FixedExecutor* executor_ = nullptr;
  std::vector<std::complex<float>> reference_, surveillance_, output_;
  unsigned clutterChecks_ = 0;
  unsigned combinedChecks_ = 0;
  bool clutterDisabled_ = false;
  bool ambiguityDisabled_ = false;
  bool combinedPending_ = false;
  std::vector<std::deque<std::complex<double>>> cpuClutter_;
  std::vector<double> clutterCpuTimes_, clutterGpuTimes_;
  ClutterTiming clutterTiming_;
  bool ambiguityGpuExecuted_ = false;
  bool ambiguityCpuExecuted_ = false;
  bool recoveryPending_ = false;
  unsigned recoveryFailures_ = 0;
  unsigned recoveryCooldownFrames_ = 0;
};
}
