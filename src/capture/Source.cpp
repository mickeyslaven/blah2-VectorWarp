#include "Source.h"

#include <iostream>
#include <algorithm>
#include <chrono>
#include <ctime>
#include <sstream>
#include <iomanip>
#include <filesystem>
#include <cctype>
#include <stdexcept>

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
  if (recordingActive) return recordingFile;
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
    recordedSamples = 0; recordingError.clear(); recordingActive = true;
  } catch (const std::exception& error) { recording_failed(error.what()); throw; }
  std::cout << "Recording IQ: " << recordingFile << std::endl;
  return recordingFile;
}

void Source::close_file()
{
  std::lock_guard<std::mutex> lock(recordingMutex);
  recordingActive = false;
  if (!recordingWriter) return;
  try {
    if (!recordingWriter->samples())
      throw std::runtime_error("No samples were recorded");
    flush_recording_locked();
    for (const auto& pending : pendingChannels)
      if (!pending.empty()) throw std::runtime_error("Recording ended with unpaired input samples");
    recordingWriter->close(); recordedSamples = recordingWriter->samples();
  }
  catch (const std::exception& error) { recording_failed(error.what()); throw; }
  recordingWriter.reset(); pendingChannels.clear();
}

void Source::recording_failed(const std::string& error) {
  if (recordingWriter) recordedSamples = recordingWriter->samples();
  recordingActive = false; recordingError = error; recordingWriter.reset();
  pendingChannels.clear();
  std::cerr << "Recording stopped: " << error << '\n';
}

Source::RecordingStatus Source::recording_status() const {
  std::lock_guard<std::mutex> lock(recordingMutex);
  return {recordingActive.load(), recordingFile, recordingError,
    recordingWriter ? recordingWriter->samples() : recordedSamples};
}

void Source::recording_discontinuity(const std::string& error) {
  std::lock_guard<std::mutex> lock(recordingMutex);
  if (recordingWriter) recording_failed(error);
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
  if (!recordingActive) return;
  std::lock_guard<std::mutex> lock(recordingMutex);
  if (!recordingActive || !recordingWriter) return;
  try { recordingWriter->append(samples); }
  catch (const std::exception& error) { recording_failed(error.what()); }
}

void Source::record_channel(unsigned channel, uint64_t firstSample,
    const std::vector<std::complex<float>>& samples) {
  if (!recordingActive) return;
  std::lock_guard<std::mutex> lock(recordingMutex);
  if (!recordingActive || !recordingWriter) return;
  try {
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
