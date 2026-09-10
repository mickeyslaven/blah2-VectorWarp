#include "process/ambiguity/GpuProcess.h"
#include <csignal>
#include <sys/socket.h>
#include <unistd.h>
#include <stdexcept>

class Double : public blah2::GpuBackend {
  std::string mode_;
  size_t output_;
public:
  Double(std::string mode, const blah2::GpuGeometry& g)
    : mode_(std::move(mode)), output_(size_t(g.doppler) * g.delays * g.channels) {}
  blah2::GpuDevice device() const override { return {"double", "Isolated test worker", 1}; }
  void process(const std::vector<std::complex<float>>& reference,
      const std::vector<std::complex<float>>&, std::vector<std::complex<float>>& output) override {
    if (mode_ == "hang-frame") for (;;) pause();
    if (mode_ == "crash-frame") raise(SIGSEGV);
    if (mode_ == "error-frame") throw std::runtime_error("Injected GPU failure");
    if (mode_ == "bad-packet") { send(3, "bad", 3, MSG_NOSIGNAL); for (;;) pause(); }
    output.assign(output_, reference.front());
  }
};
int main(int argc, char** argv) {
  const std::string mode = argc > 1 ? argv[1] : "ok";
  return blah2::runGpuWorker([&](const blah2::GpuGeometry& g, const std::string&) {
    if (mode == "hang-init") for (;;) pause();
    if (mode == "crash-init") raise(SIGSEGV);
    if (mode == "error-init") throw std::runtime_error("Injected initialization failure");
    return std::make_unique<Double>(mode, g);
  });
}
