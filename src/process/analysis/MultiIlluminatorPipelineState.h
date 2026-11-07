#ifndef MULTI_ILLUMINATOR_PIPELINE_STATE_H
#define MULTI_ILLUMINATOR_PIPELINE_STATE_H

#include "data/meta/EventTime.h"
#include "process/ambiguity/AccelerationStatus.h"

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace blah2::multi_illuminator
{

// Metadata is delayed with the channelized frame as one value. Keeping these
// fields inseparable prevents split-frame overlap from publishing prior IQ
// with the next capture's time, queue pressure, or sample identity.
struct PublicationContext
{
  uint64_t timestampMs = 0;
  EventTimeProvenance eventTime;
  bool samplePositionAvailable = false;
  uint64_t captureEpoch = 0;
  uint64_t captureFirst = 0;
  uint64_t captureEnd = 0;
  std::vector<uint64_t> captureBacklogSamples;
  std::vector<uint64_t> captureDroppedSamples;
  double channelizerMs = 0;
  bool inputDiscontinuity = false;
  bool gpuQualificationFrame = false;
  AccelerationStatus acceleration;
};

template <typename Payload>
class PendingPublication
{
public:
  void store(Payload payload, PublicationContext context)
  {
    if (pending_) throw std::logic_error("Pending publication was overwritten");
    pending_.emplace(std::move(payload), std::move(context));
  }

  bool has_value() const { return pending_.has_value(); }

  std::pair<Payload, PublicationContext> take()
  {
    if (!pending_) throw std::logic_error("Pending publication is empty");
    auto result = std::move(*pending_);
    pending_.reset();
    return result;
  }

  void reset() { pending_.reset(); }

private:
  std::optional<std::pair<Payload, PublicationContext>> pending_;
};

class PipelineState
{
public:
  PublicationContext publication(uint64_t timestampMs,
    EventTimeProvenance eventTime, bool samplePositionAvailable,
    uint64_t captureEpoch, uint64_t captureFirst, uint64_t captureEnd,
    std::vector<uint64_t> backlog, std::vector<uint64_t> dropped,
    double channelizerMs, bool gpuQualificationFrame = false,
    AccelerationStatus acceleration = {})
  {
    PublicationContext result{timestampMs, std::move(eventTime),
      samplePositionAvailable, captureEpoch, captureFirst, captureEnd,
      std::move(backlog), std::move(dropped), channelizerMs,
      nextInputDiscontinuity_, gpuQualificationFrame,
      std::move(acceleration)};
    nextInputDiscontinuity_ = false;
    return result;
  }

  template <typename ResetChannelizer, typename ResetRadar>
  void runtime_fallback(std::string reason, ResetChannelizer resetChannelizer,
    ResetRadar resetRadar)
  {
    runtimeFallback_ = true;
    fallbackReason_ = std::move(reason);
    // Both adaptive components must see an epoch boundary before the failed
    // GPU block is recomputed on CPU. The absolute input index then seeds a
    // clean FIR/mixer epoch, and no radar history crosses implementations.
    resetChannelizer();
    resetRadar();
    nextInputDiscontinuity_ = true;
  }

  void input_epoch_reset() { nextInputDiscontinuity_ = true; }

  template <typename ResetRadar>
  void qualification_fallback(std::string reason, ResetRadar resetRadar)
  {
    runtimeFallback_ = true;
    fallbackReason_ = std::move(reason);
    // The CPU oracle already advanced on the real block. Preserve that FIR
    // history and reset only adaptive downstream state before publication.
    resetRadar();
    nextInputDiscontinuity_ = true;
  }

  AccelerationStatus acceleration_status(const std::string& requested,
    bool gpuActive, bool gpuRequested, bool qualificationFrame = false) const
  {
    AccelerationStatus result;
    result.requested = requested;
    result.active = gpuActive ? "vulkan-wideband" : "cpu";
    if (runtimeFallback_)
    {
      result.state = "fallback";
      result.reason = fallbackReason_;
    }
    else if (gpuActive && qualificationFrame)
    {
      result.state = "qualifying";
      result.reason = "First real capture CPU/GPU qualification";
    }
    else if (gpuActive)
    {
      result.state = "active";
      result.reason = "Qualified isolated wideband channelizer worker";
    }
    else
    {
      result.state = "active";
      result.reason = gpuRequested ?
        "Qualified geometry/device unavailable; CPU channelizer active" :
        "Independent CPU channelized illuminator contexts";
    }
    return result;
  }

  bool runtime_fallback_active() const { return runtimeFallback_; }
  const std::string& fallback_reason() const { return fallbackReason_; }

private:
  bool runtimeFallback_ = false;
  bool nextInputDiscontinuity_ = false;
  std::string fallbackReason_;
};

} // namespace blah2::multi_illuminator

#endif
