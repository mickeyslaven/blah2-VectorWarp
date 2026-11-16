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
#include "data/meta/EventTime.h"
#include "data/Track.h"
#include "process/ambiguity/Ambiguity.h"
#include "process/ambiguity/Acceleration.h"
#include "process/analysis/StartupIq.h"
#include "process/ambiguity/GpuResidentCoordinator.h"
#include "process/ambiguity/GpuDeviceSelection.h"
#include "process/integration/DirectPathCoherentReplay.h"
#include "process/ambiguity/GpuWidebandProcess.h"
#include "process/ambiguity/GpuWidebandFallback.h"
#include "process/ambiguity/BearingManifold.h"
#include "process/ambiguity/ManifoldEvidence.h"
#include "process/clutter/WienerHopf.h"
#include "process/utility/FftwThreads.h"
#include "process/conditioning/ArrayReferenceSynthesizer.h"
#include "process/fusion/Noncoherent.h"
#include "process/fusion/CoherentBeamBank.h"
#include "process/analysis/ComplexCutoutRecorder.h"
#include "process/analysis/MultiIlluminatorStatus.h"
#include "process/analysis/ReplayEchoInjector.h"
#include "process/analysis/ProductionRawIq.h"
#include "process/analysis/MultiIlluminatorRadar.h"
#include "process/analysis/RadarAlgorithmContract.h"
#include "process/integration/LiveTemporalShadow.h"
#include "process/analysis/MultiIlluminatorPipelineState.h"
#include "process/conditioning/IlluminatorChannelizerBank.h"
#include "process/detection/CfarDetector1D.h"
#include "process/detection/Centroid.h"
#include "process/detection/Interpolate.h"
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
void timing_helper(std::vector<std::string>& timing_name, 
  std::vector<double>& timing_time, std::vector<uint64_t>& time_us, 
  std::string name);

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

std::optional<std::complex<double>> direct_path_pilot(
  const IqData& reference, const std::vector<std::unique_ptr<IqData>>& surveillance,
  const std::vector<uint32_t>& surveillanceChannels,
  const std::vector<uint32_t>& referenceChannels,
  const std::vector<std::complex<double>>& referenceWeights)
{
  const auto& referenceSamples = reference.view_data();
  if (referenceSamples.empty() || surveillance.size() != surveillanceChannels.size())
    return std::nullopt;
  std::complex<double> pilot{};
  for (std::size_t path = 0; path < surveillance.size(); ++path)
  {
    const auto& samples = surveillance[path]->view_data();
    if (samples.size() != referenceSamples.size()) return std::nullopt;
    std::complex<double> correlation{};
    double referenceEnergy = 0, surveillanceEnergy = 0;
    for (std::size_t sample = 0; sample < samples.size(); ++sample)
    {
      correlation += samples[sample] * std::conj(referenceSamples[sample]);
      referenceEnergy += std::norm(referenceSamples[sample]);
      surveillanceEnergy += std::norm(samples[sample]);
    }
    const double normalization = std::sqrt(referenceEnergy * surveillanceEnergy);
    if (!(normalization > 1e-30) || !std::isfinite(normalization)) continue;
    auto weight = std::complex<double>{1, 0};
    const auto found = std::find(referenceChannels.begin(), referenceChannels.end(),
      surveillanceChannels[path]);
    if (found != referenceChannels.end())
    {
      const std::size_t index = std::distance(referenceChannels.begin(), found);
      if (index < referenceWeights.size()) weight = std::conj(referenceWeights[index]);
    }
    pilot += weight * correlation / normalization;
  }
  if (!std::isfinite(pilot.real()) || !std::isfinite(pilot.imag()) ||
      std::abs(pilot) <= 1e-30) return std::nullopt;
  return pilot;
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
    uint64_t timestamp, bool full = true)
{
  return frame.residentProducts ? resident_product_timestamp(frame.residentProducts->map, timestamp) :
    frame.map->to_json_presentation_km(timestamp, frame.sampleRateHz, full);
}
std::string multi_detection_json(const blah2::multi_illuminator::RadarFrame& frame, uint64_t timestamp)
{
  return frame.residentProducts ? resident_product_timestamp(frame.residentProducts->detections, timestamp) :
    frame.detections->to_json_km(timestamp, frame.sampleRateHz);
}
std::string multi_track_json(const blah2::multi_illuminator::RadarFrame& frame, uint64_t timestamp)
{
  return frame.residentProducts ? resident_product_timestamp(frame.residentProducts->tracks, timestamp) :
    frame.tracks->to_json(timestamp);
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
  result.productBackend = frame.residentProducts ? "vulkan-resident" : "cpu";
  if (frame.residentProducts) result.productDevice = frame.acceleration.device;
  result.temporalExecuted = frame.residentTemporalExecuted ||
    (frame.detectorIntegration && frame.detectorIntegration->availableCpis > 0);
  result.detectorExecuted = bool(frame.detections) &&
    (!frame.detectorIntegration || frame.detectorIntegration->ready);
  result.trackerExecuted = bool(frame.tracks);
  result.bearingExecuted = frame.residentProducts && !frame.residentProducts->bearing.empty();
  return result;
}

std::string attach_live_temporal_shadow(std::string json,
  const blah2::integration::LiveTemporalShadow::Status& status);

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
  if (frame.ridgeScrubber)
  {
    writer.Key("doppler_ridge_scrubber"); writer.StartObject();
    writer.Key("mode"); writer.String(frame.ridgeScrubber->mode.c_str());
    writer.Key("state"); writer.String(frame.ridgeScrubber->state.c_str());
    writer.Key("reason"); writer.String(frame.ridgeScrubber->reason.c_str());
    writer.Key("reset_reason"); writer.String(frame.ridgeScrubber->resetReason.c_str());
    writer.Key("epoch"); writer.Uint64(frame.ridgeScrubber->epoch);
    writer.Key("comparison_available"); writer.Bool(frame.ridgeScrubber->comparisonAvailable);
    writer.Key("observed_cpis");
    if (frame.ridgeScrubber->mode == "shadow") writer.Uint(frame.ridgeScrubber->observedCpis);
    else writer.Null();
    writer.Key("maximum_cpis");
    if (frame.ridgeScrubber->mode == "shadow") writer.Uint(frame.ridgeScrubber->maximumCpis);
    else writer.Null();
    writer.Key("background_ms"); writer.Double(frame.ridgeScrubber->backgroundMs);
    writer.Key("comparison_detection_ms"); writer.Double(frame.ridgeScrubber->comparisonDetectionMs);
    writer.Key("comparison_tracking_ms"); writer.Double(frame.ridgeScrubber->comparisonTrackingMs);
    writer.Key("comparison_ms"); writer.Double(frame.ridgeScrubber->comparisonMs);
    writer.Key("total_comparison_ms");
    if (frame.ridgeScrubber->mode == "shadow") writer.Double(frame.ridgeScrubber->totalComparisonMs);
    else writer.Null();
    writer.Key("observed_rows"); writer.Uint(frame.ridgeScrubber->observedRows);
    writer.Key("qualified_rows"); writer.Uint(frame.ridgeScrubber->qualifiedRows);
    writer.Key("maximum_applied_db");
    writer.Double(frame.ridgeScrubber->maximumAppliedDb);
    writer.Key("raw_detections");
    writer.Uint64(frame.ridgeScrubber->rawDetections);
    writer.Key("scrubbed_detections");
    if (frame.ridgeScrubber->comparisonAvailable) writer.Uint64(frame.ridgeScrubber->scrubbedDetections);
    else writer.Null();
    writer.Key("raw_tracks"); writer.Uint64(frame.ridgeScrubber->rawTracks);
    writer.Key("scrubbed_tracks");
    if (frame.ridgeScrubber->comparisonAvailable) writer.Uint64(frame.ridgeScrubber->scrubbedTracks);
    else writer.Null();
    writer.Key("raw_operational_tracks");
    writer.Uint64(frame.ridgeScrubber->rawOperationalTracks);
    writer.Key("scrubbed_operational_tracks");
    if (frame.ridgeScrubber->comparisonAvailable) writer.Uint64(frame.ridgeScrubber->scrubbedOperationalTracks);
    else writer.Null();
    writer.Key("qualified_doppler_hz"); writer.StartArray();
    for (const double value : frame.ridgeScrubber->qualifiedDopplerHz)
      writer.Double(value);
    writer.EndArray();
    writer.EndObject();
  }
  if (frame.detectorIntegration) {
    writer.Key("detector_integration");
    blah2::detection::writeIntegration(writer, *frame.detectorIntegration);
  }
  writer.Key("temporalShadow"); writer.StartObject();
  writer.Key("active"); writer.Bool(frame.temporalShadowActive);
  if (frame.temporalShadowActive) {
    writer.Key("sequence"); writer.Uint64(frame.temporalShadowSequence);
    writer.Key("available_cpis"); writer.Uint(frame.temporalShadowAvailableCpis);
    writer.Key("maximum_cpis"); writer.Uint(frame.temporalShadowMaximumCpis);
    writer.Key("peak_power"); writer.Double(frame.temporalShadowPeakPower);
    writer.Key("integrated_cells");
    writer.Uint64(frame.temporalShadowIntegratedCells);
    writer.Key("input_statistic");
    writer.String(frame.temporalShadowInputStatistic.c_str());
    writer.Key("bootstrap"); writer.StartObject();
    writer.Key("trained_cells");
    writer.Uint64(frame.temporalShadowBootstrapTrainedCells);
    writer.Key("fallback_cells");
    writer.Uint64(frame.temporalShadowBootstrapFallbackCells);
    writer.Key("invalid_cells");
    writer.Uint64(frame.temporalShadowBootstrapInvalidCells);
    writer.EndObject();
    writer.Key("artifact"); writer.StartObject();
    writer.Key("format"); writer.String("VWTBD01");
    writer.Key("mature"); writer.Bool(frame.temporalShadowAvailableCpis >=
      frame.temporalShadowMaximumCpis);
    writer.EndObject();
    writer.Key("banks"); writer.StartArray();
    const std::array<const char*, 4> bankNames{
      "baseline2", "motion2", "motion4", "motion8"};
    const std::array<uint32_t, 4> bankDepths{2, 2, 4, 8};
    for (std::size_t index = 0; index < bankNames.size(); ++index) {
      writer.StartObject();
      writer.Key("name"); writer.String(bankNames[index]);
      writer.Key("depth"); writer.Uint(bankDepths[index]);
      writer.Key("ready"); writer.Bool(frame.temporalShadowBankReady[index]);
      writer.EndObject();
    }
    writer.EndArray();
  }
  writer.EndObject();
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
  return frame.liveTemporalShadow ? attach_live_temporal_shadow(
    buffer.GetString(), *frame.liveTemporalShadow) : buffer.GetString();
}

