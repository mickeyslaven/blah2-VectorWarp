#include "Source.h"
#include "RecordingAcquisitionJson.h"

#include <iostream>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <ctime>
#include <sstream>
#include <iomanip>
#include <filesystem>
#include <fstream>
#include <cctype>
#include <stdexcept>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#if __has_include("ReceiverCohort.h")
#include "ReceiverCohort.h"
#else
#define BLAH2_RECEIVER_COHORT "standalone-unqualified-build"
#define BLAH2_RECEIVER_CODE_FINGERPRINT "standalone-unqualified-build"
#endif
#ifndef BLAH2_RECEIVER_CODE_FINGERPRINT
#define BLAH2_RECEIVER_CODE_FINGERPRINT BLAH2_RECEIVER_COHORT
#endif

namespace {
constexpr double PORTABLE_LATE_CLAIM_RING_SECONDS = 10.0;
std::string decimal(uint64_t value) { return std::to_string(value); }
std::string fnv1a64(const std::string& value) {
  uint64_t hash=1469598103934665603ULL;
  for(unsigned char byte:value) { hash^=byte; hash*=1099511628211ULL; }
  std::ostringstream out; out << "fnv1a64:" << std::hex << std::setw(16)
    << std::setfill('0') << hash;
  return out.str();
}
std::string backend_name(const std::string& type) {
  if(type=="Kraken") return "heimdall-v2";
  if(type=="RspDuo") return "sdrplay-api-v3";
  if(type=="Usrp") return "uhd";
  if(type=="HackRF") return "libhackrf";
  return "test-or-unknown";
}
std::string time_source(const std::string& type) {
  if(type=="Kraken") return "kraken_heimdall_host_arrival_estimate";
  if(type=="RspDuo") return "sdrplay_callback_sample_counter";
  if(type=="Usrp") return "usrp_uhd_device_time";
  if(type=="HackRF") return "hackrf_shared_trigger_relative_sample_epoch";
  return "unavailable";
}
void publish_recording_provenance(const std::string& file, const std::string& provenance) {
  const std::filesystem::path target(file + ".provenance.json");
  const std::filesystem::path temporary(target.string() + ".tmp");
  {
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    if (!output) throw std::runtime_error("Cannot create recording provenance sidecar");
    output.write(provenance.data(), static_cast<std::streamsize>(provenance.size()));
    output.put('\n'); output.close();
    if (!output) throw std::runtime_error("Cannot write recording provenance sidecar");
  }
  std::error_code error;
  std::filesystem::rename(temporary, target, error);
  if (error) {
    std::filesystem::remove(temporary);
    throw std::runtime_error("Cannot publish recording provenance sidecar: " + error.message());
  }
}
}

Source::Source()
{
}

// constructor
Source::Source(std::string _type, uint32_t _fc, uint32_t _fs, 
    std::string _path, bool *_saveIq, uint32_t channels)
{
  type = _type;
  fc = _fc;
  fs = _fs;
  path = _path;
  saveIq = _saveIq;
  recordingChannels = channels;
}

void Source::configure_portable_clips(double preSeconds,
    double maximumClipSeconds) {
  if (!std::isfinite(preSeconds) || !std::isfinite(maximumClipSeconds) ||
      preSeconds < 0 || maximumClipSeconds <= 0 || maximumClipSeconds >
      PORTABLE_LATE_CLAIM_RING_SECONDS)
    throw std::invalid_argument("Invalid portable clip retention bounds");
  if (!blah2::PortableClipRecorder::supports_retention(recordingChannels, fs,
      PORTABLE_LATE_CLAIM_RING_SECONDS)) {
    std::lock_guard<std::mutex> lock(clipMutex);
    portableClipRecorder.reset();
    portableClipUnavailable = {0, true, {},
      "Automatic IQ clips unavailable: bounded 10-second retention exceeds the portable receiver memory limit", 0};
    return;
  }
  blah2::PortableClipRecorder::Options options;
  // The API's late-claim allowance is receiver-independent. Keep every
  // portable backend's bounded ring equal to Kraken's ten-second contract,
  // rather than silently reducing it to the configured pre-roll.
  options.ringSeconds = PORTABLE_LATE_CLAIM_RING_SECONDS;
  options.maximumClipSeconds = PORTABLE_LATE_CLAIM_RING_SECONDS;
  auto recorder = std::make_unique<blah2::PortableClipRecorder>(path,
    type, recordingChannels, fs, fc, std::move(options));
  std::lock_guard<std::mutex> lock(clipMutex);
  portableClipRecorder = std::move(recorder);
  portableClipUnavailable = {};
}

