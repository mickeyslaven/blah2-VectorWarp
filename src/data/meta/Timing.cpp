#include "Timing.h"
#include "data/meta/BackendMembership.h"
#include "data/meta/StageExecution.h"
#include <stdexcept>

#include "rapidjson/document.h"
#include "rapidjson/writer.h"
#include "rapidjson/stringbuffer.h"

// constructor
Timing::Timing(uint64_t _tStart)
{
  tStart = _tStart;
  tNow = _tStart;
  uptime = 0;
  n = 0;
  anchorMonoNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
    CycleClock::now().time_since_epoch()).count();
  anchorUtcNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
    std::chrono::system_clock::now().time_since_epoch()).count();
}

void Timing::begin_processing_evidence(CycleClock::time_point start,
    const CaptureEvidence& capture) noexcept
{
  if (evidencePending) { fail_processing_evidence("overlapping-cycle"); return; }
  if (evidenceSequence == UINT64_MAX) { fail_processing_evidence("sequence-overflow"); return; }
  pendingEvidence = {};
  pendingEvidence.sequence = ++evidenceSequence;
  pendingEvidence.startNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
    start.time_since_epoch()).count();
  pendingEvidence.before = capture;
  evidencePending = true;
}

void Timing::processing_evidence_outcome(const char* outcome) noexcept
{
  if (evidencePending) pendingEvidence.outcome = outcome;
}

uint64_t Timing::capture_sample_sum(std::initializer_list<uint64_t> values)
{
  uint64_t total = 0;
  for (const auto value : values) {
    if (value > UINT64_MAX - total) {
      fail_processing_evidence("capture-counter-overflow");
      throw std::overflow_error("Capture sample accounting overflow");
    }
    total += value;
  }
  return total;
}

void Timing::complete_processing_evidence(CycleClock::time_point end,
    const CaptureEvidence& capture) noexcept
{
  if (!evidencePending) return;
  pendingEvidence.endNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
    end.time_since_epoch()).count();
  const auto nowMono = std::chrono::duration_cast<std::chrono::nanoseconds>(
    CycleClock::now().time_since_epoch()).count();
  const auto nowUtc = std::chrono::duration_cast<std::chrono::nanoseconds>(
    std::chrono::system_clock::now().time_since_epoch()).count();
  if (pendingEvidence.endNs <= pendingEvidence.startNs ||
      pendingEvidence.startNs < anchorMonoNs || uint64_t(nowMono) < anchorMonoNs ||
      uint64_t(nowMono) - anchorMonoNs > UINT64_MAX - anchorUtcNs)
    fail_processing_evidence("monotonic-clock-fault");
  else {
    const auto mapped = anchorUtcNs + uint64_t(nowMono) - anchorMonoNs;
    const auto drift = mapped > uint64_t(nowUtc) ? mapped - uint64_t(nowUtc) : uint64_t(nowUtc) - mapped;
    if (drift > 100000000) fail_processing_evidence("utc-clock-drift");
  }
  pendingEvidence.after = capture;
  cycleEvidence[(pendingEvidence.sequence - 1) % cycleEvidence.size()] = pendingEvidence;
  evidencePending = false;
}

void Timing::update(uint64_t _tNow, std::vector<double> _time, std::vector<std::string> _name)
{
  clear_execution();
  captureTerminal = false;
  n = n + 1;
  tNow = _tNow;
  time = _time;
  name = _name;
  uptime = _tNow-tStart;
}

void Timing::record_processing_cycle(CycleClock::time_point start,
    CycleClock::time_point pipelineStart, CycleClock::time_point pipelineEnd,
    CycleClock::time_point publicationEnd) noexcept
{
  pendingCycle = {start, pipelineStart, pipelineEnd, publicationEnd};
  pendingCycleNCpi = n; pendingCycleTimestamp = tNow;
  processingCyclePending = true;
}

