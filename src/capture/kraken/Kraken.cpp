#include "Kraken.h"

#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iostream>
#include <memory>
#include <netdb.h>
#include <optional>
#include <stdexcept>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

namespace
{
class ConnectionError : public std::runtime_error
{
public:
  using std::runtime_error::runtime_error;
};
int64_t wall_clock_nanoseconds()
{
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
    std::chrono::system_clock::now().time_since_epoch()).count();
}
}

Kraken::Kraken(std::string type, uint32_t fc, uint32_t fs,
  std::string path, bool *saveIq, std::size_t channelCount,
  std::string heimdallHost, uint16_t heimdallPort)
  : Source(type, fc, fs, path, saveIq, channelCount), channelCount(channelCount),
    heimdallHost(heimdallHost), heimdallPort(heimdallPort)
{
  if (channelCount == 0 || channelCount > MAX_CHANNELS)
    throw std::invalid_argument("Kraken requires between one and eight channels");
  if (fs != SUITE_SAMPLE_RATE)
    throw std::invalid_argument(
      "KrakenSDR Suite V2 requires capture.fs to be 2400000");
  clipRecorder = std::make_unique<blah2::KrakenClipRecorder>(path,
    static_cast<uint32_t>(channelCount), fs, fc);
}

void Kraken::request_clip(const blah2::IqClipRequest& request)
{
  clipRecorder->request(request);
}

blah2::IqClipStatus Kraken::clip_status() const
{
  return clipRecorder->status();
}

Kraken::SamplePosition Kraken::sample_position() const
{
  return {samplePositionAvailable.load(std::memory_order_acquire),
    sampleEpoch.load(std::memory_order_relaxed),
    nextEpochSample.load(std::memory_order_relaxed),
    sampleEpochEstimateNanoseconds.load(std::memory_order_relaxed),
    hostReceiptResidualNanoseconds.load(std::memory_order_relaxed),
    hostReceiptMaximumResidualNanoseconds.load(std::memory_order_relaxed),
    hostReceiptDriftPpm.load(std::memory_order_relaxed),
    hostReceiptAnchors.load(std::memory_order_relaxed)};
}

Source::SampleTimePosition Kraken::sample_time_position() const
{
  const auto position = sample_position();
  return {position.available, position.epoch, position.nextSample, 0, fs};
}

std::vector<double> Kraken::actual_gain_db() const
{
  std::lock_guard<std::mutex> lock(provenanceMutex);
  if(!lastHeaderAvailable)return {};
  return {lastHeader.gains.begin(),lastHeader.gains.end()};
}

std::string Kraken::recording_provenance_json() const
{
  rapidjson::Document document(rapidjson::kObjectType);
  auto& allocator=document.GetAllocator();
  document.AddMember("schema","vectorwarp-kraken-recording-details-v1",allocator);
  document.AddMember("receiver","Kraken",allocator);
  rapidjson::Value endpoint(rapidjson::kObjectType);
  endpoint.AddMember("host",rapidjson::Value(heimdallHost.c_str(),allocator),allocator);
  endpoint.AddMember("port",heimdallPort,allocator);
  endpoint.AddMember("deviceIdentityAvailable",false,allocator);
  endpoint.AddMember("deviceIdentityReason","Heimdall V2 does not expose Kraken serial identity",allocator);
  document.AddMember("heimdall",endpoint,allocator);
  rapidjson::Value observed(rapidjson::kObjectType);
  std::lock_guard<std::mutex> lock(provenanceMutex);
  observed.AddMember("available",lastHeaderAvailable,allocator);
  if(lastHeaderAvailable) {
    observed.AddMember("phaseState",lastHeader.phaseState,allocator);
    observed.AddMember("noiseSource",lastHeader.noiseSource,allocator);
    observed.AddMember("frequencyChangeCounter",lastHeader.frequencyChangeCounter,allocator);
    observed.AddMember("groupIndex",lastHeader.currentGroupIndex,allocator);
    rapidjson::Value frequencies(rapidjson::kArrayType),gains(rapidjson::kArrayType);
    for(float value:lastHeader.frequencies) frequencies.PushBack(value,allocator);
    for(float value:lastHeader.gains) gains.PushBack(value,allocator);
    observed.AddMember("perChannelFrequencyHz",frequencies,allocator);
    observed.AddMember("perChannelGainDb",gains,allocator);
    observed.AddMember("synchronized",HeimdallFrame::is_synchronized_data(lastHeader),allocator);
  }
  document.AddMember("observedFrame",observed,allocator);
  rapidjson::Value conditioning(rapidjson::kObjectType);
  conditioning.AddMember("sampleFormat","normalized-complex-float32",allocator);
  conditioning.AddMember("dcRemoval","per-frame-per-channel-mean",allocator);
  conditioning.AddMember("gainEqualization","attenuate-to-minimum-reported-channel-gain",allocator);
  conditioning.AddMember("inputGainSource","heimdall-v2-per-frame-metadata",allocator);
  conditioning.AddMember("alreadyAppliedToRecordedIq",true,allocator);
  document.AddMember("conditioning",conditioning,allocator);
  rapidjson::StringBuffer buffer;
  rapidjson::Writer<rapidjson::StringBuffer> writer(buffer); document.Accept(writer);
  return {buffer.GetString(),buffer.GetSize()};
}