std::string Source::recording_code_sha256() {
  const std::string identity = BLAH2_RECEIVER_CODE_FINGERPRINT;
  return blah2::acquisition::sha256(identity) ? identity : std::string{};
}

void Source::set_recording_context(blah2::RecordingContext context) {
  std::lock_guard<std::mutex> lock(acquisitionMutex);
  recordingContext = std::move(context);
  recordingBlockMetadata.reset();
}

std::shared_ptr<const blah2::RecordingBlockMetadata>
Source::recording_block_metadata(uint64_t epoch) {
  const auto gains = recording_applied_gain_db();
  std::lock_guard<std::mutex> lock(acquisitionMutex);
  if (!recordingBlockMetadata || recordingMetadataEpoch != epoch ||
      recordingBlockMetadata->perChannelGainDb != gains) {
    auto value = std::make_shared<blah2::RecordingBlockMetadata>();
    value->context = recordingContext;
    // The two-input native source adapter is reference 0 / surveillance 1.
    // Array geometry/roles must be supplied by the configured pipeline.
    if (value->context.referenceMode.empty() && recordingChannels == 2) {
      value->context.referenceMode = "dedicated";
      value->context.referenceChannels = {0}; value->context.surveillanceChannels = {1};
    }
    value->receiverDetailsJson = recording_provenance_json();
    value->perChannelGainDb = gains;
    value->gainProvenance = gains.size() == recordingChannels ?
      recording_gain_provenance() : "requested-settings-only";
    recordingBlockMetadata = std::move(value); recordingMetadataEpoch = epoch;
  }
  return recordingBlockMetadata;
}

void Source::request_clip(const blah2::IqClipRequest& request) {
  std::lock_guard<std::mutex> lock(clipMutex);
  if (portableClipRecorder) portableClipRecorder->request(request);
  else if (portableClipUnavailable.error.size())
    portableClipUnavailable = {request.revision, true, {}, portableClipUnavailable.error, 0};
}

blah2::IqClipStatus Source::clip_status() const {
  std::lock_guard<std::mutex> lock(clipMutex);
  return portableClipRecorder ? portableClipRecorder->status() : portableClipUnavailable;
}

void Source::process(const std::vector<IqData *>& buffers)
{
  if (buffers.size() != 2)
  {
    throw std::invalid_argument("Two-channel source requires two buffers");
  }
  process(buffers[0], buffers[1]);
}

std::string Source::open_file()
{
  std::lock_guard<std::mutex> control(recordingControlMutex);
  std::lock_guard<std::mutex> lock(recordingMutex);
  return open_file_locked();
}

std::string Source::open_file_locked(const SampleTimePosition* startPosition)
{
  if (recordingActive) return recordingFile;
  if (recordingWriter) {
    write_recording_provenance_locked(false);
    recordingWriter.reset();
  }
  recordingResumePending = false;
  recordingResumePrepared = false;
  prepare_recording_locked();
  try {
    recordingWriter = std::make_unique<blah2::RecordingWriter>(recordingFile,
      blah2::RecordingMetadata{recordingChannels, fs, fc, 0, "blah2"});
    recordingStartPosition = startPosition ? *startPosition : sample_time_position();
    recordingActive = true;
    write_recording_provenance_locked(false);
  } catch (const std::exception& error) { recording_failed(error.what()); throw; }
  std::cout << "Recording IQ: " << recordingFile << std::endl;
  return recordingFile;
}

