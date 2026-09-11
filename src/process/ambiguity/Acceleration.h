#pragma once
#include "Ambiguity.h"
#include "GpuBackend.h"
#include "AccelerationStatus.h"
#include <functional>

namespace blah2 {
class Acceleration {
public:
  using CpuRun = std::function<void()>;
  using BackendFactory = std::function<std::unique_ptr<GpuBackend>(const GpuGeometry&, const std::string&)>;
  using TimeSource = std::function<double()>;
  Acceleration(std::string mode, GpuGeometry geometry, uint32_t correlation,
    uint32_t sampleRate, double middle, std::string device = "auto", BackendFactory factory = {},
    TimeSource timeSource = {});
  ~Acceleration();
  void process(const std::deque<std::complex<double>>& reference,
    const std::vector<IqData*>& surveillance, const std::vector<Ambiguity*>& cpu,
    const CpuRun& cpuRun);
  const AccelerationStatus& status() const { return status_; }
private:
  void fallback(const std::string& message);
  double now_ms() const;
  GpuGeometry geometry_;
  uint32_t correlation_, sampleRate_;
  double middle_;
  std::unique_ptr<GpuBackend> gpu_;
  AccelerationStatus status_;
  unsigned checks_ = 0;
  std::vector<double> cpuTimes_, gpuTimes_, sustainedGpuTimes_;
  TimeSource timeSource_;
  std::vector<std::complex<float>> reference_, surveillance_, output_;
};
}
