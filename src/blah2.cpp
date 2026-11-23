/// @file blah2.cpp
/// @brief A real-time radar.
/// @author 30hours

#include "capture/Capture.h"
#include "capture/ProcessingTopology.h"
#include "capture/PairedCpiQueue.h"
#include "capture/Sha256.h"
#include "data/IqData.h"
#include "data/CaptureChunkQueue.h"
#include "data/Map.h"
#include "data/Detection.h"
#include "data/meta/Timing.h"
#include "data/meta/StreamProfile.h"
#include "data/meta/EventTime.h"
#include "data/Track.h"
#include "process/ambiguity/Ambiguity.h"
#include "process/ambiguity/Acceleration.h"
#include "process/ambiguity/GpuResidentCoordinator.h"
#include "process/ambiguity/ReplayGpuQualification.h"
#include "process/ambiguity/GpuArtifactBinding.h"
#include "process/ambiguity/GpuDeviceSelection.h"
#include "process/integration/DirectPathCoherentReplay.h"
#include "process/ambiguity/GpuWidebandProcess.h"
#include "process/ambiguity/GpuWidebandFallback.h"
#include "process/ambiguity/BearingManifold.h"
#include "process/ambiguity/ManifoldEvidence.h"
#include "process/utility/FftwThreads.h"
#include "process/conditioning/ArrayReferenceSynthesizer.h"
#include "process/fusion/CoherentBeamBank.h"
#include "process/analysis/ComplexCutoutRecorder.h"
#include "process/analysis/ExperimentalRouteAdmission.h"
#include "process/analysis/MultiIlluminatorStatus.h"
#include "process/analysis/ReplayEchoInjector.h"
#include "process/analysis/ReplayCoherentObserverViews.h"
#include "process/analysis/MultiIlluminatorRadar.h"
#include "process/ambiguity/ResidentTemporalPublication.h"
#include "process/analysis/MultiIlluminatorPipelineState.h"
#include "process/conditioning/IlluminatorChannelizerBank.h"
#include "process/detection/DopplerRidgeAdmission.h"
#include "process/spectrum/SpectrumAnalyser.h"
#include "process/tracker/Tracker.h"
#include "process/tracker/TrackerDiagnosticsJson.h"
#include "process/utility/Socket.h"
#include "process/utility/BestEffortSocket.h"
#include "process/tracker/CausalTraceJson.h"
#include "process/utility/ProcessingThreads.h"
#include "data/meta/Constants.h"
#include "../core/TrackBeforeDetect.private.h"

#include "data/Yaml.h"
#include "capture/RecordingContextConfig.h"
#include <c4/format.hpp> // needed for the examples below
#include <sys/types.h>
#include <getopt.h>
#include <string>
#include <vector>
#include <unordered_map>
#include <fstream>
#include <sstream>
#include <thread>
#include <chrono>
#include <sys/time.h>
#include <signal.h>
#include <atomic>
#include <memory>
#include <mutex>
#include <iostream>
#include <algorithm>
#include <stdexcept>
#include <future>
#include <functional>
#include <limits>
#include <array>
#include <cmath>
#include <numeric>
#include <optional>
#include <regex>
#include <set>
#include <filesystem>
#include <fcntl.h>
#include <unistd.h>

#include "rapidjson/stringbuffer.h"
#include "rapidjson/writer.h"

volatile sig_atomic_t STOP_SIGNAL = 0;

class StartupInterrupted final : public std::exception {
public:
  const char* what() const noexcept override {
    return "Startup interrupted before capture";
  }
};
std::unique_ptr<Socket> socket_map;
std::unique_ptr<Socket> socket_detection;
std::unique_ptr<Socket> socket_track;
std::unique_ptr<blah2::BestEffortSocket> socket_causal;
std::unique_ptr<Socket> socket_illuminator;
std::unique_ptr<Socket> socket_bearing;
std::unique_ptr<Socket> socket_manifold;
std::unique_ptr<Socket> socket_timestamp;
std::unique_ptr<Socket> socket_timing;
std::unique_ptr<Socket> socket_iqdata;

void signal_callback_handler(int signum);
void getopt_print_help();
std::string getopt_process(int argc, char **argv);
std::string ryml_get_file(const char *filename);
uint64_t current_time_ms();
uint64_t current_time_us();

class AtomicFlagGuard
{
  std::atomic<bool>& flag;
public:
  explicit AtomicFlagGuard(std::atomic<bool>& value) : flag(value) { flag.store(true); }
  ~AtomicFlagGuard() { flag.store(false); }
};

class BufferLocks
{
  const std::vector<IqData*>& buffers;
  bool locked = true;
public:
  explicit BufferLocks(const std::vector<IqData*>& value) : buffers(value)
  {
    for (auto* buffer : buffers) buffer->lock();
  }
  ~BufferLocks() { unlock(); }
  void unlock()
  {
    if (!locked) return;
    for (auto it = buffers.rbegin(); it != buffers.rend(); ++it) (*it)->unlock();
    locked = false;
  }
};

class ReplayFrameGuard
{
  bool replay;
  uint64_t& frames;
public:
  ReplayFrameGuard(bool isReplay, uint64_t& value) : replay(isReplay), frames(value) {}
  ~ReplayFrameGuard() { if (replay) ++frames; }
};

blah2::resident_replay::Hash replay_hash(const std::string& text)
{
  if (text.size() != 64) throw std::invalid_argument("Resident replay hash must contain 64 hexadecimal characters");
  blah2::resident_replay::Hash result{};
  auto digit=[](char value)->uint8_t {
    if(value>='0'&&value<='9')return uint8_t(value-'0');
    if(value>='a'&&value<='f')return uint8_t(value-'a'+10);
    if(value>='A'&&value<='F')return uint8_t(value-'A'+10);
    throw std::invalid_argument("Resident replay hash contains a non-hexadecimal character");
  };
  for(size_t i=0;i<result.size();++i)result[i]=uint8_t((digit(text[2*i])<<4)|digit(text[2*i+1]));
  return result;
}

blah2::resident_replay::Hash replay_file_hash(const std::string& path)
{
  std::ifstream input(path,std::ios::binary);
  if(!input)throw std::runtime_error("Cannot open resident replay source for identity hashing");
  blah2::Sha256 hash;
  std::array<char,1<<20> buffer{};
  while(input){input.read(buffer.data(),buffer.size());const auto count=input.gcount();
    if(count>0)hash.update(buffer.data(),size_t(count));}
  if(!input.eof())throw std::runtime_error("Cannot read resident replay source for identity hashing");
  return replay_hash(hash.finish());
}

void write_replay_snapshot(const std::filesystem::path& directory,
  uint64_t logical, uint64_t consumed, const std::vector<uint8_t>& bytes)
{
  const auto destination=directory/("resident-frame-"+std::to_string(logical)+"-"+
    std::to_string(consumed)+".vwrps");
  const int fd=open(destination.c_str(),O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC,0600);
  if(fd<0)throw std::runtime_error("Cannot exclusively create resident replay snapshot: "+destination.string());
  size_t written=0;
  try{
    while(written<bytes.size()){
      const ssize_t count=write(fd,bytes.data()+written,bytes.size()-written);
      if(count<=0)throw std::runtime_error("Cannot write complete resident replay snapshot: "+destination.string());
      written+=size_t(count);
    }
    if(fsync(fd))throw std::runtime_error("Cannot sync resident replay snapshot: "+destination.string());
    if(close(fd))throw std::runtime_error("Cannot close resident replay snapshot: "+destination.string());
  }catch(...){close(fd);unlink(destination.c_str());throw;}
  std::cout<<"GPU_RESIDENT_REPLAY_SNAPSHOT path="<<destination.string()
    <<" bytes="<<bytes.size()<<" logical="<<logical<<" consumed="<<consumed<<'\n';
}

blah2::integration::coherent_replay::Sidecar replay_lineage_sidecar(
  const std::vector<uint8_t>& snapshot, const blah2::resident::Config& config,
  bool samplePositionAvailable, uint64_t captureEpoch, uint64_t firstSample,
  uint64_t endSample, uint64_t tuningHz, uint64_t gainCalibrationEpoch)
{
  if (!samplePositionAvailable)
    throw std::runtime_error("Resident replay lineage sidecar requires exact capture sample position");
  if (snapshot.size() < blah2::resident_replay::headerBytes)
    throw std::runtime_error("Resident replay snapshot header unavailable for lineage binding");
  blah2::integration::coherent_replay::Sidecar result;
  const auto read64 = [&](size_t offset) {
    uint64_t value = 0;
    for (unsigned i = 0; i < 8; ++i)
      value |= uint64_t(snapshot.at(offset + i)) << (8 * i);
    return value;
  };
  result.identity = {read64(24),read64(32),read64(40),read64(48),read64(56),read64(64)};
  std::memcpy(result.identity.configHash.data(), snapshot.data() + 96, 32);
  std::memcpy(result.identity.moduleHash.data(), snapshot.data() + 128, 32);
  std::memcpy(result.identity.rawIqHash.data(), snapshot.data() + 160, 32);
  std::memcpy(result.identity.deviceHash.data(), snapshot.data() + 480, 32);
  result.sourceHash = config.replay.sourceHash; result.rfHash = config.replay.rfHash;
  result.captureEpoch = captureEpoch; result.firstSample = firstSample; result.endSample = endSample;
  result.tuningHz = tuningHz; result.gainCalibrationEpoch = gainCalibrationEpoch;
  result.sampleRateHz = uint32_t(config.sampleRate); result.actualGainDb = config.replay.actualGainDb;
  return result;
}

std::string resident_product_timestamp(std::string json, uint64_t timestamp)
{
  rapidjson::Document document;
  document.Parse<rapidjson::kParseFullPrecisionFlag>(json.c_str());
  if (document.HasParseError() || !document.IsObject() || !document.HasMember("timestamp"))
    throw std::runtime_error("Resident product has no publication timestamp");
  document["timestamp"].SetUint64(timestamp);
  rapidjson::StringBuffer buffer;
  rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
  document.Accept(writer);
  return buffer.GetString();
}

std::string multi_map_json(const blah2::multi_illuminator::RadarFrame& frame,
    uint64_t timestamp, bool full = true, bool presentation = true)
{
  return frame.residentProducts && !frame.residentProducts->map.empty() ?
    resident_product_timestamp(frame.residentProducts->map, timestamp) :
    presentation ? frame.map->to_json_presentation_km(timestamp, frame.sampleRateHz, full) :
      frame.map->to_json_km(timestamp, frame.sampleRateHz);
}
std::string multi_detection_json(const blah2::multi_illuminator::RadarFrame& frame, uint64_t timestamp)
{
  return frame.residentProducts && !frame.residentProducts->detections.empty() ? resident_product_timestamp(frame.residentProducts->detections, timestamp) :
    frame.detections->to_json_km(timestamp, frame.sampleRateHz);
}
std::string multi_track_json(const blah2::multi_illuminator::RadarFrame& frame, uint64_t timestamp)
{
  return annotateTrackerFrameDiagnostics(frame.residentProducts && !frame.residentProducts->tracks.empty() ?
    resident_product_timestamp(frame.residentProducts->tracks, timestamp) :
    frame.tracks->to_json(timestamp),frame.trackerDiagnostics);
}

template <typename Frame>
blah2::analysis::NativeProductIdentity native_product_identity(
  const Frame& frame, const blah2::multi_illuminator::PublicationContext& publication)
{
  blah2::analysis::NativeProductIdentity result;
  result.illuminatorId = frame.illuminatorId;
  result.frequencyHz = frame.frequencyHz;
  result.sampleRateHz = frame.sampleRateHz;
  result.firstInputSample = frame.firstInputSample;
  result.endInputSample = frame.endInputSample;
  result.inputSamplesPerOutput = frame.inputSamplesPerOutput;
  result.channelIds = frame.channelIds;
  result.referenceChannelIds = frame.referenceChannelIds;
  result.captureSamplePositionAvailable = publication.samplePositionAvailable;
  result.captureEpoch = publication.captureEpoch;
  result.captureFirst = publication.captureFirst;
  result.captureEnd = publication.captureEnd;
  result.eventTime = publication.eventTime;
  return result;
}

blah2::analysis::NativeProductIdentity native_product_identity(
  const blah2::multi_illuminator::RadarFrame& frame,
  const blah2::multi_illuminator::PublicationContext& publication)
{
  auto result = native_product_identity<blah2::multi_illuminator::RadarFrame>(frame, publication);
  result.resolvedProductPlan = frame.resolvedProductPlan;
  result.productBackend = frame.residentProducts ? "vulkan-resident" :
    !frame.temporalDevice.empty() ? "vulkan-temporal" : "cpu";
  if (frame.residentProducts) result.productDevice = frame.acceleration.device;
  else if (!frame.temporalDevice.empty()) result.productDevice = frame.temporalDevice;
  result.temporalExecuted = frame.residentTemporalExecuted ||
    (frame.detectorIntegration && frame.detectorIntegration->availableCpis > 0);
  result.detectorExecuted = frame.detectionEnabled && bool(frame.detections) &&
    (!frame.detectorIntegration || frame.detectorIntegration->ready);
  result.trackerExecuted = frame.trackingEnabled && bool(frame.tracks);
  result.trackerOnHost = !frame.temporalDevice.empty() || (frame.residentProducts && frame.residentProducts->trackerOnHost);
  result.fusionOnHost = !frame.temporalDevice.empty();
  result.bearingExecuted = frame.residentProducts && !frame.residentProducts->bearing.empty();
  return result;
}


std::string multi_illuminator_frame_json(
  const blah2::multi_illuminator::RadarFrame& frame, uint64_t timestamp,
  const std::string& map, const std::string& detections,
  const std::string& tracks, bool captureSamplePositionAvailable,
  uint64_t captureEpoch, uint64_t captureFirstInputSample,
  uint64_t captureEndInputSample)
{
  rapidjson::StringBuffer buffer;
  rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
  writer.StartObject();
  writer.Key("schema"); writer.String("vectorwarp-multi-illuminator-frame-v1");
  writer.Key("id"); writer.String(frame.illuminatorId.c_str());
  writer.Key("frequency_hz"); writer.Uint64(frame.frequencyHz);
  writer.Key("sample_rate_hz"); writer.Uint(frame.sampleRateHz);
  if (frame.transmitterLocation && frame.directPathAzimuthRad) {
    writer.Key("transmitter_location"); writer.StartObject();
    writer.Key("latitude"); writer.Double(frame.transmitterLocation->at(0));
    writer.Key("longitude"); writer.Double(frame.transmitterLocation->at(1));
    writer.Key("altitude"); writer.Double(frame.transmitterLocation->at(2));
    writer.EndObject();
    writer.Key("direct_path_azimuth_rad"); writer.Double(*frame.directPathAzimuthRad);
  }
  writer.Key("first_input_sample"); writer.Uint64(frame.firstInputSample);
  writer.Key("end_input_sample"); writer.Uint64(frame.endInputSample);
  writer.Key("capture_sample_position_available");
  writer.Bool(captureSamplePositionAvailable);
  writer.Key("capture_epoch"); writer.Uint64(captureEpoch);
  writer.Key("capture_first_input_sample"); writer.Uint64(captureFirstInputSample);
  writer.Key("capture_end_input_sample"); writer.Uint64(captureEndInputSample);
  writer.Key("timestamp"); writer.Uint64(timestamp);
  writer.Key("atsc_reference"); writer.StartObject();
  writer.Key("requested_mode");
  writer.String(frame.atscReferenceRequested.c_str());
  writer.Key("effective_mode");
  writer.String(frame.atscReferenceEffective.c_str());
  writer.Key("reason"); writer.String(frame.atscReferenceReason.c_str());
  if (frame.atscPilot)
  {
    writer.Key("pilot_candidate"); writer.Bool(frame.atscPilot->pilotCandidate);
    writer.Key("coherence"); writer.Double(frame.atscPilot->coherence);
    writer.Key("cfo_hz"); writer.Double(frame.atscPilot->cfoHz);
  }
  if (frame.atscPilotProjection)
  {
    writer.Key("projection_applied");
    writer.Bool(frame.atscPilotProjection->applied);
    writer.Key("heldout_suppression_db");
    writer.Double(frame.atscPilotProjection->heldoutSuppressionDb);
    writer.Key("removed_power_fraction");
    writer.Double(frame.atscPilotProjection->removedPowerFraction);
  }
  writer.EndObject();
  writer.Key("input_processing"); writer.StartObject();
  writer.Key("path"); writer.String(frame.inputProcessing.path.c_str());
  writer.Key("staging_widened_samples"); writer.Uint64(frame.inputProcessing.widenedSamples);
  writer.Key("staging_copied_samples"); writer.Uint64(frame.inputProcessing.copiedSamples);
  writer.Key("contiguous_fp64_samples"); writer.Uint64(frame.inputProcessing.contiguousSamples);
  writer.Key("contiguous_fp64_bytes_written"); writer.Uint64(frame.inputProcessing.contiguousSamples * sizeof(std::complex<double>));
  writer.Key("contiguous_allocations"); writer.Uint64(frame.inputProcessing.contiguousAllocations);
  writer.Key("deque_materializations"); writer.Uint64(frame.inputProcessing.dequeMaterializations);
  writer.Key("deque_materialized_bytes"); writer.Uint64(frame.inputProcessing.materializedSamples * sizeof(std::complex<double>));
  writer.Key("deque_materialization_ms"); writer.Double(frame.inputProcessing.materializationMs);
  writer.Key("ingestion_ms"); writer.Double(frame.inputProcessing.ingestionMs);
  writer.Key("pilot_quality_ms"); writer.Double(frame.inputProcessing.pilotQualityMs);
  writer.Key("pilot_projection_ms"); writer.Double(frame.inputProcessing.pilotProjectionMs);
  writer.EndObject();
  const auto writeAcceleration = [&](const char* key,
      const blah2::AccelerationStatus& status, bool gpu, bool cpu) {
    writer.Key(key); writer.StartObject();
    writer.Key("requested"); writer.String(status.requested.c_str());
    writer.Key("active"); writer.String(status.active.c_str());
    writer.Key("device"); writer.String(status.device.c_str());
    writer.Key("state"); writer.String(status.state.c_str());
    writer.Key("reason"); writer.String(status.reason.c_str());
    writer.Key("cpuMs"); writer.Double(status.cpuMs);
    writer.Key("gpuMs"); writer.Double(status.gpuMs);
    writer.Key("gpuExecuted"); writer.Bool(gpu);
    writer.Key("cpuExecuted"); writer.Bool(cpu);
    writer.Key("execution");
    rapidjson::Document metadata;
    const bool validEnd = frame.inputSamplesPerOutput &&
      frame.endInputSample <= UINT64_MAX - frame.inputSamplesPerOutput;
    blah2::stage_execution_json(true, gpu, cpu,
      captureSamplePositionAvailable && validEnd, captureEpoch,
      frame.firstInputSample, validEnd ?
        frame.endInputSample + frame.inputSamplesPerOutput : 0,
      metadata.GetAllocator()).Accept(writer);
    writer.EndObject();
  };
  writeAcceleration("acceleration", frame.acceleration,
    frame.ambiguityGpuExecuted, frame.ambiguityCpuExecuted);
  writeAcceleration("clutterAcceleration", frame.clutterAcceleration,
    frame.clutterGpuExecuted, frame.clutterCpuExecuted);
  writer.Key("trackerDiagnostics");
  writeTrackerFrameDiagnostics(writer, frame.trackerDiagnostics);
  writer.Key("tracker_birth"); writer.StartObject();
  writer.Key("min_snr_db"); writer.Double(frame.trackerMinimumSnrDb);
  writer.Key("source"); writer.String(frame.trackerMinimumSnrOverridden ?
    "illuminator_override" : "global");
  writer.EndObject();
  writer.Key("clutterGpuExecuted"); writer.Bool(frame.clutterGpuExecuted);
  writer.Key("clutterCpuExecuted"); writer.Bool(frame.clutterCpuExecuted);
  writer.Key("ambiguityGpuExecuted"); writer.Bool(frame.ambiguityGpuExecuted);
  writer.Key("ambiguityCpuExecuted"); writer.Bool(frame.ambiguityCpuExecuted);
  if (frame.detectorIntegration) {
    writer.Key("detector_integration");
    blah2::detection::writeIntegration(writer, *frame.detectorIntegration);
  }
  writer.Key("map"); writer.RawValue(map.c_str(), map.size(), rapidjson::kObjectType);
  writer.Key("detections"); writer.RawValue(detections.c_str(), detections.size(), rapidjson::kObjectType);
  writer.Key("tracks"); writer.RawValue(tracks.c_str(), tracks.size(), rapidjson::kObjectType);
  if (frame.residentProducts && !frame.residentProducts->bearing.empty()) {
    const auto bearing = blah2::analysis::annotate_native_product(
      resident_product_timestamp(frame.residentProducts->bearing, timestamp),
      native_product_identity(frame, {timestamp, {}, captureSamplePositionAvailable,
        captureEpoch, captureFirstInputSample, captureEndInputSample}));
    writer.Key("bearing"); writer.RawValue(bearing.c_str(), bearing.size(), rapidjson::kObjectType);
  }
  writer.EndObject();
  return buffer.GetString();
}