void Source::prepare_recording_locked() {
  // get string of timestamp in YYYYmmdd-HHMMSS
  auto currentTime = std::chrono::system_clock::to_time_t(
    std::chrono::system_clock::now());
  std::tm* timeInfo = std::localtime(&currentTime);
  std::ostringstream oss;
  oss << std::put_time(timeInfo, "%Y%m%d-%H%M%S");
  std::string timestamp = oss.str();

  // create file path
  std::string typeLower = type;
  std::transform(typeLower.begin(), typeLower.end(), 
    typeLower.begin(), ::tolower);
  const auto unique = std::chrono::duration_cast<std::chrono::nanoseconds>(
    std::chrono::system_clock::now().time_since_epoch()).count();
  recordingFile = (std::filesystem::path(path) /
    (timestamp + "-" + std::to_string(unique) + "." + typeLower + ".blah2iq")).string();
  pendingChannels.assign(recordingChannels, {});
  pendingStarts.assign(recordingChannels, 0);
  channelStarted.assign(recordingChannels, false);
  recordedSamples = 0; recordingError.clear(); recordingDiscontinuities.clear();
  recordingFailureProvenance.clear();
  manualRecordingMetadata.reset(); manualRecordingFrames.clear();
  manualRecordingPositionAvailable = false; manualRecordingBounded = true;
  recordingStartPosition = {};
}

void Source::service_recording() {
  std::lock_guard<std::mutex> control(recordingControlMutex);
  std::unique_lock<std::mutex> lock(recordingMutex);
  if (!recordingResumePending || recordingResumePrepared ||
      (recordingWriter && !recordingWriter->abort_ready())) return;
  try {
    const auto previousFile = recordingFile;
    const auto previousProvenance = recordingFailureProvenance.empty() ?
      recording_provenance_locked(false) : recordingFailureProvenance;
    auto previousWriter = std::move(recordingWriter);
    lock.unlock();
    // No acquisition lock spans filesystem I/O or destruction/join. Pending
    // callbacks remain inactive; no source samples are invented for this gap.
    publish_recording_provenance(previousFile, previousProvenance);
    previousWriter.reset();
    lock.lock();
    if (!recordingResumePending) return;
    prepare_recording_locked();
    const auto nextFile = recordingFile;
    const auto initialProvenance = recording_provenance_locked(false);
    lock.unlock();
    auto nextWriter = std::make_unique<blah2::RecordingWriter>(nextFile,
      blah2::RecordingMetadata{recordingChannels, fs, fc, 0, "blah2"});
    publish_recording_provenance(nextFile, initialProvenance);
    lock.lock();
    if (!recordingResumePending) {
      lock.unlock(); // A terminal discontinuity cancelled this unopened epoch.
      return;
    }
    recordingWriter = std::move(nextWriter);
    recordingResumePrepared = true;
  } catch (const std::exception& error) {
    if (!lock.owns_lock()) lock.lock();
    recording_failed(error.what());
  }
}

bool Source::activate_recording_locked() {
  if (recordingResumePending) {
    if (!recordingResumePrepared) return false;
    recordingResumePrepared = false;
    recordingResumePending = false;
    recordingActive = true;
  }
  return recordingActive && bool(recordingWriter);
}

