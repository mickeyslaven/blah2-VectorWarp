#include "Timing.h"
#include "data/meta/BackendMembership.h"
#include "data/meta/StageExecution.h"
#include <iostream>
#include <cstdlib>

#include "rapidjson/document.h"
#include "rapidjson/writer.h"
#include "rapidjson/stringbuffer.h"
#include "rapidjson/filewritestream.h"

// constructor
Timing::Timing(uint64_t _tStart)
{
  tStart = _tStart;
  tNow = _tStart;
  uptime = 0;
  n = 0;
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

std::string Timing::to_json()
{
  rapidjson::Document document;
  document.SetObject();
  rapidjson::Document::AllocatorType &allocator = document.GetAllocator();

  document.AddMember("timestamp", tNow, allocator);
  document.AddMember("nCpi", n, allocator);
  document.AddMember("uptime_s", uptime/1000.0, allocator);
  document.AddMember("uptime_days", uptime/1000.0/60/60/24, allocator);
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

bool Timing::save(std::string _json, std::string filename)
{
  using namespace rapidjson;

  rapidjson::Document document;

  // create file if it doesn't exist
  if (FILE *fp = fopen(filename.c_str(), "r"); !fp)
  {
    if (fp = fopen(filename.c_str(), "w"); !fp)
      return false;
    fputs("[]", fp);
    fclose(fp);
  }

  // add the document to the file
  if (FILE *fp = fopen(filename.c_str(), "rb+"); fp)
  {
    // check if first is [
    std::fseek(fp, 0, SEEK_SET);
    if (getc(fp) != '[')
    {
      std::fclose(fp);
      return false;
    }

    // is array empty?
    bool isEmpty = false;
    if (getc(fp) == ']')
      isEmpty = true;

    // check if last is ]
    std::fseek(fp, -1, SEEK_END);
    if (getc(fp) != ']')
    {
      std::fclose(fp);
      return false;
    }

    // replace ] by ,
    fseek(fp, -1, SEEK_END);
    if (!isEmpty)
      fputc(',', fp);

    // add json element
    fwrite(_json.c_str(), sizeof(char), _json.length(), fp);

    // close the array
    std::fputc(']', fp);
    fclose(fp);
    return true;
  }
  return false;
}
