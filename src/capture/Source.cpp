#include "Source.h"

#include <iostream>
#include <algorithm>
#include <chrono>
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
  std::lock_guard<std::mutex> lock(recordingMutex);
  return open_file_locked();
}

std::string Source::open_file_locked(const SampleTimePosition* startPosition)
{
  if (recordingActive) return recordingFile;
  recordingResumePending = false;
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
  try {
    recordingWriter = std::make_unique<blah2::RecordingWriter>(recordingFile,
      blah2::RecordingMetadata{recordingChannels, fs, fc, 0, "blah2"});
    pendingChannels.assign(recordingChannels, {});
    pendingStarts.assign(recordingChannels, 0);
    channelStarted.assign(recordingChannels, false);
    recordedSamples = 0; recordingError.clear(); recordingDiscontinuities.clear();
    recordingStartPosition = startPosition ? *startPosition : sample_time_position();
    recordingActive = true;
    write_recording_provenance_locked(false);
  } catch (const std::exception& error) { recording_failed(error.what()); throw; }
  std::cout << "Recording IQ: " << recordingFile << std::endl;
  return recordingFile;
}

void Source::close_file()
{
  std::lock_guard<std::mutex> lock(recordingMutex);
  recordingResumePending = false;
  recordingActive = false;
  if (!recordingWriter) return;
  try {
    if (!recordingWriter->samples())
      throw std::runtime_error("No samples were recorded");
    flush_recording_locked();
    for (const auto& pending : pendingChannels)
      if (!pending.empty()) throw std::runtime_error("Recording ended with unpaired input samples");
    recordingWriter->close(); recordedSamples = recordingWriter->samples();
    write_recording_provenance_locked(true);
  }
  catch (const std::exception& error) { recording_failed(error.what()); throw; }
  recordingWriter.reset(); pendingChannels.clear();
}

void Source::recording_failed(const std::string& error) {
  recordingError = error;
  if (recordingDiscontinuities.empty() || recordingDiscontinuities.back()!=error)
    recordingDiscontinuities.push_back(error);
  try { write_recording_provenance_locked(false); } catch (...) {}
  if (recordingWriter) recordedSamples = recordingWriter->samples();
  recordingActive = false; recordingResumePending = false; recordingWriter.reset();
  pendingChannels.clear();
  std::cerr << "Recording stopped: " << error << '\n';
}

void Source::write_recording_provenance_locked(bool complete) {
  if (recordingFile.empty()) return;
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
  const std::string provenance(buffer.GetString(),buffer.GetSize());
  const std::filesystem::path target(recordingFile + ".provenance.json");
  const std::filesystem::path temporary(target.string() + ".tmp");
  {
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    if (!output) throw std::runtime_error("Cannot create recording provenance sidecar");
    output.write(provenance.data(), static_cast<std::streamsize>(provenance.size()));
    output.put('\n');
    output.close();
    if (!output) throw std::runtime_error("Cannot write recording provenance sidecar");
  }
  std::error_code error;
  std::filesystem::rename(temporary, target, error);
  if (error) {
    std::filesystem::remove(temporary);
    throw std::runtime_error("Cannot publish recording provenance sidecar: " + error.message());
  }
}

Source::RecordingStatus Source::recording_status() const {
  std::lock_guard<std::mutex> lock(recordingMutex);
  return {recordingActive.load(), recordingFile, recordingError,
    recordingWriter ? recordingWriter->samples() : recordedSamples,
    recordingResumePending.load()};
}

void Source::recording_discontinuity(const std::string& error) {
  std::lock_guard<std::mutex> lock(recordingMutex);
  if (recordingWriter) recording_failed(error);
}

void Source::recording_epoch_discontinuity(const std::string& error) {
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
  std::lock_guard<std::mutex> lock(recordingMutex);
  record_block_locked(samples, nullptr);
}

void Source::record_block(const blah2::IqBlock& samples,
    const SampleTimePosition& startPosition) {
  if (!recording_wants_samples()) return;
  std::lock_guard<std::mutex> lock(recordingMutex);
  record_block_locked(samples, &startPosition);
}

void Source::record_block_locked(const blah2::IqBlock& samples,
    const SampleTimePosition* startPosition) {
  try {
    if (recordingResumePending && !recordingWriter)
      open_file_locked(startPosition);
    if (!recordingActive || !recordingWriter) return;
    if (startPosition && !recordingWriter->samples() &&
        !recordingStartPosition.available) {
      recordingStartPosition = *startPosition;
      write_recording_provenance_locked(false);
    }
    recordingWriter->append(samples);
  }
  catch (const std::exception& error) { recording_failed(error.what()); }
}

void Source::record_channel(unsigned channel, uint64_t firstSample,
    const std::vector<std::complex<float>>& samples) {
  if (!recording_wants_samples()) return;
  std::lock_guard<std::mutex> lock(recordingMutex);
  try {
    if (recordingResumePending && !recordingWriter) open_file_locked();
    if (!recordingActive || !recordingWriter) return;
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