void Source::close_file()
{
  std::lock_guard<std::mutex> control(recordingControlMutex);
  std::unique_lock<std::mutex> lock(recordingMutex);
  recordingResumePending = false;
  recordingActive = false;
  if (!recordingWriter) return;
  if (!recordingError.empty() || recordingResumePrepared) {
    // Failure callbacks only abort. Explicit control-thread teardown may wait
    // for the disk and persist the diagnostic, but must never finalize IQ.
    recordingResumePrepared = false;
    recordingWriter->abort();
    write_recording_provenance_locked(false);
    recordingWriter.reset();
    return;
  }
  const auto finalizedFile = recordingFile;
  try {
    if (!recordingWriter->samples())
      throw std::runtime_error("No samples were recorded");
    flush_recording_locked();
    for (const auto& pending : pendingChannels)
      if (!pending.empty()) throw std::runtime_error("Recording ended with unpaired input samples");
    recordingWriter->close(); recordedSamples = recordingWriter->samples();
    write_recording_provenance_locked(true);
    const bool supportedAcquisition = type == "Kraken" || type == "Usrp" || type == "RspDuo" || type == "HackRF";
    std::string acquisitionJson, acquisitionReason;
    if (manualRecordingMetadata && manualRecordingBounded && !manualRecordingFrames.empty() && supportedAcquisition) {
      const auto first = manualRecordingFrames.front().sourceFirst;
      if (first > UINT64_MAX - recordedSamples) throw std::runtime_error("Recording acquisition sample interval overflow");
      acquisitionJson = blah2::acquisition::serialize(manualRecordingMetadata.get(), type,
        recordingChannels, fs, fc, recordingStartPosition.epoch, first, first + recordedSamples,
        manualRecordingPositionAvailable, manualRecordingFrames);
    } else acquisitionReason = "Acquisition frame lineage unavailable or exceeded bounded capacity; ordinary playback only";
    recordingWriter.reset(); pendingChannels.clear();
    // A full-artifact digest can outlive a callback interval. The finalized
    // filename/context are immutable locals; never hold ingress or clip locks
    // while hashing it, and never bind a concurrently started new recording.
    lock.unlock();
    if (supportedAcquisition)
      blah2::acquisition::writeManifest(finalizedFile, acquisitionJson, {}, acquisitionReason);
  }
  catch (const std::exception& error) {
    if (lock.owns_lock()) recording_failed(error.what());
    else {
      lock.lock();
      if (recordingFile == finalizedFile) recordingError = error.what();
      // The clean IQ remains ordinary-playable, but a failed/missing manifest
      // cannot satisfy exact admission. Do not corrupt a newer recording.
    }
    throw;
  }
}

void Source::recording_failed(const std::string& error) {
  recordingError = error;
  if (recordingDiscontinuities.empty() || recordingDiscontinuities.back()!=error)
    recordingDiscontinuities.push_back(error);
  if (recordingWriter) recordedSamples = recordingWriter->samples();
  recordingActive = false; recordingResumePending = false;
  recordingResumePrepared = false;
  // Never perform disk I/O or join a disk-blocked thread on the receiver's
  // callback. The initial sidecar and unfinished IQ header already fail closed.
  if (recordingWriter) recordingWriter->abort();
  pendingChannels.clear();
  // Capture failure-time identity now, but let the control context publish it.
  // Later receiver recovery must not rewrite the failed epoch's diagnostics.
  recordingFailureProvenance.clear();
  try { recordingFailureProvenance = recording_provenance_locked(false); }
  catch (...) {} // Invalid provenance must never escape an ingress failure.
  std::cerr << "Recording stopped: " << error << '\n';
}

void Source::write_recording_provenance_locked(bool complete) {
  if (recordingFile.empty()) return;
  publish_recording_provenance(recordingFile, !complete && !recordingFailureProvenance.empty() ?
    recordingFailureProvenance : recording_provenance_locked(complete));
}

