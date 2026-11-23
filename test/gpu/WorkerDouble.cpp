#include "process/ambiguity/GpuProcess.h"
#include "process/ambiguity/GpuWorkerLifetime.h"
#include <algorithm>
#include <csignal>
#include <cerrno>
#include <cstdlib>
#include <chrono>
#include <cstdio>
#include <fcntl.h>
#include <iostream>
#include <sys/socket.h>
#include <unistd.h>
#include <stdexcept>
#include <thread>

class Double : public blah2::GpuBackend {
  std::string mode_;
  size_t output_;
public:
  Double(std::string mode, const blah2::GpuGeometry& g)
    : mode_(std::move(mode)), output_(size_t(g.doppler) * g.delays * g.channels) {}
  blah2::GpuDevice device() const override { return {"double", "Isolated test worker", 1}; }
  void process(const std::vector<std::complex<float>>& reference,
      const std::vector<std::complex<float>>&, std::vector<std::complex<float>>& output) override {
    if (mode_ == "hang-frame" || mode_ == "orphan-frame") {
      if (mode_ == "orphan-frame") std::cout << getpid() << std::endl;
      for (;;) pause();
    }
    if (mode_ == "crash-frame") raise(SIGSEGV);
    if (mode_ == "error-frame") throw std::runtime_error("Injected GPU failure");
    if (mode_ == "bad-packet") { send(3, "bad", 3, MSG_NOSIGNAL); for (;;) pause(); }
    output.assign(output_, reference.front());
  }
};
class RawDouble : public blah2::GpuBackend, public blah2::GpuBufferBackend,
    public blah2::GpuClutterBufferBackend {
  size_t output_, clutterReference_, clutterSurveillance_;
  std::string mode_;
public:
  RawDouble(std::string mode, const blah2::GpuGeometry& g)
    : output_(size_t(g.doppler) * g.delays * g.channels),
      clutterReference_(g.clutterSamples),
      clutterSurveillance_(size_t(g.clutterSamples) * g.channels), mode_(std::move(mode)) {}
  ~RawDouble() override {
    if(mode_=="raw-profile") {
      std::this_thread::sleep_for(std::chrono::milliseconds(25));
      std::fprintf(stderr,"TEST_PORTABLE_PROFILE_BACKEND_DESTROYED\n");
    }
  }
  blah2::GpuDevice device() const override { return {"raw-double", "Raw isolated test worker", 1}; }
  void process(const std::vector<std::complex<float>>& reference,
      const std::vector<std::complex<float>>&, std::vector<std::complex<float>>& output) override {
    output.assign(output_, reference.front());
  }
  void processBuffers(const std::complex<float>* reference, size_t referenceCount,
      const std::complex<float>*, size_t surveillanceCount,
      std::complex<float>* output, size_t outputCount) override {
    if (!reference || !referenceCount || !surveillanceCount || !output || outputCount != output_)
      throw std::runtime_error("Invalid raw test buffers");
    std::fill_n(output, outputCount, reference[0]);
  }
  bool processClutterBuffers(const std::complex<float>* reference, size_t referenceCount,
      const std::complex<float>* surveillance, size_t surveillanceCount,
      std::complex<float>* output, size_t outputCount) override {
    if (!reference || referenceCount != clutterReference_ || !surveillance ||
        surveillanceCount != clutterSurveillance_ || !output || outputCount != clutterSurveillance_)
      throw std::runtime_error("Invalid raw clutter test buffers");
    if (mode_ == "hang-clutter") for (;;) pause();
    if (mode_ == "reject-clutter") return false;
    // Clutter output is the estimate to subtract, so zero preserves the input.
    std::fill_n(output, outputCount, std::complex<float>{});
    return true;
  }
};
class CapacityDouble final : public RawDouble, public blah2::GpuClutterAvailabilityBackend {
public:
  explicit CapacityDouble(const blah2::GpuGeometry& g) : RawDouble("capacity-clutter", g) {}
  bool clutterAvailable() const override { return false; }
  bool processClutterBuffers(const std::complex<float>*, size_t,
      const std::complex<float>*, size_t, std::complex<float>*, size_t) override {
    throw std::runtime_error("Unavailable clutter pipeline must not be dispatched");
  }
};
int main(int argc, char** argv) {
  const std::string mode = argc > 1 ? argv[1] : "ok";
  return blah2::runGpuWorker([&](const blah2::GpuGeometry& g, const std::string&) {
    if(mode=="diagnostics-live"||mode=="diagnostics-offline"){
      if(std::getenv(blah2::gpu_offline_diagnostics::profileVariable)||
          blah2::gpu_offline_diagnostics::frameLimit()!=(mode=="diagnostics-offline"?2u:0u))
        throw std::runtime_error("GPU worker diagnostic authorization differs");
    }
    if (mode == "budget-set" || mode == "budget-clear") {
      const char* actual = std::getenv("BLAH2_GPU_ALLOCATION_LIMIT_BYTES");
      if ((mode == "budget-set" && (!actual || std::string(actual) != "33554432")) ||
          (mode == "budget-clear" && actual))
        throw std::runtime_error("GPU worker allocation reservation differs");
    }
    if (mode == "fd-closed") {
      const char* inherited = std::getenv("BLAH2_TEST_INHERITED_FD");
      if (!inherited || fcntl(std::atoi(inherited), F_GETFD) >= 0 || errno != EBADF)
        throw std::runtime_error("GPU worker inherited a parent descriptor");
#ifdef __APPLE__
      if (fcntl(4, F_GETFD) >= 0 || errno != EBADF)
        throw std::runtime_error("GPU worker retained a resizable shared-memory descriptor");
#endif
    }
    if (mode == "orphan-init") {
      std::cout << getpid() << std::endl;
      for (;;) pause();
    }
    if (mode == "hang-init") for (;;) pause();
    if (mode == "crash-init") raise(SIGSEGV);
    if (mode == "error-init") throw std::runtime_error("Injected initialization failure");
    if (mode == "capacity-clutter")
      return std::unique_ptr<blah2::GpuBackend>(std::make_unique<CapacityDouble>(g));
    if (mode == "raw" || mode == "raw-profile" || mode == "hang-clutter" || mode == "reject-clutter")
      return std::unique_ptr<blah2::GpuBackend>(std::make_unique<RawDouble>(mode, g));
    return std::unique_ptr<blah2::GpuBackend>(std::make_unique<Double>(mode, g));
  });
}
