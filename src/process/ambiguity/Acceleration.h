#pragma once
#include "Ambiguity.h"
#include "GpuBackend.h"
#include "AccelerationStatus.h"
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
    TimeSource timeSource = {});
  ~Acceleration();
  void process(const std::deque<std::complex<double>>& reference,
    const std::vector<IqData*>& surveillance, const std::vector<Ambiguity*>& cpu,
    const CpuRun& cpuRun);
  bool processClutter(const IqData& reference, const std::vector<IqData*>& surveillance,
    const ClutterCpuRun& cpuRun);
  const AccelerationStatus& status() const { return status_; }
  const AccelerationStatus& clutterStatus() const { return clutterStatus_; }
  const ClutterTiming& clutterTiming() const { return clutterTiming_; }
private:
  void fallback(const std::string& message);
  void disableAmbiguity(const std::string& message);
  double now_ms() const;
  GpuGeometry geometry_;
  uint32_t correlation_, sampleRate_;
  double middle_;
  std::unique_ptr<GpuBackend> gpu_;
  AccelerationStatus status_;
  AccelerationStatus clutterStatus_;
  unsigned checks_ = 0;
  std::vector<double> cpuTimes_, gpuTimes_, sustainedGpuTimes_;
  TimeSource timeSource_;
  std::vector<std::complex<float>> reference_, surveillance_, output_;
  unsigned clutterChecks_ = 0;
  unsigned combinedChecks_ = 0;
  bool clutterDisabled_ = false;
  bool ambiguityDisabled_ = false;
  bool combinedPending_ = false;
  std::vector<std::deque<std::complex<double>>> cpuClutter_;
  std::vector<double> clutterCpuTimes_, clutterGpuTimes_;
  ClutterTiming clutterTiming_;
};
}