std::string Source::recording_provenance_locked(bool complete) const {
  const auto backendDetails = recording_provenance_json();
  rapidjson::Document details;
  details.Parse(backendDetails.c_str());
  if (details.HasParseError() || !details.IsObject())
    throw std::runtime_error("Receiver returned invalid recording provenance");
  const auto current = sample_time_position();
  rapidjson::StringBuffer canonicalDetailsBuffer;
  rapidjson::Writer<rapidjson::StringBuffer> canonicalDetailsWriter(canonicalDetailsBuffer);
  details.Accept(canonicalDetailsWriter);
  const std::string canonicalDetails(canonicalDetailsBuffer.GetString(),canonicalDetailsBuffer.GetSize());
  const uint64_t samples = recordingWriter ? recordingWriter->samples() : recordedSamples;
  rapidjson::Document document(rapidjson::kObjectType);
  auto& allocator=document.GetAllocator();
  document.AddMember("schema","vectorwarp-recording-provenance-v2",allocator);
  document.AddMember("receiver",rapidjson::Value(type.c_str(),allocator),allocator);
  const auto backend=backend_name(type);
  document.AddMember("backend",rapidjson::Value(backend.c_str(),allocator),allocator);
  document.AddMember("format","blah2iq-v1+sidecar-v2",allocator);
  document.AddMember("channels",recordingChannels,allocator);
  document.AddMember("sampleRateHz",fs,allocator);
  document.AddMember("frequencyHz",fc,allocator);
  rapidjson::Value device(rapidjson::kObjectType);
  device.AddMember("identitySource","receiverDetails",allocator);
  device.AddMember("identityReadbackRequired",true,allocator);
  document.AddMember("device",device,allocator);
  rapidjson::Value channels(rapidjson::kArrayType);
  for(unsigned channel=0;channel<recordingChannels;++channel) {
    rapidjson::Value item(rapidjson::kObjectType);
    item.AddMember("channel",channel,allocator);
    item.AddMember("identitySource","receiverDetails",allocator);
    item.AddMember("appliedRfSettingsSource","receiverDetails",allocator);
    channels.PushBack(item,allocator);
  }
  document.AddMember("channelSettings",channels,allocator);
  rapidjson::Value topology(rapidjson::kObjectType);
  topology.AddMember("source","receiverDetails",allocator);
  topology.AddMember("clockSource",rapidjson::Value(current.clockSource.c_str(),allocator),allocator);
  topology.AddMember("timeSource",rapidjson::Value(current.timeSource.c_str(),allocator),allocator);
  topology.AddMember("motherboard",rapidjson::Value(current.motherboard.c_str(),allocator),allocator);
  topology.AddMember("subdevices",rapidjson::Value(current.subdevices.c_str(),allocator),allocator);
  document.AddMember("topology",topology,allocator);
  rapidjson::Value timing(rapidjson::kObjectType);
  const auto timingSource=time_source(type);
  timing.AddMember("source",rapidjson::Value(timingSource.c_str(),allocator),allocator);
  timing.AddMember("exact",false,allocator);
  timing.AddMember("uncertaintyBounded",false,allocator);
  timing.AddMember("uncertaintyNs",rapidjson::Value(rapidjson::kNullType),allocator);
  timing.AddMember("continuous",complete && recordingDiscontinuities.empty(),allocator);
  timing.AddMember("epoch",rapidjson::Value(decimal(recordingStartPosition.epoch).c_str(),allocator),allocator);
  timing.AddMember("firstSample","0",allocator);
  timing.AddMember("endSample",rapidjson::Value(decimal(samples).c_str(),allocator),allocator);
  if(recordingStartPosition.available) {
    timing.AddMember("sourceFirstSample",rapidjson::Value(decimal(recordingStartPosition.nextSample).c_str(),allocator),allocator);
  } else timing.AddMember("sourceFirstSample",rapidjson::Value(rapidjson::kNullType),allocator);
  if(recordingStartPosition.available && recordingStartPosition.deviceTimeEpochTick>=0)
    timing.AddMember("deviceTimeEpochTick",rapidjson::Value(std::to_string(recordingStartPosition.deviceTimeEpochTick).c_str(),allocator),allocator);
  else timing.AddMember("deviceTimeEpochTick",rapidjson::Value(rapidjson::kNullType),allocator);
  if(current.available)
    timing.AddMember("sourceNextSample",rapidjson::Value(decimal(current.nextSample).c_str(),allocator),allocator);
  else timing.AddMember("sourceNextSample",rapidjson::Value(rapidjson::kNullType),allocator);
  rapidjson::Value intervals(rapidjson::kArrayType);
  if(samples) {
    rapidjson::Value interval(rapidjson::kObjectType);
    interval.AddMember("firstSample","0",allocator);
    interval.AddMember("endSample",rapidjson::Value(decimal(samples).c_str(),allocator),allocator);
    intervals.PushBack(interval,allocator);
  }
  timing.AddMember("intervals",intervals,allocator);
  timing.AddMember("gaps",rapidjson::Value(rapidjson::kArrayType),allocator);
  rapidjson::Value discontinuities(rapidjson::kArrayType);
  for(const auto& reason:recordingDiscontinuities)
    discontinuities.PushBack(rapidjson::Value(reason.c_str(),allocator),allocator);
  timing.AddMember("discontinuities",discontinuities,allocator);
  timing.AddMember("reason","No supported live backend currently supplies an independently attested exact UTC RF epoch",allocator);
  document.AddMember("timing",timing,allocator);
  rapidjson::Value fingerprints(rapidjson::kObjectType);
  const auto configFingerprint=fnv1a64(canonicalDetails+"|"+std::to_string(recordingChannels)+"|"+
    std::to_string(fs)+"|"+std::to_string(fc));
  const std::string codeFingerprint=std::string("sha256:")+BLAH2_RECEIVER_CODE_FINGERPRINT+
    ";receiver-cohort:"+BLAH2_RECEIVER_COHORT;
  fingerprints.AddMember("configuration",rapidjson::Value(configFingerprint.c_str(),allocator),allocator);
  fingerprints.AddMember("code",rapidjson::Value(codeFingerprint.c_str(),allocator),allocator);
  document.AddMember("fingerprints",fingerprints,allocator);
  rapidjson::Value recording(rapidjson::kObjectType);
  recording.AddMember("complete",complete,allocator);
  recording.AddMember("samples",rapidjson::Value(decimal(samples).c_str(),allocator),allocator);
  recording.AddMember("error",rapidjson::Value(recordingError.c_str(),allocator),allocator);
  document.AddMember("recording",recording,allocator);
  rapidjson::Value copied;
  copied.CopyFrom(details,allocator);
  document.AddMember("receiverDetails",copied,allocator);
  rapidjson::StringBuffer buffer;
  rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
  document.Accept(writer);
  return {buffer.GetString(),buffer.GetSize()};
}