// Compact observer telemetry only. Full-precision score maps never enter the
// presentation transport and never replace accepted detector/tracker products.
std::string attach_live_temporal_shadow(std::string json,
  const blah2::integration::LiveTemporalShadow::Status& status)
{
  rapidjson::StringBuffer buffer;
  rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
  writer.StartObject();
  writer.Key("enabled"); writer.Bool(status.enabled);
  writer.Key("disabled_after_failure"); writer.Bool(status.disabled);
  writer.Key("current"); writer.Bool(status.current);
  writer.Key("busy"); writer.Bool(status.busy);
  writer.Key("snapshot_available"); writer.Bool(status.snapshotAvailable);
  writer.Key("result_available"); writer.Bool(status.monitorResultAvailable);
  writer.Key("payload_available"); writer.Bool(status.resultAvailable);
  writer.Key("mode"); writer.String(status.scoringEnabled ?
    "post_clutter_bootstrap_disjoint_shadow_tracking" :
    "post_clutter_bootstrap_observation_only");
  writer.Key("null_calibrated"); writer.Bool(false);
  writer.Key("scoring_identity");
  writer.String("bootstrap_motion_max_gamma_approx_v2");
  writer.Key("generation"); writer.String(std::to_string(status.generation).c_str());
  writer.Key("accepted"); writer.Uint64(status.accepted);
  writer.Key("dropped"); writer.Uint64(status.dropped);
  writer.Key("drop_reasons"); writer.StartObject();
  for (std::size_t index = 0;
       index < blah2::integration::LiveTemporalShadow::dropReasonNames.size();
       ++index) {
    writer.Key(blah2::integration::LiveTemporalShadow::dropReasonNames[index]);
    writer.Uint64(status.dropReasons.counts[index]);
  }
  writer.EndObject();
  writer.Key("rejected"); writer.Uint64(status.rejected);
  writer.Key("budget_exhaustions"); writer.Uint64(status.budgetExhaustions);
  writer.Key("resets"); writer.Uint64(status.resets);
  writer.Key("reset_serial"); writer.Uint64(status.resetSerial);
  writer.Key("last_reset_reason"); writer.String(status.lastResetReason.c_str());
  writer.Key("resident_bytes_bound"); writer.Uint64(status.residentBytesBound);
  writer.Key("copy_ms"); writer.Double(status.copyMs);
  writer.Key("maximum_copy_ms"); writer.Double(status.maximumCopyMs);
  writer.Key("copy_overruns"); writer.Uint64(status.copyOverruns);
  writer.Key("copy_overrun_streak"); writer.Uint(status.copyOverrunStreak);
  writer.Key("disable_reason"); writer.String(status.disableReason.c_str());
  writer.Key("source_rows"); writer.Uint(status.sourceRows);
  writer.Key("source_columns"); writer.Uint(status.sourceColumns);
  writer.Key("observed_rows"); writer.Uint(status.observedRows);
  writer.Key("observed_columns"); writer.Uint(status.observedColumns);
  writer.Key("observed_column_offset"); writer.Uint(status.observedColumnOffset);
  writer.Key("work_ms"); writer.Double(status.workMs);
  writer.Key("artifact_enabled"); writer.Bool(status.artifactEnabled);
  writer.Key("artifact_disabled"); writer.Bool(status.artifactDisabled);
  writer.Key("artifact_complete"); writer.Bool(status.artifactComplete);
  writer.Key("artifact_frames"); writer.Uint64(status.artifactFrames);
  writer.Key("artifact_bytes"); writer.Uint64(status.artifactBytes);
  writer.Key("artifact_write_ms"); writer.Double(status.artifactWriteMs);
  writer.Key("shadow_tracking_totals"); writer.StartArray();
  for (const auto& lane : status.scoring) {
    writer.StartObject();
    writer.Key("depth"); writer.Uint(lane.depth);
    writer.Key("evaluated_windows"); writer.Uint64(lane.evaluatedWindows);
    writer.Key("overlapping_windows_skipped"); writer.Uint64(lane.skippedOverlappingWindows);
    writer.Key("candidates"); writer.Uint64(lane.candidates);
    writer.Key("truncated_candidates"); writer.Uint64(lane.truncatedCandidates);
    writer.Key("cumulative_associations"); writer.Uint64(lane.cumulativeAssociations);
    writer.Key("cumulative_promotions"); writer.Uint64(lane.cumulativePromotions);
    writer.Key("cumulative_misses"); writer.Uint64(lane.cumulativeMisses);
    writer.EndObject();
  }
  writer.EndArray();
  if (status.artifactFrames) {
    writer.Key("artifact_last_frame"); writer.StartObject();
    const auto exact = [&](const char* key, uint64_t value) {
      writer.Key(key); writer.String(std::to_string(value).c_str());
    };
    exact("capture_epoch", status.artifactLastFrame.captureEpoch);
    exact("sequence", status.artifactLastFrame.sequence);
    exact("first_sample", status.artifactLastFrame.firstSample);
    exact("end_sample", status.artifactLastFrame.endSample);
    exact("timestamp_ms", status.artifactLastFrame.timestampMs);
    writer.Key("capture_sample_rate_hz"); writer.Uint(status.artifactLastFrame.sampleRateHz);
    writer.Key("map_sample_rate_hz"); writer.Uint(status.artifactLastFrame.mapSampleRateHz);
    writer.EndObject();
  }
  if (status.monitorResultAvailable) {
    const auto exact = [&](const char* key, uint64_t value) {
      writer.Key(key); writer.String(std::to_string(value).c_str());
    };
    exact("capture_epoch", status.frame.captureEpoch);
    exact("sequence", status.frame.sequence);
    exact("first_sample", status.frame.firstSample);
    exact("end_sample", status.frame.endSample);
    exact("configuration_epoch", status.frame.configurationEpoch);
    exact("tuning_hz", status.frame.tuningHz);
    exact("timestamp_ms", status.frame.timestampMs);
    writer.Key("sample_rate_hz"); writer.Uint(status.frame.sampleRateHz);
    writer.Key("capture_sample_rate_hz"); writer.Uint(status.frame.sampleRateHz);
    writer.Key("map_sample_rate_hz"); writer.Uint(status.frame.mapSampleRateHz);
    writer.Key("available_cpis"); writer.Uint(status.availableCpis);
    writer.Key("peak_power"); writer.Double(status.peakPower);
    writer.Key("trained_cells"); writer.Uint64(status.trainedCells);
    writer.Key("fallback_cells"); writer.Uint64(status.fallbackCells);
    writer.Key("invalid_cells"); writer.Uint64(status.invalidCells);
    writer.Key("banks_ready"); writer.StartArray();
    for (const bool ready : status.bankReady) writer.Bool(ready);
    writer.EndArray();
    writer.Key("shadow_tracking"); writer.StartObject();
    writer.Key("enabled"); writer.Bool(status.scoringEnabled);
    writer.Key("publishes_production_tracks"); writer.Bool(false);
    writer.Key("windows"); writer.StartArray();
    for (const auto& lane : status.scoring) {
      writer.StartObject();
      writer.Key("depth"); writer.Uint(lane.depth);
      writer.Key("ready"); writer.Bool(lane.ready);
      writer.Key("evaluated"); writer.Bool(lane.evaluated);
      writer.Key("latest_evaluated_sequence");
      writer.String(std::to_string(lane.latestEvaluatedSequence).c_str());
      writer.Key("evaluated_windows"); writer.Uint64(lane.evaluatedWindows);
      writer.Key("overlapping_windows_skipped"); writer.Uint64(lane.skippedOverlappingWindows);
      writer.Key("candidates"); writer.Uint64(lane.candidates);
      writer.Key("truncated_candidates"); writer.Uint64(lane.truncatedCandidates);
      writer.Key("tracks"); writer.Uint64(lane.tracks);
      writer.Key("active_tracks"); writer.Uint64(lane.activeTracks);
      writer.Key("coasting_tracks"); writer.Uint64(lane.coastingTracks);
      writer.Key("associations"); writer.Uint64(lane.tracker.associations);
      writer.Key("promotions"); writer.Uint64(lane.tracker.promotions);
      writer.Key("misses"); writer.Uint64(lane.tracker.misses);
      writer.Key("latest_candidate_count"); writer.Uint(lane.latestCandidateCount);
      writer.Key("latest_candidates_export_limit"); writer.Uint(lane.candidateExportLimit);
      writer.Key("confirmation_m"); writer.Uint(lane.confirmationM);
      writer.Key("confirmation_n"); writer.Uint(lane.confirmationN);
      writer.Key("delete_after"); writer.Uint(lane.deleteAfter);
      writer.Key("base_cpi_seconds"); writer.Double(lane.baseCpiSeconds);
      writer.Key("update_interval_seconds"); writer.Double(lane.updateIntervalSeconds);
      writer.Key("latest_candidates"); writer.StartArray();
      for (const auto& candidate : lane.latestCandidates) {
        writer.StartObject();
        writer.Key("delay_bins"); writer.Double(candidate.delay);
        writer.Key("doppler_hz"); writer.Double(candidate.dopplerHz);
        writer.Key("snr_db"); writer.Double(candidate.snrDb);
        writer.Key("epoch");
        writer.String(std::to_string(candidate.lineage.epoch).c_str());
        writer.Key("newest_sequence");
        writer.String(std::to_string(candidate.lineage.newestSequence).c_str());
        writer.Key("depth"); writer.Uint(candidate.lineage.depth);
        writer.Key("delay_motion"); writer.Int(candidate.lineage.delayMotion);
        writer.Key("doppler_motion"); writer.Int(candidate.lineage.dopplerMotion);
        writer.Key("support_sequences"); writer.StartArray();
        for (const auto sequence : candidate.lineage.supportSequences())
          writer.String(std::to_string(sequence).c_str());
        writer.EndArray();
        writer.EndObject();
      }
      writer.EndArray();
      writer.EndObject();
    }
    writer.EndArray(); writer.EndObject();
  }
  writer.EndObject();
  if (!json.empty() && json.back() == '}') {
    json.pop_back(); json += ",\"liveTemporalShadow\":";
    json += buffer.GetString(); json += '}';
  }
  return json;
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
  bool saveIq, state, loop;
  std::string replayFormat = "auto";
  std::string replayProvenanceMode = "legacy-unqualified";
  uint32_t legacyReplayBlockSamples = 0;
  bool residentSnapshotEnabled=false;
  uint64_t residentSnapshotMaxBytes=0;
  uint32_t residentSnapshotMaxFrames=0;
  std::string residentSnapshotDirectory,residentSnapshotSourceHash,
    residentSnapshotRfHash,residentSnapshotRfFile;
  tree["capture"]["fs"] >> fs;
  tree["capture"]["fc"] >> fc;
  tree["capture"]["device"]["type"] >> type;
  tree["save"]["iq"] >> saveIq;
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

  bool multiIlluminatorEnabled = false;
  bool experimentalFmReferenceQualityFilter = false;
  blah2::multi_illuminator::AdvancedFeatureConfig multiAdvanced;
  std::vector<blah2::channelizer::IlluminatorDefinition> illuminatorDefinitions;
  std::unordered_map<std::string, double> illuminatorTrackerMinimumSnrDb;
  if (tree["process"].has_child("multi_illuminator"))
  {
    auto multi = tree["process"]["multi_illuminator"];
    if (multi.has_child("enabled")) multi["enabled"] >> multiIlluminatorEnabled;
    if (multiIlluminatorEnabled && multi.has_child("input_phase_correction")) {
      std::string mode;
      multi["input_phase_correction"] >> mode;
      if (mode != "off")
        throw std::invalid_argument(
          "Multi-illuminator input phase correction is not available in this processor build");
    }
    if (multiIlluminatorEnabled && multi.has_child("doppler_window")) {
      std::string window;
      multi["doppler_window"] >> window;
      if (window != "rectangular")
        throw std::invalid_argument(
          "Multi-illuminator Doppler windows other than rectangular are not available in this processor build");
    }
    if (multi.has_child("experimental_fm_reference_quality_filter"))
      multi["experimental_fm_reference_quality_filter"] >> experimentalFmReferenceQualityFilter;
    if (experimentalFmReferenceQualityFilter && (!state || !multiIlluminatorEnabled))
      throw std::invalid_argument(
        "Experimental FM reference quality filtering requires multi-illuminator replay");
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
        if (item.has_child("atsc_reference_mode")) {
          std::string atscReferenceMode;
          item["atsc_reference_mode"] >> atscReferenceMode;
          if (atscReferenceMode != "automatic" && atscReferenceMode != "raw" &&
              atscReferenceMode != "pilot_aided")
            throw std::invalid_argument(
              "Per-illuminator atsc_reference_mode must be automatic, raw, or pilot_aided");
          const bool atscStandard = definition.standard == "ATSC" ||
            definition.standard == "ATSC-1.0" || definition.standard == "ATSC-3.0";
          if (!atscStandard && atscReferenceMode != "raw")
            throw std::invalid_argument(
              "ATSC reference modes other than raw require an ATSC standard");
          if (atscReferenceMode == "pilot_aided" ||
              atscReferenceMode == "automatic")
            definition.atscReferenceMode = "pilot_aided";
          else
            definition.atscReferenceMode = "raw";
        }
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
  DopplerRidgeScrubberConfig dopplerRidgeScrubber;
  if (tree["process"]["detection"].has_child("doppler_ridge_scrubber")) {
    auto ridge = tree["process"]["detection"]["doppler_ridge_scrubber"];
    if (ridge.has_child("mode")) ridge["mode"] >> dopplerRidgeScrubber.mode;
    if (ridge.has_child("persistence_cpis"))
      ridge["persistence_cpis"] >> dopplerRidgeScrubber.persistenceCpis;
    if (ridge.has_child("minimum_excess_db"))
      ridge["minimum_excess_db"] >> dopplerRidgeScrubber.minimumExcessDb;
    if (ridge.has_child("maximum_subtraction_db"))
      ridge["maximum_subtraction_db"] >> dopplerRidgeScrubber.maximumSubtractionDb;
    if (ridge.has_child("doppler_tolerance_hz"))
      ridge["doppler_tolerance_hz"] >> dopplerRidgeScrubber.dopplerToleranceHz;
    if (ridge.has_child("neighborhood_rows"))
      ridge["neighborhood_rows"] >> dopplerRidgeScrubber.neighborhoodRows;
    if (ridge.has_child("delay_stride"))
      ridge["delay_stride"] >> dopplerRidgeScrubber.delayStride;
    if (ridge.has_child("shadow_maximum_cpis"))
      ridge["shadow_maximum_cpis"] >> dopplerRidgeScrubber.shadowMaximumCpis;
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
    if (process.has_child("surveillance_fusion") &&
        process["surveillance_fusion"].has_child(
          "detection_manifold_automation") &&
        process["surveillance_fusion"]["detection_manifold_automation"].has_child(
          "enabled"))
      process["surveillance_fusion"]["detection_manifold_automation"]["enabled"] >>
        multiAdvanced.surveillanceManifoldAutomation;
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
    if (detection.has_child("temporal_shadow"))
    {
      const auto shadow = detection["temporal_shadow"];
      if (shadow.has_child("enabled"))
        shadow["enabled"] >> multiAdvanced.temporalShadow;
      if (shadow.has_child("illuminator_id"))
        shadow["illuminator_id"] >>
          multiAdvanced.temporalShadowIlluminatorId;
      if (shadow.has_child("input_statistic"))
        shadow["input_statistic"] >>
          multiAdvanced.temporalShadowInputStatistic;
      if (shadow.has_child("bootstrap_covariance"))
        shadow["bootstrap_covariance"] >>
          multiAdvanced.temporalShadowBootstrapCovariance;
      if (shadow.has_child("max_cpis"))
        shadow["max_cpis"] >> multiAdvanced.temporalShadowMaximumCpis;
      if (shadow.has_child("delay_motion_bins"))
        shadow["delay_motion_bins"] >>
          multiAdvanced.temporalShadowDelayMotionBins;
      if (shadow.has_child("doppler_motion_bins"))
        shadow["doppler_motion_bins"] >>
          multiAdvanced.temporalShadowDopplerMotionBins;
      if (shadow.has_child("artifact_path"))
        shadow["artifact_path"] >> multiAdvanced.temporalShadowArtifactPath;
      if (shadow.has_child("artifact_max_mb"))
      {
        uint64_t megabytes = 0;
        shadow["artifact_max_mb"] >> megabytes;
        if (megabytes > (1ULL << 20))
          throw std::invalid_argument("Temporal artifact maximum is invalid");
        multiAdvanced.temporalShadowArtifactMaximumBytes = megabytes << 20;
      }
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
    if (trackerNode.has_child("track_guided_shadow"))
    {
      const auto guided = trackerNode["track_guided_shadow"];
      if (guided.has_child("enable"))
        guided["enable"] >> multiAdvanced.trackGuidedShadow;
      if (guided.has_child("promote"))
        guided["promote"] >> multiAdvanced.trackGuidedPromotion;
      if (guided.has_child("sustain"))
        guided["sustain"] >> multiAdvanced.trackGuidedSustain;
    }
    if (process.has_child("bearing"))
    {
      const auto bearing = process["bearing"];
      if (bearing.has_child("enable"))
        bearing["enable"] >> multiAdvanced.bearing;
      const auto activeAutoBoolean = [](auto node) {
        std::string value;
        node >> value;
        return value == "true" || value == "on" || value == "auto";
      };
      if (bearing.has_child("frequency_resolved_shadow") &&
          bearing["frequency_resolved_shadow"].has_child("enabled"))
        multiAdvanced.bearingFrequencyResolvedShadow = activeAutoBoolean(
          bearing["frequency_resolved_shadow"]["enabled"]);
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
    multiAdvanced.temporalShadowOfflineReplay = state;
    // Do this before opening output sockets or allocating capture hardware.
    blah2::multi_illuminator::validateAdvancedFeatures(multiAdvanced);
  }

  blah2::integration::LiveTemporalShadow::Config liveShadowConfig;
  std::string liveShadowIlluminatorId;
  if (tree["process"]["detection"].has_child("live_temporal_shadow")) {
    const auto shadow = tree["process"]["detection"]["live_temporal_shadow"];
    if (shadow.has_child("enabled")) shadow["enabled"] >> liveShadowConfig.enabled;
    if (shadow.has_child("illuminator_id")) shadow["illuminator_id"] >> liveShadowIlluminatorId;
    if (shadow.has_child("artifact_path")) shadow["artifact_path"] >> liveShadowConfig.artifactPath;
    if (shadow.has_child("artifact_only")) shadow["artifact_only"] >> liveShadowConfig.artifactOnly;
    if (shadow.has_child("score_tracks")) shadow["score_tracks"] >> liveShadowConfig.scoring.enabled;
    if (shadow.has_child("association_sustain"))
      shadow["association_sustain"] >> liveShadowConfig.associationSustain;
    if (shadow.has_child("association_sustain_cpis"))
      shadow["association_sustain_cpis"] >> liveShadowConfig.associationSustainCpis;
    if (shadow.has_child("maximum_candidates"))
      shadow["maximum_candidates"] >> liveShadowConfig.scoring.maximumCandidates;
    if (shadow.has_child("minimum_initiation_snr_db")) {
      shadow["minimum_initiation_snr_db"] >>
        liveShadowConfig.scoring.minimumInitiationSnrDb;
      liveShadowConfig.scoring.minimumInitiationSnrOverridden = true;
    }
    if (shadow.has_child("artifact_max_frames")) shadow["artifact_max_frames"] >> liveShadowConfig.artifactMaximumFrames;
    if (shadow.has_child("artifact_max_mb")) {
      uint64_t megabytes = 0; shadow["artifact_max_mb"] >> megabytes;
      if (megabytes < 1 || megabytes > 1024)
        throw std::invalid_argument("Live temporal artifact_max_mb must be between 1 and 1024");
      liveShadowConfig.artifactMaximumBytes = megabytes << 20;
    }
    if (shadow.has_child("max_cells")) shadow["max_cells"] >> liveShadowConfig.maximumCells;
    if (shadow.has_child("max_work_ms")) shadow["max_work_ms"] >> liveShadowConfig.maximumWorkMs;
    if (shadow.has_child("min_interval_ms")) shadow["min_interval_ms"] >> liveShadowConfig.minimumIntervalMs;
    if (shadow.has_child("max_cpis")) shadow["max_cpis"] >> liveShadowConfig.temporal.maximumCpis;
    if (shadow.has_child("delay_motion_bins")) shadow["delay_motion_bins"] >> liveShadowConfig.temporal.delayMotionBins;
    if (shadow.has_child("doppler_motion_bins")) shadow["doppler_motion_bins"] >> liveShadowConfig.temporal.dopplerMotionBins;
    if (shadow.has_child("bootstrap_covariance")) {
      std::string covariance; shadow["bootstrap_covariance"] >> covariance;
      using Covariance = blah2::detection::PostClutterBootstrap::Covariance;
      if (covariance == "diagonal") liveShadowConfig.bootstrap.covariance = Covariance::diagonal;
      else if (covariance == "shrinkage") liveShadowConfig.bootstrap.covariance = Covariance::shrinkage;
      else if (covariance == "robust_shrinkage") liveShadowConfig.bootstrap.covariance = Covariance::robustShrinkage;
      else throw std::invalid_argument("Live temporal bootstrap covariance is invalid");
    }
    liveShadowConfig.scoring.maximumCpis = liveShadowConfig.temporal.maximumCpis;
    liveShadowConfig.scoring.delayMotionBins = liveShadowConfig.temporal.delayMotionBins;
    liveShadowConfig.scoring.dopplerMotionBins = liveShadowConfig.temporal.dopplerMotionBins;
  }
  blah2::integration::LiveTemporalShadow::validate(liveShadowConfig);
  if (liveShadowConfig.enabled && state)
    throw std::invalid_argument("Live temporal shadow requires live capture; use temporal_shadow for replay");
  if (liveShadowConfig.enabled && multiIlluminatorEnabled &&
      std::count_if(illuminatorDefinitions.begin(), illuminatorDefinitions.end(),
        [&](const auto& lane) { return lane.id == liveShadowIlluminatorId; }) != 1)
    throw std::invalid_argument("Live temporal shadow illuminator_id must match exactly one enabled lane");
  if (liveShadowConfig.enabled && !multiIlluminatorEnabled && !liveShadowIlluminatorId.empty())
    throw std::invalid_argument("Live temporal shadow illuminator_id requires multi-illuminator mode");

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
    if(bearing.has_child("candidate_peaks"))bearing["candidate_peaks"]>>bearingCandidatePeaks;
    if(bearing.has_child("guard_radius"))bearing["guard_radius"]>>bearingGuardRadius;
    if(bearing.has_child("training_radius"))bearing["training_radius"]>>bearingTrainingRadius;
    if(bearing.has_child("diagonal_loading"))bearing["diagonal_loading"]>>bearingDiagonalLoading;
    if(bearing.has_child("max_rate_deg_s"))bearing["max_rate_deg_s"]>>bearingMaxRateDegPerSecond;
    if(bearing.has_child("manifold_file"))bearing["manifold_file"]>>bearingManifoldFile;
    if(bearing.has_child("channel_mapping"))for(auto value:bearing["channel_mapping"].children()){
      uint32_t channel=0;value>>channel;bearingChannelMapping.push_back(channel);}
    if(bearing.has_child("manifold_recorder")){
      auto recorder=bearing["manifold_recorder"];
      bool automatic=!recorder.has_child("enabled");
      if(recorder.has_child("enabled")){
        std::string mode;recorder["enabled"]>>mode;
        if(mode=="true"||mode=="on")manifoldRecorderEnabled=true;
        else if(mode=="auto")automatic=true;
        else if(mode!="false"&&mode!="off")throw std::invalid_argument("Manifold recorder enabled must be auto, on or off");
      }
      if(automatic){
        std::string fusion="noncoherent";bool detection=false,adsb=false;
        if(tree["process"].has_child("surveillance_fusion")&&tree["process"]["surveillance_fusion"].has_child("mode"))
          tree["process"]["surveillance_fusion"]["mode"]>>fusion;
        if(tree["process"].has_child("detection")&&tree["process"]["detection"].has_child("enable"))
          tree["process"]["detection"]["enable"]>>detection;
        if(tree.rootref().has_child("truth")&&tree["truth"].has_child("adsb")&&tree["truth"]["adsb"].has_child("enabled"))
          tree["truth"]["adsb"]["enabled"]>>adsb;
        bool mapping=false,geometry=false,orientation=false;
        if(tree["capture"]["device"].has_child("array_geometry")){
          auto array=tree["capture"]["device"]["array_geometry"];
          if(array.has_child("mapping_confirmed"))array["mapping_confirmed"]>>mapping;
          if(array.has_child("geometry_confirmed"))array["geometry_confirmed"]>>geometry;
          if(array.has_child("orientation_confirmed"))array["orientation_confirmed"]>>orientation;
        }
        manifoldRecorderEnabled=coherentInput&&appliedGainMetadata&&
          (fusion=="coherent_beam_bank"||fusion=="noise_whitened_beam_bank")&&
          detection&&adsb&&mapping&&geometry&&orientation;
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
  // This replay-only anchor consumes the canonical recorded topology above;
  // it must never depend on whichever live SDR profile happens to be selected.
  if(bearingReferenceAnchor=="fixed_site_projected"){
    if(!state||replayFormat!="blah2")
      throw std::invalid_argument("fixed_site_projected is experimental and requires explicit native blah2 replay");
    const auto allFive=[](const std::vector<uint32_t>& values){
      if(values.size()!=5)return false;
      for(uint32_t index=0;index<5;++index)if(values[index]!=index)return false;
      return true;
    };
    auto process=tree["process"];
    bool clutter=false;std::string fusion,acceleration;double nullDb=0;
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
       !clutter||acceleration!="gpu-resident"||!bearingEnabled||bearingMethod!="covariance_mvdr"||
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
          !tree["location"].has_child("rx") ||
          (!multiIlluminatorEnabled && !tree["location"].has_child("tx")))
        throw std::invalid_argument("Coherent fusion requires array orientation and receiver/transmitter locations");
      double rxLatDeg = 0, rxLonDeg = 0,
        txLatDeg = 0, txLonDeg = 0;
      geometry["x_axis_bearing_deg_true"] >> xAxisBearingDeg;
      tree["location"]["rx"]["latitude"] >> rxLatDeg;
      tree["location"]["rx"]["longitude"] >> rxLonDeg;
      if (multiIlluminatorEnabled) {
        for (const auto& lane : illuminatorDefinitions)
          if (!lane.transmitterLocation)
            throw std::invalid_argument("Coherent lane requires explicit transmitter_location: " + lane.id);
        txLatDeg = illuminatorDefinitions.front().transmitterLocation->at(0);
        txLonDeg = illuminatorDefinitions.front().transmitterLocation->at(1);
      } else {
        tree["location"]["tx"]["latitude"] >> txLatDeg;
        tree["location"]["tx"]["longitude"] >> txLonDeg;
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
  if (state) capture->set_replay(loop, replayFile, replayFormat,
    legacyReplayBlockSamples, replayProvenanceMode);
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
  const uint32_t nSamples = fs * tCpi;
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
  auto referenceData = std::make_unique<IqData>(nSamples);
  std::vector<std::unique_ptr<IqData>> captureData;
  for (std::size_t channel = 0; channel < captureChannelCount; channel++)
    captureData.push_back(std::make_unique<IqData>(nSamples));

  std::vector<std::unique_ptr<IqData>> surveillanceData;
  for (std::size_t pathIndex = 0;
       pathIndex < surveillanceChannels.size(); pathIndex++)
  {
    surveillanceData.push_back(std::make_unique<IqData>(nSamples));
  }
  // The paired live receiver has one reference and one surveillance channel.
  // Keep their completed deques and overwrite the samples next CPI instead of
  // allocating two million deque elements again on every capture handoff.
  const bool directPairedIq = pairedQueueRequested && !arrayReference &&
    surveillanceChannels.size() == 1 &&
    ((referenceChannel == 0 && surveillanceChannels[0] == 1) ||
     (referenceChannel == 1 && surveillanceChannels[0] == 0));
  std::unique_ptr<Map<std::complex<double>>> map;
  std::unique_ptr<Detection> detection;
  std::unique_ptr<Track> track;

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
  for (std::size_t pathIndex = 0;
       pathIndex < surveillanceChannels.size(); pathIndex++)
    ambiguity.push_back(std::make_unique<Ambiguity>(delayMin, delayMax,
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
  const bool residentMode=accelerationMode=="gpu-resident";
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
  if(residentMode)throw std::invalid_argument("This cohort does not provide the resident ABI14 GPU route");
#endif

  blah2::Acceleration acceleration(
    (residentMode || multiIlluminatorEnabled) ? "cpu" : accelerationMode,
    {ambiguity.front()->get_nfft(), ambiguity.front()->get_n_doppler_bins(),
     ambiguity.front()->get_n_delay_bins(), uint32_t(ambiguity.size()), delayMin,
     isClutter ? nSamples : 0u,
     isClutter ? uint32_t(clutterBins) : 0u,
     delayMinClutter},
    ambiguity.front()->get_n_corr(), fs, ambiguity.front()->get_doppler_middle(),
    gpuDevice ? gpuDevice : "auto");
  std::vector<Ambiguity*> ambiguityPointers;
  std::vector<IqData*> surveillancePointers;
  for (size_t i = 0; i < ambiguity.size(); ++i) {
    ambiguityPointers.push_back(ambiguity[i].get());
    surveillancePointers.push_back(surveillanceData[i].get());
  }

  ArrayReferenceSynthesizer::Config referenceConfig;
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
    // Offline target-protected reference experiment.  A caller selects a
    // member of a fixed, complete azimuth bank; it never consumes ADS-B truth
    // at runtime.  The chosen direction is nulled in the reference before
    // ECA/CAF, while the measured eigenbeam remains the direct-path anchor.
    if (node.has_child("target_protection_beams") ||
        node.has_child("target_protection_beam_index"))
    {
      if (!state)
        throw std::invalid_argument("Target-protection reference is replay-only");
      uint32_t beams = 0, index = 0;
      if (node.has_child("target_protection_beams"))
        node["target_protection_beams"] >> beams;
      if (node.has_child("target_protection_beam_index"))
        node["target_protection_beam_index"] >> index;
      if (beams < 4 || beams > 360 || index >= beams ||
          coherentPositions.empty())
        throw std::invalid_argument("Invalid target-protection beam-bank configuration");
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
  blah2::production_replay::RawIqStages cpuStages({delayMinClutter,
    delayMaxClutter, nSamples, surveillanceChannels.size(), surveillanceWorkers,
    false,
    referenceConfig, isClutter && !multiIlluminatorEnabled});
  auto& referenceSynthesizer = cpuStages.reference();
  uint64_t reportedReferenceUpdate = 0;
  uint64_t replayProcessedFrames = 0;
  Noncoherent mapFusion;
  std::unique_ptr<CoherentBeamBank> coherentMapFusion;
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
    auto receiver=tree["location"]["rx"];
    receiver["latitude"]>>manifoldIdentity.receiverLocation[0];
    receiver["longitude"]>>manifoldIdentity.receiverLocation[1];
    receiver["altitude"]>>manifoldIdentity.receiverLocation[2];
    if (multiIlluminatorEnabled)
      manifoldIdentity.transmitterLocation = *illuminatorDefinitions.front().transmitterLocation;
    else {
      auto transmitter=tree["location"]["tx"];
      transmitter["latitude"]>>manifoldIdentity.transmitterLocation[0];
      transmitter["longitude"]>>manifoldIdentity.transmitterLocation[1];
      transmitter["altitude"]>>manifoldIdentity.transmitterLocation[2];
    }
    const bool measuredManifold = surveillanceManifoldConfigured || !bearingManifoldFile.empty() ||
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
      blah2::RecordingReader replayIdentity(replayFile, {replayFormat,type,
        uint32_t(captureChannelCount),fs,fc,legacyReplayBlockSamples,replayProvenanceMode});
      captureGains = replayIdentity.provenance().perChannelGainDb;
    }
    else if (device.has_child("gain") && device["gain"].is_seq())
      for (auto value : device["gain"].children()) { double gain = 0; value >> gain; captureGains.push_back(gain); }
    else if(heimdall.has_child("channel_gains"))
      for(auto value:heimdall["channel_gains"].children()){double gain=0;value>>gain;captureGains.push_back(gain);}
    else if(heimdall.has_child("gain")){
      std::string gainText;heimdall["gain"]>>gainText;size_t consumed=0;double gain=0;
      try{gain=std::stod(gainText,&consumed);}catch(...){consumed=0;}
      if(consumed!=gainText.size()||!std::isfinite(gain)||gain<0)
        throw std::invalid_argument("Measured manifold requires an explicit fixed requested gain");
      captureGains.assign(captureChannelCount,gain);
    }
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
        blah2::bearing_manifold::DetectionUse::liveProduction);
      surveillanceManifoldResponses = surveillanceManifoldCalibration->responses;
      surveillanceManifoldDirectResponse = surveillanceManifoldCalibration->directResponse;
    }
    const std::vector<std::complex<double>> surveillanceManifold(
      surveillanceManifoldResponses.begin(), surveillanceManifoldResponses.end());
    const std::vector<std::complex<double>> surveillanceDirect(
      surveillanceManifoldDirectResponse.begin(), surveillanceManifoldDirectResponse.end());
    coherentMapFusion = std::make_unique<CoherentBeamBank>(
      CoherentBeamBank::Config{double(fc), coherentBeamCount, coherentPositions,
        directPathAzimuthRad, directPathNullDb, surveillanceManifold,
        surveillanceDirect});
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
  double pfa, minDoppler;
  int8_t configuredNGuard, configuredNTrain;
  int32_t configuredMinDelay, minDelay;
  int8_t nGuard, nTrain;
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
  // Power-motion evidence does not require an antenna array or FP64 GPU.
  // Coherent bearing/refinement retains its own geometry/backend prerequisites.
  const bool residentHostProducts = residentMode && !coherentFusion && !residentSnapshotEnabled;
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
  if(bearingEnabled&&(bearingScanBeams<coherentBeamCount||bearingScanBeams>127||
      bearingScanBeams%coherentBeamCount))
    throw std::invalid_argument("Bearing scan_beams must be a beam-count multiple from the beam count through 127");
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
  const double detectorPfa = coherentFusion
    ? pfa / (effectiveBeamTrials * (trackBeforeDetect ? 1.0 : motionTrials))
    : pfa / (trackBeforeDetect ? 1.0 : motionTrials);
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
  uint16_t configuredNCentroid, nCentroid;
  tree["process"]["detection"]["nCentroid"] >> configuredNCentroid;
  auto captureDetectionProjection = captureDelayProjection;
  captureDetectionProjection.minimumDelay = configuredMinDelay;
  captureDetectionProjection.guardCells = configuredNGuard;
  captureDetectionProjection.trainingCells = configuredNTrain;
  captureDetectionProjection.centroidRadius = configuredNCentroid;
  captureDetectionProjection.centroidDopplerRadius = configuredNCentroid;
  const auto effectiveCaptureDetection =
    blah2::multi_illuminator::scaleRadarConfigForLane(
      captureDetectionProjection, fs);
  minDelay = effectiveCaptureDetection.minimumDelay;
  nGuard = effectiveCaptureDetection.guardCells;
  nTrain = effectiveCaptureDetection.trainingCells;
  nCentroid = effectiveCaptureDetection.centroidRadius;
  CfarDetector1D *cfarDetector1D = new CfarDetector1D(detectorPfa, nGuard,
    nTrain, minDelay, minDoppler, residentMode && !residentHostProducts ? 1u : integrationCpis);
  std::unique_ptr<blah2::detection::DetectorIntegration> detectorIntegration;
  if ((!residentMode || residentHostProducts) && !multiIlluminatorEnabled &&
      (integrationCpis == 2 || trackBeforeDetect))
    detectorIntegration = std::make_unique<blah2::detection::DetectorIntegration>(
      trackBeforeDetect ? trackBeforeDetectCpis : integrationCpis,
      trackBeforeDetect ? track_before_detect::encodeMotionBins(
        trackBeforeDetectDelayMotionBins, trackBeforeDetectDopplerMotionBins,
        trackBeforeDetectFractionalDelay) : integrationMotionBins,
      track_before_detect::delayDriftPerHz(nSamples, fc));
  Interpolate *interpolate = new Interpolate(true, true);
  const uint16_t nCentroidDoppler =
    *effectiveCaptureDetection.centroidDopplerRadius;
  Centroid *centroid = new Centroid(nCentroid, nCentroidDoppler,
    1 / ambiguity.front()->get_cpi());

  // Parse processing admission flags before tracker defaults or validation use
  // them.  The full executable (unlike focused component fixtures) depends on
  // this ordering.

  // set up process tracker
  uint8_t m, n, nDelete;
  double maxAcc, rangeRes, lambda;
  std::string smooth,trackerAccelerationModel,trackerProfile="balanced";
  double trackerMinInitiationSnrDb=5.0;
  bool stationaryClutter=false;
  uint32_t stationaryPersistenceCpis=stationaryClutter?60u:0u;
  // Zero means the full configured ambiguity Doppler span. A positive legacy
  // value remains an optional safety bound for installations that need one.
  double stationaryMaxDopplerHz=0.0;
  bool trackGuidedShadow=false;
  bool trackGuidedPromote=false;
  bool trackGuidedSustain=false;
  uint32_t trackGuidedShadowMaxTracks=0;
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
  if(tree["process"]["tracker"].has_child("track_guided_shadow")){
    const auto& guided=tree["process"]["tracker"]["track_guided_shadow"];
    guided["enable"] >> trackGuidedShadow;
    if(trackGuidedShadow)guided["max_tracks"] >> trackGuidedShadowMaxTracks;
    if(guided.has_child("promote"))guided["promote"] >> trackGuidedPromote;
    if(guided.has_child("sustain"))guided["sustain"] >> trackGuidedSustain;
  }
  if(stationaryClutter && !isTracker)
    throw std::invalid_argument("Stationary clutter requires enabled tracking");
  if(trackGuidedShadow && (!isTracker || !trackBeforeDetect))
    throw std::invalid_argument("Track-guided shadow requires enabled tracking and track-before-detect");
  if(trackGuidedPromote && !trackGuidedShadow)
    throw std::invalid_argument("Track-guided promotion requires enabled track-guided shadow");
  if(trackGuidedSustain && !trackGuidedShadow)
    throw std::invalid_argument("Track-guided sustain requires enabled track-guided shadow");
  if(trackerAccelerationModel!="hypothesis_bank"&&trackerAccelerationModel!="adaptive_gate")
    throw std::invalid_argument("Tracker acceleration_model must be hypothesis_bank or adaptive_gate");
  rangeRes = (double)Constants::c/fs;
  lambda = (double)Constants::c/fc;
  Tracker *tracker = new Tracker(m, n, nDelete,
    ambiguity.front()->get_cpi(), maxAcc, rangeRes, lambda,
    trackerMinInitiationSnrDb, trackerAccelerationModel == "adaptive_gate",
    dopplerRidge);

  // The challenger reuses the production detector geometry and tracker
  // kinematics, but owns independent state and never publishes detections or
  // tracks. Its temporal banks are evaluated only on disjoint windows.
  if (liveShadowConfig.scoring.enabled) {
    liveShadowConfig.scoring.pfa = detectorPfa;
    liveShadowConfig.scoring.guardCells = nGuard;
    liveShadowConfig.scoring.trainingCells = nTrain;
    liveShadowConfig.scoring.minimumDelay = minDelay;
    liveShadowConfig.scoring.minimumDopplerHz = minDoppler;
    liveShadowConfig.scoring.confirmationM = m;
    liveShadowConfig.scoring.confirmationN = n;
    liveShadowConfig.scoring.deleteAfter = nDelete;
    liveShadowConfig.scoring.cpiSeconds = ambiguity.front()->get_cpi();
    liveShadowConfig.scoring.maximumAccelerationHzPerSecond = maxAcc;
    liveShadowConfig.scoring.rangeResolutionM = rangeRes;
    liveShadowConfig.scoring.wavelengthM = lambda;
    if (!liveShadowConfig.scoring.minimumInitiationSnrOverridden)
      liveShadowConfig.scoring.minimumInitiationSnrDb =
        trackerMinInitiationSnrDb;
    liveShadowConfig.scoring.adaptiveGate =
      trackerAccelerationModel == "adaptive_gate";
    blah2::integration::LiveTemporalShadow::validate(liveShadowConfig);
  }

  // set up process spectrum analyser
  double spectrumBandwidth = 2000;
  SpectrumAnalyser *spectrumAnalyser = new SpectrumAnalyser(nSamples,
    spectrumBandwidth, fc, fs);

  // process options
  if(causalEnabled&&(!residentMode||multiIlluminatorEnabled||!isTracker))
    throw std::invalid_argument("Native causal diagnostics currently require the single-lane GPU-resident tracker");
  if(manifoldRecorderEnabled&&!isDetection)
    throw std::invalid_argument("Manifold recording requires GPU-resident coherent detection");
  if(bearingEnabled&&!isTracker)
    throw std::invalid_argument("Bearing output requires tracking to be enabled");
  if (integrationBeamTolerance != UINT32_MAX && !isTracker)
    throw std::invalid_argument("Tracker beam_tolerance requires tracking to be enabled");
  if (!isDetection)
  {
    isTracker = false;
  }

  const blah2::radar_contract::Algorithm radarAlgorithm{
    ambiguity.front()->get_cpi(), nGuard, nTrain, minDelay, minDoppler,
    detectorPfa, nCentroid, nCentroidDoppler,
    trackBeforeDetect ? trackBeforeDetectCpis : integrationCpis,
    trackBeforeDetect ? track_before_detect::encodeMotionBins(
      trackBeforeDetectDelayMotionBins, trackBeforeDetectDopplerMotionBins,
      trackBeforeDetectFractionalDelay) : integrationMotionBins,
    integrationBeamTolerance, trackBeforeDetect, m, n, nDelete, maxAcc, rangeRes, lambda,
    trackerMinInitiationSnrDb,
    trackerAccelerationModel == "adaptive_gate", stationaryClutter,
    trackGuidedShadow, trackGuidedPromote, trackGuidedSustain, true,
    stationaryPersistenceCpis, trackGuidedShadowMaxTracks,
    stationaryMaxDopplerHz};
  blah2::radar_contract::validate(radarAlgorithm);
  tracker->configureRecovery(blah2::radar_contract::hostTrackerRecovery(radarAlgorithm));

  // Resident mode consumes canonical capture order. Array inputs retain the
  // reviewed identity route; paired receivers keep explicit, disjoint
  // dedicated-reference and surveillance mappings.
  blah2::resident::Config residentConfig;
  std::unique_ptr<blah2::resident::Coordinator> resident;
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
      residentConfig.detector = blah2::radar_contract::residentDetector(
        radarAlgorithm, isDetection);
    if(isTracker && !residentHostProducts){
      uint32_t capacity=0;
      if(!tree["process"]["tracker"].has_child("gpu_capacity"))throw std::invalid_argument("Resident tracker requires an explicit gpu_capacity; capacity is never inferred or shrunk");
      tree["process"]["tracker"]["gpu_capacity"] >> capacity;
      residentConfig.tracker = blah2::radar_contract::residentTracker(
        radarAlgorithm, true, capacity);
    }
    if(residentSnapshotEnabled){
      if(!state)throw std::invalid_argument("Resident snapshots are restricted to offline replay");
      residentSnapshotOutput=residentSnapshotDirectory;
      if(!residentSnapshotOutput.is_absolute()||!std::filesystem::is_directory(residentSnapshotOutput))
        throw std::invalid_argument("Resident snapshot output_directory must be an existing absolute directory");
      // Bind the snapshot to gains carried by the recording, never to the
      // current receiver settings. The two may legitimately differ.
      blah2::RecordingReader replayIdentity(replayFile,{replayFormat,type,
        uint32_t(captureChannelCount),fs,fc,legacyReplayBlockSamples,replayProvenanceMode});
      const auto& allGains=replayIdentity.provenance().perChannelGainDb;
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
    if (!multiIlluminatorEnabled)
      resident=std::make_unique<blah2::resident::Coordinator>(residentConfig);
    if(causalEnabled){socket_causal=std::make_unique<blah2::BestEffortSocket>(ip,portCausal);
      const auto fingerprint=blah2::resident::wire::configHash(residentConfig);
      causalConfigHash=blah2::causal_trace::hex(fingerprint.data(),fingerprint.size());}
  }

  std::unique_ptr<blah2::integration::LiveTemporalShadow> liveTemporalShadow;
  uint64_t liveShadowSequence = 0;
  if (liveShadowConfig.enabled && !multiIlluminatorEnabled) {
    if (!isClutter) throw std::invalid_argument("Live temporal shadow requires post-clutter CAFs");
    liveTemporalShadow = std::make_unique<blah2::integration::LiveTemporalShadow>(
      liveShadowConfig, *ambiguity.front()->result(), surveillanceChannels);
  }

  std::unique_ptr<blah2::channelizer::IlluminatorChannelizerBank>
    multiChannelizer;
  std::unique_ptr<blah2::multi_illuminator::MultiIlluminatorRadar> multiRadar;
  std::unique_ptr<blah2::utility::FixedExecutor> multiSerializationExecutor;
  std::unique_ptr<blah2::utility::FixedExecutor> multiGpuSubmissionExecutor;
  std::unique_ptr<blah2::wideband_gpu::Backend> multiGpuChannelizer;
  std::unique_ptr<blah2::wideband_gpu::SelectedOutputQualifier>
    multiGpuQualifier;
  std::vector<std::vector<std::complex<float>>> multiGpuStagingInput;
  std::vector<double> multiGroupDelays;
  bool widebandGpuRequested = false;
  bool multiGpuQualified = false;
  std::optional<std::string> multiQualificationFatal;
  uint64_t multiFallbackInputSample = 0;
  blah2::multi_illuminator::PendingPublication<std::vector<
    blah2::channelizer::IlluminatorOutput>> multiPendingPublication;
  bool multiPipelineDelayed = false;
  blah2::multi_illuminator::PipelineState multiPipelineState;
  if (multiIlluminatorEnabled)
  {
    if (!isClutter || !isDetection || !isTracker)
      throw std::invalid_argument(
        "Multi-illuminator mode requires clutter, detection, and tracking");
    blah2::multi_illuminator::RadarConfig multiConfig;
    multiConfig.cpiSeconds = tCpi;
    multiConfig.delayMin = configuredDelayMin;
    multiConfig.delayMax = configuredDelayMax;
    multiConfig.delayReferenceRateHz = delayReferenceRateHz;
    multiConfig.dopplerMin = dopplerMin;
    multiConfig.dopplerMax = dopplerMax;
    multiConfig.clutterDelayMin = configuredDelayMinClutter;
    multiConfig.clutterDelayMax = configuredDelayMaxClutter;
    multiConfig.clutterFollowsAmbiguity = followAmbiguity;
    multiConfig.pfa = pfa;
    multiConfig.guardCells = configuredNGuard;
    multiConfig.trainingCells = configuredNTrain;
    multiConfig.minimumDelay = configuredMinDelay;
    multiConfig.minimumDopplerHz = minDoppler;
    multiConfig.centroidRadius = configuredNCentroid;
    multiConfig.trackerM = m;
    multiConfig.trackerN = n;
    multiConfig.trackerDelete = nDelete;
    multiConfig.trackerMaximumAcceleration = maxAcc;
    multiConfig.trackerMinimumSnrDb = trackerMinInitiationSnrDb;
    multiConfig.trackerMinimumSnrDbByIlluminator = illuminatorTrackerMinimumSnrDb;
    multiConfig.liveTemporalShadow = liveShadowConfig;
    multiConfig.liveTemporalShadowIlluminatorId = liveShadowIlluminatorId;
    multiConfig.trackerDopplerRidge = dopplerRidge;
    multiConfig.dopplerRidgeScrubber = dopplerRidgeScrubber;
    multiConfig.workers = surveillanceWorkers;
    multiConfig.laneWorkers = multiIlluminatorWorkers;
    multiConfig.accelerationMode = accelerationMode;
    multiConfig.accelerationDevice = residentMode ? residentDevice :
      gpuDevice ? gpuDevice : "auto";
    multiConfig.arrayReference = arrayReference;
    multiConfig.referenceChannel = referenceChannel;
    multiConfig.referenceChannels = referenceChannels;
    multiConfig.surveillanceChannels = surveillanceChannels;
    multiConfig.reference = referenceConfig;
    if (coherentFusion) {
      multiConfig.coherentFusion = CoherentBeamBank::Config{double(fc), coherentBeamCount,
        coherentPositions, directPathAzimuthRad, directPathNullDb, {}, {}};
      multiConfig.manifoldIdentity = manifoldIdentity;
      multiConfig.detectionManifoldFile = surveillanceManifoldFile;
      multiConfig.bearingManifoldFile = bearingManifoldFile;
      if (residentMode) multiConfig.residentProductTemplate = residentConfig;
      multiConfig.bearingPublication = {bearingEnabled, coherentBeamCount,
        bearingScanBeams/coherentBeamCount, bearingMinConsistent, xAxisBearingDeg,
        bearingUncertaintyFloorDeg, bearingMinPeakSeparationDb,
        bearingMethod=="covariance_mvdr"?1u:0u, bearingScanBeams,
        ambiguity.front()->get_cpi(), bearingMaxRateDegPerSecond,
        stationaryClutter?stationaryPersistenceCpis:0u,
        stationaryClutter?stationaryMaxDopplerHz:0.0};
    }
    multiConfig.advanced = multiAdvanced;
    blah2::radar_contract::applyReceiverIndependent(radarAlgorithm, multiConfig);
    multiConfig.experimentalFmReferenceQualityFilter = experimentalFmReferenceQualityFilter;
    multiConfig.offlineReplay = state;
    // The resolved model includes profile defaults and any explicit initiate
    // override; do not replace it with AdvancedFeatureConfig's default.
    multiConfig.advanced.trackerAccelerationModel = trackerAccelerationModel;
    widebandGpuRequested=accelerationMode=="gpu"||
      accelerationMode=="gpu-resident"||accelerationMode=="auto";
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
        multiGpuQualifier = std::make_unique<
          blah2::wideband_gpu::SelectedOutputQualifier>(gpuConfig);
        multiGpuChannelizer=blah2::wideband_gpu::createProcess(gpuConfig,
          gpuDevice?gpuDevice:"auto");
        multiGpuStagingInput.assign(captureChannelCount,
          std::vector<std::complex<float>>(nSamples));
        std::cout<<"[MultiIlluminator] channelizer=vulkan-worker device="
          <<multiGpuChannelizer->deviceName()<<"\n";
      } catch(const std::exception& error) {
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
    multiChannelizer = std::make_unique<
      blah2::channelizer::IlluminatorChannelizerBank>(fc, fs,
        captureChannelCount, illuminatorDefinitions);
    multiRadar = std::make_unique<
      blah2::multi_illuminator::MultiIlluminatorRadar>(fs,
        captureChannelCount, illuminatorDefinitions, multiConfig);
    multiSerializationExecutor = std::make_unique<blah2::utility::FixedExecutor>(
      std::min(illuminatorDefinitions.size(),
        std::max<std::size_t>(1, threadPlan.availableCpus - 1)));
    multiGpuSubmissionExecutor =
      std::make_unique<blah2::utility::FixedExecutor>(
        std::min(captureChannelCount,
          std::max<std::size_t>(1, threadPlan.availableCpus - 1)));
    std::cout << "[MultiIlluminator] enabled=1 inputs="
      << illuminatorDefinitions.size() << " capture_rate_hz=" << fs << "\n";
  }

  const bool nativeFloatGpuCapture = !state && multiGpuChannelizer &&
    !echoInjector.enabled();
  if (nativeFloatGpuCapture)
    for (auto* buffer : captureBuffers) buffer->enable_float_capture(nSamples, 2);

  // set up output data
  bool saveMap, saveDetection;
  tree["save"]["map"] >> saveMap;
  tree["save"]["detection"] >> saveDetection;
  std::string savePath, saveMapPath, saveDetectionPath;
  if (saveIq || saveMap || saveDetection)
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
  }
  if (residentMode && complexCutoutConfig.enabled)
    throw std::invalid_argument(
      "Complex cutout recording is not supported by the GPU-resident pathway");
  blah2::analysis::ComplexCutoutRecorder complexCutout(complexCutoutConfig, fs, state);
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
        view.maps, view.referenceWeights, std::nullopt, &identity);
    });
  const auto processMultiFrames = [&](std::vector<blah2::channelizer::IlluminatorOutput> input,
      const blah2::multi_illuminator::PublicationContext& publication) {
    nativeCutoutPublication = &publication;
    try {
      auto frames = multiRadar->process(std::move(input),
        blah2::multi_illuminator::LiveTemporalIdentity{publication.timestampMs,
          publication.captureEpoch, publication.samplePositionAvailable, publication.inputGainDb});
      nativeCutoutPublication = nullptr;
      return frames;
    } catch (...) {
      nativeCutoutPublication = nullptr;
      throw;
    }
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
        frame.residentProducts ? multi_map_json(frame, publication.timestampMs) :
          frame.map->to_json_km(publication.timestampMs, frame.sampleRateHz)), saveMapPath))
      throw std::runtime_error("Cannot save multi-illuminator native map output");
    if (saveDetection && !frame.detections->save(annotate(
        multi_detection_json(frame, publication.timestampMs)), saveDetectionPath))
      throw std::runtime_error("Cannot save multi-illuminator detection output");
  };

  // Finish first-use CPU/GPU qualification before any backend admits IQ.
  // Private synthetic products never enter capture or output. Resident warmup
  // exercises tracking too, then discards all learned signal/temporal state.
  std::cout << "[Startup] preparing DSP before capture source=synthetic publish=0\n";
  if (multiRadar)
    multiRadar->prepareStartup();
  else if (resident && !state)
  {
    // Offline replay keeps its real-IQ qualification and snapshot identities.
    // Live capture must not start while the eight full CPU oracles run. Keep
    // worker frame/token IDs monotonic across reset; these private transactions
    // do not advance capture samples, Timing::nCpi, observers or output sinks.
    std::vector<IqData*> startupInput, startupReference;
    for (auto& channel : captureData) startupInput.push_back(channel.get());
    if (arrayReference)
      for (uint32_t channel : referenceChannels)
        startupReference.push_back(captureData[channel].get());
    auto startupFusion = coherentMapFusion ?
      std::make_unique<CoherentBeamBank>(*coherentMapFusion) : nullptr;
    std::unique_ptr<Tracker> startupTracker;
    if (isTracker)
      startupTracker = std::make_unique<Tracker>(m, n, nDelete,
        ambiguity.front()->get_cpi(), maxAcc, rangeRes, lambda,
        trackerMinInitiationSnrDb, trackerAccelerationModel == "adaptive_gate",
        dopplerRidge);
    resident->prepareStartup([&](unsigned frame) {
      if (STOP_SIGNAL) throw std::runtime_error("Startup interrupted before capture");
      blah2::startup::fillIq(startupInput, nSamples, frame);
      const uint64_t timestamp = 1000 + (uint64_t(frame) * nSamples * 1000) / fs;
      resident->begin(captureData, {++residentLogical, ++residentConsumed, timestamp});
      ArrayReferenceSynthesizer::Metrics dedicatedReferenceMetrics;
      if (arrayReference)
        referenceSynthesizer.process_into(startupReference, *referenceData);
      else
        referenceData->replace(
          captureData[referenceChannel]->drain_front(nSamples));
      const auto& metrics = arrayReference ?
        referenceSynthesizer.get_metrics() : dedicatedReferenceMetrics;
      resident->checkReference(*referenceData, metrics);
      if (startupFusion) startupFusion->calibrate(metrics.directPathWeights);
      for (size_t path = 0; path < surveillanceChannels.size(); ++path)
        surveillanceData[path]->replace(
          captureData[surveillanceChannels[path]]->drain_front(nSamples));
      if (!cpuStages.clutter(*referenceData, surveillancePointers))
        throw std::runtime_error("Resident startup CPU clutter rejected IQ");
      resident->checkFiltered(surveillancePointers);
      cpuStages.ambiguity(*referenceData, surveillancePointers, ambiguityPointers, false);
      resident->checkMaps(ambiguityPointers);
      std::vector<Map<std::complex<double>>*> channelMaps;
      for (auto* channel : ambiguityPointers) channelMaps.push_back(channel->result());
      auto startupMap = startupFusion ? startupFusion->process(channelMaps) :
        mapFusion.process(channelMaps);
      startupMap->set_metrics();
      std::unique_ptr<Detection> startupDetections;
      if (isDetection) {
        auto candidates = cfarDetector1D->process(startupMap.get());
        auto clusters = centroid->process(candidates.get());
        startupDetections = interpolate->process(clusters.get(), startupMap.get());
      }
      std::unique_ptr<Track> startupTracks;
      if (startupTracker)
        startupTracks = startupTracker->process(startupDetections.get(), timestamp);
      const auto products = resident->format(timestamp);
      resident->checkProducts(products, startupMap->to_json_km(timestamp, fs),
        startupDetections ? startupDetections->to_json_km(timestamp, fs) : std::string{},
        startupTracks ? startupTracks->to_json(timestamp) : std::string{});
      resident->commit();
    }, [&] {
      // The coordinator has already acknowledged the device reference,
      // integration/TBD and tracker reset. Restore the CPU reference/clutter
      // state too; private CPU fusion/tracker objects die before admission.
      cpuStages.resetReference();
      reportedReferenceUpdate = 0;
      referenceData->clear();
      for (auto* channel : startupInput) channel->clear();
      for (auto* channel : surveillancePointers) channel->clear();
    });
    if (resident->qualifying())
      throw std::logic_error("Resident startup qualification incomplete before capture");
    std::cout << "[Startup] resident qualification complete checks="
      << resident->qualificationChecks()
      << " source=synthetic publish=0 adaptive_state_reset=1\n";
  }
  else if (!resident)
  {
    std::vector<IqData*> startupInput{referenceData.get()};
    startupInput.insert(startupInput.end(), surveillancePointers.begin(),
      surveillancePointers.end());
    acceleration.prepareStartup([&](unsigned frame) {
      if (STOP_SIGNAL) throw std::runtime_error("Startup interrupted before capture");
      blah2::startup::fillIq(startupInput, nSamples, frame);
      if (isClutter && !acceleration.processClutter(*referenceData,
          surveillancePointers, [&] {
            return cpuStages.clutter(*referenceData, surveillancePointers);
          }))
        throw std::runtime_error("Startup clutter qualification rejected IQ");
      acceleration.process(referenceData->view_data(), surveillancePointers,
        ambiguityPointers, [&] {
          cpuStages.ambiguity(*referenceData, surveillancePointers,
            ambiguityPointers, pairedQueueRequested);
        });
    });
    for (auto* input : startupInput) input->clear();
  }
  if (STOP_SIGNAL) return 0;
  std::cout << "[Startup] DSP ready for capture\n";

  // set up output timing
  uint64_t tStart = current_time_ms();
  Timing *timing = new Timing(tStart);
  timing->set_capture_storage_contract(pairedQueueRequested ? 4u :
    (nativeFloatGpuCapture ? 8u : 16u), captureBufferSamples);
  std::vector<std::string> timing_name;
  std::vector<double> timing_time;
  std::string jsonTiming;
  std::vector<uint64_t> time;

  // set up output json
  std::string mapJson, detectionJson, jsonTracker, jsonIqData;

  // run process.  Keep capture dormant until all later configuration/DSP setup
  // has succeeded, otherwise a startup exception would strand a joinable thread.
  std::thread t1;
  std::thread t2([&]{
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
        const bool pairedDirectFrame = usePairedQueue && directPairedIq &&
          !residentMode;
        // ReplayPlayer owns EOF/loop draining by observing the producer FIFOs.
        // Never move replay tails into the live-only consumer queue where that
        // ownership protocol cannot see them.
        const bool chunkLiveCapture = !state && !usePairedQueue;
        const bool directFloatCapture = chunkLiveCapture && multiIlluminatorEnabled &&
          bool(multiGpuChannelizer) && !echoInjector.enabled();
        const auto residentWholeStart=std::chrono::steady_clock::now();
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
              if (pairedDirectFrame) {
                if (referenceChannel == 0)
                  referenceData->assign_paired_i16(block.iq.data(), nSamples,
                    *surveillanceData[0]);
                else
                  surveillanceData[0]->assign_paired_i16(block.iq.data(), nSamples,
                    *referenceData);
              } else {
                captureData[0]->assign_paired_i16(block.iq.data(), nSamples,
                  *captureData[1]);
              }
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
            if (liveTemporalShadow) liveTemporalShadow->invalidate();
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
          if (extractedInputGeneration != inputGeneration)
          {
            inputGeneration = extractedInputGeneration;
            cpuStages.resetReference();
            if (detectorIntegration) detectorIntegration->reset();
            reportedReferenceUpdate = 0;
            replayProcessedFrames = 0;
            if(liveTemporalShadow)liveTemporalShadow->invalidate();
            if(resident){
              // A capture gap clears signal history, not device qualification or
              // immutable geometry. Rebuilding here turns slow startup oracles
              // into an overflow/rebuild loop. Keep frame/token IDs advancing.
              resident->resetInput();
              if(socket_causal)try{std::array<uint32_t,blah2::causal_trace::words> trace{};
                trace[0]=blah2::causal_trace::schema;
                const uint64_t values[]={residentLogical,residentConsumed,frameTimestampMs,residentConfig.epoch};
                std::memcpy(trace.data()+4,values,sizeof(values));
                socket_causal->sendData(blah2::causal_trace::json(trace.data(),trace.size(),rangeRes/1000,
                  causalSequence++,causalMaximum,{captureSamplePositionAvailable,captureEpoch,
                    captureFirstEpochSample,captureEndEpochSample},true,socket_causal->dropped()+causalFormatDrops,
                    causalConfigHash,causalModuleHash));
              }catch(...){++causalFormatDrops;}
              std::cout<<"GPU_RESIDENT_INPUT_RESET capture_generation="<<inputGeneration
                <<" frame="<<residentLogical<<" consumed="<<residentConsumed
                <<" qualification_checks="<<resident->qualificationChecks()
                <<" worker_retained=1 capture_drop_counters_retained=1\n";
            }
            tracker->reset();
            if (multiChannelizer) multiChannelizer->reset();
            if (multiGpuChannelizer) multiGpuChannelizer->reset();
            if (multiGpuQualifier) multiGpuQualifier->reset();
            if (multiRadar) multiRadar->reset();
            multiPendingPublication.reset();
            multiPipelineDelayed = !state && multiGpuChannelizer &&
              multiGpuChannelizer->supportsSplitFrame();
            multiPipelineState.input_epoch_reset();
            // A capture gap invalidates FIR/radar history, not the already
            // completed device/geometry qualification. Runtime GPU errors
            // still clear the backend and fail over.
            multiQualificationFatal.reset();
            multiFallbackInputSample = 0;
            multiSpectrumFrame = 0;
            multiMapFrame = 0;
          }
          // Advance signal time for every accepted input CPI, even if clutter
          // later rejects it. UI publication timestamps remain wall-clock.
          const uint64_t trackerFrame = replayProcessedFrames;
          const uint64_t frameSignalTimeMs = [&] {
            if (captureSamplePositionAvailable)
              return blah2::detection::signalTimeMilliseconds(captureEndEpochSample, fs);
            if (!state) return frameTimestampMs;
            // Legacy unpositioned replay retains deterministic nominal cadence,
            // but remains ineligible for lineage-qualified integration.
            if (!nSamples || trackerFrame >= UINT64_MAX / uint64_t(nSamples))
              throw std::overflow_error("Replay nominal sample time exhausted");
            return blah2::detection::signalTimeMilliseconds(
              (trackerFrame + 1) * uint64_t(nSamples), fs);
          }();
          ReplayFrameGuard replayFrame(state, replayProcessedFrames);
          std::vector<IqData*> physicalChannels;
          physicalChannels.reserve(captureData.size());
          for (auto& channel : captureData) physicalChannels.push_back(channel.get());
          echoInjector.apply(trackerFrame * uint64_t(nSamples), physicalChannels);
          if (multiIlluminatorEnabled)
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
            const bool publishFullMap = !recoveringBacklog && fullMapCadence;
            // Multi-lane derived products are narrowband, but the spectrum is
            // intentionally the complete captured passband. Publish it from
            // one physical channel without altering the channelizer input.
            // Spectrum publication is intentionally slower than radar CPI
            // processing because it is a UI-only product.
            if (publishSpectrum) {
              if (directFloatCapture)
              {
                auto& spectrumInput = captureData[referenceChannel]->
                  resize_for_write(nSamples);
                auto destination = spectrumInput.begin();
                for (const auto sample : multiGpuStagingInput[referenceChannel])
                  *destination++ = std::complex<double>(sample);
              }
              spectrumAnalyser->process(captureData[referenceChannel].get());
              socket_iqdata->sendData(attachEventTime(
                captureData[referenceChannel]->to_json(frameTimestampMs)));
            }
            std::vector<const std::deque<std::complex<double>>*>
              channelizerInput;
            channelizerInput.reserve(captureData.size());
            for (const auto& channel : captureData)
              channelizerInput.push_back(&channel->view_data());
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
            multiFallbackInputSample = firstInputSample + nSamples;
            std::vector<blah2::channelizer::IlluminatorOutput> channelized;
            const auto convertGpu = [&](std::vector<blah2::wideband_gpu::LaneOutput> gpu) {
              std::vector<blah2::channelizer::IlluminatorOutput> converted;
              converted.reserve(gpu.size());
              for(size_t index=0;index<gpu.size();++index){blah2::channelizer::SubbandChannelizer::Output samples;samples.channels=std::move(gpu[index].channels);samples.firstInputSample=gpu[index].firstInputSample;samples.hasFirstInputSample=gpu[index].hasFirstInputSample;samples.inputSamplesPerOutput=gpu[index].inputSamplesPerOutput;samples.groupDelayInputSamples=multiGroupDelays[index];converted.push_back({illuminatorDefinitions[index].id,gpu[index].frequencyHz,std::move(samples)});}
              return converted;
            };
            using PipelineClock = std::chrono::steady_clock;
            const auto pipelineStart = PipelineClock::now();
            const double capturePrepareMs = std::chrono::duration<double,
              std::milli>(pipelineStart - residentWholeStart).count();
            const auto channelizerStart = pipelineStart;
            bool splitBegan=false,splitScheduled=false,splitFinished=false,
              haveChannelized=false,publicationIsFatal=false;
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
              return publication;
            };
            const auto runtimeFallback = [&](const char* stage,
                const std::exception& error) {
              const std::string reason = std::string("GPU channelizer ") +
                stage + " failed: " + error.what();
              std::cerr << "[MultiIlluminator] " << reason <<
                "; CPU same-block reset/fallback\n";
              multiGpuChannelizer.reset();
              multiGpuQualifier.reset();
              multiGpuQualified = false;
              multiPipelineState.runtime_fallback(reason,
                [&] { multiChannelizer->reset(); },
                [&] { multiRadar->reset(); });
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
                return true;
              }
              // Silence is checked exhaustively (zero input/history/output),
              // so it can be published without declaring the device qualified.
              if (result.verifiedSilence) return true;
              const std::string reason = result.reason;
              multiGpuChannelizer.reset();
              multiGpuQualifier.reset();
              multiGpuQualified = false;
              // Unlike the old full-block oracle, sampling never advances the
              // CPU bank. Fail closed at an explicit epoch boundary and then
              // recompute this complete input block using the original CPU DSP.
              multiPipelineState.runtime_fallback(reason,
                [&] { multiChannelizer->reset(); },
                [&] { multiRadar->reset(); });
              if (accelerationMode != "auto")
                multiQualificationFatal = reason;
              return false;
            };
            if(multiPendingPublication.has_value()){
              auto pending=multiPendingPublication.take();
              channelized=std::move(pending.first);
              publication=std::move(pending.second);
              haveChannelized=true;
              publicationIsFatal=multiQualificationFatal.has_value();
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
            if(publicationIsFatal){
              // Explicit GPU qualification publishes its CPU fallback exactly
              // once, then fails without consuming another capture on CPU.
            }else if(!state&&multiGpuChannelizer&&multiGpuChannelizer->supportsSplitFrame()){
              multiPipelineDelayed=true;
              // Capture owns these deques and may recycle them as soon as the
              // processing thread advances. Stage them into the isolated GPU
              // shared buffer before doing prior-frame radar work; only the
              // GPU execution remains in flight. Background staging borrowed
              // unstable deques and could stall or corrupt a live frame.
              splitBegan=blah2::wideband_gpu::attemptWithAutoFallback(
                accelerationMode=="auto",
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
              const bool gpuSucceeded=blah2::wideband_gpu::attemptWithAutoFallback(
                accelerationMode=="auto",
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
              auto current=multiChannelizer->process(channelizerInput,firstInputSample);
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
              if(!began)began=blah2::wideband_gpu::attemptWithAutoFallback(
                accelerationMode=="auto",[&]{splitSubmission.get();splitBegan=true;},
                [&](const std::exception& error){runtimeFallback("begin",error);});
              bool success=false;
              if(began)success=blah2::wideband_gpu::attemptWithAutoFallback(
                accelerationMode=="auto",[&]{gpu=multiGpuChannelizer->finishFrame();},
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
              }else next=convertGpu(std::move(gpu));
              const double channelizerMs=std::chrono::duration<double,std::milli>(
                PipelineClock::now()-channelizerStart).count();
              multiPendingPublication.store(std::move(next),currentPublication(
                channelizerMs,qualificationFrame,gpuExecuted,cpuExecuted));
            };
            if(!haveChannelized){finishCurrent();
              // Explicit qualification failure publishes its just-computed
              // CPU fallback from this capture before throwing. Do not wait for
              // or extract a newer CPI merely to drive the delayed pipeline.
              if(multiQualificationFatal&&multiPendingPublication.has_value()){
                auto failed=multiPendingPublication.take();
                channelized=std::move(failed.first);
                publication=std::move(failed.second);
                haveChannelized=true;
              }else{timing_time.clear();timing_name.clear();time.clear();
                std::fill(consumerClaimedSamples.begin(),
                  consumerClaimedSamples.end(), 0);continue;}}
            // Until the first representative block qualifies, complete and
            // measure GPU work before either the direct oracle or prior-frame
            // radar can contend with it. Steady-state overlap starts only
            // after qualification passes.
            if(splitScheduled&&!multiGpuQualified)finishCurrent();
            if(!publication)
              throw std::logic_error("Channelized frame lacks publication metadata");
            const auto radarStart=PipelineClock::now();
            auto frames = processMultiFrames(std::move(channelized), *publication);
            const auto laneHealth = multiRadar->laneHealth();
            const auto radarEnd=PipelineClock::now();
            const auto attachPublicationTime = [&](std::string json) {
              return blah2::annotate_event_time(std::move(json),
                publication->eventTime);
            };
            struct SerializedIlluminatorFrame
            {
              std::string map, detections, tracks;
            };
            std::vector<SerializedIlluminatorFrame> serialized(frames.size());
            multiSerializationExecutor->parallel_for(frames.size(),
              [&](std::size_t index) {
                const auto& frame = frames[index];
                const auto identity = native_product_identity(frame, *publication);
                const auto annotate = [&](std::string json) {
                  return blah2::detection::annotateIntegration(
                    blah2::analysis::annotate_native_product(std::move(json), identity),
                    frame.detectorIntegration);
                };
                // This JSON is a monitor product only. Keep the complete
                // full-precision map in the radar/backend path and bound the
                // display raster so serialization and socket backpressure
                // cannot consume the real-time acquisition margin.
                serialized[index].map = annotate(
                  multi_map_json(frame, publication->timestampMs, publication->publishFullMap));
                serialized[index].detections = annotate(
                  multi_detection_json(frame, publication->timestampMs));
                serialized[index].tracks = annotate(
                  multi_track_json(frame, publication->timestampMs));
              });
            const auto serializationEnd=PipelineClock::now();
            for (std::size_t index = 0; index < frames.size(); ++index)
            {
              const auto& frame = frames[index];
              const auto& product = serialized[index];
              saveMultiProducts(frame, *publication);
              socket_illuminator->sendData(multi_illuminator_frame_json(
                frame, publication->timestampMs, product.map,
                product.detections, product.tracks,
                publication->samplePositionAvailable,
                publication->captureEpoch, publication->captureFirst,
                publication->captureEnd));
            }
            const auto outputEnd=PipelineClock::now();
            finishCurrent();
            const auto pipelineEnd=PipelineClock::now();
            const auto milliseconds=[](auto end,auto start){return std::chrono::duration<double,std::milli>(end-start).count();};
            const std::vector<std::string> multiTimingNames{
              "multi_capture_prepare", "multi_channelizer_wall", "multi_illuminator_radar",
              "multi_serialize", "multi_output", "multi_pipeline_advance",
              "cpi"};
            const std::vector<double> multiTimingValues{
              capturePrepareMs, publication->channelizerMs,
              milliseconds(radarEnd,radarStart),
              milliseconds(serializationEnd,radarEnd),
              milliseconds(outputEnd,serializationEnd),
              milliseconds(pipelineEnd,outputEnd),
              milliseconds(pipelineEnd,pipelineStart)};
            timing->update(publication->timestampMs, multiTimingValues,
              multiTimingNames);
            timing->set_capture_queues(publication->captureBacklogSamples,
              publication->captureDroppedSamples);
            timing->set_capture_sample_position(
              publication->samplePositionAvailable, publication->captureEpoch,
              publication->captureFirst, publication->captureEnd);
            timing->set_input_discontinuity(publication->inputDiscontinuity);
            timing->set_multi_lane_health(laneHealth);
            const auto& multiStatus=publication->acceleration;
            timing->set_wideband_acceleration(multiStatus,
              publication->widebandGpuExecuted,
              publication->widebandCpuExecuted);
            if (!frames.empty()) {
              const auto aggregate = blah2::multi_illuminator::aggregate_status(frames);
              timing->set_acceleration(aggregate.ambiguity,
                aggregate.ambiguityGpuExecuted, aggregate.ambiguityCpuExecuted);
              timing->set_clutter_acceleration(aggregate.clutter,
                aggregate.clutterGpuExecuted, aggregate.clutterCpuExecuted);
            } else {
              blah2::AccelerationStatus noFrame;
              noFrame.requested=accelerationMode;
              noFrame.active="none";
              noFrame.state="no-frame";
              noFrame.reason="No lane CPI was admitted for this publication";
              timing->set_acceleration(noFrame,false,false);
              timing->set_clutter_acceleration(noFrame,false,false);
            }
            socket_timing->sendData(attachPublicationTime(timing->to_json()));
            socket_timestamp->sendData("{\"timestamp\":" +
              std::to_string(publication->timestampMs) + "}");
            if(multiQualificationFatal)
              throw std::runtime_error(*multiQualificationFatal);
            timing_time.clear();
            timing_name.clear();
            time.clear();
            std::fill(consumerClaimedSamples.begin(),
              consumerClaimedSamples.end(), 0);
            continue;
          }
          const bool residentOracle=resident&&resident->qualifying();
          if(resident){
            if(residentLogical==UINT64_MAX||residentConsumed==UINT64_MAX)throw std::overflow_error("Resident frame sequence exhausted");
            resident->begin(captureData,{++residentLogical,++residentConsumed,frameSignalTimeMs});
          }
          // This block is the unchanged CPU route, or its bounded startup oracle.
          // Accepted resident frames cannot enter CPU computational stages.
          std::vector<Map<std::complex<double>> *> channelMaps(ambiguity.size());
          std::optional<blah2::detection::IntegrationMetrics> integrationMetrics;
          if(!resident||residentOracle){
          if (!arrayReference && !pairedDirectFrame)
            referenceData->replace(captureData[referenceChannel]->drain_front(nSamples));
          timing_helper(timing_name, timing_time, time, "extract_buffer");

          std::vector<IqData *> surveillancePointers;
          for (auto& channel : surveillanceData)
            surveillancePointers.push_back(channel.get());
          if (arrayReference)
          {
            std::vector<IqData *> referencePointers;
            for (uint32_t channel : referenceChannels)
              referencePointers.push_back(captureData[channel].get());
            referenceSynthesizer.process_into(referencePointers, *referenceData);
            const auto& metrics = referenceSynthesizer.get_metrics();
            if (metrics.updates != reportedReferenceUpdate)
            {
              std::cout << "[ArrayReference] channels="
                << referencePointers.size() << " update="
                << metrics.updates << " coherent_fraction="
                << metrics.coherentFraction << " coherent_gain_db="
                << metrics.coherentGainDb << " weights=";
              for (std::size_t channel = 0;
                   channel < metrics.weights.size(); channel++)
              {
                if (channel) std::cout << ",";
                std::cout << referenceChannels[channel] << ":"
                  << std::abs(metrics.weights[channel])
                  << "@" << std::arg(metrics.weights[channel]);
              }
              std::cout << "\n";
              reportedReferenceUpdate = metrics.updates;
            }
            if (coherentMapFusion)
              coherentMapFusion->calibrate(metrics.directPathWeights);
          }
          if(resident)resident->checkReference(*referenceData,referenceSynthesizer.get_metrics());
          // Synthesis is the last reader of the captured channels. Transfer
          // complete sample blocks to conditioning instead of copying a CPI.
          if (!pairedDirectFrame)
            for (std::size_t pathIndex = 0;
                 pathIndex < surveillanceChannels.size(); pathIndex++)
              surveillanceData[pathIndex]->replace(
                captureData[surveillanceChannels[pathIndex]]->drain_front(nSamples));
          const auto cutoutPilot = complexCutout.enabled() ? direct_path_pilot(
            *referenceData, surveillanceData, surveillanceChannels, referenceChannels,
            referenceSynthesizer.get_metrics().directPathWeights) : std::nullopt;
          timing_helper(timing_name, timing_time, time,
            "reference_synthesis");
          
          // spectrum
          spectrumAnalyser->process(referenceData.get());
          timing_helper(timing_name, timing_time, time, "spectrum");

          
          // Filter each surveillance channel independently.
          if (isClutter)
          {
            const bool success = acceleration.processClutter(*referenceData,
              surveillancePointers, [&] {
                return cpuStages.clutter(*referenceData, surveillancePointers);
              });
            if (!success)
            {
              if(resident)throw std::runtime_error("Resident startup CPU clutter oracle rejected the CPI");
              if (detectorIntegration) detectorIntegration->reset();
              tracker->reset();
              time.clear();
              timing_name.clear();
              timing_time.clear();
              std::fill(consumerClaimedSamples.begin(),
                consumerClaimedSamples.end(), 0);
              continue;
            }
            timing_helper(timing_name, timing_time, time, "clutter_filter");
          }
          
          if(resident)resident->checkFiltered(surveillancePointers);
          // Produce one map per surveillance channel, then combine their power.
          acceleration.process(referenceData->view_data(), surveillancePointers,
            ambiguityPointers, [&] {
              cpuStages.ambiguity(*referenceData, surveillancePointers,
                ambiguityPointers, pairedDirectFrame);
            });
          for (size_t channel = 0; channel < ambiguity.size(); ++channel) {
            channelMaps[channel] = ambiguity[channel]->result();
          }
          complexCutout.append(frameTimestampMs, channelMaps,
            referenceSynthesizer.get_metrics().weights, cutoutPilot);
          if(resident)resident->checkMaps(ambiguityPointers);
          } // CPU/portable frontend or bounded resident startup oracle
          if (!resident || residentOracle || residentHostProducts) {
          if (residentHostProducts && !residentOracle) {
            const auto& output = resident->receivePrepared();
            const auto& shape = resident->config().geometry;
            const uint64_t cells = uint64_t(shape.doppler) * shape.delays;
            for (uint32_t channel = 0; channel < shape.channels; ++channel) {
              channelMaps[channel] = ambiguity[channel]->result();
              for (uint32_t row = 0; row < shape.doppler; ++row)
                for (uint32_t column = 0; column < shape.delays; ++column)
                  channelMaps[channel]->data[row][column] = output.maps.data[
                    channel * cells + uint64_t(column) * shape.doppler +
                    (row + shape.doppler / 2 + 1) % shape.doppler];
            }
          }
          // Only the fused map is detected/published. Fusion consumes complex
          // samples, so per-channel display metrics would be unused log passes.
          map = coherentMapFusion ? coherentMapFusion->process(channelMaps) :
            mapFusion.process(channelMaps);
          map->set_metrics();
          timing_helper(timing_name, timing_time, time, "ambiguity_processing");
          
          if (detectorIntegration)
            integrationMetrics = detectorIntegration->process(map, captureEpoch,
              captureFirstEpochSample, captureEndEpochSample,
              captureSamplePositionAvailable);

          // The CPU and portable-GPU routes share the channelized detector's
          // temporal warm-up, CFAR, centroid and interpolation contract.
          if (isDetection)
          {
            detection = blah2::detection::detectProducts(*map, *cfarDetector1D,
              *centroid, *interpolate, integrationMetrics);
            timing_helper(timing_name, timing_time, time, "detector");
          }

          // tracker process
          if (isTracker)
          {
            track = blah2::detection::trackProducts(*detection, *tracker,
              frameSignalTimeMs, integrationMetrics, cfarDetector1D);
            timing_helper(timing_name, timing_time, time, "tracker");
          }

          } // legacy computation or explicitly labeled startup oracle
          blah2::resident::Coordinator::Publication residentPublication;
          std::string cpuManifoldEvidence;
          if(resident){
            auto evidenceMapping=bearingChannelMapping;
            if(manifoldRecorderEnabled&&evidenceMapping.empty())
              for(uint32_t i=0;i<surveillanceChannels.size();++i)evidenceMapping.push_back(i);
            const blah2::resident::BearingConfig evidenceBearing{
              true,
              bearingMethod=="covariance_mvdr"?blah2::resident::BearingMethod::covarianceMvdr:
                blah2::resident::BearingMethod::coherentBeamBank,
              bearingReferenceAnchor=="fixed_site_projected"?
                blah2::resident::BearingReferenceAnchor::fixedSiteProjected:
                bearingReferenceAnchor=="fixed_site"?blah2::resident::BearingReferenceAnchor::fixedSite:
                blah2::resident::BearingReferenceAnchor::liveEigenbeam,
              bearingScanBeams,bearingCandidatePeaks,bearingGuardRadius,bearingTrainingRadius,
              bearingDiagonalLoading,std::move(evidenceMapping),{}};
            resident->receivePrepared();
            residentPublication=resident->format(frameTimestampMs,
              {bearingEnabled,coherentBeamCount,bearingScanBeams/coherentBeamCount,bearingMinConsistent,xAxisBearingDeg,
               bearingUncertaintyFloorDeg,bearingMinPeakSeparationDb,
               bearingMethod=="covariance_mvdr"?1u:0u,bearingScanBeams,
               ambiguity.front()->get_cpi(),bearingMaxRateDegPerSecond,
               stationaryClutter?stationaryPersistenceCpis:0u,
               stationaryClutter?stationaryMaxDopplerHz:0.0},manifoldRecorderEnabled,
               evidenceBearing,{surveillanceGainDb.data(),surveillanceGainDb.size()});
            if(residentOracle)resident->checkProducts(residentPublication,
              map->to_json_km(frameTimestampMs,fs),
              isDetection?detection->to_json_km(frameTimestampMs,fs):std::string{},
              isTracker?track->to_json(frameTimestampMs):std::string{});
            resident->commit();
            timing_helper(timing_name,timing_time,time,"gpu_resident_ready");
          }
          else if(manifoldRecorderEnabled)
            cpuManifoldEvidence=blah2::manifold_evidence::cpu(frameTimestampMs,double(fc),double(fs),
              *detection,channelMaps,referenceSynthesizer.get_metrics().weights,surveillanceGainDb);

          if (liveTemporalShadow) {
            const blah2::integration::LiveTemporalShadow::Frame shadowFrame{
              captureEpoch, ++liveShadowSequence, captureFirstEpochSample,
              captureEndEpochSample, uint64_t(fc), extractedInputGeneration + 1, fs,
              captureSamplePositionAvailable, frameTimestampMs, fs};
            // Existing native CAF readback only: no new GPU work or CPU oracle.
            if (resident) liveTemporalShadow->offerResident(shadowFrame,
              resident->prepared().maps.data, resident->prepared().maps.size);
            else liveTemporalShadow->offerCpu(shadowFrame, channelMaps);
          }

          // output IqData meta data
          jsonIqData = attachEventTime(resident ?
            std::move(residentPublication.spectrum) :
            referenceData->to_json(frameTimestampMs));
          socket_iqdata->sendData(jsonIqData);

          // output map data
          blah2::analysis::NativeProductIdentity identity;
          identity.illuminatorId = "primary";
          identity.frequencyHz = fc; identity.sampleRateHz = fs;
          identity.captureSamplePositionAvailable = captureSamplePositionAvailable;
          identity.captureEpoch = captureEpoch;
          identity.captureFirst = identity.firstInputSample = captureFirstEpochSample;
          identity.captureEnd = captureEndEpochSample;
          identity.endInputSample = captureEndEpochSample ? captureEndEpochSample - 1 : 0;
          identity.channelIds = surveillanceChannels;
          identity.referenceChannelIds = referenceChannels;
          identity.eventTime = eventTime;
          identity.productBackend = resident && !residentHostProducts ? "vulkan-resident" : "cpu";
          identity.productDevice = resident && !residentHostProducts ? residentDevice : "";
          identity.temporalExecuted = resident && !residentHostProducts ?
            integrationCpis > 1 || trackBeforeDetect :
            integrationMetrics && integrationMetrics->availableCpis > 0;
          identity.detectorExecuted = isDetection &&
            (!integrationMetrics || integrationMetrics->ready);
          identity.trackerExecuted = isTracker;
          identity.bearingExecuted = resident && bearingEnabled;
          const auto annotateRadar = [&](std::string json) {
            return blah2::detection::annotateIntegration(
              blah2::analysis::annotate_native_product(std::move(json), identity),
              integrationMetrics);
          };
          mapJson = annotateRadar(resident && !residentHostProducts ? std::move(residentPublication.map) :
            map->to_json_km(frameTimestampMs, fs));
          if (saveMap)
          {
            if (!map->save(mapJson, saveMapPath))
              throw std::runtime_error("Failed to save radar map output");
          }
          socket_map->sendData(mapJson);

          // output detection data
          if (isDetection)
          {
            detectionJson = annotateRadar(resident && !residentHostProducts ?
              std::move(residentPublication.detections) :
              detection->to_json_km(frameTimestampMs, fs));
            socket_detection->sendData(detectionJson);
            if(manifoldRecorderEnabled)socket_manifold->sendData(attachEventTime(
              resident ? std::move(residentPublication.manifoldEvidence) :
              std::move(cpuManifoldEvidence)));
          }
          if (saveDetection && isDetection)
          {
            if (!detection->save(detectionJson, saveDetectionPath))
              throw std::runtime_error("Failed to save radar detection output");
          }

          // output tracker data
          if (isTracker)
          {
            jsonTracker = annotateRadar(resident && !residentHostProducts ?
              std::move(residentPublication.tracks) :
              track->to_json(frameTimestampMs));
            socket_track->sendData(jsonTracker);
            if(bearingEnabled)socket_bearing->sendData(attachEventTime(
              std::move(residentPublication.bearing)));
            if(socket_causal&&resident)try{const auto trace=resident->prepared().causalTrace;
              const auto proof=resident->prepared().preparedEvidence;
              if(causalModuleHash.empty()&&proof.size>=192)
                causalModuleHash=blah2::causal_trace::hex(proof.data+160,32);
              socket_causal->sendData(blah2::causal_trace::json(trace.data,trace.size,rangeRes/1000,
                causalSequence++,causalMaximum,{captureSamplePositionAvailable,captureEpoch,
                  captureFirstEpochSample,captureEndEpochSample},false,socket_causal->dropped()+causalFormatDrops,
                  causalConfigHash,causalModuleHash));
            }catch(...){++causalFormatDrops;}
          }

          // output radar data timer
          timing_helper(timing_name, timing_time, time, "output_radar_data");

          // cpi timer
          time.push_back(current_time_us());
          double delta_ms = (double)(time.back()-time[0]) / 1000;
          timing_name.push_back("cpi");
          timing_time.push_back(delta_ms);
          std::cout << "CPI time (ms): " << delta_ms << "\n";

          // output timing data
          timing->update(frameTimestampMs, timing_time, timing_name);
          timing->set_capture_queues(captureBacklogSamples,captureDroppedSamples);
          timing->set_capture_sample_position(captureSamplePositionAvailable,
            captureEpoch, captureFirstEpochSample, captureEndEpochSample);
          timing->set_acceleration(
            resident ? resident->status() : acceleration.status(),
            resident ? true : acceleration.ambiguityGpuExecuted(),
            resident ? residentOracle : acceleration.ambiguityCpuExecuted());
          if (isClutter)
            timing->set_clutter_acceleration(
              resident ? resident->status() : acceleration.clutterStatus(),
              resident ? true : acceleration.clutterTiming().gpuExecuted,
              resident ? residentOracle : acceleration.clutterTiming().cpuExecuted);
          else {
            blah2::AccelerationStatus disabled;
            disabled.requested = accelerationMode;
            disabled.state = "disabled";
            disabled.reason = "Clutter filtering is disabled";
            timing->set_clutter_acceleration(disabled, false, false);
          }
          jsonTiming = timing->to_json();
          if (liveTemporalShadow) jsonTiming = attach_live_temporal_shadow(
            std::move(jsonTiming), liveTemporalShadow->status());
          jsonTiming = blah2::detection::annotateIntegration(
            attachEventTime(std::move(jsonTiming)), integrationMetrics);
          socket_timing->sendData(jsonTiming);
          timing_time.clear();
          timing_name.clear();

          // output CPI timestamp for updating data
          // TCP is a byte stream, so delimit each timestamp as a complete JSON
          // object. The API parser must tolerate fragmentation and coalescing.
          std::string t0_string = "{\"timestamp\":" + std::to_string(frameTimestampMs) + "}";
          socket_timestamp->sendData(t0_string);
          if(resident){
            // sendData is a completed local TCP write, not a browser/application
            // acknowledgement. Any exception stops; there is no blind replay.
            resident->published();
            if(residentSnapshotEnabled&&residentLogical<=residentSnapshotMaxFrames) {
              if(!captureSamplePositionAvailable)
                throw std::runtime_error("Resident replay snapshot export requires exact capture sample position");
              const auto snapshot=resident->snapshot();
              write_replay_snapshot(residentSnapshotOutput,residentLogical,residentConsumed,snapshot);
              const auto path=residentSnapshotOutput/("resident-frame-"+
                std::to_string(residentLogical)+"-"+std::to_string(residentConsumed)+".vwrps");
              blah2::integration::coherent_replay::writeSidecarExclusive(path,
                replay_lineage_sidecar(snapshot,residentConfig,captureSamplePositionAvailable,
                  captureEpoch,captureFirstEpochSample,captureEndEpochSample,uint64_t(fc),residentConfig.epoch));
            }
            const auto residentWholeEnd=std::chrono::steady_clock::now();
            const double wholeMs=std::chrono::duration<double,std::milli>(residentWholeEnd-residentWholeStart).count();
            std::cout<<"GPU_RESIDENT_MAIN frame="<<residentLogical<<" consumed="<<residentConsumed<<" timestamp_ms="<<frameTimestampMs
              <<" startup_cpu_oracle="<<residentOracle<<" whole_ms="<<wholeMs
              <<" whole_start_ns="<<std::chrono::duration_cast<std::chrono::nanoseconds>(residentWholeStart.time_since_epoch()).count()
              <<" whole_end_ns="<<std::chrono::duration_cast<std::chrono::nanoseconds>(residentWholeEnd.time_since_epoch()).count()<<" includes_fifo=1 includes_pack_ipc=1 includes_json=1 includes_all_socket_writes=1 backlog=";
            for(size_t i=0;i<captureBacklogSamples.size();++i)std::cout<<(i?",":"")<<captureBacklogSamples[i];
            std::cout<<" drops=";
            for(size_t i=0;i<captureDroppedSamples.size();++i)std::cout<<(i?",":"")<<captureDroppedSamples[i];
            std::cout<<'\n';
          }
          time.clear();
          std::fill(consumerClaimedSamples.begin(),
            consumerClaimedSamples.end(), 0);

        }
        else
        {
          // short delay to prevent tight looping
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
      }
      // A finite replay can end with one completed GPU CPI intentionally held
      // for overlap. Publish it before normal EOF; epoch resets clear this
      // state explicitly above so identities can never cross configurations.
      if(multiPendingPublication.has_value()){using DrainClock=std::chrono::steady_clock;
        const auto drainStart=DrainClock::now();auto pending=
          multiPendingPublication.take();auto publication=std::move(pending.second);
        auto frames=processMultiFrames(std::move(pending.first), publication);
        const auto laneHealth=multiRadar->laneHealth();
        const auto radarEnd=DrainClock::now();
        struct FinalSerialized{std::string map,detections,tracks;};
        std::vector<FinalSerialized> serialized(frames.size());
        multiSerializationExecutor->parallel_for(frames.size(),
          [&](std::size_t index){const auto& frame=frames[index];
            const auto identity = native_product_identity(frame, publication);
            const auto annotate = [&](std::string json) {
              return blah2::detection::annotateIntegration(
                blah2::analysis::annotate_native_product(std::move(json), identity),
                frame.detectorIntegration);
            };
            serialized[index].map=annotate(
              multi_map_json(frame, publication.timestampMs, publication.publishFullMap));
            serialized[index].detections=annotate(
              multi_detection_json(frame, publication.timestampMs));
            serialized[index].tracks=annotate(
              multi_track_json(frame, publication.timestampMs));});
        const auto serializationEnd=DrainClock::now();
        for(std::size_t index=0;index<frames.size();++index){const auto& frame=frames[index];const auto& product=serialized[index];saveMultiProducts(frame, publication);socket_illuminator->sendData(multi_illuminator_frame_json(frame,publication.timestampMs,product.map,product.detections,product.tracks,publication.samplePositionAvailable,publication.captureEpoch,publication.captureFirst,publication.captureEnd));}
        const auto drainEnd=DrainClock::now();
        const auto milliseconds=[](auto end,auto start){return std::chrono::duration<double,std::milli>(end-start).count();};
        timing->update(publication.timestampMs,
          {publication.channelizerMs,milliseconds(radarEnd,drainStart),
           milliseconds(serializationEnd,radarEnd),
           milliseconds(drainEnd,serializationEnd),0.0,
           milliseconds(drainEnd,drainStart)},
          {"multi_channelizer_wall","multi_illuminator_radar","multi_serialize",
           "multi_output","multi_pipeline_advance","cpi"});
        timing->set_capture_queues(publication.captureBacklogSamples,
          publication.captureDroppedSamples);
        timing->set_capture_sample_position(publication.samplePositionAvailable,
          publication.captureEpoch,publication.captureFirst,
          publication.captureEnd);
        timing->set_input_discontinuity(publication.inputDiscontinuity);
        timing->set_multi_lane_health(laneHealth);
        const auto& multiStatus=publication.acceleration;
        timing->set_wideband_acceleration(multiStatus,
          publication.widebandGpuExecuted,
          publication.widebandCpuExecuted);
        if (!frames.empty()) {
          const auto aggregate = blah2::multi_illuminator::aggregate_status(frames);
          timing->set_acceleration(aggregate.ambiguity,
            aggregate.ambiguityGpuExecuted, aggregate.ambiguityCpuExecuted);
          timing->set_clutter_acceleration(aggregate.clutter,
            aggregate.clutterGpuExecuted, aggregate.clutterCpuExecuted);
        } else {
          blah2::AccelerationStatus noFrame;
          noFrame.requested=accelerationMode;
          noFrame.active="none";
          noFrame.state="no-frame";
          noFrame.reason="No lane CPI was admitted for this publication";
          timing->set_acceleration(noFrame,false,false);
          timing->set_clutter_acceleration(noFrame,false,false);
        }
        socket_timing->sendData(blah2::annotate_event_time(
          timing->to_json(),publication.eventTime));
        socket_timestamp->sendData("{\"timestamp\":"+
          std::to_string(publication.timestampMs)+"}");
        if(multiQualificationFatal)
          throw std::runtime_error(*multiQualificationFatal);}
      publishTerminalCaptureAccounting();
    } catch (const std::exception& error) {
      publishTerminalCaptureAccounting();
      if(resident)resident->stop();
      capture->processing_error(error.what());
      capture->request_input_failure();
    } catch (...) {
      publishTerminalCaptureAccounting();
      if(resident)resident->stop();
      capture->processing_error("Unknown processing failure");
      capture->request_input_failure();
    }
    });
  t2.join();
  if (t1.joinable()) t1.join();

  return capture->fatal_live_input_failure() ? 1 : 0;
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

void timing_helper(std::vector<std::string>& timing_name, 
  std::vector<double>& timing_time, std::vector<uint64_t>& time_us, 
  std::string name)
{
  time_us.push_back(current_time_us());
  double delta_ms = (double)(time_us.back()-time_us[time_us.size()-2]) / 1000;
  timing_name.push_back(name);
  timing_time.push_back(delta_ms);
}
