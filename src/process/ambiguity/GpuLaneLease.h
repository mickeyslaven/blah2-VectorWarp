#pragma once
#include "GpuBackend.h"
#include "ResidentLaneBudget.h"
#include "data/meta/StreamProfile.h"
#include <memory>
#include <type_traits>

namespace blah2 {
namespace lane_lease {
struct NoFrame {};
struct NoClutter {};
struct NoHostWork {};
// Preserve the wrapped worker's frame/clutter interfaces. Host-work scheduling
// forwards overlap when available and otherwise runs before the clutter call.
// Buffer copies and CPU fallback occur outside this worker-local transaction.
// Independent portable lanes may overlap within RadarBank's bounded executor;
// the shared reservation caps memory, not the number of in-flight GPU queues.
template<bool Frame, bool Clutter>
class Backend final : public GpuBackend,
    public std::conditional_t<Frame, GpuFrameBackend, NoFrame>,
    public std::conditional_t<Clutter, GpuClutterFrameBackend, NoClutter>,
    public std::conditional_t<Clutter, GpuClutterHostWorkBackend, NoHostWork> {
  std::unique_ptr<GpuBackend> backend_;
  std::shared_ptr<resident::LaneBudget> budget_;
  std::mutex transaction_;
  stream_profile::Worker profile_{"lane-lease",2};
  const bool profiling_=stream_profile::frameLimit()!=0;
  uint64_t sequence_=0;
  template<class Function> auto transaction(Function&& operation) {
    const auto begin=stream_profile::Activity::capture(profiling_);
    const auto start=profiling_?stream_profile::Clock::now():stream_profile::Clock::time_point{};
    std::unique_lock<std::mutex> lock(transaction_);
    // Both the record-store check and insertion remain under transaction_;
    // only immutable enablement and clock probes run before the lease.
    stream_profile::WorkScope scope(profile_,++sequence_,reinterpret_cast<uintptr_t>(this));
    scope.record.begin=begin;scope.record.time[0]=start;
    scope.mark(1); // phase0 is worker-local transaction/lease acquisition.
    if constexpr(std::is_void_v<std::invoke_result_t<Function>>) {
      operation();scope.mark(2);scope.completed();
    } else {
      auto result=operation();scope.mark(2);scope.completed();return result;
    }
  }
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
    transaction([&]{backend_->process(reference, surveillance, output);});
  }
  GpuFrameBuffers frameBuffers() {
    if constexpr (Frame) return dynamic_cast<GpuFrameBackend&>(*backend_).frameBuffers();
    else throw std::logic_error("GPU lane has no frame interface");
  }
  void processFrame() {
    transaction([&]{
      if constexpr (Frame) dynamic_cast<GpuFrameBackend&>(*backend_).processFrame();
      else throw std::logic_error("GPU lane has no frame interface");
    });
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
    return transaction([&]() -> bool {
      if constexpr (Clutter) return dynamic_cast<GpuClutterFrameBackend&>(*backend_).processClutterFrame();
      else throw std::logic_error("GPU lane has no clutter interface");
    });
  }
  bool processClutterFrameWithHostWork(const std::function<void()>& hostWork) {
    return transaction([&]() -> bool {
      if constexpr (Clutter) {
        if (auto* overlap = dynamic_cast<GpuClutterHostWorkBackend*>(backend_.get()))
          return overlap->processClutterFrameWithHostWork(hostWork);
        if (hostWork) hostWork();
        return dynamic_cast<GpuClutterFrameBackend&>(*backend_).processClutterFrame();
      } else throw std::logic_error("GPU lane has no clutter interface");
    });
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