Source::RecordingStatus Source::recording_status() const {
  std::lock_guard<std::mutex> lock(recordingMutex);
  return {recordingActive.load(), recordingFile, recordingError,
    recordingWriter ? recordingWriter->samples() : recordedSamples,
    recordingResumePending.load()};
}

void Source::recording_discontinuity(const std::string& error) {
  processingEpoch.fetch_add(1, std::memory_order_acq_rel);
  if (portableClipRecorder) portableClipRecorder->discontinuity(error);
  std::lock_guard<std::mutex> lock(recordingMutex);
  if (recordingWriter || recordingResumePending) recording_failed(error);
}

void Source::recording_epoch_discontinuity(const std::string& error,
    bool advanceProcessingEpoch) {
  if (advanceProcessingEpoch) advance_processing_epoch();
  if (portableClipRecorder) portableClipRecorder->discontinuity(error);
  std::lock_guard<std::mutex> lock(recordingMutex);
  if (!recordingWriter || !recordingActive) return;
  recording_failed(error);
  recordingResumePending = true;
}

void Source::flush_recording_locked() {
  if (!recordingWriter || !std::all_of(channelStarted.begin(), channelStarted.end(),
      [](bool started) { return started; })) return;
  const auto commonStart = *std::max_element(pendingStarts.begin(), pendingStarts.end());
  for (unsigned ch=0; ch<recordingChannels; ++ch) {
    const auto trim = std::min<uint64_t>(commonStart-pendingStarts[ch], pendingChannels[ch].size());
    pendingChannels[ch].erase(pendingChannels[ch].begin(), pendingChannels[ch].begin()+trim);
    pendingStarts[ch] += trim;
  }
  while (true) {
    size_t count = 262144;
    for (const auto& values : pendingChannels) count = std::min(count, values.size());
    if (!count) return;
    blah2::IqBlock block(recordingChannels);
    for (unsigned ch=0; ch<recordingChannels; ++ch) {
      block[ch].assign(pendingChannels[ch].begin(), pendingChannels[ch].begin()+count);
      pendingChannels[ch].erase(pendingChannels[ch].begin(), pendingChannels[ch].begin()+count);
      pendingStarts[ch] += count;
    }
    recordingWriter->append(block); recordedSamples = recordingWriter->samples();
  }
}