int main(int argc, char **argv)
try
{
  // This read-only command checks adapter/runtime ABI without creating a
  // receiver, opening hardware, reading a configuration or starting a service.
  if (argc == 2 && std::string(argv[1]) == "--receiver-status") {
    std::cout << blah2::receiver_module_status_json() << '\n';
    return 0;
  }
  // Package construction uses this hardware-free report to prove that the
  // processor and its optional GPU modules were produced by the same build.
  if (argc == 2 && std::string(argv[1]) == "--processing-capabilities") {
    std::cout << "{\"schema\":1,\"portableGpu\":"
#ifdef VECTORWARP_GPU
              << "true"
#else
              << "false"
#endif
              << ",\"residentGpu\":"
#ifdef VECTORWARP_RESIDENT_GPU
              << "true"
#else
              << "false"
#endif
              << ",\"widebandGpu\":"
#ifdef VECTORWARP_WIDEBAND_GPU
              << "true"
#else
              << "false"
#endif
              << ",\"reason\":\"\"}\n";
    return 0;
  }
  // input handling
  signal(SIGTERM, signal_callback_handler);
  signal(SIGINT, signal_callback_handler);
  std::string file = getopt_process(argc, argv);
  std::ifstream filePath(file);
  if (!filePath.is_open())
  {
    std::cout << "Error: Config file does not exist." << "\n";
    exit(1);
  }

  // config handling
  std::string contents = ryml_get_file(file.c_str());
  ryml::Tree tree = ryml::parse_in_arena(ryml::to_csubstr(contents));

  // set up capture
  uint32_t fs = 0, fc = 0;
  uint16_t port_capture = 0;
  std::string type, path, replayFile, ip_capture;
  bool state, loop;
  std::string replayFormat = "auto";
  std::string replayProvenanceMode = "legacy-unqualified";
  std::string replayRecordingHash, replayAcquisitionHash;
  uint32_t legacyReplayBlockSamples = 0;
  uint64_t replayStartSample = 0;
  bool residentSnapshotEnabled=false;
  uint64_t residentSnapshotMaxBytes=0;
  uint32_t residentSnapshotMaxFrames=0;
  std::string residentSnapshotDirectory,residentSnapshotSourceHash,
    residentSnapshotRfHash,residentSnapshotRfFile;
  tree["capture"]["fs"] >> fs;
  tree["capture"]["fc"] >> fc;
  tree["capture"]["device"]["type"] >> type;
  tree["save"]["path"] >> path;
  tree["capture"]["replay"]["state"] >> state;
  tree["capture"]["replay"]["loop"] >> loop;
  tree["capture"]["replay"]["file"] >> replayFile;
  if (tree["capture"]["replay"].has_child("format"))
    tree["capture"]["replay"]["format"] >> replayFormat;
  if (tree["capture"]["replay"].has_child("legacy_block_samples"))
    tree["capture"]["replay"]["legacy_block_samples"] >> legacyReplayBlockSamples;
  if (tree["capture"]["replay"].has_child("provenance"))
    tree["capture"]["replay"]["provenance"] >> replayProvenanceMode;
  if (tree["capture"]["replay"].has_child("start_sample"))
    tree["capture"]["replay"]["start_sample"] >> replayStartSample;
  if(tree["capture"]["replay"].has_child("artifacts")){
    auto artifacts=tree["capture"]["replay"]["artifacts"];
    blah2::gpu_artifact_binding::Config config;
    artifacts["enabled"] >> config.enabled;artifacts["confirmation"] >> config.confirmation;
    artifacts["descriptor"] >> config.descriptor;artifacts["nonce"] >> config.nonce;
    blah2::gpu_artifact_binding::configure(config,state,replayFormat);
  }
  if (tree["capture"]["replay"].has_child("qualification")) {
    auto qualification=tree["capture"]["replay"]["qualification"];
    blah2::replay_gpu_qualification::Config config;
    qualification["enabled"] >> config.enabled;
    qualification["confirmation"] >> config.confirmation;
    if(qualification.has_child("fault")) {
      auto fault=qualification["fault"];
      fault["kind"] >> config.kind;fault["route"] >> config.route;
      fault["frame"] >> config.frame;fault["lane"] >> config.lane;
      if(config.kind.empty())throw std::invalid_argument("GPU qualification fault kind is empty");
    }
    blah2::replay_gpu_qualification::configure(config,state,replayFormat);
  }
  const std::array replayHashes{std::make_pair("recording_sha256", &replayRecordingHash),
      std::make_pair("acquisition_sha256", &replayAcquisitionHash)};
  for (const auto& entry : replayHashes)
    if (tree["capture"]["replay"].has_child(ryml::to_csubstr(entry.first)))
      tree["capture"]["replay"][ryml::to_csubstr(entry.first)] >> *entry.second;
  if (tree["capture"]["replay"].has_child("qualification") &&
      tree["capture"]["replay"]["qualification"].has_child("acquisition")) {
    const auto acquisition = tree["capture"]["replay"]["qualification"]["acquisition"];
    for (const auto& entry : replayHashes)
      if (acquisition.has_child(ryml::to_csubstr(entry.first))) {
      std::string value; acquisition[ryml::to_csubstr(entry.first)] >> value;
      if (!entry.second->empty() && *entry.second != value)
        throw std::invalid_argument("Replay acquisition trust anchors disagree");
      *entry.second = value;
    }
  }
  if ((!replayRecordingHash.empty() && !blah2::acquisition::sha256(replayRecordingHash)) ||
      (!replayAcquisitionHash.empty() && (!blah2::acquisition::sha256(replayAcquisitionHash) || replayRecordingHash.empty())))
    throw std::invalid_argument("Replay trust anchors require lowercase recording SHA-256 and optional acquisition SHA-256");
  if (tree["capture"]["replay"].has_child("resident_snapshot")) {
    auto snapshot=tree["capture"]["replay"]["resident_snapshot"];
    snapshot["enabled"] >> residentSnapshotEnabled;
    if(residentSnapshotEnabled){
      snapshot["output_directory"] >> residentSnapshotDirectory;
      snapshot["max_frames"] >> residentSnapshotMaxFrames;
      snapshot["max_bytes"] >> residentSnapshotMaxBytes;
      snapshot["source_sha256"] >> residentSnapshotSourceHash;
      snapshot["rf_profile_sha256"] >> residentSnapshotRfHash;
      snapshot["rf_profile_file"] >> residentSnapshotRfFile;
    }
  }
  if (state) {
    auto replay = tree["capture"]["replay"];
    if (replay.has_child("sample_rate_hz")) replay["sample_rate_hz"] >> fs;
    if (replay.has_child("frequency_hz")) replay["frequency_hz"] >> fc;
  }
  tree["network"]["ip"] >> ip_capture;
  tree["network"]["ports"]["api"] >> port_capture;
  if (fs == 0 || fc == 0 || port_capture == 0)
  {
    std::cerr << "Invalid configuration: sample rate, centre frequency and API port must be positive.\n";
    return 1;
  }

  if (tree["process"]["tracker"].has_child("publication")) {
    const auto publication = tree["process"]["tracker"]["publication"];
    std::string mode;
    if (publication.has_child("mode")) publication["mode"] >> mode;
    if (mode != "native")
      throw std::invalid_argument("process.tracker.publication is retired: remove the external/hybrid publication block to explicitly select canonical native tracking");
  }
  bool multiIlluminatorEnabled = false;
  bool experimentalFmReferenceQualityFilter = false;
  blah2::multi_illuminator::AdvancedFeatureConfig multiAdvanced;
  std::vector<blah2::channelizer::IlluminatorDefinition> illuminatorDefinitions;
  std::unordered_map<std::string, double> illuminatorTrackerMinimumSnrDb;
  if (tree["process"].has_child("multi_illuminator"))
  {
    auto multi = tree["process"]["multi_illuminator"];
    if (multi.has_child("enabled")) multi["enabled"] >> multiIlluminatorEnabled;
    if (multi.has_child("input_phase_correction")) {
      std::string mode;
      multi["input_phase_correction"] >> mode;
      if (mode != "off")
        throw std::invalid_argument(
          "Multi-illuminator input phase correction is not available in this processor build");
    }
    if (multi.has_child("doppler_window")) {
      std::string window;
      multi["doppler_window"] >> window;
      if (window != "rectangular")
        throw std::invalid_argument(
          "Multi-illuminator Doppler windows other than rectangular are not available in this processor build");
    }
    if (multi.has_child("experimental_fm_reference_quality_filter"))
      multi["experimental_fm_reference_quality_filter"] >> experimentalFmReferenceQualityFilter;
    if (experimentalFmReferenceQualityFilter && !multiIlluminatorEnabled)
      throw std::invalid_argument(
        "Experimental FM reference quality filtering requires multi-illuminator processing");
    // Saved lane definitions are dormant while multi-illuminator mode is
    // disabled.  Do not validate them against a temporary single-lane tune;
    // otherwise switching modes can make a valid direct receiver unstartable.
    if (multiIlluminatorEnabled && multi.has_child("illuminators"))
    {
      std::set<std::string> ids;
      std::set<uint64_t> frequencies;
      for (auto item : multi["illuminators"].children())
      {
        bool enabled = false;
        if (item.has_child("enabled")) item["enabled"] >> enabled;
        if (!enabled) continue;
        blah2::channelizer::IlluminatorDefinition definition;
        item["id"] >> definition.id;
        item["standard"] >> definition.standard;
        item["frequency_hz"] >> definition.frequencyHz;
        item["occupied_bandwidth_hz"] >> definition.occupiedBandwidthHz;
        item["output_sample_rate_hz"] >> definition.outputRateHz;
        if (item.has_child("transmitter_location")) {
          auto site = item["transmitter_location"];
          if (!site.has_child("latitude") || !site.has_child("longitude") || !site.has_child("altitude"))
            throw std::invalid_argument("Lane transmitter_location requires latitude, longitude, altitude");
          std::array<double, 3> location{};
          site["latitude"] >> location[0]; site["longitude"] >> location[1]; site["altitude"] >> location[2];
          if (!std::all_of(location.begin(), location.end(), [](double x) { return std::isfinite(x); }) ||
              std::abs(location[0]) > 90 || std::abs(location[1]) > 180)
            throw std::invalid_argument("Invalid lane transmitter_location");
          definition.transmitterLocation = location;
        }
        if (item.has_child("detection_manifold_file")) item["detection_manifold_file"] >> definition.detectionManifoldFile;
        if (item.has_child("bearing_manifold_file")) item["bearing_manifold_file"] >> definition.bearingManifoldFile;
        if (item.has_child("tracker_min_snr_db")) {
          double threshold = 0;
          item["tracker_min_snr_db"] >> threshold;
          if (!std::isfinite(threshold) || threshold < -100 || threshold > 100)
            throw std::invalid_argument("Per-illuminator tracker_min_snr_db must be finite and between -100 and 100");
          illuminatorTrackerMinimumSnrDb.emplace(definition.id, threshold);
        }
        if (item.has_child("filter_taps_per_phase"))
          item["filter_taps_per_phase"] >> definition.tapsPerPhase;
        std::string inputRoleMode = "manual";
        if (item.has_child("input_role_mode"))
          item["input_role_mode"] >> inputRoleMode;
        if (inputRoleMode != "manual" && inputRoleMode != "automatic")
          throw std::invalid_argument(
            "Per-illuminator input_role_mode must be manual or automatic");
        if (inputRoleMode == "automatic")
          throw std::invalid_argument(
            "Automatic per-illuminator input roles are not available in this processor build");
        const bool atscStandard = definition.standard == "ATSC" ||
          definition.standard == "ATSC-1.0" || definition.standard == "ATSC-3.0";
        definition.atscReferenceMode = atscStandard ? "automatic" : "raw";
        if (item.has_child("atsc_reference_mode"))
          item["atsc_reference_mode"] >> definition.atscReferenceMode;
        (void)blah2::conditioning::resolveAtscReferenceMode(definition.standard,
          definition.atscReferenceMode, state);
        if (item.has_child("reference_channel"))
        {
          definition.hasInputRoles = true;
          item["reference_channel"] >> definition.referenceChannel;
          if (item.has_child("surveillance_channels"))
            for (auto channel : item["surveillance_channels"].children())
            {
              uint32_t value = 0;
              channel >> value;
              definition.surveillanceChannels.push_back(value);
            }
        }
        if (!std::regex_match(definition.id,
              std::regex("^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$")) ||
            !ids.insert(definition.id).second ||
            !frequencies.insert(definition.frequencyHz).second ||
            (definition.standard != "AM" && definition.standard != "FM" &&
              definition.standard != "DAB" && definition.standard != "DRM" &&
              definition.standard != "ATSC" &&
              definition.standard != "ATSC-1.0" &&
              definition.standard != "ATSC-3.0" &&
              definition.standard != "DVB-T/T2" &&
              definition.standard != "ISDB-T" &&
              definition.standard != "DTMB" &&
              definition.standard != "CUSTOM") ||
            !definition.frequencyHz || !definition.outputRateHz ||
            !definition.occupiedBandwidthHz || definition.tapsPerPhase < 4 ||
            definition.tapsPerPhase > 128 || fs % definition.outputRateHz ||
            definition.occupiedBandwidthHz >= definition.outputRateHz ||
            std::abs(int64_t(definition.frequencyHz) - int64_t(fc)) +
              int64_t(definition.occupiedBandwidthHz) / 2 >= int64_t(fs) / 2)
          throw std::invalid_argument("Invalid enabled multi-illuminator definition");
        illuminatorDefinitions.push_back(std::move(definition));
      }
    }
    if (multiIlluminatorEnabled &&
        (illuminatorDefinitions.empty() || illuminatorDefinitions.size() > 16))
      throw std::invalid_argument("Multi-illuminator mode requires 1 to 16 enabled illuminators");
  }
  DopplerRidgeConfig dopplerRidge;
  if (tree["process"]["tracker"]["initiate"].has_child("doppler_ridge_clutter")) {
    auto ridge = tree["process"]["tracker"]["initiate"]["doppler_ridge_clutter"];
    if (ridge.has_child("enable")) ridge["enable"] >> dopplerRidge.enabled;
    if (ridge.has_child("persistence_cpis"))
      ridge["persistence_cpis"] >> dopplerRidge.persistenceCpis;
    if (ridge.has_child("doppler_tolerance_hz"))
      ridge["doppler_tolerance_hz"] >> dopplerRidge.toleranceHz;
    if (ridge.has_child("minimum_delay_separation_km"))
      ridge["minimum_delay_separation_km"] >> dopplerRidge.minimumDelaySeparationKm;
  }
  if (multiIlluminatorEnabled)
  {
    const auto process = tree["process"];
    if (process.has_child("surveillance_fusion") &&
        process["surveillance_fusion"].has_child("mode"))
      process["surveillance_fusion"]["mode"] >>
        multiAdvanced.surveillanceFusionMode;
    const auto detection = process["detection"];
    if (detection.has_child("integration_cpis"))
      detection["integration_cpis"] >> multiAdvanced.detectorIntegrationCpis;
    if (detection.has_child("integration_motion_bins"))
      detection["integration_motion_bins"] >>
        multiAdvanced.detectorIntegrationMotionBins;
    if (detection.has_child("track_before_detect") &&
        detection["track_before_detect"].has_child("enable"))
      detection["track_before_detect"]["enable"] >>
        multiAdvanced.trackBeforeDetect;
    if (multiAdvanced.trackBeforeDetect) {
      const auto tbd = detection["track_before_detect"];
      uint32_t delay = 1, doppler = 1;
      bool fractional = false;
      multiAdvanced.detectorIntegrationCpis = 8;
      if (tbd.has_child("max_cpis")) tbd["max_cpis"] >> multiAdvanced.detectorIntegrationCpis;
      if (tbd.has_child("motion_bins")) { tbd["motion_bins"] >> delay; doppler = delay; }
      if (tbd.has_child("delay_motion_bins")) tbd["delay_motion_bins"] >> delay;
      if (tbd.has_child("doppler_motion_bins")) tbd["doppler_motion_bins"] >> doppler;
      if (tbd.has_child("fractional_delay")) tbd["fractional_delay"] >> fractional;
      multiAdvanced.detectorIntegrationMotionBins =
        track_before_detect::encodeMotionBins(delay, doppler, fractional);
    }
    const auto trackerNode = process["tracker"];
    multiAdvanced.trackerBeamToleranceConfigured =
      trackerNode.has_child("beam_tolerance");
    multiAdvanced.trackerGpuCapacityConfigured =
      trackerNode.has_child("gpu_capacity");
    if (trackerNode.has_child("initiate"))
    {
      const auto initiate = trackerNode["initiate"];
      if (initiate.has_child("acceleration_model"))
        initiate["acceleration_model"] >>
          multiAdvanced.trackerAccelerationModel;
      if (initiate.has_child("stationary_clutter") &&
          initiate["stationary_clutter"].has_child("enable"))
        initiate["stationary_clutter"]["enable"] >>
          multiAdvanced.stationaryClutter;
    }
    if (process.has_child("bearing"))
    {
      const auto bearing = process["bearing"];
      if (bearing.has_child("enable"))
        bearing["enable"] >> multiAdvanced.bearing;
      if (bearing.has_child("manifold_recorder") &&
          bearing["manifold_recorder"].has_child("enabled"))
      {
        std::string value;
        bearing["manifold_recorder"]["enabled"] >> value;
        // Auto cannot activate with the only admitted (noncoherent) fusion
        // mode, while explicit on/true would otherwise be silently ignored.
        multiAdvanced.bearingManifoldRecorder =
          value == "true" || value == "on";
      }
    }
    // Do this before opening output sockets or allocating capture hardware.
    blah2::multi_illuminator::validateAdvancedFeatures(multiAdvanced);
  }


  // set up socket
  sleep(2);
  // Input adapters establish coherence and metadata support; radar features
  // below are gated by those facts and the validated channel topology.
  const auto inputAdapter = blah2::capture_topology::adapter(type);
  blah2::capture_topology::Request topologyRequest;
  const auto readChannels = [](ryml::NodeRef node) {
    std::vector<uint32_t> result;
    for (auto child : node.children()) {
      uint32_t channel = UINT32_MAX; child >> channel; result.push_back(channel);
    }
    return result;
  };
  if (tree["process"].has_child("reference_synthesis")) {
    auto reference = tree["process"]["reference_synthesis"];
    if (reference.has_child("mode")) reference["mode"] >> topologyRequest.referenceMode;
    if (reference.has_child("channels"))
      topologyRequest.referenceChannels = readChannels(reference["channels"]);
  }
  const bool recordedTopology = state && tree["capture"]["replay"].has_child("channel_count");
  if (recordedTopology) {
    auto replay = tree["capture"]["replay"];
    replay["channel_count"] >> topologyRequest.channelCount;
    if (replay.has_child("reference_mode")) replay["reference_mode"] >> topologyRequest.referenceMode;
    // A recording owns its channel mapping. Never inherit live receiver roles.
    topologyRequest.referenceChannels.reset();
    if (replay.has_child("reference_channel")) {
      uint32_t channel = UINT32_MAX; replay["reference_channel"] >> channel;
      topologyRequest.referenceChannel = channel;
    }
    if (replay.has_child("reference_channels"))
      topologyRequest.referenceChannels = readChannels(replay["reference_channels"]);
    if (replay.has_child("surveillance_channels"))
      topologyRequest.surveillanceChannels = readChannels(replay["surveillance_channels"]);
    topologyRequest.coherent = topologyRequest.referenceMode == "array_eigenbeam";
  } else {
    topologyRequest.coherent = inputAdapter.coherent;
    if (inputAdapter.variableChannels) {
      auto device = tree["capture"]["device"];
      if (!device.has_child("channel_count"))
        throw std::invalid_argument("Variable-channel capture requires channel_count");
      device["channel_count"] >> topologyRequest.channelCount;
      if (device.has_child("reference_channel")) {
        uint32_t channel = UINT32_MAX; device["reference_channel"] >> channel;
        topologyRequest.referenceChannel = channel;
      }
      if (device.has_child("surveillance_channels"))
        topologyRequest.surveillanceChannels = readChannels(device["surveillance_channels"]);
    }
  }
  const auto inputTopology = blah2::capture_topology::resolve(topologyRequest);
  const bool coherentInput = inputTopology.coherent;
  const bool arrayReference = inputTopology.arrayReference;
  const auto captureChannelCount = std::size_t(inputTopology.channelCount);
  blah2::RecordingProvenance replayIdentityProvenance;
  std::optional<std::array<double,3>> geographicRx, geographicTx;
  const auto coordinates = [](const rapidjson::Value& site) {
    return std::array<double,3>{site["latitude"].GetDouble(), site["longitude"].GetDouble(), site["altitude"].GetDouble()};
  };
  if (state) {
    blah2::ReplayOptions options{replayFormat,type,uint32_t(captureChannelCount),fs,fc,
      legacyReplayBlockSamples,replayProvenanceMode};
    options.expectedRecordingSha256 = replayRecordingHash;
    options.expectedAcquisitionSha256 = replayAcquisitionHash;
    blah2::RecordingReader identity(replayFile, options);
    replayIdentityProvenance = identity.provenance();
    for (auto& lane : illuminatorDefinitions) lane.transmitterLocation.reset();
    if (replayIdentityProvenance.geographicSitesAvailable) {
      rapidjson::Document sites;
      sites.Parse<rapidjson::kParseFullPrecisionFlag>(replayIdentityProvenance.acquisitionContext.geographicSitesJson.c_str());
      geographicRx = coordinates(sites["rx"]);
      if (!sites["tx"].IsNull()) geographicTx = coordinates(sites["tx"]);
      for (auto& lane : illuminatorDefinitions) if (sites["illuminators"].HasMember(lane.id.c_str())) {
        const auto& recorded = sites["illuminators"][lane.id.c_str()];
        if (recorded["frequencyHz"].GetUint64() != lane.frequencyHz)
          throw std::invalid_argument("Replay illuminator identity/frequency differs from recorded geographic site: " + lane.id);
        lane.transmitterLocation = coordinates(recorded["transmitter"]);
      }
    }
  } else if (tree.rootref().has_child("location")) {
    const std::array geographicSites{std::make_pair("rx", &geographicRx),
        std::make_pair("tx", &geographicTx)};
    for (const auto& entry : geographicSites)
      if (tree["location"].has_child(ryml::to_csubstr(entry.first))) {
        const auto site = tree["location"][ryml::to_csubstr(entry.first)]; std::array<double,3> value{};
        site["latitude"] >> value[0]; site["longitude"] >> value[1]; site["altitude"] >> value[2];
        *entry.second = value;
      }
  }
  const auto referenceChannel = inputTopology.referenceChannel;
  const auto& referenceChannels = inputTopology.referenceChannels;
  const auto& surveillanceChannels = inputTopology.surveillanceChannels;
  const auto& referenceMode = topologyRequest.referenceMode;
  const bool appliedGainMetadata = state || inputAdapter.appliedGains;
  bool bearingEnabled=false,manifoldRecorderEnabled=false;
  uint32_t bearingMinConsistent=3,bearingMinPeakSeparationDb=3;
  std::string bearingMethod="coherent_beam_bank",bearingReferenceAnchor="live_eigenbeam";uint32_t bearingScanBeams=0,
    bearingCandidatePeaks=3,bearingGuardRadius=1,bearingTrainingRadius=3;
  double bearingDiagonalLoading=.01,bearingMaxRateDegPerSecond=0;std::vector<uint32_t>bearingChannelMapping;
  std::string bearingManifoldFile;
  if(tree["process"].has_child("bearing")){
    auto bearing=tree["process"]["bearing"];
    if(bearing.has_child("enable"))bearing["enable"]>>bearingEnabled;
    if(bearing.has_child("min_consistent"))bearing["min_consistent"]>>bearingMinConsistent;
    if(bearing.has_child("min_peak_separation_db"))
      bearing["min_peak_separation_db"]>>bearingMinPeakSeparationDb;
    if(bearing.has_child("method"))bearing["method"]>>bearingMethod;
    if(bearing.has_child("reference_anchor"))bearing["reference_anchor"]>>bearingReferenceAnchor;
    if(bearing.has_child("scan_beams"))bearing["scan_beams"]>>bearingScanBeams;
    if(bearing.has_child("candidate_peaks")) {
      uint32_t legacyCandidatePeaks=0; bearing["candidate_peaks"]>>legacyCandidatePeaks;
      if(legacyCandidatePeaks!=3)
        throw std::invalid_argument("process.bearing.candidate_peaks is retired; only the canonical three-lobe contract is supported");
    }
    if(bearing.has_child("guard_radius"))bearing["guard_radius"]>>bearingGuardRadius;
    if(bearing.has_child("training_radius"))bearing["training_radius"]>>bearingTrainingRadius;
    if(bearing.has_child("diagonal_loading"))bearing["diagonal_loading"]>>bearingDiagonalLoading;
    if(bearing.has_child("max_rate_deg_s"))bearing["max_rate_deg_s"]>>bearingMaxRateDegPerSecond;
    if(bearing.has_child("manifold_file"))bearing["manifold_file"]>>bearingManifoldFile;
    if(bearing.has_child("channel_mapping"))for(auto value:bearing["channel_mapping"].children()){
      uint32_t channel=0;value>>channel;bearingChannelMapping.push_back(channel);}
    if(bearing.has_child("manifold_recorder")){
      auto recorder=bearing["manifold_recorder"];
      if(recorder.has_child("enabled")){
        std::string mode;recorder["enabled"]>>mode;
        if(mode=="true"||mode=="on")manifoldRecorderEnabled=true;
        else if(mode=="auto") {
          bool detectionEnabled=false,truthEnabled=false,mapping=false,geometryConfirmed=false,
            orientationConfirmed=false;
          if(tree["process"].has_child("detection")&&
             tree["process"]["detection"].has_child("enable"))
            tree["process"]["detection"]["enable"]>>detectionEnabled;
          if(tree.rootref().has_child("truth")&&tree["truth"].has_child("adsb")&&
             tree["truth"]["adsb"].has_child("enabled"))
            tree["truth"]["adsb"]["enabled"]>>truthEnabled;
          std::string fusionMode="noncoherent";
          if(tree["process"].has_child("surveillance_fusion")&&
             tree["process"]["surveillance_fusion"].has_child("mode"))
            tree["process"]["surveillance_fusion"]["mode"]>>fusionMode;
          const auto device=tree["capture"]["device"];
          if(device.has_child("array_geometry")) {
            const auto geometry=device["array_geometry"];
            if(geometry.has_child("mapping_confirmed"))geometry["mapping_confirmed"]>>mapping;
            if(geometry.has_child("geometry_confirmed"))geometry["geometry_confirmed"]>>geometryConfirmed;
            if(geometry.has_child("orientation_confirmed"))geometry["orientation_confirmed"]>>orientationConfirmed;
          }
          std::string gainControl="fixed";
          if(device.has_child("heimdall")&&device["heimdall"].has_child("gain_control"))
            device["heimdall"]["gain_control"]>>gainControl;
          const bool requestedGainsAvailable=!blah2::acquisition::requestedGainsFromDevice(device).empty();
          manifoldRecorderEnabled=!state&&gainControl=="fixed"&&requestedGainsAvailable&&
            coherentInput&&appliedGainMetadata&&detectionEnabled&&truthEnabled&&
            mapping&&geometryConfirmed&&orientationConfirmed&&
            (fusionMode=="coherent_beam_bank"||fusionMode=="noise_whitened_beam_bank")&&
            tree["network"]["ports"].has_child("manifold");
        }
        else if(mode!="false"&&mode!="off")throw std::invalid_argument(
          "Manifold recorder requires auto/on/off");
      }
    }
  }
  if (manifoldRecorderEnabled && !appliedGainMetadata)
    throw std::invalid_argument("Manifold recording requires per-channel applied gain metadata");
  uint16_t port_map, port_detection, port_timestamp, 
    port_timing, port_iqdata, port_track;
  uint16_t port_bearing=0,port_manifold=0;
  uint16_t port_illuminator=0;
  bool causalEnabled=false;uint32_t causalMaximum=64;uint16_t portCausal=0;
  uint64_t causalSequence=0,causalFormatDrops=0;
  std::string causalConfigHash,causalModuleHash;
  if(tree["process"].has_child("evidence")&&tree["process"]["evidence"].has_child("causal_funnel")){
    const auto node=tree["process"]["evidence"]["causal_funnel"];
    node["enabled"]>>causalEnabled;
    if(causalEnabled){node["max_candidates"]>>causalMaximum;
      if(!causalMaximum||causalMaximum>64||!tree["network"]["ports"].has_child("causal_funnel"))
        throw std::invalid_argument("Causal diagnostics require bounded candidates and a port");
      tree["network"]["ports"]["causal_funnel"]>>portCausal;
      if(!portCausal)throw std::invalid_argument("Causal diagnostics require nonzero port");}}
  std::string ip;
  tree["network"]["ports"]["map"] >> port_map;
  tree["network"]["ports"]["detection"] >> port_detection;
  tree["network"]["ports"]["track"] >> port_track;
  if(bearingEnabled){
    if(!tree["network"]["ports"].has_child("bearing"))
      throw std::invalid_argument("Bearing output requires network.ports.bearing");
    tree["network"]["ports"]["bearing"]>>port_bearing;
  }
  if(manifoldRecorderEnabled){
    if(!tree["network"]["ports"].has_child("manifold"))
      throw std::invalid_argument("Manifold recorder requires network.ports.manifold");
    tree["network"]["ports"]["manifold"]>>port_manifold;
  }
  if(multiIlluminatorEnabled){
    if(!tree["network"]["ports"].has_child("illuminator"))
      throw std::invalid_argument("Multi-illuminator output requires network.ports.illuminator");
    tree["network"]["ports"]["illuminator"]>>port_illuminator;
  }
  tree["network"]["ports"]["timestamp"] >> port_timestamp;
  tree["network"]["ports"]["timing"] >> port_timing;
  tree["network"]["ports"]["iqdata"] >> port_iqdata;
  tree["network"]["ip"] >> ip;
  
  try {
    socket_map = std::make_unique<Socket>(ip, port_map);
    socket_detection = std::make_unique<Socket>(ip, port_detection);
    socket_track = std::make_unique<Socket>(ip, port_track);
    if(multiIlluminatorEnabled)
      socket_illuminator = std::make_unique<Socket>(ip, port_illuminator);
    if(bearingEnabled)socket_bearing = std::make_unique<Socket>(ip, port_bearing);
    if(manifoldRecorderEnabled)socket_manifold = std::make_unique<Socket>(ip, port_manifold);
    socket_timestamp = std::make_unique<Socket>(ip, port_timestamp);
    socket_timing = std::make_unique<Socket>(ip, port_timing);
    socket_iqdata = std::make_unique<Socket>(ip, port_iqdata);
  } catch (const std::exception& e) {
    std::cerr << "Failed to initialize socket connections: " << e.what() << "\n";
    std::cerr << "Make sure the server at " << ip << " is reachable." << "\n";
    return 1;
  }

  // set up FFTW multithreading
  if (fftw_init_threads() == 0)
  {
    std::cout << "Error in FFTW multithreading." << "\n";
    return -1;
  }
  // The experimental anchor consumes canonical live or recorded topology;
  // transport selection must not change its geometry or processing admission.
  if(bearingReferenceAnchor=="fixed_site_projected"){
    const auto allFive=[](const std::vector<uint32_t>& values){
      if(values.size()!=5)return false;
      for(uint32_t index=0;index<5;++index)if(values[index]!=index)return false;
      return true;
    };
    auto process=tree["process"];
    bool clutter=false;std::string fusion,acceleration="auto";double nullDb=0;
    if(process.has_child("clutter")&&process["clutter"].has_child("enable"))process["clutter"]["enable"]>>clutter;
    if(process.has_child("performance")&&process["performance"].has_child("acceleration"))
      process["performance"]["acceleration"]>>acceleration;
    if(process.has_child("surveillance_fusion")){
      auto node=process["surveillance_fusion"];
      if(node.has_child("mode"))node["mode"]>>fusion;
      if(node.has_child("direct_path_null_db"))node["direct_path_null_db"]>>nullDb;
    }
    if(captureChannelCount!=5||!allFive(surveillanceChannels)||
       referenceMode!="array_eigenbeam"||!allFive(referenceChannels)||
       !clutter||(acceleration!="gpu-resident"&&acceleration!="auto")||!bearingEnabled||bearingMethod!="covariance_mvdr"||
       tree["process"]["bearing"].has_child("manifold_file")||fusion!="coherent_beam_bank"||nullDb!=0)
      throw std::invalid_argument("fixed_site_projected requires enabled five-channel GPU-resident array-eigenbeam clutter cancellation, ideal covariance_mvdr bearing and coherent_beam_bank with direct_path_null_db: 0");
  }

  std::string surveillanceFusionMode = "noncoherent";
  uint32_t coherentBeamCount = 12;
  double effectiveBeamTrials = 0;
  double directPathNullDb = 0;
  std::string surveillanceManifoldFile;
  bool surveillanceManifoldConfigured = false;
  std::vector<std::complex<float>> surveillanceManifoldResponses;
  std::vector<std::complex<float>> surveillanceManifoldDirectResponse;
  std::optional<blah2::bearing_manifold::Calibration> surveillanceManifoldCalibration,
    bearingManifoldCalibration;
  std::vector<std::array<double, 3>> coherentPositions;
  double directPathAzimuthRad = 0,xAxisBearingDeg = 0,bearingUncertaintyFloorDeg = 0;
  if (tree["process"].has_child("surveillance_fusion"))
  {
    auto fusion = tree["process"]["surveillance_fusion"];
    if (fusion.has_child("mode")) fusion["mode"] >> surveillanceFusionMode;
    if (fusion.has_child("beams")) fusion["beams"] >> coherentBeamCount;
    if (fusion.has_child("effective_beam_trials"))
      fusion["effective_beam_trials"] >> effectiveBeamTrials;
    if (fusion.has_child("direct_path_null_db"))
      fusion["direct_path_null_db"] >> directPathNullDb;
    if (fusion.has_child("manifold_file")) {
      surveillanceManifoldConfigured = true;
      fusion["manifold_file"] >> surveillanceManifoldFile;
    }
  }
  if (surveillanceFusionMode != "noncoherent" &&
      surveillanceFusionMode != "coherent_beam_bank" &&
      surveillanceFusionMode != "noise_whitened_beam_bank")
    throw std::invalid_argument("Unknown surveillance-fusion mode");
  const bool coherentFusion = surveillanceFusionMode != "noncoherent";
  if (!effectiveBeamTrials) effectiveBeamTrials = coherentBeamCount;
  if (!std::isfinite(effectiveBeamTrials) || effectiveBeamTrials < 1 ||
      effectiveBeamTrials > coherentBeamCount)
    throw std::invalid_argument("Effective beam trials must be from 1 through the beam count");
  if (!std::isfinite(directPathNullDb) || directPathNullDb < 0 || directPathNullDb > 40)
    throw std::invalid_argument("Direct-path null depth must be from 0 through 40 dB");
  if (!coherentFusion && (effectiveBeamTrials != coherentBeamCount || directPathNullDb != 0 ||
      surveillanceManifoldConfigured))
    throw std::invalid_argument("Beam trial/null controls require coherent surveillance fusion");
  if (coherentFusion)
  {
    if (!coherentInput || surveillanceChannels.size() < 2 ||
        !tree["capture"]["device"].has_child("array_geometry") ||
        !tree["capture"]["device"]["array_geometry"].has_child("elements"))
      throw std::invalid_argument("Coherent surveillance fusion requires at least two coherent channels and array geometry");
    const auto fusionGeometry = tree["capture"]["device"]["array_geometry"];
    bool fusionMappingConfirmed=false,fusionGeometryConfirmed=false,
      fusionOrientationConfirmed=false;
    if(fusionGeometry.has_child("mapping_confirmed"))
      fusionGeometry["mapping_confirmed"]>>fusionMappingConfirmed;
    if(fusionGeometry.has_child("geometry_confirmed"))
      fusionGeometry["geometry_confirmed"]>>fusionGeometryConfirmed;
    if(fusionGeometry.has_child("orientation_confirmed"))
      fusionGeometry["orientation_confirmed"]>>fusionOrientationConfirmed;
    if(!fusionMappingConfirmed||!fusionGeometryConfirmed||
       !fusionOrientationConfirmed)
      throw std::invalid_argument("Coherent surveillance fusion requires confirmed channel mapping, geometry, and orientation");
    std::vector<std::array<double, 3>> positions(captureChannelCount);
    std::vector<bool> found(captureChannelCount, false);
    for (auto element : tree["capture"]["device"]["array_geometry"]["elements"].children())
    {
      uint32_t channel = std::numeric_limits<uint32_t>::max();
      if (!element.has_child("daq_channel") || !element.has_child("position")) continue;
      element["daq_channel"] >> channel;
      if (channel >= captureChannelCount) continue;
      std::array<double, 3> position{};
      std::size_t coordinate = 0;
      for (auto value : element["position"].children())
      {
        if (coordinate >= position.size()) break;
        value >> position[coordinate++];
      }
      if (coordinate == position.size())
      {
        positions[channel] = position;
        found[channel] = true;
      }
    }
    for (uint32_t channel : surveillanceChannels)
    {
      if (!found[channel])
        throw std::invalid_argument("Coherent surveillance fusion geometry is missing a surveillance channel");
      coherentPositions.push_back(positions[channel]);
    }
    double apertureM = 0;
    for (size_t a = 0; a < coherentPositions.size(); ++a)
      for (size_t b = a + 1; b < coherentPositions.size(); ++b)
      {
        const double dx = coherentPositions[a][0] - coherentPositions[b][0];
        const double dy = coherentPositions[a][1] - coherentPositions[b][1];
        const double dz = coherentPositions[a][2] - coherentPositions[b][2];
        apertureM = std::max(apertureM, std::sqrt(dx * dx + dy * dy + dz * dz));
      }
    if (!(apertureM > 0) || !std::isfinite(apertureM))
      throw std::invalid_argument("Coherent fusion has invalid array aperture");
    // Fine scan spacing interpolates the beam maximum; it does not defeat the
    // physical aperture limit. Publish at least the half-power half-width.
    const double wavelengthM = 299792458.0 / double(fc);
    bearingUncertaintyFloorDeg = std::min(90.0,
      0.443 * wavelengthM / apertureM * 180.0 / std::acos(-1.0));
    {
      if (!arrayReference || referenceChannels != surveillanceChannels)
        throw std::invalid_argument("Coherent fusion requires the array reference to use every surveillance channel in the same order");
      auto geometry = tree["capture"]["device"]["array_geometry"];
      if (!geometry.has_child("x_axis_bearing_deg_true") ||
          !geographicRx || (!multiIlluminatorEnabled && !geographicTx))
        throw std::invalid_argument("Coherent fusion requires array orientation and receiver/transmitter sites; replay requires externally authenticated recorded sites");
      double rxLatDeg = 0, rxLonDeg = 0,
        txLatDeg = 0, txLonDeg = 0;
      geometry["x_axis_bearing_deg_true"] >> xAxisBearingDeg;
      rxLatDeg = geographicRx->at(0); rxLonDeg = geographicRx->at(1);
      if (multiIlluminatorEnabled) {
        for (const auto& lane : illuminatorDefinitions)
          if (!lane.transmitterLocation)
            throw std::invalid_argument("Coherent lane requires explicit transmitter_location: " + lane.id);
        txLatDeg = illuminatorDefinitions.front().transmitterLocation->at(0);
        txLonDeg = illuminatorDefinitions.front().transmitterLocation->at(1);
      } else {
        txLatDeg = geographicTx->at(0); txLonDeg = geographicTx->at(1);
      }
      const double radians = std::acos(-1.0) / 180.0;
      const double rxLat = rxLatDeg * radians, txLat = txLatDeg * radians;
      const double dLon = (txLonDeg - rxLonDeg) * radians;
      const double trueBearing = std::atan2(std::sin(dLon) * std::cos(txLat),
        std::cos(rxLat) * std::sin(txLat) - std::sin(rxLat) *
        std::cos(txLat) * std::cos(dLon));
      directPathAzimuthRad = std::remainder(xAxisBearingDeg * radians -
        trueBearing, 2 * std::acos(-1.0));
      if (!std::isfinite(directPathAzimuthRad) ||
          (rxLatDeg == txLatDeg && rxLonDeg == txLonDeg))
        throw std::invalid_argument("Coherent fusion has invalid receiver/transmitter geometry");
    }
  }

  // Each surveillance path owns its processing state and working buffers.
  // Zero selects automatic sizing; omitted values use the same bounded
  // defaults on every capture adapter.
  uint32_t configuredWorkers = 1;
  uint32_t fftThreads = 1;
  if (tree["process"].has_child("performance") &&
      tree["process"]["performance"].has_child("surveillance_workers"))
    tree["process"]["performance"]["surveillance_workers"] >>
      configuredWorkers;
  if (tree["process"].has_child("performance") &&
      tree["process"]["performance"].has_child("fft_threads"))
    tree["process"]["performance"]["fft_threads"] >> fftThreads;
  const auto threadPlan = blah2::choose_processing_threads(surveillanceChannels.size(),
    configuredWorkers, fftThreads, blah2::available_cpu_threads());
  const std::size_t surveillanceWorkers = threadPlan.workers;
  fftThreads = static_cast<uint32_t>(threadPlan.fftThreads);
  const std::size_t multiIlluminatorWorkers = multiIlluminatorEnabled ?
    blah2::choose_illuminator_lane_workers(illuminatorDefinitions.size(),
      0, threadPlan.availableCpus) : 1;
  if (multiIlluminatorEnabled)
    // Every active wideband lane consumes the common FFT output even when
    // radar contexts are processed in narrower cache-bounded batches.
    fftThreads = static_cast<uint32_t>(blah2::cap_fft_threads_for_lanes(
      fftThreads, illuminatorDefinitions.size(), threadPlan.availableCpus));
  blah2::set_fftw_planner_threads(fftThreads);
  std::cout << "Surveillance paths=" << surveillanceChannels.size()
    << " workers=" << surveillanceWorkers << " fft_threads=" << fftThreads
    << " available_cpus=" << threadPlan.availableCpus
    << "\n";

  Capture *capture = new Capture(type, fs, fc, path);
  capture->set_recording_context(blah2::acquisition::contextFromConfig(tree.rootref(),
    contents, referenceMode, referenceChannels, surveillanceChannels, coherentInput));
  if (state) capture->set_replay(loop, replayFile, replayFormat,
    legacyReplayBlockSamples, replayProvenanceMode, replayStartSample);
  if (state) capture->trust_replay_acquisition(replayRecordingHash, replayAcquisitionHash);
  if (tree["capture"]["replay"].has_child("qualification") &&
      tree["capture"]["replay"]["qualification"].has_child("acquisition")) {
    const auto acquisition = tree["capture"]["replay"]["qualification"]["acquisition"];
    if (!state || !blah2::replay_gpu_qualification::enabled())
      throw std::invalid_argument("Exact acquisition qualification requires explicit replay qualification confirmation");
    bool required = false;
    if (acquisition.has_child("require_exact")) acquisition["require_exact"] >> required;
    std::string recordingHash, acquisitionHash;
    if (acquisition.has_child("recording_sha256")) acquisition["recording_sha256"] >> recordingHash;
    if (acquisition.has_child("acquisition_sha256")) acquisition["acquisition_sha256"] >> acquisitionHash;
    if (!required || !blah2::acquisition::sha256(recordingHash) ||
        (!acquisitionHash.empty() && !blah2::acquisition::sha256(acquisitionHash)))
      throw std::invalid_argument("Exact acquisition qualification requires require_exact:true and trusted lowercase SHA-256 identities");
    capture->require_exact_replay_acquisition(recordingHash, acquisitionHash);
  }
  if (!state && tree["capture"].has_child("aircraft_clips")) {
    bool enabled = false;
    double preSeconds = 0, postSeconds = 0;
    auto clips = tree["capture"]["aircraft_clips"];
    if (clips.has_child("enabled")) clips["enabled"] >> enabled;
    if (clips.has_child("pre_seconds")) clips["pre_seconds"] >> preSeconds;
    if (clips.has_child("post_seconds")) clips["post_seconds"] >> postSeconds;
    capture->configure_aircraft_clips(enabled, preSeconds, postSeconds);
  }

  // Keep one bounded capture queue per coherent input.
  double tCpi, tBuffer;
  tree["process"]["data"]["cpi"] >> tCpi;
  tree["process"]["data"]["buffer"] >> tBuffer;
  const uint32_t captureBufferSamples = static_cast<uint32_t>(tCpi*tBuffer*fs);
  const uint32_t nSamples = blah2::pipeline::resolveCpiSamples(fs, tCpi);
  const char* pairedQueueEnv = std::getenv("VECTORWARP_RSPDUO_CPI_QUEUE");
  const bool pairedQueueRequested = !state && type == "RspDuo" &&
    captureChannelCount == 2 && pairedQueueEnv && std::string(pairedQueueEnv) == "1";
  std::vector<std::unique_ptr<IqData>> captureStorage;
  std::vector<IqData *> captureBuffers;
  for (std::size_t channel = 0; channel < captureChannelCount; channel++)
  {
    captureStorage.push_back(std::make_unique<IqData>(captureBufferSamples));
    captureBuffers.push_back(captureStorage.back().get());
  }

  // set up process CPI
  std::unique_ptr<PairedCpiQueue> pairedCpiQueue;
  if (pairedQueueRequested) {
    if (!nSamples || captureBufferSamples < 2 * uint64_t(nSamples))
      throw std::invalid_argument("Invalid paired CPI queue geometry");
    // One CPI may be actively filled; retain the configured bounded capture
    // capacity across ready plus partial data, rather than adding a hidden CPI.
    const size_t cpis = captureBufferSamples / nSamples;
    pairedCpiQueue = std::make_unique<PairedCpiQueue>(nSamples,
      cpis - 1);
    capture->set_paired_cpi_queue(pairedCpiQueue.get());
    std::cout << "RSPduo paired CPI queue requested: " << cpis << " CPI capacity\n";
  }
  auto captureOwner = std::make_shared<std::vector<std::unique_ptr<IqData>>>();
  auto& captureData = *captureOwner;
  for (std::size_t channel = 0; channel < captureChannelCount; ++channel)
    captureData.push_back(std::make_unique<IqData>(nSamples));

  // set up process ambiguity
  int32_t configuredDelayMin, configuredDelayMax;
  int32_t delayMin, delayMax;
  int32_t dopplerMin, dopplerMax;
  bool roundHamming = true;
  tree["process"]["ambiguity"]["delayMin"] >> configuredDelayMin;
  tree["process"]["ambiguity"]["delayMax"] >> configuredDelayMax;
  tree["process"]["ambiguity"]["dopplerMin"] >> dopplerMin;
  tree["process"]["ambiguity"]["dopplerMax"] >> dopplerMax;
  std::optional<uint32_t> delayReferenceRateHz;
  if (tree["process"].has_child("delay_reference_sample_rate_hz"))
  {
    uint32_t value = 0;
    tree["process"]["delay_reference_sample_rate_hz"] >> value;
    if (!value) throw std::invalid_argument("Delay reference sample rate must be positive");
    delayReferenceRateHz = value;
  }
  blah2::multi_illuminator::RadarConfig captureDelayProjection;
  captureDelayProjection.delayMin = configuredDelayMin;
  captureDelayProjection.delayMax = configuredDelayMax;
  // This first projection establishes only ambiguity geometry. Detector
  // settings are read and projected later; defaults here must not reject an
  // otherwise valid high-rate configuration.
  captureDelayProjection.minimumDelay = 0;
  captureDelayProjection.guardCells = 0;
  captureDelayProjection.trainingCells = 0;
  captureDelayProjection.centroidRadius = 0;
  // Multi-illuminator processing projects each lane independently below.
  // Its unused full-passband compatibility objects must not reject valid
  // narrow lanes because full-rate CFAR neighborhoods would exceed int8.
  captureDelayProjection.delayReferenceRateHz = multiIlluminatorEnabled ?
    std::nullopt : delayReferenceRateHz;
  const auto effectiveCaptureDelay =
    blah2::multi_illuminator::scaleRadarConfigForLane(
      captureDelayProjection, fs);
  delayMin = effectiveCaptureDelay.delayMin;
  delayMax = effectiveCaptureDelay.delayMax;
  std::vector<std::unique_ptr<Ambiguity>> ambiguity;
  // One shape object resolves immutable configuration; RadarBank owns DSP.
  ambiguity.push_back(blah2::pipeline::makeAmbiguity(delayMin, delayMax,
      dopplerMin, dopplerMax, fs, nSamples, roundHamming));

  // Set up clutter before the shared accelerator so its worker can allocate a
  // bounded batched pipeline only when filtering is enabled.
  int32_t configuredDelayMinClutter, configuredDelayMaxClutter;
  int32_t delayMinClutter, delayMaxClutter;
  bool isClutter;
  const auto& clutterConfig = tree["process"]["clutter"];
  bool followAmbiguity = false;
  if (clutterConfig.has_child("followAmbiguity"))
    clutterConfig["followAmbiguity"] >> followAmbiguity;
  if (followAmbiguity) {
    // Ambiguity limits are inclusive; Wiener-Hopf consumes a half-open span.
    // Calculate in 64 bits so a malformed boundary cannot wrap into a huge
    // allocation before the startup checks reject it.
    const int64_t derivedDelayMax = int64_t(configuredDelayMax) + 1;
    if (derivedDelayMax > INT32_MAX)
      throw std::invalid_argument("Clutter followAmbiguity range exceeds the processor delay limit");
    configuredDelayMinClutter = configuredDelayMin;
    configuredDelayMaxClutter = int32_t(derivedDelayMax);
  } else {
    clutterConfig["delayMin"] >> configuredDelayMinClutter;
    clutterConfig["delayMax"] >> configuredDelayMaxClutter;
  }
  auto captureClutterProjection = captureDelayProjection;
  captureClutterProjection.clutterDelayMin = configuredDelayMinClutter;
  captureClutterProjection.clutterDelayMax = configuredDelayMaxClutter;
  captureClutterProjection.clutterFollowsAmbiguity = followAmbiguity;
  const auto effectiveCaptureClutter =
    blah2::multi_illuminator::scaleRadarConfigForLane(
      captureClutterProjection, fs);
  delayMinClutter = effectiveCaptureClutter.clutterDelayMin;
  delayMaxClutter = effectiveCaptureClutter.clutterDelayMax;
  clutterConfig["enable"] >> isClutter;
  const int64_t clutterBins = int64_t(delayMaxClutter) - delayMinClutter;
  if (isClutter && (clutterBins <= 0 || clutterBins > nSamples || clutterBins > UINT32_MAX))
    throw std::invalid_argument("Clutter delay range must be a non-empty half-open interval no longer than the CPI");

  std::string accelerationMode = "auto";
  if (tree["process"].has_child("performance") &&
      tree["process"]["performance"].has_child("acceleration"))
    tree["process"]["performance"]["acceleration"] >> accelerationMode;
  const char* gpuDevice = std::getenv("BLAH2_GPU_DEVICE");
  auto requiredResidentFeatures = multiAdvanced;
  requiredResidentFeatures.surveillanceFusionMode = surveillanceFusionMode;
  requiredResidentFeatures.bearing = bearingEnabled;
  uint32_t requestedBeamTolerance = UINT32_MAX;
  if (tree["process"]["tracker"].has_child("beam_tolerance"))
    tree["process"]["tracker"]["beam_tolerance"] >> requestedBeamTolerance;
  requiredResidentFeatures.trackerBeamToleranceConfigured = requestedBeamTolerance != UINT32_MAX;
  const bool residentMode = blah2::pipeline::selectRequiredResidentRoute(accelerationMode,
    blah2::pipeline::requiresResidentProducts(requiredResidentFeatures) ||
    causalEnabled || residentSnapshotEnabled);
  if(blah2::replay_gpu_qualification::enabled()) {
    const auto& fault=blah2::replay_gpu_qualification::state().config;
    blah2::replay_gpu_qualification::validateRoute(fault,multiIlluminatorEnabled,
      multiIlluminatorEnabled?illuminatorDefinitions.size():1,accelerationMode);
  }
  if(tree["capture"]["replay"].has_child("qualification")&&
      tree["capture"]["replay"]["qualification"].has_child("clutter_checkpoint")){
    namespace checkpoint=blah2::clutter_replay;
    const auto qualification=tree["capture"]["replay"]["qualification"];
    checkpoint::require(state&&!loop&&accelerationMode=="gpu"&&!multiIlluminatorEnabled&&isClutter&&
      blah2::gpu_artifact_binding::active()&&qualification.has_child("acquisition")&&
      qualification.has_child("observer"),"Clutter checkpoint requires exact single-lane portable replay and observer");
    auto plan=checkpoint::declaredPlan(qualification["clutter_checkpoint"]);
    const auto acquisition=qualification["acquisition"],observer=qualification["observer"];
    std::string recordingHash,acquisitionHash;bool exact=false,summary=false,observeClutter=false,inject=false;
    acquisition["require_exact"]>>exact;acquisition["recording_sha256"]>>recordingHash;
    acquisition["acquisition_sha256"]>>acquisitionHash;
    if(observer.has_child("summary_only"))observer["summary_only"]>>summary;
    if(observer.has_child("clutter"))observer["clutter"]["enable"]>>observeClutter;
    if(tree["capture"]["replay"].has_child("inject_echo"))
      tree["capture"]["replay"]["inject_echo"]["state"]>>inject;
    checkpoint::require(exact&&checkpoint::hex(recordingHash)&&checkpoint::hex(acquisitionHash)&&
      observeClutter&&!summary&&inject,"Clutter checkpoint requires pinned acquisition, complex clutter and muted-prefix injector");
    plan.samples=nSamples;plan.channels=uint32_t(surveillanceChannels.size());
    plan.taps=uint32_t(clutterBins);plan.delayMin=delayMinClutter;
    plan.configSha=checkpoint::hash(checkpoint::configBytes(tree.rootref()));
    plan.identity=checkpoint::hash(plan.sourceSha+plan.configSha+recordingHash+acquisitionHash+
      blah2::gpu_artifact_binding::env("PROCESSOR_SHA256")+
      blah2::gpu_artifact_binding::env("WORKER_SHA256")+
      blah2::gpu_artifact_binding::env("PORTABLE_SHA256"));
    blah2::Sha256 identity;identity.update(plan.identity.data(),plan.identity.size());
    for(const auto& calibration:{surveillanceManifoldFile,bearingManifoldFile})if(!calibration.empty()){
      const auto bytes=replay_file_hash(calibration);identity.update(bytes.data(),bytes.size());}
    plan.identity=identity.finish();
    blah2::replay_gpu_qualification::configureClutter(std::move(plan));
  }
  // Pin enumeration's exact FP64 identity before any resident session starts.
  // Driver/module failures and explicit ineligible selectors never fall back.
  const std::string residentDevice = residentMode ? blah2::selectResidentGpuDevice(
    blah2::queryGpuDriverStatus(), gpuDevice ? gpuDevice : "auto") : "";
  if (surveillanceManifoldConfigured &&
      surveillanceManifoldFile.find_first_not_of(" \t\r\n") == std::string::npos)
    throw std::invalid_argument("Measured surveillance manifold_file must be a non-empty path");
  if (surveillanceManifoldConfigured &&
      surveillanceFusionMode != "coherent_beam_bank" &&
      surveillanceFusionMode != "noise_whitened_beam_bank")
    throw std::invalid_argument("Measured surveillance manifold requires coherent surveillance fusion");
  if (surveillanceFusionMode == "noise_whitened_beam_bank" && !residentMode)
    throw std::invalid_argument("Noise-whitened surveillance fusion requires gpu-resident acceleration");
#ifndef VECTORWARP_RESIDENT_GPU
  if(residentMode)throw std::invalid_argument("This cohort does not provide the current resident GPU route");
#endif

  ArrayReferenceSynthesizer::Config referenceConfig;
  bool targetProtectionEnabled = false;
  uint32_t targetProtectionBeams = 12, targetProtectionBeamIndex = 0;
  if (tree["process"].has_child("reference_synthesis"))
  {
    auto node = tree["process"]["reference_synthesis"];
    if (node.has_child("analysis_samples"))
      node["analysis_samples"] >> referenceConfig.analysisSamples;
    if (node.has_child("analysis_interval"))
      node["analysis_interval"] >> referenceConfig.analysisInterval;
    if (node.has_child("power_iterations"))
      node["power_iterations"] >> referenceConfig.powerIterations;
    if (node.has_child("covariance_smoothing"))
      node["covariance_smoothing"] >> referenceConfig.covarianceSmoothing;
    if (node.has_child("diagonal_loading"))
      node["diagonal_loading"] >> referenceConfig.diagonalLoading;
    // Explicit experimental target-protected reference. A caller selects a
    // member of a fixed, complete azimuth bank; it never consumes ADS-B truth
    // at runtime.  The chosen direction is nulled in the reference before
    // ECA/CAF, while the measured eigenbeam remains the direct-path anchor.
    if (node.has_child("target_protection_enabled"))
      node["target_protection_enabled"] >> targetProtectionEnabled;
    else if (node.has_child("target_protection_beams") || node.has_child("target_protection_beam_index"))
      throw std::invalid_argument("Target protection requires explicit Experimental target_protection_enabled on/off");
    if (node.has_child("target_protection_beams"))
      node["target_protection_beams"] >> targetProtectionBeams;
    if (node.has_child("target_protection_beam_index"))
      node["target_protection_beam_index"] >> targetProtectionBeamIndex;
    blah2::analysis::validateTargetProtection(targetProtectionEnabled, state,
      multiIlluminatorEnabled, accelerationMode, arrayReference,
      coherentFusion && !coherentPositions.empty(), targetProtectionBeams,
      targetProtectionBeamIndex);
    if (targetProtectionEnabled)
    {
      const uint32_t beams = targetProtectionBeams, index = targetProtectionBeamIndex;
      const double targetAzimuth = 2 * std::acos(-1.0) * double(index) / double(beams);
      const double directX = std::cos(directPathAzimuthRad);
      const double directY = std::sin(directPathAzimuthRad);
      const double targetX = std::cos(targetAzimuth);
      const double targetY = std::sin(targetAzimuth);
      const double waveNumber = 2 * std::acos(-1.0) * double(fc) / 299792458.0;
      referenceConfig.protectionRelativeSteering.reserve(coherentPositions.size());
      for (const auto& position : coherentPositions)
      {
        const double relativePhase = waveNumber *
          (position[0] * (targetX - directX) +
           position[1] * (targetY - directY));
        referenceConfig.protectionRelativeSteering.push_back(
          std::polar(1.0, -relativePhase));
      }
      std::cout << "[ReferenceSynthesis] target_protection_beam=" << index
        << "/" << beams << " azimuth_deg=" <<
        targetAzimuth * 180.0 / std::acos(-1.0) << "\n";
    }
  }
  uint64_t replayProcessedFrames = 0;
  blah2::detection::SignalProgressClock trackerProgress;
  uint64_t qualificationCaptureFrames = 0;
  blah2::bearing_manifold::Identity manifoldIdentity;
  if (coherentFusion)
  {
    auto device=tree["capture"]["device"];
    auto geometry=device["array_geometry"];
    bool mappingConfirmed=false,geometryConfirmed=false,orientationConfirmed=false;
    if(geometry.has_child("mapping_confirmed"))geometry["mapping_confirmed"]>>mappingConfirmed;
    if(geometry.has_child("geometry_confirmed"))geometry["geometry_confirmed"]>>geometryConfirmed;
    if(geometry.has_child("orientation_confirmed"))geometry["orientation_confirmed"]>>orientationConfirmed;
    if(!mappingConfirmed||!geometryConfirmed||!orientationConfirmed)
      throw std::invalid_argument("Measured manifold requires confirmed channel mapping, geometry, and orientation");
    manifoldIdentity.channelMapping = surveillanceChannels;
    manifoldIdentity.positionsM = coherentPositions;
    manifoldIdentity.frequencyHz = double(fc);
    manifoldIdentity.sampleRateHz = double(fs);
    manifoldIdentity.xAxisBearingDeg = xAxisBearingDeg;
    manifoldIdentity.directPathAzimuthDeg=std::fmod(std::fmod(
      directPathAzimuthRad*180.0/std::acos(-1.0),360.0)+360.0,360.0);
    manifoldIdentity.receiverLocation = *geographicRx;
    if (multiIlluminatorEnabled)
      manifoldIdentity.transmitterLocation = *illuminatorDefinitions.front().transmitterLocation;
    else {
      manifoldIdentity.transmitterLocation = *geographicTx;
    }
    // Recording a manifold is itself an identity-sensitive operation.  Replay
    // exports must prove the acquisition-time requested gains just as strictly
    // as loading a measured bank; applied readback is never a substitute.
    const bool measuredManifold = manifoldRecorderEnabled || surveillanceManifoldConfigured ||
      !bearingManifoldFile.empty() ||
      std::any_of(illuminatorDefinitions.begin(), illuminatorDefinitions.end(), [](const auto& lane) {
        return !lane.detectionManifoldFile.empty() || !lane.bearingManifoldFile.empty(); });
    if (measuredManifold) {
    auto heimdall=device["heimdall"];
    if(!state&&heimdall.has_child("gain_control")){
      std::string gainControl;heimdall["gain_control"]>>gainControl;
      if(gainControl!="fixed")
        throw std::invalid_argument("Measured manifold requires fixed gain control");
    }
    std::vector<double> captureGains;
    if (state) {
      captureGains = replayIdentityProvenance.requested_gains_for_manifold();
    }
    else captureGains = blah2::acquisition::requestedGainsFromDevice(device);
    if(captureGains.size()!=captureChannelCount)
      throw std::invalid_argument("Measured manifold requested gain vector differs from capture channels");
    for(uint32_t channel:surveillanceChannels){
      const double gain=captureGains[channel];
      if(!std::isfinite(gain))
        throw std::invalid_argument("Measured manifold requested gain is invalid");
      manifoldIdentity.requestedGainDb.push_back(gain);
    }
    }
  }
  if (coherentFusion)
  {
    if (surveillanceManifoldConfigured && !multiIlluminatorEnabled)
    {
      surveillanceManifoldCalibration=blah2::bearing_manifold::loadDetection(surveillanceManifoldFile,
        double(fc),coherentBeamCount,uint32_t(coherentPositions.size()),manifoldIdentity,
        state ? blah2::bearing_manifold::DetectionUse::offlineReplay :
          blah2::bearing_manifold::DetectionUse::liveProduction);
      surveillanceManifoldResponses = surveillanceManifoldCalibration->responses;
      surveillanceManifoldDirectResponse = surveillanceManifoldCalibration->directResponse;
    }
    std::cout << "[SurveillanceFusion] mode=" << surveillanceFusionMode << " beams="
      << coherentBeamCount << " channels=" << coherentPositions.size();
    std::cout << " direct_path_azimuth_deg=" <<
      directPathAzimuthRad * 180.0 / std::acos(-1.0);
    std::cout << " effective_beam_trials=" << effectiveBeamTrials
      << " direct_path_null_db=" << directPathNullDb;
    if (surveillanceManifoldConfigured)
      std::cout << " measured_manifold=" << surveillanceManifoldFile;
    std::cout << "\n";
  }
  else
    std::cout << "[SurveillanceFusion] mode=noncoherent\n";

  blah2::analysis::ReplayEchoInjector::Config echoConfig;
  if (tree["capture"]["replay"].has_child("inject_echo")) {
    auto echo = tree["capture"]["replay"]["inject_echo"];
    if (echo.has_child("state")) echo["state"] >> echoConfig.enabled;
    if (echo.has_child("amplitude_db")) echo["amplitude_db"] >> echoConfig.amplitudeDb;
    if (echo.has_child("delay_km")) echo["delay_km"] >> echoConfig.delayKm;
    if (echo.has_child("doppler_hz")) echo["doppler_hz"] >> echoConfig.dopplerHz;
    double targetAzimuthDeg = 0;
    if (echo.has_child("target_azimuth_deg_array"))
      echo["target_azimuth_deg_array"] >> targetAzimuthDeg;
    echoConfig.targetAzimuthRad = targetAzimuthDeg * std::acos(-1.0) / 180.0;
  }
  // Noncoherent replay still needs one injected signal per physical channel,
  // but it deliberately does not construct the coherent-fusion geometry.
  // Load the physical array here as well so an injected echo is not copied
  // identically into the array reference and cancelled as direct-path energy.
  auto echoPositions = coherentPositions;
  if (echoConfig.enabled && echoPositions.empty())
  {
    echoPositions.resize(captureChannelCount);
    if (tree["capture"]["device"].has_child("array_geometry") &&
        tree["capture"]["device"]["array_geometry"].has_child("elements"))
      for (auto element : tree["capture"]["device"]["array_geometry"]["elements"].children())
      {
        uint32_t channel = std::numeric_limits<uint32_t>::max();
        if (!element.has_child("daq_channel") ||
            !element.has_child("position")) continue;
        element["daq_channel"] >> channel;
        if (channel >= captureChannelCount) continue;
        std::size_t coordinate = 0;
        for (auto value : element["position"].children())
        {
          if (coordinate >= echoPositions[channel].size()) break;
          value >> echoPositions[channel][coordinate++];
        }
        if (coordinate != echoPositions[channel].size())
          echoPositions[channel] = {};
      }
  }
  blah2::analysis::ReplayEchoInjector echoInjector(echoConfig, fs, fc,
    directPathAzimuthRad, std::move(echoPositions), state);

  // set up process detection
  bool isDetection, isTracker;
  tree["process"]["detection"]["enable"] >> isDetection;
  tree["process"]["tracker"]["enable"] >> isTracker;
  blah2::pipeline::Stages{isClutter, isDetection, isTracker}.validate();
  double pfa, minDoppler;
  int8_t configuredNGuard, configuredNTrain;
  int32_t configuredMinDelay;
  uint32_t integrationCpis = 1;
  uint32_t integrationMotionBins = 0;
  bool trackBeforeDetect = false;
  bool trackBeforeDetectFractionalDelay = false;
  uint32_t trackBeforeDetectCpis = 8;
  uint32_t trackBeforeDetectMotionBins = 1;
  uint32_t trackBeforeDetectDelayMotionBins = 1;
  uint32_t trackBeforeDetectDopplerMotionBins = 1;
  uint32_t integrationBeamTolerance = UINT32_MAX;
  tree["process"]["detection"]["pfa"] >> pfa;
  tree["process"]["detection"]["nGuard"] >> configuredNGuard;
  tree["process"]["detection"]["nTrain"] >> configuredNTrain;
  tree["process"]["detection"]["minDelay"] >> configuredMinDelay;
  tree["process"]["detection"]["minDoppler"] >> minDoppler;
  if (tree["process"]["detection"].has_child("integration_cpis"))
    tree["process"]["detection"]["integration_cpis"] >> integrationCpis;
  if (tree["process"]["detection"].has_child("integration_motion_bins"))
    tree["process"]["detection"]["integration_motion_bins"] >> integrationMotionBins;
  if (tree["process"]["detection"].has_child("track_before_detect"))
  {
    auto node = tree["process"]["detection"]["track_before_detect"];
    if (node.has_child("enable")) node["enable"] >> trackBeforeDetect;
    if (node.has_child("fractional_delay")) node["fractional_delay"] >> trackBeforeDetectFractionalDelay;
    if (node.has_child("max_cpis")) node["max_cpis"] >> trackBeforeDetectCpis;
    if (node.has_child("motion_bins")) {
      node["motion_bins"] >> trackBeforeDetectMotionBins;
      trackBeforeDetectDelayMotionBins=trackBeforeDetectMotionBins;
      trackBeforeDetectDopplerMotionBins=trackBeforeDetectMotionBins;
    }
    if(node.has_child("delay_motion_bins"))node["delay_motion_bins"]>>trackBeforeDetectDelayMotionBins;
    if(node.has_child("doppler_motion_bins"))node["doppler_motion_bins"]>>trackBeforeDetectDopplerMotionBins;
  }
  if (tree["process"]["tracker"].has_child("beam_tolerance"))
    tree["process"]["tracker"]["beam_tolerance"] >> integrationBeamTolerance;
  if (integrationCpis < 1 || integrationCpis > 2)
    throw std::invalid_argument("Detection integration_cpis must be 1 or 2");
  if (integrationMotionBins > 1 || (integrationMotionBins && integrationCpis != 2))
    throw std::invalid_argument("Detection integration_motion_bins requires two-CPI integration and must be 0 or 1");
  track_before_detect::validateBeamHistory(trackBeforeDetect?trackBeforeDetectCpis:integrationCpis,
    integrationBeamTolerance);
  // Power-motion evidence does not require an antenna array or FP64 GPU.
  // Coherent bearing/refinement retains its own geometry/backend prerequisites.
  const bool residentHostProducts = residentMode && !coherentFusion;
  blah2::detection::validateDopplerRidgeAdmission(residentMode && !residentHostProducts,
    dopplerRidge.enabled);
  if (dopplerRidge.enabled && (!isDetection || !isTracker))
    throw std::invalid_argument("Doppler ridge birth filtering requires enabled detection and tracking");
  if (trackBeforeDetect && !isDetection)
    throw std::invalid_argument("Track-before-detect requires enabled detection");
  if (trackBeforeDetectFractionalDelay && !trackBeforeDetect)
    throw std::invalid_argument("fractional_delay requires enabled track-before-detect");
  if(bearingEnabled&&(!residentMode||!coherentFusion))
    throw std::invalid_argument("Bearing output requires GPU-resident coherent tracking");
  if(manifoldRecorderEnabled&&!coherentFusion)
    throw std::invalid_argument("Manifold recording requires coherent detection");
  if(bearingEnabled&&!trackBeforeDetect)
    throw std::invalid_argument("Bearing output requires track-before-detect candidate maps");
  if(bearingMethod!="coherent_beam_bank"&&bearingMethod!="covariance_mvdr")
    throw std::invalid_argument("Bearing method must be coherent_beam_bank or covariance_mvdr");
  if(bearingReferenceAnchor!="live_eigenbeam"&&bearingReferenceAnchor!="fixed_site"&&bearingReferenceAnchor!="fixed_site_projected")
    throw std::invalid_argument("Bearing reference_anchor must be live_eigenbeam, fixed_site or fixed_site_projected");
  if(!bearingScanBeams)bearingScanBeams=coherentBeamCount*4u;
  if(residentMode&&!residentHostProducts)
    track_before_detect::validateRefinementGrid(trackBeforeDetect?trackBeforeDetectCpis:integrationCpis,
      coherentBeamCount,bearingEnabled?bearingScanBeams:0);
  if(bearingCandidatePeaks!=3||bearingGuardRadius>3||bearingTrainingRadius<=bearingGuardRadius||
      bearingTrainingRadius>6||!std::isfinite(bearingDiagonalLoading)||
      bearingDiagonalLoading<=0||bearingDiagonalLoading>1)
    throw std::invalid_argument("Invalid bearing covariance/candidate configuration");
  if(bearingMinConsistent<2||bearingMinConsistent>100)
    throw std::invalid_argument("Bearing min_consistent must be from 2 through 100");
  if(bearingMinPeakSeparationDb<1||bearingMinPeakSeparationDb>15)
    throw std::invalid_argument("Bearing min_peak_separation_db must be from 1 through 15");
  if(!std::isfinite(bearingMaxRateDegPerSecond)||bearingMaxRateDegPerSecond<0||bearingMaxRateDegPerSecond>360)
    throw std::invalid_argument("Bearing max_rate_deg_s must be from 0 through 360");
  if (trackBeforeDetect && (!track_before_detect::validMaximum(trackBeforeDetectCpis) ||
      trackBeforeDetectDelayMotionBins > 2 || trackBeforeDetectDopplerMotionBins > 16))
    throw std::invalid_argument("Track-before-detect requires max_cpis 4 or 8, delay_motion_bins 0 through 2, and doppler_motion_bins 0 through 16");
  if (integrationBeamTolerance != UINT32_MAX &&
      (!coherentFusion || integrationBeamTolerance > coherentBeamCount / 2))
    throw std::invalid_argument("Tracker beam_tolerance requires coherent fusion and at most half the beam count");
  blah2::detection::validateTemporalRouteAdmission(
    residentMode&&!residentHostProducts,
    integrationBeamTolerance!=UINT32_MAX,integrationCpis>1||trackBeforeDetect,isTracker);
  if ((integrationCpis > 1 || trackBeforeDetect) &&
      integrationBeamTolerance != UINT32_MAX && (!residentMode || residentHostProducts))
    throw std::invalid_argument("Beam-conditioned temporal integration requires the resident backend");
  // A beam bank reports the maximum of several trials per cell. Apply a
  // Bonferroni correction so its configured false-alarm probability retains
  // the same upper bound as the single noncoherent statistic.
  const double motionTrials = trackBeforeDetect
    ? track_before_detect::trialCount(trackBeforeDetectCpis,
      trackBeforeDetectDelayMotionBins,trackBeforeDetectDopplerMotionBins)
    : (integrationMotionBins ? 9.0 : 1.0);
  // TBD adds its motion-bank correction inside the resident detector while
  // retaining the proven two-CPI baseline at this beam-corrected PFA.
  const double detectorPfa = blah2::pipeline::RadarPlan::detectorPfa(pfa,
    coherentFusion ? effectiveBeamTrials : 1.0,
    trackBeforeDetect ? trackBeforeDetectCpis : integrationCpis, integrationMotionBins);
  if (coherentFusion)
    std::cout << "[SurveillanceFusion] configured_pfa=" << pfa
      << " effective_pfa=" << detectorPfa << " effective_beam_trials="
      << effectiveBeamTrials << " motion_trials=" << motionTrials;
  if (coherentFusion && trackBeforeDetect)
    std::cout << " tbd_effective_pfa=" << detectorPfa / motionTrials;
  if (coherentFusion) std::cout << "\n";
  if (trackBeforeDetect)
    std::cout << "[TrackBeforeDetect] enabled=1 banks="
      << track_before_detect::bankCount(trackBeforeDetectCpis)
      << " max_cpis=" << trackBeforeDetectCpis
      << " delay_motion_bins=" << trackBeforeDetectDelayMotionBins
      << " doppler_motion_bins=" << trackBeforeDetectDopplerMotionBins
      << " fractional_delay=" << trackBeforeDetectFractionalDelay
      << " coherent_refinement_factor="
      << (bearingEnabled ? bearingScanBeams / coherentBeamCount : 0u)
      << " bearing_method=" << (bearingEnabled ? bearingMethod : "disabled") << "\n";
  // set up process centroid
  uint16_t configuredNCentroid;
  tree["process"]["detection"]["nCentroid"] >> configuredNCentroid;

  // Parse processing admission flags before tracker defaults or validation use
  // them.  The full executable (unlike focused component fixtures) depends on
  // this ordering.

  // set up process tracker
  uint8_t m, n, nDelete;
  double maxAcc, rangeRes;
  std::string trackerAccelerationModel,trackerProfile="balanced";
  double trackerMinInitiationSnrDb=5.0;
  bool stationaryClutter=false;
  uint32_t stationaryPersistenceCpis=stationaryClutter?60u:0u;
  // Zero means the full configured ambiguity Doppler span. A positive legacy
  // value remains an optional safety bound for installations that need one.
  double stationaryMaxDopplerHz=0.0;
  tree["process"]["tracker"]["initiate"]["M"] >> m;
  tree["process"]["tracker"]["initiate"]["N"] >> n;
  tree["process"]["tracker"]["delete"] >> nDelete;
  tree["process"]["tracker"]["initiate"]["maxAcc"] >> maxAcc;
  if(tree["process"]["tracker"].has_child("profile"))
    tree["process"]["tracker"]["profile"] >> trackerProfile;
  if(trackerProfile!="conservative"&&trackerProfile!="balanced"&&trackerProfile!="permissive")
    throw std::invalid_argument("Tracker profile must be conservative, balanced, or permissive");
  trackerAccelerationModel = trackerProfile == "balanced" ?
    "adaptive_gate" : "hypothesis_bank";
  if(trackerProfile=="conservative")trackerMinInitiationSnrDb=7.0;
  if(trackerProfile=="permissive")trackerMinInitiationSnrDb=4.0;
  if(tree["process"]["tracker"]["initiate"].has_child("min_snr_db"))
    tree["process"]["tracker"]["initiate"]["min_snr_db"] >> trackerMinInitiationSnrDb;
  if(!std::isfinite(trackerMinInitiationSnrDb)||trackerMinInitiationSnrDb < -100 ||
     trackerMinInitiationSnrDb > 100)
    throw std::invalid_argument("Tracker min_snr_db must be between -100 and 100 dB");
  if(tree["process"]["tracker"]["initiate"].has_child("acceleration_model"))
    tree["process"]["tracker"]["initiate"]["acceleration_model"] >> trackerAccelerationModel;
  if(tree["process"]["tracker"]["initiate"].has_child("stationary_clutter")){
    const auto& stationary=tree["process"]["tracker"]["initiate"]["stationary_clutter"];
    stationary["enable"] >> stationaryClutter;
    if(stationaryClutter){stationary["persistence_cpis"] >> stationaryPersistenceCpis;
      if(stationary.has_child("max_doppler_hz"))stationary["max_doppler_hz"] >> stationaryMaxDopplerHz;}
    else {stationaryPersistenceCpis=0;stationaryMaxDopplerHz=0;}
  }

  if(stationaryClutter && !isTracker)
    throw std::invalid_argument("Stationary clutter requires enabled tracking");
  // Validate the configured temporal confirmation contract before backend
  // selection; private resident qualification uses that identical depth.
  if(isTracker)
    tracker_confirmation::validate(m,n,nDelete,
      trackBeforeDetect ? trackBeforeDetectCpis : integrationCpis);
  if(trackerAccelerationModel!="hypothesis_bank"&&trackerAccelerationModel!="adaptive_gate")
    throw std::invalid_argument("Tracker acceleration_model must be hypothesis_bank or adaptive_gate");
  rangeRes = (double)Constants::c/fs;
  // set up process spectrum analyser
  double spectrumBandwidth = 2000;
  std::unique_ptr<SpectrumAnalyser> spectrumAnalyser;
  if (multiIlluminatorEnabled)
    spectrumAnalyser = std::make_unique<SpectrumAnalyser>(nSamples, spectrumBandwidth, fc, fs);

  // process options
  if(causalEnabled&&(!residentMode||multiIlluminatorEnabled||!isTracker))
    throw std::invalid_argument("Native causal diagnostics currently require the single-lane GPU-resident tracker");
  if(manifoldRecorderEnabled&&!isDetection)
    throw std::invalid_argument("Manifold recording requires GPU-resident coherent detection");
  if(bearingEnabled&&!isTracker)
    throw std::invalid_argument("Bearing output requires tracking to be enabled");
  if (integrationBeamTolerance != UINT32_MAX && !isTracker)
    throw std::invalid_argument("Tracker beam_tolerance requires tracking to be enabled");
  // One operator-intent object feeds direct and channelized plan resolution.
  // Its delay units remain unchanged until each canonical sample rate is known.
  blah2::pipeline::RadarConfig radarIntent;
  radarIntent.stages = {isClutter, isDetection, isTracker};
  radarIntent.cpiSeconds = tCpi;
  radarIntent.delayMin = configuredDelayMin;
  radarIntent.delayMax = configuredDelayMax;
  radarIntent.delayReferenceRateHz = delayReferenceRateHz;
  radarIntent.dopplerMin = dopplerMin;
  radarIntent.dopplerMax = dopplerMax;
  radarIntent.clutterDelayMin = configuredDelayMinClutter;
  radarIntent.clutterDelayMax = configuredDelayMaxClutter;
  radarIntent.clutterFollowsAmbiguity = followAmbiguity;
  radarIntent.pfa = detectorPfa;
  radarIntent.guardCells = configuredNGuard;
  radarIntent.trainingCells = configuredNTrain;
  radarIntent.minimumDelay = configuredMinDelay;
  radarIntent.minimumDopplerHz = minDoppler;
  radarIntent.centroidRadius = configuredNCentroid;
  radarIntent.centroidDopplerRadius = configuredNCentroid;
  radarIntent.trackerM = m;
  radarIntent.trackerN = n;
  radarIntent.trackerDelete = nDelete;
  radarIntent.trackerMaximumAcceleration = maxAcc;
  radarIntent.trackerMinimumSnrDb = trackerMinInitiationSnrDb;
  radarIntent.trackerBeamTolerance = integrationBeamTolerance;
  radarIntent.trackerRecovery = {stationaryClutter, stationaryPersistenceCpis, stationaryMaxDopplerHz};
  radarIntent.trackerMinimumSnrDbByIlluminator = illuminatorTrackerMinimumSnrDb;
  radarIntent.trackerDopplerRidge = dopplerRidge;
  radarIntent.workers = surveillanceWorkers;
  radarIntent.laneWorkers = multiIlluminatorWorkers;
  radarIntent.accelerationMode = accelerationMode;
  radarIntent.accelerationDevice = residentMode ? residentDevice : gpuDevice ? gpuDevice : "auto";
  radarIntent.arrayReference = arrayReference;
  radarIntent.referenceChannel = referenceChannel;
  radarIntent.referenceChannels = referenceChannels;
  radarIntent.surveillanceChannels = surveillanceChannels;
  radarIntent.reference = referenceConfig;
  radarIntent.advanced = multiAdvanced;
  radarIntent.advanced.surveillanceFusionMode = surveillanceFusionMode;
  radarIntent.advanced.detectorIntegrationCpis = trackBeforeDetect ? trackBeforeDetectCpis : integrationCpis;
  radarIntent.advanced.detectorIntegrationMotionBins = trackBeforeDetect ?
    track_before_detect::encodeMotionBins(trackBeforeDetectDelayMotionBins,
      trackBeforeDetectDopplerMotionBins, trackBeforeDetectFractionalDelay) : integrationMotionBins;
  radarIntent.advanced.trackBeforeDetect = trackBeforeDetect;
  radarIntent.advanced.bearing = bearingEnabled;
  radarIntent.advanced.trackerBeamToleranceConfigured = integrationBeamTolerance != UINT32_MAX;
  radarIntent.advanced.trackerAccelerationModel = trackerAccelerationModel;
  radarIntent.advanced.stationaryClutter = stationaryClutter;
  radarIntent.advanced.bearingManifoldRecorder = manifoldRecorderEnabled;
  radarIntent.advanced.residentDiagnostics = causalEnabled || residentSnapshotEnabled;
  if (!multiIlluminatorEnabled) radarIntent.referenceSpectrumBandwidthHz = spectrumBandwidth;
  auto evidenceMapping = bearingChannelMapping;
  if (manifoldRecorderEnabled && evidenceMapping.empty())
    for (uint32_t channel = 0; channel < surveillanceChannels.size(); ++channel)
      evidenceMapping.push_back(channel);
  radarIntent.manifoldEvidence = {true,
    bearingMethod == "covariance_mvdr" ? blah2::resident::BearingMethod::covarianceMvdr :
      blah2::resident::BearingMethod::coherentBeamBank,
    bearingReferenceAnchor == "fixed_site_projected" ? blah2::resident::BearingReferenceAnchor::fixedSiteProjected :
      bearingReferenceAnchor == "fixed_site" ? blah2::resident::BearingReferenceAnchor::fixedSite :
      blah2::resident::BearingReferenceAnchor::liveEigenbeam,
    bearingScanBeams, bearingCandidatePeaks, bearingGuardRadius, bearingTrainingRadius,
    bearingDiagonalLoading, std::move(evidenceMapping), {}};
  radarIntent.experimentalFmReferenceQualityFilter = experimentalFmReferenceQualityFilter;
  radarIntent.offlineReplay = state;
  if (coherentFusion) {
    radarIntent.coherentFusion = CoherentBeamBank::Config{double(fc), coherentBeamCount,
      coherentPositions, directPathAzimuthRad, directPathNullDb, {}, {}};
    radarIntent.manifoldIdentity = manifoldIdentity;
    radarIntent.detectionManifoldFile = surveillanceManifoldFile;
    radarIntent.bearingManifoldFile = bearingManifoldFile;
    radarIntent.bearingPublication = {bearingEnabled, coherentBeamCount,
      bearingScanBeams/coherentBeamCount, bearingMinConsistent, xAxisBearingDeg,
      bearingUncertaintyFloorDeg, bearingMinPeakSeparationDb,
      bearingMethod=="covariance_mvdr"?1u:0u, bearingScanBeams,
      ambiguity.front()->get_cpi(), bearingMaxRateDegPerSecond,
      stationaryClutter?stationaryPersistenceCpis:0u,
      stationaryClutter?stationaryMaxDopplerHz:0.0};
  }
  auto directIntent = radarIntent;
  if (multiIlluminatorEnabled) directIntent.delayReferenceRateHz.reset();
  const auto radarPlan = blah2::pipeline::resolveRadarPlan(
    blah2::pipeline::scaleRadarConfigForLane(std::move(directIntent), fs),
    *ambiguity.front(), fs, fc, nSamples, residentMode && !residentHostProducts);

  // Resident mode consumes canonical capture order. Array inputs retain the
  // reviewed identity route; paired receivers keep explicit, disjoint
  // dedicated-reference and surveillance mappings.
  blah2::resident::Config residentConfig;
  uint64_t residentLogical=0,residentConsumed=0;
  std::filesystem::path residentSnapshotOutput;
  if(residentSnapshotEnabled&&(!state||!residentMode||residentHostProducts||multiIlluminatorEnabled||
      !arrayReference))
    throw std::invalid_argument("Resident snapshot export requires single-lane array-reference offline GPU-resident replay");
  if(residentMode){
    if(!isClutter)
      throw std::invalid_argument("Resident GPU requires clutter cancellation");
    if(arrayReference&&surveillanceChannels.size()!=captureChannelCount)
      throw std::invalid_argument("Resident array reference requires identity all-capture surveillance");
    residentConfig.geometry={ambiguity.front()->get_nfft(),ambiguity.front()->get_n_doppler_bins(),ambiguity.front()->get_n_delay_bins(),uint32_t(surveillanceChannels.size()),delayMin,nSamples,uint32_t(clutterBins),delayMinClutter};
    residentConfig.inputChannels=captureChannelCount;
    residentConfig.correlation=ambiguity.front()->get_n_corr();
    residentConfig.sampleRate=fs;
    residentConfig.middle=ambiguity.front()->get_doppler_middle();
    residentConfig.epoch=1;
    residentConfig.device=residentDevice;
    residentConfig.surveillanceChannels=surveillanceChannels;
    residentConfig.delayAxis.assign(ambiguity.front()->result()->delay.begin(),ambiguity.front()->result()->delay.end());
    residentConfig.dopplerAxis.assign(ambiguity.front()->result()->doppler.begin(),ambiguity.front()->result()->doppler.end());
    const std::vector<uint32_t> residentReferenceChannels = arrayReference ?
      referenceChannels : std::vector<uint32_t>{referenceChannel};
    residentConfig.reference.channels = residentReferenceChannels;
    residentConfig.reference.analysisSamples = referenceConfig.analysisSamples;
    residentConfig.reference.analysisInterval = referenceConfig.analysisInterval;
    residentConfig.reference.powerIterations = referenceConfig.powerIterations;
    residentConfig.reference.covarianceSmoothing =
      referenceConfig.covarianceSmoothing;
    residentConfig.reference.diagonalLoading = referenceConfig.diagonalLoading;
    residentConfig.reference.bandwidthHz = spectrumBandwidth;
    residentConfig.reference.centreHz = double(fc);
    residentConfig.reference.mode = arrayReference ?
      blah2::resident::ReferenceMode::capturedArray :
      blah2::resident::ReferenceMode::capturedDedicated;
    if(coherentFusion){
      std::string shape;
      auto geometry=tree["capture"]["device"]["array_geometry"];
      if(geometry.has_child("shape"))geometry["shape"]>>shape;
      if(shape!="regular_polygon"||coherentPositions.size()<3)
        throw std::invalid_argument("Resident coherent fusion requires regular-polygon geometry");
      double centreX=0,centreY=0,centreZ=0;
      for(const auto& position:coherentPositions){centreX+=position[0];centreY+=position[1];centreZ+=position[2];}
      centreX/=coherentPositions.size();centreY/=coherentPositions.size();centreZ/=coherentPositions.size();
      std::vector<double> angles,radii;
      for(const auto& position:coherentPositions){const double x=position[0]-centreX,y=position[1]-centreY;
        angles.push_back(std::atan2(y,x));radii.push_back(std::hypot(x,y));}
      const double radius=std::accumulate(radii.begin(),radii.end(),0.0)/radii.size();
      const double step=std::remainder(angles[1]-angles[0],2*std::acos(-1.0));
      // The resident ABI represents an exact regular polygon. Do not silently
      // replace a merely-near-regular configured array with its mean radius and
      // uniform angles: the CPU path uses the exact coordinates and would then
      // be steering a different physical array.
      const double tolerance=std::max(1e-6,radius*1e-5);
      const double angularTolerance=1e-5;
      if(std::abs(step)<=angularTolerance||
         std::abs(std::remainder(coherentPositions.size()*step,2*std::acos(-1.0)))>
           angularTolerance)
        throw std::invalid_argument("Resident coherent fusion channel order does not form one complete regular-polygon cycle");
      for(size_t channel=0;channel<radii.size();++channel)
        if(std::abs(radii[channel]-radius)>tolerance||
           std::abs(coherentPositions[channel][2]-centreZ)>tolerance||
           std::abs(std::remainder(angles[channel]-angles[0]-channel*step,2*std::acos(-1.0)))>angularTolerance||
           std::any_of(angles.begin(),angles.begin()+channel,[&](double prior){
             return std::abs(std::remainder(angles[channel]-prior,2*std::acos(-1.0)))<=angularTolerance;
           }))
          throw std::invalid_argument("Resident coherent fusion geometry/order is not a uniformly stepped regular polygon");
      const auto fusionMode=surveillanceFusionMode == "noise_whitened_beam_bank"
        ? blah2::resident::FusionMode::noiseWhitenedRegularPolygon
        : blah2::resident::FusionMode::coherentRegularPolygon;
      residentConfig.fusion={fusionMode,
        coherentBeamCount,double(fc),radius,angles[0],step,directPathAzimuthRad,
        directPathNullDb,surveillanceManifoldResponses,
        surveillanceManifoldDirectResponse};
      if(bearingEnabled){if(bearingChannelMapping.empty())for(uint32_t i=0;i<surveillanceChannels.size();++i)bearingChannelMapping.push_back(i);
       std::vector<std::complex<float>> manifold;
       if(!bearingManifoldFile.empty()&&!multiIlluminatorEnabled){bearingManifoldCalibration=blah2::bearing_manifold::loadBearing(
        bearingManifoldFile,double(fc),bearingScanBeams,uint32_t(surveillanceChannels.size()),manifoldIdentity);
        manifold=bearingManifoldCalibration->responses;}
       residentConfig.bearing={true,bearingMethod=="covariance_mvdr"?blah2::resident::BearingMethod::covarianceMvdr:blah2::resident::BearingMethod::coherentBeamBank,
        bearingReferenceAnchor=="fixed_site_projected"?blah2::resident::BearingReferenceAnchor::fixedSiteProjected:
        bearingReferenceAnchor=="fixed_site"?blah2::resident::BearingReferenceAnchor::fixedSite:blah2::resident::BearingReferenceAnchor::liveEigenbeam,
        bearingScanBeams,bearingCandidatePeaks,bearingGuardRadius,bearingTrainingRadius,bearingDiagonalLoading,bearingChannelMapping,manifold};}
    }
    const double spectrumResolution=double(fs)/nSamples;
    const uint32_t decimation=uint32_t(std::min<double>(nSamples,std::max(1.0,std::ceil(spectrumBandwidth/spectrumResolution))));
    residentConfig.spectrumBins=1+(nSamples-1)/decimation;
    if (!residentHostProducts)
      residentConfig.detector = radarPlan.residentDetector();
    if(isTracker && !residentHostProducts){
      uint32_t capacity=0;
      if(!tree["process"]["tracker"].has_child("gpu_capacity"))throw std::invalid_argument("Resident tracker requires an explicit gpu_capacity; capacity is never inferred or shrunk");
      tree["process"]["tracker"]["gpu_capacity"] >> capacity;
      residentConfig.tracker = radarPlan.residentTracker(capacity);
    }
    if(residentSnapshotEnabled){
      if(!state)throw std::invalid_argument("Resident snapshots are restricted to offline replay");
      residentSnapshotOutput=residentSnapshotDirectory;
      if(!residentSnapshotOutput.is_absolute()||!std::filesystem::is_directory(residentSnapshotOutput))
        throw std::invalid_argument("Resident snapshot output_directory must be an existing absolute directory");
      // Bind the snapshot to gains carried by the recording, never to the
      // current receiver settings. The two may legitimately differ.
      const auto& allGains=replayIdentityProvenance.perChannelGainDb;
      std::vector<double> residentGains;
      for(auto channel:surveillanceChannels){
        if(channel>=allGains.size())throw std::invalid_argument("Resident snapshot gain identity differs from channel mapping");
        residentGains.push_back(allGains[channel]);
      }
      const auto configuredSourceHash=replay_hash(residentSnapshotSourceHash);
      if(replay_file_hash(replayFile)!=configuredSourceHash)
        throw std::invalid_argument("Resident snapshot source_sha256 differs from the replay file");
      const auto configuredRfHash=replay_hash(residentSnapshotRfHash);
      if(residentSnapshotRfFile.empty()||!std::filesystem::path(residentSnapshotRfFile).is_absolute()||
          replay_file_hash(residentSnapshotRfFile)!=configuredRfHash)
        throw std::invalid_argument("Resident snapshot rf_profile_sha256 differs from its absolute profile file");
      residentConfig.replay={residentSnapshotMaxBytes,residentSnapshotMaxFrames,double(fc),
        detectorPfa*std::max(1u,residentConfig.fusion.beams),configuredSourceHash,
        configuredRfHash,residentGains};
    }
    residentConfig.presence=blah2::resident::wire::mandatoryPresence(residentConfig)|blah2::resident::bit(blah2::resident::Section::filtered)|blah2::resident::bit(blah2::resident::Section::fullReference)|
      (manifoldRecorderEnabled?blah2::resident::bit(blah2::resident::Section::detectionCells):0)|
      (causalEnabled?blah2::resident::bit(blah2::resident::Section::causalTrace):0);
    if(causalEnabled){socket_causal=std::make_unique<blah2::BestEffortSocket>(ip,portCausal);
      const auto fingerprint=blah2::resident::wire::configHash(residentConfig);
      causalConfigHash=blah2::causal_trace::hex(fingerprint.data(),fingerprint.size());}
  }

  if (!multiIlluminatorEnabled) {
    blah2::iq::SignalDefinition primary;
    primary.id = "primary"; primary.frequencyHz = fc;
    primary.outputRateHz = fs; primary.occupiedBandwidthHz = fs;
    primary.transmitterLocation = geographicTx;
    illuminatorDefinitions.push_back(std::move(primary));
  }
  std::unique_ptr<blah2::channelizer::IlluminatorChannelizerBank>
    multiChannelizer;
  std::unique_ptr<blah2::pipeline::RadarBank> multiRadar;
  std::unique_ptr<blah2::utility::FixedExecutor> multiSerializationExecutor;
  std::unique_ptr<blah2::utility::FixedExecutor> multiGpuSubmissionExecutor;
  std::unique_ptr<blah2::wideband_gpu::Backend> multiGpuChannelizer;
  std::unique_ptr<blah2::wideband_gpu::SelectedOutputQualifier>
    multiGpuQualifier;
  std::optional<blah2::wideband_gpu::Config> multiGpuConfig;
  blah2::GpuProcessOptions multiGpuOptions;
  std::string multiGpuDevice;
  auto multiGpuStagingOwner = std::make_shared<
    std::vector<std::vector<std::complex<float>>>>();
  auto& multiGpuStagingInput = *multiGpuStagingOwner;
  std::vector<double> multiGroupDelays;
  bool widebandGpuRequested = false;
  bool multiGpuQualified = false;
  bool multiGpuRecoveryPending = false;
  unsigned multiGpuRecoveryFailures = 0;
  unsigned multiGpuRecoveryCooldownFrames = 0;
  uint64_t multiFallbackInputSample = 0;
  blah2::multi_illuminator::PendingPublication<std::vector<
    blah2::channelizer::IlluminatorOutput>> multiPendingPublication;
  blah2::multi_illuminator::PublicationFifo multiProductPublications(
    2 * illuminatorDefinitions.size() + 1);
  bool multiPipelineDelayed = false;
  blah2::multi_illuminator::PipelineState multiPipelineState;
  {
    auto multiConfig = radarIntent;
    if (coherentFusion && residentMode) multiConfig.residentProductTemplate = residentConfig;
    widebandGpuRequested=multiIlluminatorEnabled && (accelerationMode=="gpu"||
      accelerationMode=="gpu-resident"||accelerationMode=="auto");
    if(widebandGpuRequested){
      try {
        blah2::wideband_gpu::Config gpuConfig;gpuConfig.inputRateHz=fs;
        gpuConfig.channels=captureChannelCount;gpuConfig.maxInputSamples=nSamples;
        for(const auto& definition:illuminatorDefinitions){auto taps=
            blah2::wideband_gpu::designFilter(fs,definition.outputRateHz,
              definition.occupiedBandwidthHz,definition.tapsPerPhase);
          multiGroupDelays.push_back((taps.size()-1)/2.0);
          gpuConfig.lanes.push_back({definition.frequencyHz,
            fs/definition.outputRateHz,
            double(int64_t(definition.frequencyHz)-int64_t(fc)),
            std::move(taps)});}
        multiGpuConfig = gpuConfig;
        multiGpuQualifier = std::make_unique<
          blah2::wideband_gpu::SelectedOutputQualifier>(*multiGpuConfig);
        // CPI timing is diagnostic. A transient scheduling delay must not
        // permanently select the slower CPU channelizer; retain the bounded
        // transport hang timeout and qualify numerical output independently.
        multiGpuOptions.offlineTransportTrace=state;
        multiGpuOptions.startupMs=blah2::gpuCpiStartupDeadlineMs(
          tCpi*1000.0,multiGpuOptions.startupMs);
        if(!state)multiGpuOptions.frameMs=blah2::gpuCpiFrameDeadlineMs(
          tCpi*1000.0,multiGpuOptions.frameMs);
        multiGpuDevice=gpuDevice?gpuDevice:"auto";
        multiGpuStagingInput.assign(captureChannelCount,
          std::vector<std::complex<float>>(nSamples));
        multiGpuChannelizer=blah2::wideband_gpu::createLeasedProcess(
          *multiGpuConfig,multiGpuDevice,multiGpuOptions);
        std::cout<<"[MultiIlluminator] channelizer=vulkan-worker device="
          <<multiGpuChannelizer->deviceName()<<"\n";
      } catch(const std::exception& error) {
        multiGpuRecoveryPending = !state && multiGpuConfig &&
          blah2::wideband_gpu::recoverableWorkerFailure(error.what());
        multiGpuChannelizer.reset();
        multiGpuQualifier.reset();
        multiGroupDelays.clear();
        // Wideband channelization is optional even for explicit GPU lanes.
        // macOS supplies the generic lane GPU worker, not this Linux module.
        multiPipelineState.startup_fallback(
          std::string("Optional GPU channelizer unavailable: ") + error.what());
        std::cerr<<"[MultiIlluminator] channelizer=cpu reason="
          <<error.what()<<"\n";
      }
    }
    // Keep the full CPU implementation for exact same-block reset/failover.
    // Startup validation uses a bounded independent direct-FIR oracle instead
    // of advancing this complete CPU bank while live acquisition queues grow.
    if (multiIlluminatorEnabled)
      multiChannelizer = std::make_unique<
        blah2::channelizer::IlluminatorChannelizerBank>(fc, fs,
          captureChannelCount, illuminatorDefinitions);
    multiRadar = std::make_unique<
      blah2::pipeline::RadarBank>(fs,
        captureChannelCount, illuminatorDefinitions, multiConfig);
    multiSerializationExecutor = std::make_unique<blah2::utility::FixedExecutor>(
      std::min(illuminatorDefinitions.size(),
        std::max<std::size_t>(1, threadPlan.availableCpus - 1)));
    multiGpuSubmissionExecutor =
      std::make_unique<blah2::utility::FixedExecutor>(
        std::min(captureChannelCount,
          std::max<std::size_t>(1, threadPlan.availableCpus - 1)));
    std::cout << "[CanonicalRadar] inputs="
      << illuminatorDefinitions.size() << " capture_rate_hz=" << fs << "\n";
  }

  bool directResidentFloatCapture = false;
  bool nativeFloatGpuCapture = false;

  // set up output data
  bool saveMap, saveDetection;
  tree["save"]["map"] >> saveMap;
  tree["save"]["detection"] >> saveDetection;
  std::string savePath, saveMapPath, saveDetectionPath;
  if (saveMap || saveDetection)
  {
    char startTimeStr[16];
    struct timeval currentTime = {0, 0};
    gettimeofday(&currentTime, NULL);
    strftime(startTimeStr, 16, "%Y%m%d-%H%M%S", localtime(&currentTime.tv_sec));
    savePath = path + startTimeStr;
  }
  if (saveMap)
  {
    saveMapPath = savePath + ".map";
  }
  if (saveDetection)
  {
    saveDetectionPath = savePath + ".detection";
  }
  blah2::analysis::ComplexCutoutRecorder::Config complexCutoutConfig;
  if (tree["save"].has_child("complex_cutout")) {
    auto cutout = tree["save"]["complex_cutout"];
    if (cutout.has_child("state")) cutout["state"] >> complexCutoutConfig.enabled;
    if (cutout.has_child("path")) cutout["path"] >> complexCutoutConfig.path;
    if (cutout.has_child("delay_km")) cutout["delay_km"] >> complexCutoutConfig.delayKm;
    if (cutout.has_child("doppler_hz")) cutout["doppler_hz"] >> complexCutoutConfig.dopplerHz;
    if (cutout.has_child("delay_radius_bins"))
      cutout["delay_radius_bins"] >> complexCutoutConfig.delayRadiusBins;
    if (cutout.has_child("doppler_radius_bins"))
      cutout["doppler_radius_bins"] >> complexCutoutConfig.dopplerRadiusBins;
    if (cutout.has_child("max_frames")) cutout["max_frames"] >> complexCutoutConfig.maxFrames;
    if (cutout.has_child("max_mb")) {
      uint64_t megabytes = 0; cutout["max_mb"] >> megabytes;
      if (megabytes < 1 || megabytes > 256)
        throw std::invalid_argument("Complex cutout max_mb must be from 1 through 256");
      complexCutoutConfig.maximumBytes = megabytes << 20;
    }
  }
  blah2::analysis::validateComplexCutoutRoute(complexCutoutConfig.enabled, state, accelerationMode);
  blah2::analysis::ComplexCutoutRecorder complexCutout(complexCutoutConfig, fs, state);
  const auto attachExperimentStatus = [&](std::string json) {
    if (!targetProtectionEnabled && !complexCutout.enabled()) return json;
    if (json.empty() || json.back() != '}')
      throw std::runtime_error("Experimental status requires a JSON object");
    json.pop_back();
    return json + ",\"experiments\":{\"target_protection\":{\"enabled\":" +
      (targetProtectionEnabled ? "true" : "false") + ",\"beams\":" +
      std::to_string(targetProtectionBeams) + ",\"beam_index\":" +
      std::to_string(targetProtectionBeamIndex) + "},\"complex_cutout\":" +
      complexCutout.statusJson() + "}}";
  };
  std::shared_ptr<blah2::qualification_observer::Observer> qualificationObserver;
  if(tree["capture"]["replay"].has_child("qualification")&&
      tree["capture"]["replay"]["qualification"].has_child("observer")) {
    auto node=tree["capture"]["replay"]["qualification"]["observer"];
    blah2::qualification_observer::Config config;
    if(node.has_child("directory"))node["directory"]>>config.directory;
    if(node.has_child("max_frames"))node["max_frames"]>>config.maxFrames;
    if(node.has_child("max_samples_per_channel"))node["max_samples_per_channel"]>>config.maxSamplesPerChannel;
    if(node.has_child("max_map_cells"))node["max_map_cells"]>>config.maxMapCells;
    if(node.has_child("max_bytes"))node["max_bytes"]>>config.maxBytes;
    if(node.has_child("summary_only"))node["summary_only"]>>config.summaryOnly;
    if(node.has_child("clutter")){
      auto clutter=node["clutter"];
      if(clutter.has_child("enable"))clutter["enable"]>>config.clutter;
      if(clutter.has_child("filter_policy"))clutter["filter_policy"]>>config.clutterFilterPolicy;
    }
    qualificationObserver=std::make_shared<blah2::qualification_observer::Observer>(
      config,state,blah2::replay_gpu_qualification::enabled());
    if(multiRadar)multiRadar->setQualificationObserver(qualificationObserver);
  }
  std::mutex nativeCutoutMutex;
  const blah2::multi_illuminator::PublicationContext* nativeCutoutPublication = nullptr;
  if (multiRadar && complexCutout.enabled())
    multiRadar->setNativeCafObserver([&](const blah2::multi_illuminator::NativeCafView& view) {
      std::lock_guard<std::mutex> lock(nativeCutoutMutex);
      if (!nativeCutoutPublication)
        throw std::logic_error("Native cutout lacks its publication identity");
      auto identity = native_product_identity(view, *nativeCutoutPublication);
      identity.channelIds = view.channelIds;
      identity.referenceChannelIds = view.referenceChannelIds;
      complexCutout.appendNative(nativeCutoutPublication->timestampMs,
        view.maps, view.referenceWeights, view.directPathPilot, &identity,
        view.pilotAnchorChannel);
    });
  const auto processMultiFrames = [&](std::vector<blah2::channelizer::IlluminatorOutput> input,
      const blah2::multi_illuminator::PublicationContext& publication) {
    multiProductPublications.retain(multiRadar->pendingPublicationSequences());
    const auto sequence = multiProductPublications.append(publication);
    nativeCutoutPublication = &publication;
    try {
      auto frames = multiRadar->processPipelined(std::move(input),
        blah2::multi_illuminator::LiveTemporalIdentity{publication.timestampMs,
          publication.captureEpoch, publication.samplePositionAvailable,
          publication.inputGainDb, sequence, publication.signalTimestampMs,
          std::array<uint64_t, 2>{publication.captureFirst, publication.captureEnd}});
      nativeCutoutPublication = nullptr;
      return frames;
    } catch (...) {
      nativeCutoutPublication = nullptr;
      throw;
    }
  };
  const auto resetMultiRadar = [&] {
    if (multiRadar) multiRadar->reset();
    multiProductPublications.reset();
  };
  const auto saveMultiProducts = [&](const blah2::multi_illuminator::RadarFrame& frame,
      const blah2::multi_illuminator::PublicationContext& publication) {
    if (!saveMap && !saveDetection) return;
    const auto identity = native_product_identity(frame, publication);
    const auto annotate = [&](std::string json) {
      return blah2::detection::annotateIntegration(
        blah2::analysis::annotate_native_product(std::move(json), identity),
        frame.detectorIntegration);
    };
    if (saveMap && !frame.map->save(annotate(
        multi_map_json(frame, publication.timestampMs, true, false)), saveMapPath))
      throw std::runtime_error("Cannot save multi-illuminator native map output");
    if (saveDetection && frame.detectionEnabled && !frame.detections->save(annotate(
        multi_detection_json(frame, publication.timestampMs)), saveDetectionPath))
      throw std::runtime_error("Cannot save multi-illuminator detection output");
  };
  // All transports consume the same owned frame. A socket failure stops input;
  // it never retries a settled backend transaction or changes radar history.
  const auto publishRadarFrame = [&](const blah2::pipeline::RadarFrame& frame,
      const blah2::multi_illuminator::PublicationContext& publication,
      const std::string& map, const std::string& detections, const std::string& tracks) {
    saveMultiProducts(frame, publication);
    if (multiIlluminatorEnabled) {
      socket_illuminator->sendData(multi_illuminator_frame_json(frame,
        publication.timestampMs, map, detections, tracks,
        publication.samplePositionAvailable, publication.captureEpoch,
        publication.captureFirst, publication.captureEnd));
    } else {
      socket_iqdata->sendData(blah2::annotate_event_time(frame.referenceSpectrum, publication.eventTime));
      socket_map->sendData(map);
      if (frame.detectionEnabled) socket_detection->sendData(detections);
      if (frame.trackingEnabled) socket_track->sendData(tracks);
      if (bearingEnabled && frame.residentProducts) {
        auto bearing = resident_product_timestamp(frame.residentProducts->bearing, publication.timestampMs);
        socket_bearing->sendData(blah2::detection::annotateIntegration(
          blah2::analysis::annotate_native_product(std::move(bearing),
            native_product_identity(frame, publication)), frame.detectorIntegration));
      }
    }
    if (manifoldRecorderEnabled && frame.detectionEnabled)
      socket_manifold->sendData(blah2::annotate_event_time(frame.manifoldEvidence, publication.eventTime));
    if (!frame.residentDiagnostics) return;
    const auto& diagnostic = *frame.residentDiagnostics;
    residentLogical = diagnostic.logical; residentConsumed = diagnostic.consumed;
    const auto fingerprint = blah2::resident::wire::configHash(*diagnostic.config);
    causalConfigHash = blah2::causal_trace::hex(fingerprint.data(), fingerprint.size());
    causalModuleHash = blah2::causal_trace::hex(diagnostic.moduleHash.data(), diagnostic.moduleHash.size());
    if (socket_causal) try {
      socket_causal->sendData(blah2::causal_trace::json(diagnostic.causalTrace.data(),
        diagnostic.causalTrace.size(), rangeRes / 1000, causalSequence++, causalMaximum,
        {publication.samplePositionAvailable, publication.captureEpoch,
          publication.captureFirst, publication.captureEnd}, false,
        socket_causal->dropped() + causalFormatDrops, causalConfigHash, causalModuleHash));
    } catch (...) { ++causalFormatDrops; }
    if (!diagnostic.snapshot.empty()) {
      write_replay_snapshot(residentSnapshotOutput, diagnostic.logical, diagnostic.consumed,
        diagnostic.snapshot);
      const auto snapshotPath = residentSnapshotOutput / ("resident-frame-" +
        std::to_string(diagnostic.logical) + "-" + std::to_string(diagnostic.consumed) + ".vwrps");
      blah2::integration::coherent_replay::writeSidecarExclusive(snapshotPath,
        replay_lineage_sidecar(diagnostic.snapshot, *diagnostic.config,
          publication.samplePositionAvailable, publication.captureEpoch,
          publication.captureFirst, publication.captureEnd, frame.frequencyHz,
          diagnostic.config->epoch));
    }
  };

  // Finish first-use CPU/GPU qualification before any backend admits IQ.
  // Private synthetic products never enter capture or output. Resident warmup
  // exercises tracking too, then discards all learned signal/temporal state.
  std::cout << "[Startup] preparing DSP before capture source=synthetic publish=0\n";
  multiRadar->prepareStartup();
  if (STOP_SIGNAL) return 0;
  // Admission is capability based and occurs only after the resident backend
  // has completed numerical qualification.  A failed qualification must keep
  // ordinary capture storage available for the canonical CPU path.
  directResidentFloatCapture = !multiIlluminatorEnabled &&
    multiRadar->prefersNativeFloatCapture();
  nativeFloatGpuCapture = !state && !echoInjector.enabled() &&
    (bool(multiGpuChannelizer) || directResidentFloatCapture);
  if (nativeFloatGpuCapture) {
    if (multiGpuStagingInput.empty())
      multiGpuStagingInput.assign(captureChannelCount,
        std::vector<std::complex<float>>(nSamples));
    // Two active blocks plus one asynchronously replenished reserve. Keep the
    // existing total pool/sample bounds and lazy fallback, not full precommit.
    for (auto* buffer : captureBuffers) {
      buffer->enable_float_capture(nSamples, 2);
      buffer->prepare_float_capture_reserve();
    }
    const auto pool = captureBuffers.front()->float_capture_pool_stats();
    std::cout << "[Startup] native_float_capture blocks_per_channel=" << pool.limit
      << " block_samples=" << nSamples << " pool_bytes="
      << uint64_t(pool.allocated) * nSamples * sizeof(std::complex<float>) * captureChannelCount
      << " staging_bytes="
      << uint64_t(nSamples) * sizeof(std::complex<float>) * captureChannelCount
      << " prepared_blocks_per_channel=" << pool.allocated
      << " warmed_reserve_per_channel=1 receive_sample_allocations=bounded_lazy_fallback\n";
  }
  std::cout << "[Startup] DSP ready for capture\n";

  // set up output timing
  uint64_t tStart = current_time_ms();
  Timing *timing = new Timing(tStart);
  timing->set_capture_storage_contract(pairedQueueRequested ? 4u :
    (nativeFloatGpuCapture ? 8u : 16u), captureBufferSamples);
  std::vector<uint64_t> time;

  // run process.  Keep capture dormant until all later configuration/DSP setup
  // has succeeded, otherwise a startup exception would strand a joinable thread.
  std::thread t1;
  std::thread t2([&]{
    blah2::stream_profile::Consumer streamProfile;
    uint64_t streamProfileSequence=0;
    std::vector<CaptureChunkQueue> pendingCapture(captureChannelCount);
    std::vector<FloatCaptureChunkQueue> pendingFloatCapture(
      captureChannelCount);
    std::vector<uint64_t> consumerBoundaryDiscarded(captureChannelCount, 0);
    std::vector<uint64_t> consumerStopDiscarded(captureChannelCount, 0);
    std::vector<uint64_t> consumerClaimedSamples(captureChannelCount, 0);
    bool pairedQueueWasActive = false;
    bool terminalCaptureAccountingPublished = false;
    std::function<void()> publishTerminalCaptureAccounting = [] {};
    try {
      const auto publishRadarTiming = [&](const std::vector<blah2::pipeline::RadarFrame>& frames,
          const auto& publications,
          const std::vector<blah2::multi_illuminator::LaneHealthStatus>& laneHealth,
          const std::array<double, 5>& completionMs) {
        // Completion costs describe this consumer batch. Input metadata and
        // upstream costs belong to each returned token, never newer ingress.
        // Empty warm-up/rejected batches have no radar publication timestamp.
        for (const auto& [sequence, framePublication] : publications) {
          timing->update(framePublication.timestampMs,
            {framePublication.capturePrepareMs, framePublication.channelizerMs,
              completionMs[0], completionMs[1], completionMs[2], completionMs[3], completionMs[4]},
            {"multi_capture_prepare", "multi_channelizer_wall", "multi_illuminator_radar",
              "multi_serialize", "multi_output", "multi_pipeline_advance", "cpi"});
          timing->set_capture_queues(framePublication.captureBacklogSamples,
            framePublication.captureDroppedSamples);
          timing->set_capture_sample_position(framePublication.samplePositionAvailable,
            framePublication.captureEpoch, framePublication.captureFirst, framePublication.captureEnd);
          timing->set_input_discontinuity(framePublication.inputDiscontinuity);
          timing->set_multi_lane_health(laneHealth);
          timing->set_wideband_acceleration(framePublication.acceleration,
            framePublication.widebandGpuExecuted, framePublication.widebandCpuExecuted);
          const auto aggregate = blah2::multi_illuminator::aggregate_status(frames, sequence);
          timing->set_acceleration(aggregate.ambiguity,
            aggregate.ambiguityGpuExecuted, aggregate.ambiguityCpuExecuted);
          timing->set_clutter_acceleration(aggregate.clutter,
            aggregate.clutterGpuExecuted, aggregate.clutterCpuExecuted);
          socket_timing->sendData(blah2::annotate_event_time(
            attachExperimentStatus(timing->to_json()), framePublication.eventTime));
          socket_timestamp->sendData("{\"timestamp\":" +
            std::to_string(framePublication.timestampMs) + "}");
        }
      };
      const auto drainMultiPublications = [&] {
        while(multiRadar && (multiPendingPublication.has_value() ||
            !multiRadar->pendingPublicationSequences().empty())){using DrainClock=std::chrono::steady_clock;
          const auto drainStart=DrainClock::now();
          blah2::multi_illuminator::PublicationContext publication;
          std::vector<blah2::multi_illuminator::RadarFrame> frames;
          if (multiPendingPublication.has_value()) {
            auto pending=multiPendingPublication.take();
            publication=std::move(pending.second);
            frames=processMultiFrames(std::move(pending.first), publication);
          } else frames=multiRadar->drain();
          const auto framePublications = multiProductPublications.snapshotsFor(frames);
          const auto laneHealth=multiRadar->laneHealth();
          const auto radarEnd=DrainClock::now();
          struct FinalSerialized{std::string map,detections,tracks;};
          std::vector<FinalSerialized> serialized(frames.size());
          multiSerializationExecutor->parallel_for(frames.size(),
            [&](std::size_t index){const auto& frame=frames[index];
              const auto& framePublication=multiProductPublications.at(frame.publicationSequence);
              const auto identity = native_product_identity(frame, framePublication);
              const auto annotate = [&](std::string json) {
                return blah2::detection::annotateIntegration(
                  blah2::analysis::annotate_native_product(std::move(json), identity),
                  frame.detectorIntegration);
              };
              serialized[index].map=annotate(
                multi_map_json(frame, framePublication.timestampMs,
                  framePublication.publishFullMap, !state));
              serialized[index].detections=annotate(
                multi_detection_json(frame, framePublication.timestampMs));
              serialized[index].tracks=annotate(
                multi_track_json(frame, framePublication.timestampMs));});
          const auto serializationEnd=DrainClock::now();
          for(std::size_t index=0;index<frames.size();++index){
            const auto& frame=frames[index];const auto& product=serialized[index];
            const auto& framePublication=multiProductPublications.at(frame.publicationSequence);
            publishRadarFrame(frame, framePublication, product.map, product.detections, product.tracks);}
          const auto drainEnd=DrainClock::now();
          const auto milliseconds=[](auto end,auto start){return std::chrono::duration<double,std::milli>(end-start).count();};
          publishRadarTiming(frames, framePublications, laneHealth,
            {milliseconds(radarEnd,drainStart),
             milliseconds(serializationEnd,radarEnd),
             milliseconds(drainEnd,serializationEnd),0.0,
             milliseconds(drainEnd,drainStart)});
        }
      };
      uint64_t inputGeneration = 0;
      bool haveInputGeneration = false;
      // The wideband spectrum is a display product, not a radar input.  At
      // high capture rates its full-CPI fold can consume enough CPU on every
      // frame to make real-time radar fall behind.  Keep a bounded diagnostic
      // update while processing and publishing every CPI for radar.
      const uint64_t multiSpectrumStride = std::max<uint64_t>(1,
        static_cast<uint64_t>(std::ceil(10.0 / tCpi)));
      uint64_t multiSpectrumFrame = 0;
      const uint64_t multiFullMapStride = std::max<uint64_t>(1,
        static_cast<uint64_t>(std::ceil(2.0 / tCpi)));
      uint64_t multiMapFrame = 0;
      // Keep sub-CPI tails outside the producer-owned queues. A complete FIFO
      // transfer is constant-time under their locks; multi-million-sample
      // splitting then happens while UHD is free to continue recv().
      uint64_t pendingCaptureGeneration = 0;
      bool havePendingCaptureGeneration = false;
      publishTerminalCaptureAccounting = [&]() noexcept {
        if (terminalCaptureAccountingPublished || state) return;
        terminalCaptureAccountingPublished = true;
        try {
          std::vector<uint64_t> backlog(captureChannelCount),
            dropped(captureChannelCount);
          if (pairedQueueWasActive && pairedCpiQueue) {
            const auto queue = pairedCpiQueue->stats();
            const uint64_t queued = pairedCpiQueue->backlog_samples();
            const uint64_t retired = queue.discardedSamples +
              queue.trailingSamples;
            for (std::size_t channel = 0; channel < captureChannelCount;
                 ++channel) {
              backlog[channel] = queued + consumerClaimedSamples[channel];
              consumerStopDiscarded[channel] += queue.trailingSamples +
                consumerClaimedSamples[channel];
              dropped[channel] = retired +
                consumerBoundaryDiscarded[channel] +
                consumerClaimedSamples[channel];
            }
          } else {
            BufferLocks locks(captureBuffers);
            for (std::size_t channel = 0; channel < captureChannelCount;
                 ++channel) {
              const uint64_t consumer = nativeFloatGpuCapture ?
                pendingFloatCapture[channel].size() :
                pendingCapture[channel].size();
              backlog[channel] = consumer + consumerClaimedSamples[channel] +
                captureBuffers[channel]->get_length();
              consumerStopDiscarded[channel] += consumer +
                consumerClaimedSamples[channel];
              dropped[channel] = captureBuffers[channel]->get_dropped_samples() +
                consumerBoundaryDiscarded[channel] +
                consumerStopDiscarded[channel];
              pendingCapture[channel].clear();
              pendingFloatCapture[channel].clear();
            }
          }
          timing->observe_capture_peak(backlog);
          timing->set_capture_queues(backlog, dropped);
          timing->set_capture_discard_accounting(consumerBoundaryDiscarded,
            consumerStopDiscarded);
          timing->set_capture_terminal(true);
          socket_timing->sendData(timing->to_json());
        } catch (const std::exception& error) {
          std::cerr << "Final consumer capture accounting: " << error.what()
            << '\n';
        } catch (...) {
          std::cerr << "Final consumer capture accounting failed\n";
        }
      };
      // The consumer's queues/accounting callbacks are ready as well as DSP.
      if (STOP_SIGNAL) return;
      t1 = std::thread([&]{capture->process(captureBuffers,
        tree["capture"]["device"], ip_capture, port_capture, nSamples);
      });
      while (!capture->stopping.load() || (pairedQueueWasActive &&
        (capture->pairedCpiQueueActive.load() || pairedCpiQueue->backlog_samples() >= nSamples)))
      {
        if (STOP_SIGNAL && !capture->stopping.load()) capture->request_stop();
        pairedQueueWasActive = pairedQueueWasActive ||
          (pairedCpiQueue && capture->pairedCpiQueueActive.load());
        const bool usePairedQueue = pairedQueueWasActive;
        // Resident processing owns canonical channel order, including the
        // dedicated reference. Do not compact this CPI into legacy objects.
        // ReplayPlayer owns EOF/loop draining by observing the producer FIFOs.
        // Never move replay tails into the live-only consumer queue where that
        // ownership protocol cannot see them.
        const bool chunkLiveCapture = !state && !usePairedQueue;
        const bool directFloatCapture = chunkLiveCapture && nativeFloatGpuCapture;
        const auto residentWholeStart=std::chrono::steady_clock::now();
        streamProfile.complete(residentWholeStart);
        const bool profileCycle=streamProfile.enabled();
        blah2::stream_profile::CycleRecord profileRecord;
        if(profileCycle){profileRecord.time.fill(residentWholeStart);
          profileRecord.threadBegin=blah2::stream_profile::Activity::capture(true);
          profileRecord.processBegin=blah2::stream_profile::Activity::capture(true,true);}
        // The prior iteration's frame/map/JSON owners have now been destroyed.
        // Complete its inclusive timing before doing any work for this input.
        timing->complete_processing_cycle(residentWholeStart);
        bool ready = false;
        uint64_t captureStartUs = 0;
        uint64_t extractedInputGeneration = 0;
        std::vector<uint64_t> captureBacklogSamples, captureDroppedSamples;
        bool captureSamplePositionAvailable = false;
        uint64_t captureEpoch = 0, captureFirstEpochSample = 0,
          captureEndEpochSample = 0;
        int64_t captureEpochEstimateNanoseconds = 0;
        int64_t captureHostReceiptResidualNanoseconds = 0;
        int64_t captureHostReceiptMaximumResidualNanoseconds = 0;
        int64_t captureHostReceiptDriftPpm = 0;
        uint32_t captureHostReceiptAnchors = 0;
        int64_t captureDeviceTimeEpochTick = 0;
        std::string captureClockSource, captureTimeSource,
          captureMotherboard, captureSubdevices;
        // Claim consumer ownership under the queue locks only after a complete
        // CPI is available. Replay EOF/loop logic must not observe drained
        // queues as idle while that CPI is installed, but an empty polling
        // iteration must remain idle so replay can finish.
        std::optional<AtomicFlagGuard> processing;
        if (usePairedQueue) {
          size_t slot = 0;
          if (pairedCpiQueue->acquire(slot)) {
            try {
              const auto& block = pairedCpiQueue->block(slot);
              captureStartUs = current_time_us();
              captureData[0]->assign_paired_i16(block.iq.data(), nSamples,
                *captureData[1]);
              // PairedCpiQueue is the RSPduo capture-epoch authority. Its
              // generation advances on gain/reset/gap boundaries and its
              // first offset is delivered-sample-relative, never UTC.
              if (block.epoch != UINT64_MAX) {
                captureSamplePositionAvailable = true;
                captureEpoch = block.epoch + 1;
                captureFirstEpochSample = block.first;
                captureEndEpochSample = uint64_t(block.first) + nSamples;
              }
              ready = true;
              extractedInputGeneration = capture->input_generation();
            } catch (...) { pairedCpiQueue->release(slot); throw; }
            if (!pairedCpiQueue->release(slot)) ready = false;
          }
          const auto queue = pairedCpiQueue->stats();
          captureBacklogSamples.resize(2);
          captureDroppedSamples.resize(2);
          const uint64_t queued = uint64_t(pairedCpiQueue->backlog_samples());
          captureBacklogSamples[0] = captureBacklogSamples[1] = queued;
          captureDroppedSamples[0] = captureDroppedSamples[1] = queue.discardedSamples;
        } else {
          BufferLocks captureLocks(captureBuffers);
          if(profileCycle){profileRecord.time[1]=std::chrono::steady_clock::now();
            profileRecord.queued=captureBuffers.front()->get_length();
            profileRecord.pending=nativeFloatGpuCapture?pendingFloatCapture.front().size():pendingCapture.front().size();
            profileRecord.capacity=captureBuffers.front()->get_n();}
          const uint64_t observedGeneration = capture->input_generation();
          if (havePendingCaptureGeneration &&
              observedGeneration != pendingCaptureGeneration) {
            for (std::size_t channel = 0; channel < captureChannelCount;
                 ++channel) {
              consumerBoundaryDiscarded[channel] += nativeFloatGpuCapture ?
                pendingFloatCapture[channel].size() :
                pendingCapture[channel].size();
              pendingCapture[channel].clear();
              pendingFloatCapture[channel].clear();
            }
            havePendingCaptureGeneration = false;
          }
          if (!havePendingCaptureGeneration) {
            pendingCaptureGeneration = observedGeneration;
            havePendingCaptureGeneration = true;
          }
          if (chunkLiveCapture) {
            // Producer FIFO plus consumer-owned chunks share exactly the
            // configured aggregate budget. Increasing capacity after a CPI
            // drain is safe; shrinking occurs only while the producer is
            // empty immediately after the whole-deque transfer below.
            for (std::size_t channel = 0; channel < captureBuffers.size(); ++channel) {
              const uint64_t pending = nativeFloatGpuCapture ?
                pendingFloatCapture[channel].size() :
                pendingCapture[channel].size();
              if (pending > captureBufferSamples)
                throw std::runtime_error("Consumer capture queue exceeds configured budget");
              captureBuffers[channel]->set_capacity_unlocked(
                captureBufferSamples - static_cast<uint32_t>(pending));
            }
          }
          std::vector<std::deque<std::complex<double>>> extractedCapture(
            captureChannelCount);
          std::vector<FloatCaptureBlockBatch> extractedFloatCapture(captureChannelCount);
          ready = true;
          std::vector<uint64_t> observedBacklog(captureBuffers.size());
          for (std::size_t channel = 0; channel < captureBuffers.size(); ++channel) {
            const std::size_t consumerSamples = chunkLiveCapture ?
              (nativeFloatGpuCapture ? pendingFloatCapture[channel].size() :
                pendingCapture[channel].size()) : 0;
            observedBacklog[channel] = consumerSamples +
              captureBuffers[channel]->get_length();
            ready = ready && observedBacklog[channel] >= nSamples;
          }
          timing->observe_capture_peak(observedBacklog);
          timing->set_capture_discard_accounting(consumerBoundaryDiscarded,
            consumerStopDiscarded);
          if (ready) {
            processing.emplace(capture->processingBusy);
            captureBacklogSamples.reserve(captureBuffers.size());
            captureDroppedSamples.reserve(captureBuffers.size());
            for (std::size_t channel = 0; channel < captureBuffers.size(); ++channel) {
              auto* buffer = captureBuffers[channel];
              captureBacklogSamples.push_back((chunkLiveCapture ?
                (nativeFloatGpuCapture ? pendingFloatCapture[channel].size() :
                  pendingCapture[channel].size()) : 0) + buffer->get_length());
              captureDroppedSamples.push_back(buffer->get_dropped_samples() +
                consumerBoundaryDiscarded[channel] +
                consumerStopDiscarded[channel]);
            }
            const auto position = capture->live_sample_position();
            if (position.available && !captureBacklogSamples.empty() &&
                std::all_of(captureBacklogSamples.begin(),
                  captureBacklogSamples.end(), [&](uint64_t value) {
                    return value == captureBacklogSamples.front();
                  }) &&
                position.nextSample >= captureBacklogSamples.front() &&
                captureBacklogSamples.front() >= nSamples) {
              captureSamplePositionAvailable = true;
              captureEpoch = position.epoch;
              captureEpochEstimateNanoseconds =
                position.epochEstimateNanoseconds;
              captureHostReceiptResidualNanoseconds = position.hostReceiptResidualNanoseconds;
              captureHostReceiptMaximumResidualNanoseconds = position.hostReceiptMaximumResidualNanoseconds;
              captureHostReceiptDriftPpm = position.hostReceiptDriftPpm;
              captureHostReceiptAnchors = position.hostReceiptAnchors;
              captureDeviceTimeEpochTick = position.deviceTimeEpochTick;
              captureClockSource = position.clockSource;
              captureTimeSource = position.timeSource;
              captureMotherboard = position.motherboard;
              captureSubdevices = position.subdevices;
              captureFirstEpochSample = position.nextSample -
                captureBacklogSamples.front();
              captureEndEpochSample = captureFirstEpochSample + nSamples;
            }
            // If the consumer-side tail already holds a CPI, leave producer
            // FIFOs available to recv(). Otherwise move them in constant time;
            // splitting and old-storage destruction happen after unlocking.
            if (!chunkLiveCapture) {
              // Keep replay ownership visible to ReplayPlayer: remove exactly
              // one CPI while processingBusy is held and leave all later CPIs
              // and the terminal sub-CPI tail in the producer FIFOs.
              for (std::size_t channel = 0; channel < captureChannelCount; channel++)
                extractedCapture[channel] =
                  captureBuffers[channel]->drain_front(nSamples);
            } else {
              const bool needsTransfer = nativeFloatGpuCapture ?
                std::any_of(pendingFloatCapture.begin(),
                  pendingFloatCapture.end(), [&](const auto& samples) {
                    return samples.size() < nSamples;
                  }) :
                std::any_of(pendingCapture.begin(), pendingCapture.end(),
                  [&](const auto& samples) { return samples.size() < nSamples; });
              if (needsTransfer) {
                for (std::size_t channel = 0; channel < captureChannelCount; channel++) {
                  const uint64_t pending = nativeFloatGpuCapture ?
                    pendingFloatCapture[channel].size() :
                    pendingCapture[channel].size();
                  if (nativeFloatGpuCapture) {
                    extractedFloatCapture[channel] =
                      captureBuffers[channel]->take_float_blocks(
                        pending < nSamples ? nSamples - pending : 0);
                  } else {
                    extractedCapture[channel] = captureBuffers[channel]->take_all();
                  }
                  const uint64_t transferred = nativeFloatGpuCapture ?
                    extractedFloatCapture[channel].size() :
                    extractedCapture[channel].size();
                  if (pending + transferred < nSamples)
                    throw std::runtime_error("Ready capture transfer is shorter than one CPI");
                  const uint64_t retained = pending + transferred - nSamples;
                  captureBuffers[channel]->set_capacity_unlocked(
                    captureBufferSamples - static_cast<uint32_t>(retained));
                }
              } else {
                // The CPI will be consumed from the pending queue after these
                // locks are released. Credit that exact budget now so a
                // receiver block arriving before the next polling iteration
                // cannot fail despite available aggregate headroom.
                for (std::size_t channel = 0; channel < captureChannelCount;
                     ++channel) {
                  const uint64_t pending = nativeFloatGpuCapture ?
                    pendingFloatCapture[channel].size() :
                    pendingCapture[channel].size();
                  if (pending < nSamples)
                    throw std::runtime_error(
                      "Pending capture queue is shorter than one CPI");
                  captureBuffers[channel]->set_capacity_unlocked(
                    captureBufferSamples -
                      static_cast<uint32_t>(pending - nSamples));
                }
              }
            }
            extractedInputGeneration = observedGeneration;
          } else if (chunkLiveCapture) {
            // FP64 tails move to the consumer as before. Native FP32 keeps a
            // partial CPI in its reusable producer block; arbitrary receiver
            // block sizes append there until a complete CPI can move intact.
            for (std::size_t channel = 0; channel < captureChannelCount; ++channel) {
              const uint64_t pending = nativeFloatGpuCapture ?
                pendingFloatCapture[channel].size() :
                pendingCapture[channel].size();
              if (nativeFloatGpuCapture)
                extractedFloatCapture[channel] =
                  captureBuffers[channel]->take_float_blocks();
              else
                extractedCapture[channel] = captureBuffers[channel]->take_all();
              const uint64_t transferred = nativeFloatGpuCapture ?
                extractedFloatCapture[channel].size() :
                extractedCapture[channel].size();
              const uint64_t retained = pending + transferred;
              captureBuffers[channel]->set_capacity_unlocked(
                captureBufferSamples - static_cast<uint32_t>(retained));
            }
            extractedInputGeneration = observedGeneration;
          }
          captureLocks.unlock();
          if(profileCycle)profileRecord.time[2]=std::chrono::steady_clock::now();
          if (chunkLiveCapture) {
            for (std::size_t channel = 0; channel < captureChannelCount; ++channel)
              if (nativeFloatGpuCapture)
                pendingFloatCapture[channel].push(
                  std::move(extractedFloatCapture[channel]));
              else
                pendingCapture[channel].push(
                  std::move(extractedCapture[channel]));
          }
          if (ready && chunkLiveCapture) {
            const auto prepareCapture = [&](std::size_t channel) {
              if (nativeFloatGpuCapture) {
                if (directFloatCapture)
                  pendingFloatCapture[channel].take(nSamples,
                    multiGpuStagingInput[channel]);
                else {
                  std::vector<std::complex<float>> converted;
                  pendingFloatCapture[channel].take(nSamples, converted);
                  extractedCapture[channel] = {converted.begin(), converted.end()};
                }
              } else {
                auto& pending = pendingCapture[channel];
                if (directFloatCapture)
                  pending.take_float(nSamples, multiGpuStagingInput[channel]);
                else
                  extractedCapture[channel] = pending.take(nSamples);
              }
            };
            if (multiGpuSubmissionExecutor)
              multiGpuSubmissionExecutor->parallel_for(captureChannelCount,
                prepareCapture);
            else
              for (std::size_t channel = 0; channel < captureChannelCount; channel++)
                prepareCapture(channel);
          }
          if (ready && !directFloatCapture) {
            const auto installCapture = [&](std::size_t channel) {
              captureData[channel]->replace(std::move(extractedCapture[channel]));
            };
            if (multiGpuSubmissionExecutor)
              multiGpuSubmissionExecutor->parallel_for(captureChannelCount,
                installCapture);
            else
              for (std::size_t channel = 0; channel < captureChannelCount; channel++)
                installCapture(channel);
          }
        }
        if (ready)
        {
          if(profileCycle){profileRecord.time[3]=std::chrono::steady_clock::now();
            profileRecord.sequence=++streamProfileSequence;profileRecord.epoch=captureEpoch;
            profileRecord.first=captureFirstEpochSample;profileRecord.end=captureEndEpochSample;
            profileRecord.rate=fs;}
          // Keep ReplayPlayer's EOF/loop drain from observing an empty queue
          // between extraction and setting the consumer-busy state.
          // Bind processing to the epoch observed during extraction. A
          // boundary after extraction invalidates this CPI instead of letting
          // old samples consume the new epoch's state reset.
          if (!state)
            std::fill(consumerClaimedSamples.begin(),
              consumerClaimedSamples.end(), nSamples);
          const uint64_t currentInputGeneration = capture->input_generation();
          if (currentInputGeneration != extractedInputGeneration) {
            for (std::size_t channel = 0; channel < captureChannelCount;
                 ++channel) {
              consumerBoundaryDiscarded[channel] +=
                consumerClaimedSamples[channel];
              consumerClaimedSamples[channel] = 0;
            }
            timing->set_capture_discard_accounting(
              consumerBoundaryDiscarded, consumerStopDiscarded);
            continue;
          }
          if (!haveInputGeneration) {
            inputGeneration = extractedInputGeneration;
            haveInputGeneration = true;
          }
          if(blah2::replay_gpu_qualification::enabled()) {
            if(!captureSamplePositionAvailable)
              throw std::runtime_error("GPU qualification requires exact capture sample positions");
            blah2::replay_gpu_qualification::bind(++qualificationCaptureFrames,
              captureEpoch,captureFirstEpochSample,captureEndEpochSample);
            blah2::replay_gpu_qualification::event("capture-owned");
            if(blah2::replay_gpu_qualification::discardGap()) {
              // The CPI has been extracted and owned once. Discard its real
              // samples; the next CPI retains its original recording position.
              for(std::size_t channel=0;channel<captureChannelCount;++channel){
                consumerBoundaryDiscarded[channel]+=nSamples;
                consumerClaimedSamples[channel]=0;
              }
              timing->set_capture_discard_accounting(consumerBoundaryDiscarded,
                consumerStopDiscarded);
              continue;
            }
          }
          const bool qualificationGap=blah2::replay_gpu_qualification::takeGapReset();
          const auto captureGainDb=capture->input_gain_db();
          std::vector<double> surveillanceGainDb;
          if(manifoldRecorderEnabled||surveillanceManifoldCalibration||bearingManifoldCalibration){
            if(captureGainDb.size()!=captureChannelCount)
              throw std::runtime_error("Measured manifold requires per-channel actual gain metadata");
            for(uint32_t channel:surveillanceChannels)surveillanceGainDb.push_back(captureGainDb[channel]);
            if(surveillanceManifoldCalibration)
              blah2::bearing_manifold::validateActualGains(*surveillanceManifoldCalibration,surveillanceGainDb);
            if(bearingManifoldCalibration)
              blah2::bearing_manifold::validateActualGains(*bearingManifoldCalibration,surveillanceGainDb);
          }
          time.push_back(captureStartUs ? captureStartUs : current_time_us());
          auto eventTime = capture->requires_kraken_time_provenance() ?
            blah2::kraken_event_time_unavailable() :
            capture->requires_usrp_time_provenance() ?
              blah2::usrp_event_time_unavailable() :
            capture->requires_hackrf_time_provenance() ?
              blah2::hackrf_event_time_unavailable() :
            capture->requires_rspduo_time_provenance() ?
              blah2::rspduo_event_time_unavailable() :
              blah2::EventTimeProvenance{};
          if (captureSamplePositionAvailable &&
              capture->requires_usrp_time_provenance())
            eventTime = blah2::usrp_cpi_midpoint_time(
              captureEpoch, captureDeviceTimeEpochTick, captureFirstEpochSample,
              captureEndEpochSample, fs, captureClockSource,
              captureTimeSource, captureMotherboard, captureSubdevices);
          else if (captureSamplePositionAvailable &&
                   capture->requires_hackrf_time_provenance())
            eventTime = blah2::hackrf_cpi_sample_epoch(captureEpoch,
              captureFirstEpochSample, captureEndEpochSample, fs);
          else if (captureSamplePositionAvailable &&
                   capture->requires_rspduo_time_provenance())
            eventTime = blah2::rspduo_cpi_sample_epoch(captureEpoch,
              captureFirstEpochSample, captureEndEpochSample, fs);
          else if (captureSamplePositionAvailable)
            eventTime = blah2::kraken_cpi_midpoint_time(
              captureEpochEstimateNanoseconds, captureEpoch,
              captureFirstEpochSample, captureEndEpochSample, fs,
              captureHostReceiptResidualNanoseconds,
              captureHostReceiptMaximumResidualNanoseconds,
              captureHostReceiptDriftPpm, captureHostReceiptAnchors);
          const uint64_t frameTimestampMs = blah2::event_time_milliseconds(
            eventTime, time[0] / 1000);
          const auto attachEventTime = [&](std::string json) {
            return blah2::annotate_event_time(std::move(json), eventTime);
          };
          if (extractedInputGeneration != inputGeneration || qualificationGap)
          {
            if(blah2::replay_gpu_qualification::clutterActive())
              throw std::runtime_error("Clutter checkpoint replay cannot reset its warm input/frontend history");
            drainMultiPublications();
            inputGeneration = extractedInputGeneration;
            replayProcessedFrames = 0;
            if (socket_causal) try {
              std::array<uint32_t, blah2::causal_trace::words> trace{};
              trace[0] = blah2::causal_trace::schema;
              const uint64_t values[]{residentLogical, residentConsumed, frameTimestampMs, residentConfig.epoch};
              std::memcpy(trace.data() + 4, values, sizeof(values));
              socket_causal->sendData(blah2::causal_trace::json(trace.data(), trace.size(), rangeRes / 1000,
                causalSequence++, causalMaximum, {captureSamplePositionAvailable, captureEpoch,
                  captureFirstEpochSample, captureEndEpochSample}, true,
                socket_causal->dropped() + causalFormatDrops, causalConfigHash, causalModuleHash));
            } catch (...) { ++causalFormatDrops; }
            if (multiChannelizer) multiChannelizer->reset();
            if (multiGpuChannelizer)
              blah2::wideband_gpu::attemptOptionalWithFallback(
                [&] { multiGpuChannelizer->reset(); },
                [&](const std::exception& error) {
                  const auto reason=std::string("GPU channelizer reset failed: ")+
                    error.what();
                  std::cerr<<"[MultiIlluminator] "<<reason<<"; CPU fallback\n";
                  multiGpuRecoveryPending = !state && multiGpuConfig &&
                    blah2::wideband_gpu::recoverableWorkerFailure(error.what());
                  multiGpuChannelizer.reset();
                  multiGpuQualifier.reset();
                  multiGpuQualified=false;
                  // CPU FIR/radar reset and the epoch boundary below already
                  // cover this capture; retain the diagnostic across epochs.
                  multiPipelineState.startup_fallback(reason);
                });
            if (multiGpuQualifier) multiGpuQualifier->reset();
            resetMultiRadar();
            multiPendingPublication.reset();
            multiPipelineDelayed = !state && multiGpuChannelizer &&
              multiGpuChannelizer->supportsSplitFrame();
            multiPipelineState.input_epoch_reset();
            // A capture gap invalidates FIR/radar history, not the already
            // completed device/geometry qualification. Runtime GPU errors
            // still clear the backend and fail over.
            multiFallbackInputSample = 0;
            multiSpectrumFrame = 0;
            multiMapFrame = 0;
            blah2::replay_gpu_qualification::event("history-reset");
          }
          // Advance every accepted input frame, including metadata outages
          // and frames later rejected by clutter. Optional recording cannot
          // switch the tracker's value domain. Exact lineage remains separate.
          const uint64_t frameSignalTimeMs = trackerProgress.advance(nSamples,
            fs, captureSamplePositionAvailable, captureEpoch, captureEndEpochSample);
          ReplayFrameGuard replayFrame(state, replayProcessedFrames);
          std::vector<IqData*> physicalChannels;
          physicalChannels.reserve(captureData.size());
          for (auto& channel : captureData) physicalChannels.push_back(channel.get());
          echoInjector.apply({captureEpoch, captureFirstEpochSample,
            captureEndEpochSample, captureSamplePositionAvailable}, physicalChannels,
            blah2::replay_gpu_qualification::injectEcho());
          {
            const uint64_t maximumCaptureBacklog = captureBacklogSamples.empty() ? 0 :
              *std::max_element(captureBacklogSamples.begin(),
                captureBacklogSamples.end());
            // Preserve every radar CPI, but yield optional large display
            // products while more than one additional CPI is queued. This
            // gives the processor deterministic recovery headroom after a
            // cold start or a competing GPU burst instead of letting a
            // transient backlog become permanent or fill the input queue.
            const bool recoveringBacklog = maximumCaptureBacklog >
              uint64_t(nSamples) * 2;
            const bool spectrumCadence =
              (multiSpectrumFrame++ % multiSpectrumStride) == 0;
            const bool fullMapCadence =
              (multiMapFrame++ % multiFullMapStride) == 0;
            const bool publishSpectrum = !recoveringBacklog && spectrumCadence;
            // Direct displays do not yet retain a metadata-only map across
            // frames, so give them a bounded presentation raster every CPI.
            // Channelized views own that cache and can use the slower cadence.
            // Neither route serializes the native full-resolution resident map.
            const bool publishFullMap = !multiIlluminatorEnabled ||
              (!recoveringBacklog && fullMapCadence);
            // Multi-lane derived products are narrowband, but the spectrum is
            // intentionally the complete captured passband. Publish it from
            // one physical channel without altering the channelizer input.
            // Spectrum publication is intentionally slower than radar CPI
            // processing because it is a UI-only product.
            if(profileCycle)profileRecord.time[4]=std::chrono::steady_clock::now();
            if (multiIlluminatorEnabled && publishSpectrum) {
              if (directFloatCapture)
                spectrumAnalyser->process(multiGpuStagingInput[referenceChannel],
                  captureData[referenceChannel].get());
              else
                spectrumAnalyser->process(captureData[referenceChannel].get());
              socket_iqdata->sendData(attachEventTime(
                captureData[referenceChannel]->to_json(frameTimestampMs)));
            }
            if(profileCycle)profileRecord.time[5]=std::chrono::steady_clock::now();
            std::vector<const std::deque<std::complex<double>>*>
              channelizerInput;
            channelizerInput.reserve(captureData.size());
            if (multiIlluminatorEnabled) for (const auto& channel : captureData)
              channelizerInput.push_back(&channel->view_data());
            if(multiGpuRecoveryPending && !multiGpuChannelizer){
              if(multiGpuRecoveryCooldownFrames) --multiGpuRecoveryCooldownFrames;
              else try {
                if(!multiGpuConfig)
                  throw std::logic_error("Wideband GPU recovery lacks its frozen configuration");
                auto replacement=blah2::wideband_gpu::createLeasedProcess(
                  *multiGpuConfig,multiGpuDevice,multiGpuOptions);
                auto qualifier=std::make_unique<
                  blah2::wideband_gpu::SelectedOutputQualifier>(*multiGpuConfig);
                // Both implementations start a fresh FIR epoch. Downstream
                // adaptive state must not bridge the CPU recovery block and
                // the replacement worker's independently qualified history.
                multiChannelizer->reset();
                drainMultiPublications();
                resetMultiRadar();
                multiPipelineState.input_epoch_reset();
                multiGpuChannelizer=std::move(replacement);
                multiGpuQualifier=std::move(qualifier);
                multiGpuQualified=false;
                multiGpuRecoveryPending=false;
                multiGpuRecoveryFailures=0;
                multiGpuRecoveryCooldownFrames=0;
                multiPipelineDelayed=!state&&
                  multiGpuChannelizer->supportsSplitFrame();
                std::cerr<<"[MultiIlluminator] recreated GPU channelizer worker "
                  <<multiGpuChannelizer->deviceName()
                  <<"; numerical requalification required\n";
              } catch(const std::exception& error) {
                ++multiGpuRecoveryFailures;
                multiGpuRecoveryCooldownFrames=1u<<std::min(
                  multiGpuRecoveryFailures,4u);
                std::cerr<<"[MultiIlluminator] GPU channelizer recovery failed: "
                  <<error.what()<<"; retry in "<<multiGpuRecoveryCooldownFrames
                  <<" frames\n";
              }
            }
            if(multiGpuChannelizer && !directFloatCapture){
              if(multiGpuStagingInput.size()!=channelizerInput.size())
                throw std::runtime_error("GPU staging channel count changed");
              multiGpuSubmissionExecutor->parallel_for(channelizerInput.size(),
                [&](std::size_t channel){
                if(channelizerInput[channel]->size()<nSamples)
                  throw std::runtime_error(
                    "GPU capture is shorter than one CPI");
                auto& owned=multiGpuStagingInput[channel];
                for(std::size_t sample=0;sample<nSamples;++sample)
                  owned[sample]=std::complex<float>(
                    (*channelizerInput[channel])[sample]);
              });
            }
            const uint64_t firstInputSample = captureSamplePositionAvailable ?
              captureFirstEpochSample : multiFallbackInputSample;
            if(profileCycle)profileRecord.time[6]=std::chrono::steady_clock::now();
            multiFallbackInputSample = firstInputSample + nSamples;
            std::vector<blah2::channelizer::IlluminatorOutput> channelized;
            const auto convertGpu = [&](std::vector<blah2::wideband_gpu::LaneOutput> gpu) {
              std::vector<blah2::channelizer::IlluminatorOutput> converted;
              converted.reserve(gpu.size());
              for(size_t index=0;index<gpu.size();++index){blah2::channelizer::SubbandChannelizer::Output samples;samples.channels=std::move(gpu[index].channels);samples.firstInputSample=gpu[index].firstInputSample;samples.hasFirstInputSample=gpu[index].hasFirstInputSample;samples.inputSamplesPerOutput=gpu[index].inputSamplesPerOutput;samples.groupDelayInputSamples=multiGroupDelays[index];converted.push_back({illuminatorDefinitions[index].id,gpu[index].frequencyHz,std::move(samples)});}
              return converted;
            };
            const auto convertLeasedGpu = [&](std::vector<blah2::wideband_gpu::LeasedLaneOutput> gpu) {
              std::vector<blah2::channelizer::IlluminatorOutput> converted;
              converted.reserve(gpu.size());
              for(size_t index=0;index<gpu.size();++index){
                blah2::channelizer::SubbandChannelizer::Output samples;
                samples.borrowed=std::move(gpu[index].samples);
                samples.firstInputSample=gpu[index].firstInputSample;
                samples.hasFirstInputSample=gpu[index].hasFirstInputSample;
                samples.inputSamplesPerOutput=gpu[index].inputSamplesPerOutput;
                samples.groupDelayInputSamples=multiGroupDelays[index];
                converted.push_back({illuminatorDefinitions[index].id,
                  gpu[index].frequencyHz,std::move(samples)});
              }
              return converted;
            };
            using PipelineClock = std::chrono::steady_clock;
            const auto pipelineStart = PipelineClock::now();
            if(profileCycle)profileRecord.time[7]=pipelineStart;
            const double capturePrepareMs = std::chrono::duration<double,
              std::milli>(pipelineStart - residentWholeStart).count();
            const auto channelizerStart = pipelineStart;
            bool splitBegan=false,splitScheduled=false,splitFinished=false,
              haveChannelized=false;
            std::future<void> splitSubmission;
            std::optional<blah2::multi_illuminator::PublicationContext>
              publication;
            const auto widebandDevice = multiGpuChannelizer ?
              multiGpuChannelizer->deviceName() : std::string{};
            const auto currentPublication = [&](double channelizerMs,
                bool qualificationFrame = false, bool gpuExecuted = false,
                bool cpuExecuted = false) {
              auto status=multiPipelineState.acceleration_status(
                accelerationMode,bool(multiGpuChannelizer),
                widebandGpuRequested,qualificationFrame);
              status.device = widebandDevice;
              status.gpuExecutionBackend = gpuExecuted ? "vulkan-wideband" : "";
              auto publication = multiPipelineState.publication(frameTimestampMs,
                eventTime, captureSamplePositionAvailable, captureEpoch,
                captureFirstEpochSample, captureEndEpochSample,
                captureBacklogSamples, captureDroppedSamples, channelizerMs,
                qualificationFrame,gpuExecuted,cpuExecuted,status,publishFullMap);
              publication.inputGainDb = captureGainDb;
              publication.signalTimestampMs = frameSignalTimeMs;
              publication.capturePrepareMs = capturePrepareMs;
              return publication;
            };
            const auto runtimeFallback = [&](const char* stage,
                const std::exception& error) {
              const std::string reason = std::string("GPU channelizer ") +
                stage + " failed: " + error.what();
              std::cerr << "[MultiIlluminator] " << reason <<
                "; CPU same-block reset/fallback\n";
              multiGpuRecoveryPending = !state && multiGpuConfig &&
                blah2::wideband_gpu::recoverableWorkerFailure(error.what());
              multiGpuChannelizer.reset();
              multiGpuQualifier.reset();
              multiGpuQualified = false;
              multiPipelineState.runtime_fallback(reason,
                [&] { multiChannelizer->reset(); },
                [&] { drainMultiPublications(); resetMultiRadar(); });
            };
            const auto qualifyGpu = [&](const std::vector<
                blah2::wideband_gpu::LaneOutput>& gpu,
                double gpuMilliseconds) {
              if (!multiGpuQualifier)
                throw std::logic_error("GPU channelizer lacks its qualification oracle");
              const auto oracleStart = PipelineClock::now();
              const auto result = multiGpuQualifier->check(multiGpuStagingInput,
                firstInputSample, gpu, gpuMilliseconds, tCpi * 1000.0);
              if (!result.verifiedSilence)
                std::cerr << "[MultiIlluminator] " << result.reason <<
                  " checked=" << result.comparedSamples <<
                  " oracle_macs=" << result.oracleMultiplyAdds <<
                  " oracle_ms=" << std::chrono::duration<double, std::milli>(
                    PipelineClock::now() - oracleStart).count() <<
                  " rms=" << result.worstRms << " peak=" << result.worstPeak << "\n";
              if (result.status ==
                  blah2::wideband_gpu::QualificationStatus::passed)
              {
                multiGpuQualified = true;
                multiPipelineState.gpu_requalified();
                return true;
              }
              // Silence is checked exhaustively (zero input/history/output),
              // so it can be published without declaring the device qualified.
              if (result.verifiedSilence) return true;
              const std::string reason = result.reason;
              multiGpuRecoveryPending = false;
              multiGpuChannelizer.reset();
              multiGpuQualifier.reset();
              multiGpuQualified = false;
              // Unlike the old full-block oracle, sampling never advances the
              // CPU bank. Fail closed at an explicit epoch boundary and then
              // recompute this complete input block using the original CPU DSP.
              multiPipelineState.runtime_fallback(reason,
                [&] { multiChannelizer->reset(); },
                [&] { drainMultiPublications(); resetMultiRadar(); });
              return false;
            };
            if(multiPendingPublication.has_value()){
              auto pending=multiPendingPublication.take();
              channelized=std::move(pending.first);
              publication=std::move(pending.second);
              haveChannelized=true;
            }
            const auto queueCurrent=[&](std::vector<
                blah2::channelizer::IlluminatorOutput> current,
                blah2::multi_illuminator::PublicationContext context){
              if(multiPipelineDelayed||haveChannelized)
                multiPendingPublication.store(std::move(current),
                  std::move(context));
              else{
                channelized=std::move(current);
                publication=std::move(context);
                haveChannelized=true;
              }
            };
            if (!multiIlluminatorEnabled) {
              blah2::iq::CoherentSamples samples;
              samples.firstInputSample = firstInputSample;
              samples.hasFirstInputSample = true;
              samples.inputSamplesPerOutput = 1;
              if(directFloatCapture){
                blah2::iq::BorrowedSamples::FloatChannels views;
                for(const auto& channel:multiGpuStagingInput)
                  views.push_back({channel.data(),channel.size()});
                samples.borrowed=blah2::iq::BorrowedSamples{
                  multiGpuStagingOwner,std::move(views)};
              }else{
                blah2::iq::BorrowedSamples::DoubleChannels views;
                for(const auto& channel:captureData)
                  views.push_back(channel->sample_view());
                samples.borrowed=blah2::iq::BorrowedSamples{
                  captureOwner,std::move(views)};
              }
              std::vector<blah2::iq::SignalBlock> current;
              current.push_back({"primary", fc, std::move(samples)});
              queueCurrent(std::move(current), currentPublication(0));
            } else if(!state&&multiGpuChannelizer&&multiGpuChannelizer->supportsSplitFrame()){
              multiPipelineDelayed=true;
              // Capture owns these deques and may recycle them as soon as the
              // processing thread advances. Stage them into the isolated GPU
              // shared buffer before doing prior-frame radar work; only the
              // GPU execution remains in flight. Background staging borrowed
              // unstable deques and could stall or corrupt a live frame.
              splitBegan=blah2::wideband_gpu::attemptOptionalWithFallback(
                [&]{multiGpuChannelizer->begin(
                  multiGpuStagingInput,firstInputSample);},
                [&](const std::exception& error){
                  runtimeFallback("begin",error);
                });
              splitScheduled=splitBegan;
              if(!splitBegan){
                const auto cpuStart=PipelineClock::now();
                auto current=multiChannelizer->process(
                  multiGpuStagingInput,firstInputSample);
                const double cpuMs=std::chrono::duration<double,std::milli>(
                  PipelineClock::now()-cpuStart).count();
                queueCurrent(std::move(current),currentPublication(cpuMs,
                  false,false,true));
              }
            }else if(multiGpuChannelizer){
              std::vector<blah2::wideband_gpu::LaneOutput> gpu;
              const bool qualificationFrame=!multiGpuQualified;
              const bool gpuSucceeded=blah2::wideband_gpu::attemptOptionalWithFallback(
                [&]{gpu=multiGpuChannelizer->process(multiGpuStagingInput,firstInputSample);},
                [&](const std::exception& error){runtimeFallback("process",error);});
              const bool gpuExecuted=gpuSucceeded;
              bool cpuExecuted=!gpuSucceeded;
              std::vector<blah2::channelizer::IlluminatorOutput> current;
              if(!gpuSucceeded)current=multiChannelizer->process(multiGpuStagingInput,firstInputSample);
              else if(qualificationFrame){
                const double gpuMs=std::chrono::duration<double,std::milli>(
                  PipelineClock::now()-channelizerStart).count();
                if(qualifyGpu(gpu,gpuMs))current=convertGpu(std::move(gpu));
                else{current=multiChannelizer->process(multiGpuStagingInput,
                  firstInputSample);cpuExecuted=true;}
              }else current=convertGpu(std::move(gpu));
              const double channelizerMs=std::chrono::duration<double,std::milli>(
                PipelineClock::now()-channelizerStart).count();
              queueCurrent(std::move(current),currentPublication(channelizerMs,
                qualificationFrame,gpuExecuted,cpuExecuted));
            }else{
              auto current=directFloatCapture?
                multiChannelizer->process(multiGpuStagingInput,firstInputSample):
                multiChannelizer->process(channelizerInput,firstInputSample);
              const double channelizerMs=std::chrono::duration<double,std::milli>(
                PipelineClock::now()-channelizerStart).count();
              queueCurrent(std::move(current),currentPublication(channelizerMs,
                false,false,true));
            }
            const auto finishCurrent=[&]{
              if(!splitScheduled||splitFinished)return;
              splitFinished=true;
              std::vector<blah2::channelizer::IlluminatorOutput> next;
              std::vector<blah2::wideband_gpu::LaneOutput> gpu;
              const bool qualificationFrame=!multiGpuQualified;
              bool began=splitBegan;
              if(!began)began=blah2::wideband_gpu::attemptOptionalWithFallback(
                [&]{splitSubmission.get();splitBegan=true;},
                [&](const std::exception& error){runtimeFallback("begin",error);});
              bool success=false;
              // Qualification retains the existing owned oracle contract.
              // Steady-state shared views pin a bounded slot until canonical
              // processing consumes the prior publication, including EOF drain.
              auto* leased=multiGpuChannelizer?dynamic_cast<
                blah2::wideband_gpu::LeasedOutputBackend*>(multiGpuChannelizer.get()):nullptr;
              const bool useLease=!qualificationFrame&&leased&&leased->supportsLeasedOutput();
              if(began)success=blah2::wideband_gpu::attemptOptionalWithFallback(
                [&]{
                  if(useLease)next=convertLeasedGpu(leased->finishLeasedFrame());
                  else gpu=multiGpuChannelizer->finishFrame();
                },
                [&](const std::exception& error){runtimeFallback("finish",error);});
              const bool gpuExecuted=success;
              bool cpuExecuted=!success;
              if(!success)next=multiChannelizer->process(multiGpuStagingInput,firstInputSample);
              else if(qualificationFrame){
                const double gpuMs=std::chrono::duration<double,std::milli>(
                  PipelineClock::now()-channelizerStart).count();
                if(qualifyGpu(gpu,gpuMs))next=convertGpu(std::move(gpu));
                else{next=multiChannelizer->process(multiGpuStagingInput,
                  firstInputSample);cpuExecuted=true;}
              }else if(!useLease)next=convertGpu(std::move(gpu));
              const double channelizerMs=std::chrono::duration<double,std::milli>(
                PipelineClock::now()-channelizerStart).count();
              multiPendingPublication.store(std::move(next),currentPublication(
                channelizerMs,qualificationFrame,gpuExecuted,cpuExecuted));
            };
            if(!haveChannelized){finishCurrent();
              time.clear();
              std::fill(consumerClaimedSamples.begin(),
                consumerClaimedSamples.end(), 0);continue;}
            // Until the first representative block qualifies, complete and
            // measure GPU work before either the direct oracle or prior-frame
            // radar can contend with it. Steady-state overlap starts only
            // after qualification passes.
            if(splitScheduled&&!multiGpuQualified)finishCurrent();
            if(!publication)
              throw std::logic_error("Channelized frame lacks publication metadata");
            const auto radarStart=PipelineClock::now();
            if(profileCycle){profileRecord.time[8]=radarStart;
              profileRecord.publishedFirst=publication->captureFirst;}
            auto frames = processMultiFrames(std::move(channelized), *publication);
            const auto framePublications = multiProductPublications.snapshotsFor(frames);
            if (profileCycle && !framePublications.empty())
              profileRecord.publishedFirst = framePublications.front().second.captureFirst;
            const auto laneHealth = multiRadar->laneHealth();
            const auto radarEnd=PipelineClock::now();
            if(profileCycle)profileRecord.time[9]=radarEnd;
            struct SerializedIlluminatorFrame
            {
              std::string map, detections, tracks;
            };
            std::vector<SerializedIlluminatorFrame> serialized(frames.size());
            multiSerializationExecutor->parallel_for(frames.size(),
              [&](std::size_t index) {
                const auto& frame = frames[index];
                const auto& framePublication = multiProductPublications.at(frame.publicationSequence);
                const auto identity = native_product_identity(frame, framePublication);
                const auto annotate = [&](std::string json) {
                  return blah2::detection::annotateIntegration(
                    blah2::analysis::annotate_native_product(std::move(json), identity),
                    frame.detectorIntegration);
                };
                // Live displays stay bounded; offline replay retains every
                // canonical map cell for numerical and lifecycle comparison.
                serialized[index].map = annotate(
                  multi_map_json(frame, framePublication.timestampMs,
                    framePublication.publishFullMap, !state));
                serialized[index].detections = annotate(
                  multi_detection_json(frame, framePublication.timestampMs));
                serialized[index].tracks = annotate(
                  multi_track_json(frame, framePublication.timestampMs));
              });
            const auto serializationEnd=PipelineClock::now();
            if(profileCycle)profileRecord.time[10]=serializationEnd;
            for (std::size_t index = 0; index < frames.size(); ++index)
            {
              const auto& frame = frames[index];
              const auto& product = serialized[index];
              const auto& framePublication = multiProductPublications.at(frame.publicationSequence);
              publishRadarFrame(frame, framePublication, product.map, product.detections, product.tracks);
            }
            const auto outputEnd=PipelineClock::now();
            if(profileCycle)profileRecord.time[11]=outputEnd;
            const auto milliseconds=[](auto end,auto start){return std::chrono::duration<double,std::milli>(end-start).count();};
            // The advance belongs to the next input. It can fail and
            // synchronously drain that newer token, so publish this completed
            // token's timing before advancing. Full consumer cadence is still
            // recorded through pipelineEnd below.
            blah2::multi_illuminator::publish_then_advance([&] {
              publishRadarTiming(frames, framePublications, laneHealth,
                {milliseconds(radarEnd,radarStart),
                milliseconds(serializationEnd,radarEnd),
                milliseconds(outputEnd,serializationEnd),0.0,
                milliseconds(outputEnd,pipelineStart)});
            }, finishCurrent);
            const auto pipelineEnd=PipelineClock::now();
            if(profileCycle)profileRecord.time[12]=pipelineEnd;
            blah2::replay_gpu_qualification::event("publication-complete");
            time.clear();
            std::fill(consumerClaimedSamples.begin(),
              consumerClaimedSamples.end(), 0);
            if (!framePublications.empty())
              timing->record_processing_cycle(residentWholeStart, pipelineStart,
                pipelineEnd, PipelineClock::now());
            if(profileCycle){profileRecord.time[13]=PipelineClock::now();
              streamProfile.record(profileRecord);}
            continue;
          }

        }
        else
        {
          // short delay to prevent tight looping
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
      }
      timing->complete_processing_cycle(std::chrono::steady_clock::now());
      // Live stop and finite replay both complete the held channelizer and
      // product CPIs. Capture/loop boundaries use this same publication drain.
      drainMultiPublications();
      publishTerminalCaptureAccounting();
    } catch (const std::exception& error) {
      try { resetMultiRadar(); } catch (...) {}
      publishTerminalCaptureAccounting();
      capture->processing_failure_or_stop(error.what());
    } catch (...) {
      try { resetMultiRadar(); } catch (...) {}
      publishTerminalCaptureAccounting();
      capture->processing_failure_or_stop("Unknown processing failure");
    }
    });
  t2.join();
  if (t1.joinable()) t1.join();
  if (nativeFloatGpuCapture) {
    for (std::size_t channel = 0; channel < captureBuffers.size(); ++channel) {
      const auto pool = captureBuffers[channel]->float_capture_pool_stats();
      const auto reserve = captureBuffers[channel]->float_capture_reserve_stats();
      std::cerr << "CAPTURE_POOL channel=" << channel << " allocated=" << pool.allocated
        << " limit=" << pool.limit << " allocations=" << pool.allocations
        << " reuses=" << pool.reuses << " reserve_target=" << reserve.target
        << " reserve_available=" << reserve.available << " reserve_pending=" << reserve.pending
        << " reserve_misses=" << reserve.misses << " lazy_allocations=" << reserve.lazyAllocations
        << " async_allocations=" << reserve.asyncAllocations << " async_failures=" << reserve.asyncFailures
        << " reserve_waits=" << reserve.waits << '\n';
    }
  }

  return capture->fatal_live_input_failure() ? 1 : 0;
}
catch (const StartupInterrupted&) {
  return 0;
}
catch (const std::exception& error) {
  std::cerr << "Radar startup failed: " << error.what() << '\n';
  return 1;
}

void signal_callback_handler(int signum) {
  (void)signum;
  // Signal context cannot safely touch a Source (replay has none) or do I/O.
  STOP_SIGNAL = 1;
}

void getopt_print_help()
{
  std::cout << "--config <file.yml>: 	Configuration file\n"
               "--help:              	Show help\n";
  exit(1);
}

std::string getopt_process(int argc, char **argv)
{
  const char *const short_opts = "c:h";
  const option long_opts[] = {
      {"config", required_argument, nullptr, 'c'},
      {"help", no_argument, nullptr, 'h'},
      {nullptr, no_argument, nullptr, 0}};

  if (argc == 1)
  {
    std::cout << "Error: No arguments provided." << "\n";
    exit(1);
  }

  std::string file;

  while (true)
  {
    const auto opt = getopt_long(argc, argv, short_opts, long_opts, nullptr);

    // handle input "-", ":", etc
    if ((argc == 2) && (-1 == opt))
    {
      std::cout << "Error: No arguments provided." << "\n";
      exit(1);
    }

    if (-1 == opt)
      break;

    switch (opt)
    {
    case 'c':
      file = std::string(optarg);
      break;

    case 'h':
      getopt_print_help();

    // unrecognised option
    case '?':
      exit(1);

    default:
      break;
    }
  }

  return file;
}

std::string ryml_get_file(const char *filename)
{
  std::ifstream in(filename, std::ios::in | std::ios::binary);
  if (!in)
  {
    std::cerr << "could not open " << filename << "\n";
    exit(1);
  }
  std::ostringstream contents;
  contents << in.rdbuf();
  return contents.str();
}

uint64_t current_time_ms()
{
  // current time in POSIX ms
  return std::chrono::duration_cast<std::chrono::milliseconds>
  (std::chrono::system_clock::now().time_since_epoch()).count();
}

uint64_t current_time_us()
{
  // current time in POSIX us
  return std::chrono::duration_cast<std::chrono::microseconds>
  (std::chrono::system_clock::now().time_since_epoch()).count();
}
