#pragma once
#include "GpuBackend.h"
#include "ResidentLaneBudget.h"
#include <memory>
#include <type_traits>

namespace blah2 {
namespace lane_lease {
struct NoFrame {};
struct NoClutter {};
// Advertise exactly the optional interfaces implemented by the wrapped worker.
// Buffer copies and CPU fallback occur outside this device-only lease.
template<bool Frame, bool Clutter>
class Backend final : public GpuBackend,
    public std::conditional_t<Frame, GpuFrameBackend, NoFrame>,
    public std::conditional_t<Clutter, GpuClutterFrameBackend, NoClutter> {
  std::unique_ptr<GpuBackend> backend_;
  std::shared_ptr<resident::LaneBudget> budget_;
public:
  Backend(std::unique_ptr<GpuBackend> backend, std::shared_ptr<resident::LaneBudget> budget)
    : backend_(std::move(backend)), budget_(std::move(budget)) {}
  ~Backend() override {
    std::lock_guard<std::mutex> lease(budget_->execution);
    backend_.reset();
  }
  GpuDevice device() const override { return backend_->device(); }
  void process(const std::vector<std::complex<float>>& reference,
      const std::vector<std::complex<float>>& surveillance,
      std::vector<std::complex<float>>& output) override {
    std::lock_guard<std::mutex> lease(budget_->execution);
    backend_->process(reference, surveillance, output);
  }
  GpuFrameBuffers frameBuffers() {
    if constexpr (Frame) return dynamic_cast<GpuFrameBackend&>(*backend_).frameBuffers();
    else throw std::logic_error("GPU lane has no frame interface");
  }
  void processFrame() {
    std::lock_guard<std::mutex> lease(budget_->execution);
    if constexpr (Frame) dynamic_cast<GpuFrameBackend&>(*backend_).processFrame();
    else throw std::logic_error("GPU lane has no frame interface");
  }
  bool clutterAvailable() const {
    if constexpr (Clutter) return dynamic_cast<GpuClutterFrameBackend&>(*backend_).clutterAvailable();
    else return false;
  }
  GpuClutterBuffers clutterBuffers() {
    if constexpr (Clutter) return dynamic_cast<GpuClutterFrameBackend&>(*backend_).clutterBuffers();
    else throw std::logic_error("GPU lane has no clutter interface");
  }
  bool processClutterFrame() {
    std::lock_guard<std::mutex> lease(budget_->execution);
    if constexpr (Clutter) return dynamic_cast<GpuClutterFrameBackend&>(*backend_).processClutterFrame();
    else throw std::logic_error("GPU lane has no clutter interface");
  }
};
}
inline std::unique_ptr<GpuBackend> leaseGpuLane(std::unique_ptr<GpuBackend> backend,
    std::shared_ptr<resident::LaneBudget> budget) {
  if (!backend || !budget) throw std::invalid_argument("GPU lane lease requires a worker and reservation");
  const bool frame = dynamic_cast<GpuFrameBackend*>(backend.get());
  const bool clutter = dynamic_cast<GpuClutterFrameBackend*>(backend.get());
  if (frame && clutter) return std::make_unique<lane_lease::Backend<true, true>>(std::move(backend), std::move(budget));
  if (frame) return std::make_unique<lane_lease::Backend<true, false>>(std::move(backend), std::move(budget));
  if (clutter) return std::make_unique<lane_lease::Backend<false, true>>(std::move(backend), std::move(budget));
  return std::make_unique<lane_lease::Backend<false, false>>(std::move(backend), std::move(budget));
}
}