void Source::record_block(const blah2::IqBlock& samples) {
  if (!recording_wants_samples()) return;
  if (portableClipRecorder) portableClipRecorder->ingest(samples, 0, 0, false, 0,
    recording_block_metadata(processing_epoch()));
  std::lock_guard<std::mutex> lock(recordingMutex);
  record_block_locked(samples, nullptr);
}

void Source::record_block(const blah2::IqBlock& samples,
    const SampleTimePosition& startPosition) {
  if (!recording_wants_samples()) return;
  if (portableClipRecorder) portableClipRecorder->ingest(samples,
    startPosition.epoch, startPosition.nextSample, startPosition.available, 0,
    recording_block_metadata(startPosition.epoch));
  std::lock_guard<std::mutex> lock(recordingMutex);
  record_block_locked(samples, &startPosition);
}

void Source::record_block_locked(const blah2::IqBlock& samples,
    const SampleTimePosition* startPosition) {
  try {
    if (!activate_recording_locked()) return;
    if (!recordingWriter->samples())
      recordingStartPosition = startPosition ? *startPosition : sample_time_position();
    const auto prior = recordingWriter->samples();
    const auto metadata = recording_block_metadata(startPosition ? startPosition->epoch : processing_epoch());
    if (!prior) {
      manualRecordingMetadata = metadata;
      manualRecordingPositionAvailable = startPosition && startPosition->available;
    } else if (manualRecordingMetadata != metadata) manualRecordingBounded = false;
    recordingWriter->append(samples);
    if (manualRecordingBounded) {
      const auto first = startPosition && startPosition->available ? startPosition->nextSample :
        (recordingStartPosition.available ? recordingStartPosition.nextSample + prior : prior);
      const auto count = static_cast<uint64_t>(samples.front().size());
      if (prior && (!startPosition || !startPosition->available)) manualRecordingPositionAvailable = false;
      const auto sequence = manualRecordingFrames.empty() ? 1 :
        manualRecordingFrames.back().ingressSequence + manualRecordingFrames.back().repeat;
      manualRecordingBounded = blah2::acquisition::appendFrame(manualRecordingFrames,
        {sequence, first, count, 0, count});
    }
  }
  catch (const std::exception& error) { recording_failed(error.what()); }
}

void Source::record_channel(unsigned channel, uint64_t firstSample,
    const std::vector<std::complex<float>>& samples) {
  if (!recording_wants_samples()) return;
  std::lock_guard<std::mutex> lock(recordingMutex);
  try {
    if (!activate_recording_locked()) return;
    if (channel >= recordingChannels || samples.empty()) throw std::runtime_error("Invalid recording input channel");
    auto& pending = pendingChannels[channel];
    if (channelStarted[channel] && firstSample != pendingStarts[channel] + pending.size())
      throw std::runtime_error("Recording input has a sample gap");
    if (!channelStarted[channel]) { pendingStarts[channel] = firstSample; channelStarted[channel] = true; }
    if (pending.size() + samples.size() > 1048576)
      throw std::runtime_error("Recording channels are too far apart; check receiver synchronization");
    pending.insert(pending.end(), samples.begin(), samples.end());
    if (!std::all_of(channelStarted.begin(), channelStarted.end(), [](bool started) { return started; })) return;
    flush_recording_locked();
  } catch (const std::exception& error) { recording_failed(error.what()); }
}

void Source::kill()
{
  try { close_file(); } catch (...) {}
  if (type == "RspDuo")
  {
    stop();
  } else if (type == "HackRF" || type == "Kraken")
  {
    stop();
  }
  exit(0);
}
