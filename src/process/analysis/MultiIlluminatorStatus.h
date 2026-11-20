#pragma once

#include "process/analysis/MultiIlluminatorRadar.h"
#include <algorithm>

namespace blah2::multi_illuminator {
struct AggregateStatus {
  AccelerationStatus ambiguity, clutter;
  bool ambiguityGpuExecuted = false, ambiguityCpuExecuted = false;
  bool clutterGpuExecuted = false, clutterCpuExecuted = false;
};

inline AccelerationStatus aggregate_stage(const std::vector<RadarFrame>& frames,
    bool clutter) {
  if (frames.empty()) return {};
  auto result = clutter ? frames.front().clutterAcceleration : frames.front().acceleration;
  result.members.clear();
  result.gpuExecutionBackend.clear(); // Execution identity is per lane below.
  bool fallback = false, checking = false, allReady = true;
  std::string reasons;
  for (const auto& frame : frames) {
    const auto& stage = clutter ? frame.clutterAcceleration : frame.acceleration;
    const bool positioned = frame.inputSamplesPerOutput &&
      frame.endInputSample <= UINT64_MAX - frame.inputSamplesPerOutput &&
      frame.endInputSample + frame.inputSamplesPerOutput > frame.firstInputSample;
    result.members.push_back({frame.illuminatorId, stage.active, stage.device,
      stage.state, stage.reason,
      clutter ? frame.clutterGpuExecuted : frame.ambiguityGpuExecuted,
      clutter ? frame.clutterCpuExecuted : frame.ambiguityCpuExecuted,
      positioned, frame.firstInputSample,
      positioned ? frame.endInputSample + frame.inputSamplesPerOutput : 0,
      stage.gpuExecutionBackend});
    if (stage.requested != result.requested) result.requested = "mixed";
    if (stage.active != result.active) result.active = "mixed";
    if (stage.device != result.device) result.device = "multiple";
    fallback = fallback || stage.state == "fallback";
    checking = checking || stage.state == "checking" || stage.state == "qualifying";
    allReady = allReady && stage.state == "ready";
    // These remain lane measurements, not a claimed parallel-pipeline time.
    result.cpuMs = std::max(result.cpuMs, stage.cpuMs);
    result.gpuMs = std::max(result.gpuMs, stage.gpuMs);
    if (!stage.reason.empty()) {
      if (!reasons.empty()) reasons += "; ";
      reasons += frame.illuminatorId + ": " + stage.reason;
    }
  }
  result.state = fallback ? "fallback" : checking ? "checking" :
    allReady ? "ready" : result.active == "cpu" ? "selected" : "mixed";
  result.reason = std::move(reasons);
  if (fallback && result.reason.empty())
    result.reason = "At least one illuminator lane fell back";
  return result;
}

inline AggregateStatus aggregate_status(const std::vector<RadarFrame>& frames) {
  AggregateStatus result;
  result.ambiguity = aggregate_stage(frames, false);
  result.clutter = aggregate_stage(frames, true);
  for (const auto& frame : frames) {
    result.ambiguityGpuExecuted |= frame.ambiguityGpuExecuted;
    result.ambiguityCpuExecuted |= frame.ambiguityCpuExecuted;
    result.clutterGpuExecuted |= frame.clutterGpuExecuted;
    result.clutterCpuExecuted |= frame.clutterCpuExecuted;
  }
  return result;
}
}
