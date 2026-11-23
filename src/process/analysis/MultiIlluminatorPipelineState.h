#ifndef MULTI_ILLUMINATOR_PIPELINE_STATE_H
#define MULTI_ILLUMINATOR_PIPELINE_STATE_H

#include "data/meta/EventTime.h"
#include "process/ambiguity/AccelerationStatus.h"

#include <cstdint>
#include <algorithm>
#include <deque>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace blah2::multi_illuminator
{

// A failing next-input advance may synchronously drain and reset the pipeline.
// Complete publication of the older result first so products, timing and
// timestamps retain the same FIFO order across that recovery boundary.
template <typename Complete, typename Advance>
void publish_then_advance(Complete&& complete, Advance&& advance)
{
  std::forward<Complete>(complete)();
  std::forward<Advance>(advance)();
}

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
  bool widebandGpuExecuted = false;
  bool widebandCpuExecuted = false;
  AccelerationStatus acceleration;
  bool publishFullMap = false;
  std::vector<double> inputGainDb;
  std::optional<uint64_t> signalTimestampMs;
  double capturePrepareMs = 0;
};

// Product completion can differ by lane after a rejection or reset. Bind each
// result by its opaque input token, retaining only returned or in-flight CPIs.
class PublicationFifo
{
public:
  explicit PublicationFifo(std::size_t maximum) : maximum_(maximum) {}
  uint64_t append(PublicationContext context) {
    if (entries_.size() >= maximum_ || sequence_ == UINT64_MAX)
      throw std::logic_error("CPI publication FIFO capacity exhausted");
    entries_.emplace_back(++sequence_, std::move(context));
    return sequence_;
  }
  const PublicationContext& at(uint64_t sequence) const {
    for (const auto& entry : entries_) if (entry.first == sequence) return entry.second;
    throw std::logic_error("CPI product has no publication context");
  }
  // Copy only contexts represented by completed products, in input order.
  // A newer ingress/fallback may reset the FIFO before timing is emitted.
  template <typename Frame>
  std::vector<std::pair<uint64_t, PublicationContext>> snapshotsFor(
      const std::vector<Frame>& frames) const {
    for (const auto& frame : frames) (void)at(frame.publicationSequence);
    std::vector<std::pair<uint64_t, PublicationContext>> result;
    for (const auto& entry : entries_)
      if (std::any_of(frames.begin(), frames.end(), [&](const auto& frame) {
          return frame.publicationSequence == entry.first;
        })) result.push_back(entry);
    return result;
  }
  void retain(const std::vector<uint64_t>& sequences) {
    for (auto sequence : sequences) (void)at(sequence);
    entries_.erase(std::remove_if(entries_.begin(), entries_.end(), [&](const auto& entry) {
      return std::find(sequences.begin(), sequences.end(), entry.first) == sequences.end();
    }), entries_.end());
  }
  void reset() { entries_.clear(); }
  std::size_t size() const { return entries_.size(); }
private:
  std::size_t maximum_;
  uint64_t sequence_ = 0;
  std::deque<std::pair<uint64_t, PublicationContext>> entries_;
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
    bool widebandGpuExecuted = false, bool widebandCpuExecuted = false,
    AccelerationStatus acceleration = {}, bool publishFullMap = false)
  {
    PublicationContext result{timestampMs, std::move(eventTime),
      samplePositionAvailable, captureEpoch, captureFirst, captureEnd,
      std::move(backlog), std::move(dropped), channelizerMs,
      nextInputDiscontinuity_, gpuQualificationFrame,
      widebandGpuExecuted, widebandCpuExecuted,
      std::move(acceleration), publishFullMap};
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

  void gpu_requalified()
  {
    runtimeFallback_ = false;
    fallbackReason_.clear();
  }

  void startup_fallback(std::string reason) {
    runtimeFallback_ = true;
    fallbackReason_ = std::move(reason);
  }

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
