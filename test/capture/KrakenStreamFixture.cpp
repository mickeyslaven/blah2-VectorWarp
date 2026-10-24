// Actual Kraken socket capture with loopback Heimdall frames, not an SDR.
#include "capture/kraken/Kraken.h"
#include <arpa/inet.h>
#include <sys/socket.h>
#include <poll.h>
#include <unistd.h>
#include <cassert>
#include <chrono>
#include <cstring>
#include <future>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <rapidjson/document.h>
#include <rapidjson/istreamwrapper.h>
#include <thread>

namespace {
void be(std::vector<uint8_t>& out, uint32_t value) {
  for (int shift : {24, 16, 8, 0}) out.push_back((value >> shift) & 255);
}
void le_float(std::vector<uint8_t>& out, float value) {
  uint32_t bits; std::memcpy(&bits, &value, sizeof(bits));
  for (int shift : {0, 8, 16, 24}) out.push_back((bits >> shift) & 255);
}
std::vector<uint8_t> frame(unsigned channels = 5, unsigned counter = 0,
    unsigned phase = 4, float frequency = 527000000.0F, float gain = 15.0F,
    unsigned noise = 0, unsigned retuning = 0) {
  std::vector<uint8_t> out;
  for (uint32_t word : {0x4d434851u, channels, 8u, phase, noise, counter, 0u, retuning}) be(out, word);
  for (unsigned ch = 0; ch < channels; ++ch) { le_float(out, frequency); le_float(out, gain); }
  for (unsigned ch = 0; ch < channels; ++ch)
    for (unsigned sample = 0; sample < 8; ++sample) {
      out.push_back(100 + ch + sample); out.push_back(120 + ch + 2 * sample);
    }
  return out;
}
struct Capture {
  int listener = -1, client = -1;
  bool record = false;
  std::vector<std::unique_ptr<IqData>> storage;
  std::vector<IqData*> buffers;
  std::unique_ptr<Kraken> source;
  std::promise<std::string> finished;
  std::future<std::string> result = finished.get_future();
  std::thread worker;
  explicit Capture(const std::string& recordingPath = "/unused") {
    listener = socket(AF_INET, SOCK_STREAM, 0); assert(listener >= 0);
    sockaddr_in address{}; address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    assert(bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
    assert(listen(listener, 2) == 0);
    socklen_t size = sizeof(address);
    assert(getsockname(listener, reinterpret_cast<sockaddr*>(&address), &size) == 0);
    for (unsigned ch = 0; ch < 5; ++ch) { storage.emplace_back(new IqData(128)); buffers.push_back(storage.back().get()); }
    source.reset(new Kraken("Kraken", 527000000, 2400000, recordingPath, &record,
      5, "127.0.0.1", ntohs(address.sin_port)));
    source->start();
    worker = std::thread([this] {
      try { source->process(buffers); finished.set_value("stopped"); }
      catch (const std::exception& error) { finished.set_value(error.what()); }
    });
    accept_next();
  }
  void accept_next() {
    pollfd waiting{listener, POLLIN, 0}; assert(poll(&waiting, 1, 4000) == 1);
    client = accept(listener, nullptr, nullptr); assert(client >= 0);
  }
  void send_bytes(const std::vector<uint8_t>& bytes, bool fragmented = false) {
    for (size_t offset = 0; offset < bytes.size();) {
      const auto size = fragmented ? std::min<size_t>(7, bytes.size() - offset) : bytes.size() - offset;
      const auto sent = send(client, bytes.data() + offset, size, MSG_NOSIGNAL);
      assert(sent > 0); offset += static_cast<size_t>(sent);
    }
  }
  void disconnect() { shutdown(client, SHUT_RDWR); close(client); client = -1; }
  std::string finish(bool addSentinel = true) {
    // A deliberately mismatched complete frame terminates the actual capture
    // thread after every preceding frame; buffer assertions cannot race input.
    if (addSentinel) send_bytes(frame(4));
    assert(result.wait_for(std::chrono::seconds(4)) == std::future_status::ready);
    auto text = result.get(); worker.join();
    if (addSentinel) assert(text.find("channel count does not match") != std::string::npos);
    return text;
  }
  void length(unsigned count) { for (const auto* buffer : buffers) assert(const_cast<IqData*>(buffer)->get_length() == count); }
  void wait_recording(const std::string& previous) {
    for (unsigned attempt = 0; attempt < 400; ++attempt) {
      const auto status = source->recording_status();
      if (status.active && status.file != previous && status.samples == 8) return;
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    assert(false && "Kraken recording did not roll to the new setting epoch");
  }
  ~Capture() {
    source->stop();
    if (client >= 0) close(client);
    if (listener >= 0) close(listener);
    if (worker.joinable()) worker.join();
  }
};
}
int main() {
  { Capture c; c.send_bytes({'x', 'M', 'x'}); c.send_bytes(frame(), true); c.finish(); c.length(8); }
  { Capture c; c.send_bytes(frame()); c.send_bytes(frame(5, 1)); c.send_bytes(frame(5, 1)); c.finish(); c.length(16); }
  // A per-channel setting change is an epoch boundary even when the Suite's
  // frequency-change counter does not advance.
  { Capture c; c.send_bytes(frame()); c.send_bytes(frame(5, 0, 4, 527000000.0F, 16.0F));
    c.finish(); c.length(8); }
  { Capture c; c.send_bytes(frame()); c.send_bytes(frame(5, 0, 4, 527000128.0F, 15.0F));
    c.finish(); c.length(8); }
  { Capture c; c.send_bytes(frame()); c.send_bytes(frame(5, 0, 0)); c.send_bytes(frame()); c.finish(); c.length(8); }
  // Latest Suite tags non-coherent streams even when the low calibration byte
  // happens to be CALIBRATED.  They must create a discontinuity and never be
  // appended to the coherent passive-radar buffers.
  for (unsigned nonCoherent : {0x400u, 0x800u}) {
    Capture c; c.send_bytes(frame()); c.send_bytes(frame(5, 0, 4 | nonCoherent));
    c.send_bytes(frame()); c.finish(); c.length(8);
  }
  // Active calibration noise and an in-progress retune also delimit an input
  // epoch; following coherent payload may resume only after buffers reset.
  { Capture c; c.send_bytes(frame()); c.send_bytes(frame(5, 0, 4, 527000000.0F, 15.0F, 1));
    c.send_bytes(frame()); c.finish(); c.length(8); }
  { Capture c; c.send_bytes(frame()); c.send_bytes(frame(5, 0, 4, 527000000.0F, 15.0F, 0, 1));
    c.send_bytes(frame()); c.finish(); c.length(8); }
  { Capture c; c.send_bytes(frame()); auto partial = frame(); partial.resize(83);
    c.send_bytes(partial); c.disconnect(); c.accept_next(); c.send_bytes(frame(), true); c.finish(); c.length(8); }
  for (float value : {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(),
      -std::numeric_limits<float>::infinity()}) {
    { Capture c; c.send_bytes(frame(5, 0, 4, value));
      assert(c.finish(false).find("RF frequency metadata") != std::string::npos); c.length(0); }
    { Capture c; c.send_bytes(frame(5, 0, 4, 527000000.0F, value));
      assert(c.finish(false).find("gain metadata") != std::string::npos); c.length(0); }
  }
  // Portable IQ is already DC-removed and gain-equalized. A setting change
  // closes the old file incomplete and lazily records the new epoch with the
  // new observed gain and exact source-relative starting sample.
  {
    char pattern[] = "/tmp/vectorwarp-kraken-setting-XXXXXX";
    const char* directory = mkdtemp(pattern); assert(directory);
    Capture c(directory);
    const auto oldPath = c.source->open_file();
    c.send_bytes(frame());
    for (unsigned attempt = 0; attempt < 400 &&
        c.source->recording_status().samples != 8; ++attempt)
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    assert(c.source->recording_status().samples == 8);
    c.send_bytes(frame(5, 0, 4, 527000000.0F, 16.0F));
    c.wait_recording(oldPath);
    const auto newPath = c.source->recording_status().file;
    c.finish(); c.length(8);
    c.source->close_file();
    std::ifstream sidecar(newPath + ".provenance.json"); assert(sidecar);
    rapidjson::IStreamWrapper stream(sidecar); rapidjson::Document provenance;
    provenance.ParseStream(stream); assert(!provenance.HasParseError());
    const auto& details = provenance["receiverDetails"];
    assert(details["conditioning"]["alreadyAppliedToRecordedIq"].GetBool());
    assert(details["observedFrame"]["perChannelGainDb"][0].GetFloat() == 16.0F);
    assert(provenance["timing"]["sourceFirstSample"] == "0");
    blah2::ReplayOptions options; options.receiver = "Kraken"; options.channels = 5;
    options.sampleRate = 2400000; options.frequency = 527000000;
    bool incomplete = false;
    try { blah2::RecordingReader old(oldPath, options); }
    catch (const std::exception& error) {
      incomplete = std::string(error.what()).find("not finished cleanly") != std::string::npos;
    }
    assert(incomplete);
    blah2::RecordingReader recovered(newPath, options);
    assert(recovered.provenance().contractValid && recovered.provenance().continuous &&
      !recovered.provenance().decisionGrade);
    std::filesystem::remove(oldPath); std::filesystem::remove(oldPath + ".provenance.json");
    std::filesystem::remove(newPath); std::filesystem::remove(newPath + ".provenance.json");
    std::filesystem::remove(directory);
  }
  std::cout << "Kraken actual socket capture: setting-boundary, fragmentation/resync, calibration/noise/retune/noncoherent loss, recording rollover/replay, partial disconnect/reconnect and nonfinite cases passed. No SDR opened.\n";
}