void Kraken::start()
{
  running = true;
}

void Kraken::stop()
{
  running = false;
  if (socketFd >= 0)
  {
    shutdown(socketFd, SHUT_RDWR);
    close(socketFd);
    socketFd = -1;
  }
}

void Kraken::process(IqData *buffer1, IqData *buffer2)
{
  process(std::vector<IqData *>{buffer1, buffer2});
}

void Kraken::process(const std::vector<IqData *>& buffers)
{
  if (buffers.empty() || buffers.size() > MAX_CHANNELS ||
      buffers.size() != channelCount)
    throw std::invalid_argument("Kraken buffers must match configured channels");
  process_heimdall(buffers);
}

void Kraken::process_heimdall(const std::vector<IqData *>& buffers)
{
  while (running)
  {
    try
    {
      stream_heimdall(buffers);
    }
    catch (const ConnectionError& error)
    {
      if (socketFd >= 0)
      {
        close(socketFd);
        socketFd = -1;
      }
      if (!running) return;
      recording_epoch_discontinuity("Kraken input connection was interrupted");
      clipRecorder->discontinuity("Kraken input connection was interrupted");
      samplePositionAvailable.store(false, std::memory_order_release);
      {
        std::lock_guard<std::mutex> lock(provenanceMutex);
        lastHeaderAvailable=false;
      }
      clear_buffers(buffers);
      std::cerr << "[Kraken] " << error.what()
        << "; retrying in 1 second" << std::endl;
      std::this_thread::sleep_for(std::chrono::seconds(1));
    }
  }
}

void Kraken::stream_heimdall(const std::vector<IqData *>& buffers)
{
  connect_heimdall();
  std::optional<uint32_t> frequencyChangeCounter;
  std::vector<float> frequencies, gains;
  bool resetPending = false;
  while (running)
  {
    const auto headerBytes = receive_header();
    auto header = HeimdallFrame::decode_header(headerBytes);
    std::vector<uint8_t> metadata(HeimdallFrame::metadata_size(header));
    receive_exact(metadata.data(), metadata.size());
    HeimdallFrame::decode_metadata(header, metadata);
    std::vector<uint8_t> payload(HeimdallFrame::payload_size(header));
    receive_exact(payload.data(), payload.size());
    const int64_t receiptNanoseconds = wall_clock_nanoseconds();

    if (header.numChannels != buffers.size())
      throw std::runtime_error(
        "Heimdall V2 channel count does not match BLAH2 configuration");
    for (float frequency : header.frequencies)
      if (std::abs(static_cast<double>(frequency) - fc) > 256.0)
        throw std::runtime_error(
          "Heimdall V2 frequency does not match BLAH2 configuration");

    if (frequencyChangeCounter &&
        *frequencyChangeCounter != header.frequencyChangeCounter) {
      recording_epoch_discontinuity("Kraken input retuned during recording");
      clipRecorder->discontinuity("Kraken input retuned during clip recording");
      samplePositionAvailable.store(false, std::memory_order_release);
      resetPending = true;
    }
    frequencyChangeCounter = header.frequencyChangeCounter;
    if (!HeimdallFrame::is_synchronized_data(header))
    {
      recording_epoch_discontinuity("Kraken input synchronization was lost");
      clipRecorder->discontinuity("Kraken input synchronization was lost during clip recording");
      samplePositionAvailable.store(false, std::memory_order_release);
      resetPending = true;
      continue;
    }
    if (!resetPending && ((!frequencies.empty() && frequencies != header.frequencies) ||
        (!gains.empty() && gains != header.gains))) {
      recording_epoch_discontinuity(
        "Kraken input per-channel frequency or gain metadata changed");
      clipRecorder->discontinuity(
        "Kraken input per-channel frequency or gain metadata changed");
      samplePositionAvailable.store(false, std::memory_order_release);
      resetPending = true;
    }
    frequencies = header.frequencies;
    gains = header.gains;
    {
      std::lock_guard<std::mutex> lock(provenanceMutex);
      lastHeader=header; lastHeaderAvailable=true;
    }
    if (resetPending)
    {
      samplePositionAvailable.store(false, std::memory_order_release);
      clear_buffers(buffers);
      resetPending = false;
    }

    const auto channels = HeimdallFrame::decode_payload(header, payload);
    for (auto *buffer : buffers) buffer->lock();
    for (std::size_t channel = 0; channel < channels.size(); channel++)
      buffers[channel]->append_unlocked(channels[channel]);
    blah2::KrakenClipRecorder::Position position;
    const bool recorded = clipRecorder->ingest(headerBytes, std::move(metadata),
      std::move(payload), header, &position, receiptNanoseconds);
    const bool hostReceiptDiscontinuity = recorded && position.discontinuity;
    if (recorded && position.available)
    {
      sampleEpoch.store(position.epoch, std::memory_order_relaxed);
      nextEpochSample.store(position.nextSample, std::memory_order_relaxed);
      sampleEpochEstimateNanoseconds.store(position.epochEstimateNanoseconds,
        std::memory_order_relaxed);
      hostReceiptResidualNanoseconds.store(position.hostReceiptResidualNanoseconds,
        std::memory_order_relaxed);
      hostReceiptMaximumResidualNanoseconds.store(
        position.hostReceiptMaximumResidualNanoseconds, std::memory_order_relaxed);
      hostReceiptDriftPpm.store(position.hostReceiptDriftPpm,
        std::memory_order_relaxed);
      hostReceiptAnchors.store(position.hostReceiptAnchors,
        std::memory_order_relaxed);
      samplePositionAvailable.store(true, std::memory_order_release);
    }
    else samplePositionAvailable.store(false, std::memory_order_release);
    for (auto it = buffers.rbegin(); it != buffers.rend(); ++it)
      (*it)->unlock();

    if (hostReceiptDiscontinuity) {
      // The current frame begins a new host/sample epoch. Do not combine it
      // with pre-step buffered samples in one CPI or recording interval.
      recording_epoch_discontinuity("Kraken host-receipt/sample anchor discontinuity");
      clear_buffers(buffers);
      continue;
    }

    Source::SampleTimePosition recordingStart;
    if (recorded && position.available &&
        position.nextSample >= header.samplesPerChannel)
      recordingStart = {true, position.epoch,
        position.nextSample - header.samplesPerChannel,
        position.epochEstimateNanoseconds, fs};
    record_block(channels, recordingStart);
  }
}