void Timing::complete_processing_cycle(CycleClock::time_point end) noexcept
{
  if (!processingCyclePending) return;
  const auto milliseconds = [](auto stop, auto start) {
    return std::chrono::duration<double, std::milli>(stop - start).count();
  };
  processingCycleMetrics[0].observe(milliseconds(pendingCycle[1], pendingCycle[0]));
  processingCycleMetrics[1].observe(milliseconds(pendingCycle[2], pendingCycle[1]));
  processingCycleMetrics[2].observe(milliseconds(pendingCycle[3], pendingCycle[2]));
  processingCycleMetrics[3].observe(milliseconds(end, pendingCycle[3]));
  processingCycleMetrics[4].observe(milliseconds(end, pendingCycle[0]));
  if (havePreviousCycleEnd)
    processingCycleMetrics[5].observe(milliseconds(end, previousCycleEnd));
  previousCycleEnd = end; havePreviousCycleEnd = true;
  completedCycleNCpi = pendingCycleNCpi;
  completedCycleTimestamp = pendingCycleTimestamp;
  processingCyclePending = false;
}

std::string Timing::to_json()
{
  return to_json(false);
}

std::string Timing::to_json(bool evidenceOnly)
{
  rapidjson::Document document;
  document.SetObject();
  rapidjson::Document::AllocatorType &allocator = document.GetAllocator();
  if (evidenceOnly) document.AddMember("cycleEvidenceOnly", true, allocator);

  document.AddMember("timestamp", tNow, allocator);
  document.AddMember("nCpi", n, allocator);
  document.AddMember("uptime_s", uptime/1000.0, allocator);
  document.AddMember("uptime_days", uptime/1000.0/60/60/24, allocator);
  const auto exact = [&](uint64_t value) {
    const auto text = std::to_string(value);
    return rapidjson::Value(text.c_str(), allocator);
  };
  rapidjson::Value evidence(rapidjson::kObjectType), records(rapidjson::kArrayType);
  evidence.AddMember("schema", "vectorwarp-processing-cycles-v1", allocator);
  evidence.AddMember("anchorMonoNs", exact(anchorMonoNs), allocator);
  evidence.AddMember("anchorUtcNs", exact(anchorUtcNs), allocator);
  evidence.AddMember("fault", rapidjson::Value(evidenceFault, allocator), allocator);
  const auto captureJson = [&](const CaptureEvidence& value) {
    rapidjson::Value result(rapidjson::kObjectType), lanes(rapidjson::kArrayType);
    result.AddMember("available", value.available && value.lanes && value.lanes <= value.accepted.size(), allocator);
    result.AddMember("epoch", exact(value.epoch), allocator);
    result.AddMember("discontinuities", exact(value.discontinuities), allocator);
    result.AddMember("requalifications", exact(value.requalifications), allocator);
    result.AddMember("retiredCpis", exact(value.retiredCpis), allocator);
    result.AddMember("retiredSamples", exact(value.retiredSamples), allocator);
    for (std::size_t i = 0; i < std::min(value.lanes, value.accepted.size()); ++i) {
      rapidjson::Value lane(rapidjson::kObjectType);
      lane.AddMember("acceptedSamples", exact(value.accepted[i]), allocator);
      lane.AddMember("droppedSamples", exact(value.dropped[i]), allocator);
      lane.AddMember("discardedSamples", exact(value.discarded[i]), allocator);
      lanes.PushBack(lane, allocator);
    }
    result.AddMember("lanes", lanes, allocator);
    return result;
  };
  const auto through = evidenceSequence - (evidencePending ? 1 : 0);
  const auto first = through < cycleEvidence.size() ? 1 : through - cycleEvidence.size() + 1;
  for (uint64_t sequence = first; sequence && sequence <= through; ++sequence) {
    const auto& value = cycleEvidence[(sequence - 1) % cycleEvidence.size()];
    rapidjson::Value record(rapidjson::kObjectType);
    record.AddMember("sequence", exact(value.sequence), allocator);
    record.AddMember("startMonoNs", exact(value.startNs), allocator);
    record.AddMember("endMonoNs", exact(value.endNs), allocator);
    record.AddMember("drainBatches", exact(value.drainBatches), allocator);
    record.AddMember("outcome", rapidjson::Value(value.outcome, allocator), allocator);
    record.AddMember("captureBefore", captureJson(value.before), allocator);
    record.AddMember("captureAfter", captureJson(value.after), allocator);
    records.PushBack(record, allocator);
  }
  evidence.AddMember("records", records, allocator);
  document.AddMember("cycleEvidence", evidence, allocator);
  if (processingCycleMetrics[4].samples) {
    rapidjson::Value cycles(rapidjson::kObjectType);
    cycles.AddMember("scope", "completed_multi_consumer_cycles", allocator);
    cycles.AddMember("lastCompletedNCpi", completedCycleNCpi, allocator);
    cycles.AddMember("lastCompletedTimestamp", completedCycleTimestamp, allocator);
    // No stage summation: completion_interval includes capture waits, while
    // processing includes preparation, pipeline, publication and destruction.
    const char* keys[] = {"capture_prepare", "pipeline", "publication",
      "post_publication", "processing", "completion_interval"};
    for (std::size_t i = 0; i < processingCycleMetrics.size(); ++i) {
      const auto& metric = processingCycleMetrics[i];
      rapidjson::Value value(rapidjson::kObjectType);
      value.AddMember("samples", metric.samples, allocator);
      value.AddMember("latestMs", metric.latest, allocator);
      value.AddMember("totalMs", metric.total, allocator);
      value.AddMember("meanMs", metric.samples ? metric.total / metric.samples : 0,
        allocator);
      value.AddMember("maximumMs", metric.maximum, allocator);
      cycles.AddMember(rapidjson::Value(keys[i], allocator), value, allocator);
    }
    document.AddMember("processingCycle", cycles, allocator);
  }
  const auto backendJson = [&](const blah2::AccelerationStatus& status, bool known) {
    rapidjson::Value gpu(rapidjson::kObjectType);
    const auto addText = [&](const char* key, const std::string& value) {
      gpu.AddMember(rapidjson::Value(key, allocator), rapidjson::Value(value.c_str(), allocator), allocator);
    };
    addText("requested", status.requested); addText("active", status.active);
    addText("device", status.device); addText("state", status.state);
    addText("reason", status.reason);
    addText("gpuExecutionBackend", known ? status.gpuExecutionBackend : "");
    gpu.AddMember("cpuMs", status.cpuMs, allocator);
    gpu.AddMember("gpuMs", status.gpuMs, allocator);
    gpu.AddMember("members", blah2::backend_members_json(status, known,
      captureSamplePositionAvailable, captureEpoch, allocator), allocator);
    return gpu;
  };
  auto ambiguity = backendJson(acceleration, ambiguityExecutionSet);
  const auto execution = [&](bool known, bool gpu, bool cpu) {
    return blah2::stage_execution_json(known, gpu, cpu,
      captureSamplePositionAvailable, captureEpoch, captureFirstEpochSample,
      captureEndEpochSample, allocator);
  };
  ambiguity.AddMember("execution", execution(ambiguityExecutionSet,
    ambiguityGpuExecuted, ambiguityCpuExecuted), allocator);
  if (ambiguityExecutionSet) {
    ambiguity.AddMember("gpuExecuted", ambiguityGpuExecuted, allocator);
    ambiguity.AddMember("cpuExecuted", ambiguityCpuExecuted, allocator);
  }
  document.AddMember("acceleration", ambiguity, allocator);
  if (widebandAccelerationSet) {
    auto wideband = backendJson(widebandAcceleration, widebandExecutionSet);
    wideband.AddMember("execution", execution(widebandExecutionSet,
      widebandGpuExecuted, widebandCpuExecuted), allocator);
    if (widebandExecutionSet) {
      wideband.AddMember("gpuExecuted", widebandGpuExecuted, allocator);
      wideband.AddMember("cpuExecuted", widebandCpuExecuted, allocator);
    }
    document.AddMember("widebandAcceleration", wideband, allocator);
  }
  auto clutter = backendJson(clutterAcceleration, clutterExecutionSet);
  clutter.AddMember("execution", execution(clutterExecutionSet,
    clutterGpuExecuted, clutterCpuExecuted), allocator);
  clutter.AddMember("gpuExecuted", clutterGpuExecuted, allocator);
  clutter.AddMember("cpuExecuted", clutterCpuExecuted, allocator);
  document.AddMember("clutterAcceleration", clutter, allocator);
  rapidjson::Value laneHealth(rapidjson::kArrayType);
  for (const auto& lane : multiLaneHealth) {
    rapidjson::Value item(rapidjson::kObjectType);
    item.AddMember("id", rapidjson::Value(lane.illuminatorId.c_str(), allocator), allocator);
    item.AddMember("frequencyHz", lane.frequencyHz, allocator);
    item.AddMember("acceptedCpis", lane.acceptedCpis, allocator);
    item.AddMember("rejectedCpis", lane.rejectedCpis, allocator);
    item.AddMember("hasLastCpi", lane.hasLastCpi, allocator);
    item.AddMember("lastAccepted", lane.lastAccepted, allocator);
    item.AddMember("lastFirstInputSample", lane.lastFirstInputSample, allocator);
    item.AddMember("lastEndInputSample", lane.lastEndInputSample, allocator);
    item.AddMember("lastReason", rapidjson::Value(lane.lastReason.c_str(), allocator), allocator);
    laneHealth.PushBack(item, allocator);
  }
  document.AddMember("multiIlluminatorLaneHealth", laneHealth, allocator);
  rapidjson::Value backlog(rapidjson::kArrayType), dropped(rapidjson::kArrayType),
    peak(rapidjson::kArrayType), boundaryDiscarded(rapidjson::kArrayType),
    stopDiscarded(rapidjson::kArrayType);
  for (uint64_t value : captureBacklogSamples) backlog.PushBack(value, allocator);
  for (uint64_t value : captureDroppedSamples) dropped.PushBack(value, allocator);
  uint64_t aggregatePeakBytes = 0;
  for (uint64_t value : capturePeakBacklogSamples) {
    peak.PushBack(value, allocator);
    const uint64_t bytes = captureStorageBytesPerSample &&
      value > UINT64_MAX / captureStorageBytesPerSample ? UINT64_MAX :
      value * captureStorageBytesPerSample;
    aggregatePeakBytes = bytes > UINT64_MAX - aggregatePeakBytes ?
      UINT64_MAX : aggregatePeakBytes + bytes;
  }
  for (uint64_t value : captureBoundaryDiscardedSamples)
    boundaryDiscarded.PushBack(value, allocator);
  for (uint64_t value : captureStopDiscardedSamples)
    stopDiscarded.PushBack(value, allocator);
  document.AddMember("captureBacklogSamples", backlog, allocator);
  document.AddMember("captureDroppedSamples", dropped, allocator);
  document.AddMember("capturePeakBacklogSamples", peak, allocator);
  document.AddMember("capturePeakBytes", aggregatePeakBytes, allocator);
  document.AddMember("captureBoundaryDiscardedSamples", boundaryDiscarded,
    allocator);
  document.AddMember("captureStopDiscardedSamples", stopDiscarded, allocator);
  document.AddMember("captureStorageBytesPerSample",
    captureStorageBytesPerSample, allocator);
  document.AddMember("captureConfiguredSamplesPerChannel",
    captureConfiguredSamplesPerChannel, allocator);
  document.AddMember("captureSamplePositionAvailable",
    captureSamplePositionAvailable, allocator);
  document.AddMember("inputDiscontinuity", inputDiscontinuity, allocator);
  document.AddMember("captureTerminal", captureTerminal, allocator);
  if (captureSamplePositionAvailable) {
    document.AddMember("captureEpoch", captureEpoch, allocator);
    document.AddMember("captureFirstEpochSample", captureFirstEpochSample,
      allocator);
    document.AddMember("captureEndEpochSample", captureEndEpochSample,
      allocator);
  }
  rapidjson::Value name_value;
  for (size_t i = 0; i < time.size(); i++)
  {
    name_value = rapidjson::StringRef(name[i].c_str());
    document.AddMember(name_value, time[i], allocator);
  }

  rapidjson::StringBuffer strbuf;
  rapidjson::Writer<rapidjson::StringBuffer> writer(strbuf);
  writer.SetMaxDecimalPlaces(2);
  document.Accept(writer);

  return strbuf.GetString();
}