void Kraken::connect_heimdall()
{
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo *addresses = nullptr;
  const std::string port = std::to_string(heimdallPort);
  if (getaddrinfo(heimdallHost.c_str(), port.c_str(), &hints, &addresses) != 0)
    throw ConnectionError("Unable to resolve Heimdall V2 host");
  std::unique_ptr<addrinfo, decltype(&freeaddrinfo)> guard(addresses, freeaddrinfo);
  for (auto *address = addresses; address; address = address->ai_next)
  {
    socketFd = socket(address->ai_family, address->ai_socktype,
      address->ai_protocol);
    if (socketFd >= 0 &&
        connect(socketFd, address->ai_addr, address->ai_addrlen) == 0)
    {
      std::cout << "[Kraken] Connected to KrakenSDR Suite V2 Heimdall at "
        << heimdallHost << ":" << heimdallPort << std::endl;
      return;
    }
    if (socketFd >= 0) close(socketFd);
    socketFd = -1;
  }
  throw ConnectionError("Unable to connect to Heimdall V2");
}

void Kraken::receive_exact(void *data, std::size_t length)
{
  auto *bytes = static_cast<uint8_t *>(data);
  std::size_t received = 0;
  while (received < length)
  {
    const ssize_t count = recv(socketFd, bytes + received, length - received, 0);
    if (count == 0) throw ConnectionError("Heimdall V2 closed connection");
    if (count < 0)
    {
      if (errno == EINTR) continue;
      throw ConnectionError(std::strerror(errno));
    }
    received += static_cast<std::size_t>(count);
  }
}

std::array<uint8_t, HeimdallFrame::FIXED_HEADER_SIZE> Kraken::receive_header()
{
  constexpr std::array<uint8_t, 4> magic{'M', 'C', 'H', 'Q'};
  std::array<uint8_t, HeimdallFrame::FIXED_HEADER_SIZE> header{};
  std::size_t matched = 0;
  while (matched < magic.size())
  {
    uint8_t byte;
    receive_exact(&byte, 1);
    if (byte == magic[matched])
      header[matched++] = byte;
    else
      matched = byte == magic[0] ? 1 : 0;
  }
  receive_exact(header.data() + magic.size(), header.size() - magic.size());
  return header;
}

void Kraken::clear_buffers(const std::vector<IqData *>& buffers)
{
  for (auto *buffer : buffers) buffer->lock();
  for (auto *buffer : buffers) buffer->clear();
  for (auto it = buffers.rbegin(); it != buffers.rend(); ++it)
    (*it)->unlock();
}
