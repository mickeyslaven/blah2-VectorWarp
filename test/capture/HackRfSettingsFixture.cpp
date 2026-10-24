// Real HackRf.cpp with local libhackrf symbols: no USB enumeration or radio IO.
#include "capture/hackrf/HackRf.h"
#include "data/meta/EventTime.h"
#include <array>
#include <cassert>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <rapidjson/document.h>
#include <rapidjson/istreamwrapper.h>
#include <stdexcept>
#include <unistd.h>
#include <vector>

struct hackrf_device { unsigned channel; };
namespace {
struct Applied {
  uint64_t frequency = 0;
  double rate = 0;
  uint32_t lna = 0, vga = 0;
  uint8_t amp = 0, sync = 0, clock = 0;
  hackrf_sample_block_cb_fn callback = nullptr;
  void* context = nullptr;
};
std::array<Applied, 3> applied;
hackrf_device devices[3]{{0}, {1}, {2}};
std::array<unsigned, 3> openCounts{};
hackrf_device_list_t deviceList{};
std::vector<std::string> listedSerials;
std::vector<char*> serialPointers;
std::string failAt;
unsigned opened = 0, closed = 0, initialized = 0, exited = 0, listed = 0, freed = 0;
int result(const std::string& operation) { return operation == failAt ? HACKRF_ERROR_OTHER : HACKRF_SUCCESS; }
int channelResult(const char* operation, hackrf_device* device) { return result(std::string(operation) + std::to_string(device->channel)); }
void reset(std::vector<std::string> found = {"000000000000000000000000000000a1", "000000000000000000000000000000b2"}) {
  applied = {}; failAt.clear();
  openCounts = {};
  opened = closed = initialized = exited = listed = freed = 0;
  listedSerials = std::move(found);
  serialPointers.clear();
  for (auto& value : listedSerials) serialPointers.push_back(value.empty() ? nullptr : value.data());
  deviceList = {}; deviceList.devicecount = static_cast<int>(listedSerials.size());
  deviceList.serial_numbers = serialPointers.data();
}
}
int hackrf_init() { const int r = result("init"); if (!r) ++initialized; return r; }
int hackrf_exit() { ++exited; return HACKRF_SUCCESS; }
hackrf_device_list_t* hackrf_device_list() {
  if (failAt == "list") return nullptr;
  ++listed; return &deviceList;
}
void hackrf_device_list_free(hackrf_device_list_t* list) {
  assert(list == &deviceList); ++freed;
}
int hackrf_device_list_open(hackrf_device_list_t* list, int index, hackrf_device** output) {
  assert(list == &deviceList && index >= 0 && index < list->devicecount);
  const unsigned channel = static_cast<unsigned>(index);
  assert(channel < 3);
  if (openCounts[channel]) return HACKRF_ERROR_BUSY;
  const auto r = channelResult("open", &devices[channel]);
  if (!r) { *output = &devices[channel]; ++opened; ++openCounts[channel]; }
  return r;
}
int hackrf_open_by_serial(const char* wanted, hackrf_device** output) {
  if (!wanted) return HACKRF_ERROR_NOT_FOUND;
  const std::string suffix(wanted);
  for (int index = 0; index < deviceList.devicecount; ++index) {
    if (!deviceList.serial_numbers[index]) continue;
    const std::string found(deviceList.serial_numbers[index]);
    if (found.size() >= suffix.size() &&
        found.compare(found.size() - suffix.size(), suffix.size(), suffix) == 0)
      return hackrf_device_list_open(&deviceList, index, output);
  }
  return HACKRF_ERROR_NOT_FOUND;
}
int hackrf_close(hackrf_device* device) {
  assert(device && openCounts[device->channel] == 1);
  --openCounts[device->channel]; ++closed; return HACKRF_SUCCESS;
}
int hackrf_stop_rx(hackrf_device*) { return HACKRF_SUCCESS; }
int hackrf_set_freq(hackrf_device* device, uint64_t value) { applied[device->channel].frequency = value; return channelResult("frequency", device); }
int hackrf_set_sample_rate(hackrf_device* device, double value) { applied[device->channel].rate = value; return channelResult("rate", device); }
int hackrf_set_amp_enable(hackrf_device* device, uint8_t value) { applied[device->channel].amp = value; return channelResult("amp", device); }
int hackrf_set_lna_gain(hackrf_device* device, uint32_t value) { applied[device->channel].lna = value; return channelResult("lna", device); }
int hackrf_set_vga_gain(hackrf_device* device, uint32_t value) { applied[device->channel].vga = value; return channelResult("vga", device); }
int hackrf_set_hw_sync_mode(hackrf_device* device, uint8_t value) { applied[device->channel].sync = value; return channelResult("sync", device); }
int hackrf_set_clkout_enable(hackrf_device* device, uint8_t value) { applied[device->channel].clock = value; return channelResult("clock", device); }
int hackrf_start_rx(hackrf_device* device, hackrf_sample_block_cb_fn callback, void* context) {
  applied[device->channel].callback = callback; applied[device->channel].context = context;
  return channelResult("stream", device);
}

int main() {
  bool record = false;
  const auto callback = [](unsigned channel, uint8_t* bytes, int valid, int capacity) {
    hackrf_transfer transfer{};
    transfer.device = &devices[channel]; transfer.buffer = bytes;
    transfer.buffer_length = capacity; transfer.valid_length = valid;
    transfer.rx_ctx = applied[channel].context;
    return applied[channel].callback(&transfer);
  };
  {
    const auto event = blah2::hackrf_cpi_sample_epoch(3, 10, 20, 2000000);
    assert(event.present && !event.estimateAvailable && event.epoch == 3);
    rapidjson::Document document(rapidjson::kObjectType);
    document.AddMember("eventTime", blah2::event_time_json(event,
      document.GetAllocator()), document.GetAllocator());
    assert(!document["eventTime"]["exact"].GetBool());
    assert(!document["eventTime"]["uncertaintyBounded"].GetBool());
    assert(document["eventTime"]["source"] ==
      "hackrf_shared_trigger_relative_sample_epoch");
  }
  for (uint32_t rate : {2000000u, 6000000u, 20000000u}) {
    reset();
    HackRf receiver("HackRF", 527000000, rate, "/unused", &record,
      {"a1", "b2"}, {16, 32}, {20, 40}, {false, true});
    receiver.start();
    IqData first(16), second(16);
    receiver.process(&first, &second);
    assert(applied[0].lna == 16 && applied[1].lna == 32);
    assert(applied[0].vga == 20 && applied[1].vga == 40);
    assert(applied[0].amp == 0 && applied[1].amp == 1);
    // Existing fixed synchronization roles: surveillance starts first and
    // provides CLKOUT; reference starts second. Physical wiring is required.
    assert(applied[1].sync == 1 && applied[1].clock == 1);
    for (unsigned channel = 0; channel < 2; ++channel) {
      assert(applied[channel].frequency == 527000000 && applied[channel].rate == rate);
      assert(applied[channel].context && applied[channel].callback);
    }
    uint8_t reference[4]{1, 2, 3, 4}, surveillance[4]{5, 6, 7, 8};
    assert(callback(0, reference, 4, 4) == 0);
    assert(first.get_length() == 0 && second.get_length() == 0);
    assert(callback(1, surveillance, 4, 4) == 0);
    assert(first.get_length() == 0 && second.get_length() == 0);
    assert(callback(0, reference, 4, 4) == 0);
    assert(callback(1, surveillance, 4, 4) == 0);
    assert(first.get_length() == 4 && second.get_length() == 4);
    assert(first.get_data()[3] == std::complex<double>(3, 4));
    assert(second.get_data()[3] == std::complex<double>(7, 8));
    const auto position = receiver.sample_time_position();
    assert(position.available && position.epoch == 1 && position.nextSample == 4);
    rapidjson::Document health;
    health.Parse(receiver.health_json().c_str());
    assert(!health.HasParseError() && health["pairing"]["qualified"].GetBool());
    assert(health["pairing"]["state"] == "qualified");
    assert(health["pairing"]["qualificationObservedPairs"].GetUint64() == 2);
    assert(std::string(health["pairing"]["pairedSamples"].GetString()) == "4");
    receiver.stop(); receiver.stop();
    assert(opened == closed && initialized == exited && listed == freed);
  }
  // A short callback invalidates only the ambiguous epoch. One callback from
  // each radio is drained, then two matched pairs automatically requalify.
  {
    reset();
    HackRf receiver("HackRF", 527000000, 6000000, "/unused", &record,
      {"a1", "b2"}, {16, 32}, {20, 40}, {false, true});
    receiver.start();
    IqData first(16), second(16); receiver.process(&first, &second);
    uint8_t bytes[4]{1, 2, 3, 4};
    assert(callback(0, bytes, 2, 4) == 0);
    assert(!first.get_length() && !second.get_length());
    assert(!receiver.sample_time_position().available);
    rapidjson::Document health; health.Parse(receiver.health_json().c_str());
    assert(std::string(health["pairing"]["discontinuities"].GetString()) == "1");
    assert(std::string(health["pairing"]["reason"].GetString()).find("short") != std::string::npos);
    assert(health["monitoring"].GetBool() && health["pairing"]["state"] == "requalifying");
    assert(callback(0, bytes, 4, 4) == 0); // drain guard
    assert(callback(1, bytes, 4, 4) == 0); // drain guard
    for (unsigned pair = 0; pair < 2; ++pair) {
      assert(callback(0, bytes, 4, 4) == 0);
      assert(callback(1, bytes, 4, 4) == 0);
    }
    assert(first.get_length() == 4 && second.get_length() == 4);
    const auto position = receiver.sample_time_position();
    assert(position.available && position.epoch == 2 && position.nextSample == 4);
    health.Parse(receiver.health_json().c_str());
    assert(health["pairing"]["state"] == "qualified");
    assert(std::string(health["pairing"]["requalifications"].GetString()) == "1");
    assert(std::string(health["pairing"]["consecutiveDiscontinuities"].GetString()) == "0");
    receiver.stop();
  }
  // A second callback from one device before its mate is a progression gap.
  {
    reset();
    HackRf receiver("HackRF", 527000000, 6000000, "/unused", &record,
      {"a1", "b2"}, {16, 32}, {20, 40}, {false, true});
    receiver.start();
    IqData first(16), second(16); receiver.process(&first, &second);
    uint8_t bytes[4]{1, 2, 3, 4};
    assert(callback(0, bytes, 4, 4) == 0);
    assert(callback(0, bytes, 4, 4) == 0);
    assert(!first.get_length() && !second.get_length());
    assert(callback(0, bytes, 4, 4) == 0); // drain guard
    assert(callback(1, bytes, 4, 4) == 0); // drain guard
    for (unsigned pair = 0; pair < 2; ++pair) {
      assert(callback(0, bytes, 4, 4) == 0);
      assert(callback(1, bytes, 4, 4) == 0);
    }
    assert(first.get_length() == 4 && second.get_length() == 4);
    assert(receiver.sample_time_position().available);
    receiver.stop();
  }
  // A one-sided software FIFO overflow/drop counter change invalidates both.
  {
    reset();
    HackRf receiver("HackRF", 527000000, 6000000, "/unused", &record,
      {"a1", "b2"}, {16, 32}, {20, 40}, {false, true});
    receiver.start();
    IqData first(8), second(8); receiver.process(&first, &second);
    for (unsigned sample = 0; sample < 9; ++sample) first.push_back({1, 1});
    uint8_t bytes[4]{1, 2, 3, 4};
    assert(callback(0, bytes, 4, 4) == 0);
    assert(callback(1, bytes, 4, 4) == 0);
    assert(callback(0, bytes, 4, 4) == 0);
    assert(callback(1, bytes, 4, 4) == 0);
    assert(!first.get_length() && !second.get_length());
    rapidjson::Document health; health.Parse(receiver.health_json().c_str());
    assert(!health["pairing"]["qualified"].GetBool());
    assert(std::string(health["pairing"]["discardedSamples"][0].GetString()) != "0");
    assert(callback(0, bytes, 4, 4) == 0); // drain guard
    assert(callback(1, bytes, 4, 4) == 0); // drain guard
    for (unsigned pair = 0; pair < 2; ++pair) {
      assert(callback(0, bytes, 4, 4) == 0);
      assert(callback(1, bytes, 4, 4) == 0);
    }
    assert(first.get_length() == 4 && second.get_length() == 4);
    assert(receiver.sample_time_position().available);
    receiver.stop();
  }
  // Repeated demonstrable faults eventually exhaust the declared retry budget.
  {
    reset();
    HackRf receiver("HackRF", 527000000, 6000000, "/unused", &record,
      {"a1", "b2"}, {16, 32}, {20, 40}, {false, true});
    receiver.start();
    IqData first(16), second(16); receiver.process(&first, &second);
    uint8_t bytes[4]{1, 2, 3, 4};
    for (unsigned fault = 0; fault < 8; ++fault)
      assert(callback(0, bytes, 2, 4) == 0);
    assert(callback(0, bytes, 2, 4) != 0);
    rapidjson::Document health; health.Parse(receiver.health_json().c_str());
    assert(!health["monitoring"].GetBool());
    assert(health["pairing"]["state"] == "unavailable");
    assert(std::string(health["pairing"]["retryBudget"].GetString()) == "8");
    assert(std::string(health["pairing"]["consecutiveDiscontinuities"].GetString()) == "9");
    receiver.stop();
  }
  // A pairing fault closes only the corrupt recording epoch. Requalification
  // lazily starts a new provisional file and replay preserves that boundary.
  {
    char pattern[] = "/tmp/vectorwarp-hackrf-fixture-XXXXXX";
    const char* directory = mkdtemp(pattern);
    assert(directory);
    reset();
    HackRf receiver("HackRF", 527000000, 6000000, directory, &record,
      {"a1", "b2"}, {16, 32}, {20, 40}, {false, true});
    receiver.start();
    IqData first(16), second(16); receiver.process(&first, &second);
    const auto path = receiver.open_file();
    uint8_t reference[4]{1, 2, 3, 4}, surveillance[4]{5, 6, 7, 8};
    for (unsigned pair = 0; pair < 2; ++pair) {
      assert(callback(0, reference, 4, 4) == 0);
      assert(callback(1, surveillance, 4, 4) == 0);
    }
    assert(callback(0, reference, 4, 4) == 0);
    assert(callback(0, reference, 4, 4) == 0);
    const auto incompleteRecording = receiver.recording_status();
    assert(!incompleteRecording.active && incompleteRecording.resumePending &&
      incompleteRecording.samples == 4 && !incompleteRecording.error.empty());
    std::ifstream provenance(path + ".provenance.json");
    assert(provenance);
    rapidjson::IStreamWrapper stream(provenance);
    rapidjson::Document document; document.ParseStream(stream);
    assert(!document.HasParseError());
    assert(document["schema"] == "vectorwarp-recording-provenance-v2");
    const auto& details = document["receiverDetails"];
    assert(details["schema"] == "vectorwarp-hackrf-recording-provenance-v1");
    assert(details["startup"]["requested"]["lnaGainDb"][0].GetUint() == 16);
    assert(details["startup"]["requested"]["vgaGainDb"][1].GetUint() == 40);
    assert(!details["startup"]["readbackAvailable"].GetBool());
    assert(std::string(details["startup"]["deviceIdentity"]["resolvedSerials"][0].GetString()) == "000000000000000000000000000000a1");
    assert(std::string(details["startup"]["deviceIdentity"]["resolvedSerials"][1].GetString()) == "000000000000000000000000000000b2");
    assert(std::string(details["pairing"]["pairing"]["discontinuities"].GetString()) == "1");
    assert(!details["pairing"]["pairing"]["qualified"].GetBool());
    assert(details["pairing"]["pairing"]["state"] == "requalifying");
    assert(!document["recording"]["complete"].GetBool());
    assert(!document["timing"]["discontinuities"].Empty());
    blah2::ReplayOptions options;
    options.receiver = "HackRF"; options.channels = 2;
    options.sampleRate = 6000000; options.frequency = 527000000;
    bool incomplete = false;
    try { blah2::RecordingReader reader(path, options); }
    catch (const std::exception& error) {
      incomplete = std::string(error.what()).find("not finished cleanly") != std::string::npos;
    }
    assert(incomplete);
    // Discard one post-fault callback from each radio, then buffer two matched
    // pairs before publishing and rolling to the new recording epoch.
    assert(callback(0, reference, 4, 4) == 0);
    assert(callback(1, surveillance, 4, 4) == 0);
    for (unsigned pair = 0; pair < 2; ++pair) {
      assert(callback(0, reference, 4, 4) == 0);
      assert(callback(1, surveillance, 4, 4) == 0);
    }
    assert(callback(0, reference, 4, 4) == 0);
    assert(callback(1, surveillance, 4, 4) == 0);
    auto recoveredRecording = receiver.recording_status();
    assert(recoveredRecording.active && !recoveredRecording.resumePending &&
      recoveredRecording.samples == 6 && recoveredRecording.file != path);
    const auto recoveredPath = recoveredRecording.file;
    receiver.close_file();
    std::ifstream recoveredProvenance(recoveredPath + ".provenance.json");
    assert(recoveredProvenance);
    rapidjson::IStreamWrapper recoveredStream(recoveredProvenance);
    rapidjson::Document recoveredDocument; recoveredDocument.ParseStream(recoveredStream);
    assert(!recoveredDocument.HasParseError());
    assert(recoveredDocument["recording"]["complete"].GetBool());
    assert(recoveredDocument["timing"]["epoch"] == "2");
    assert(recoveredDocument["timing"]["sourceFirstSample"] == "0");
    const auto& recoveredPairing = recoveredDocument["receiverDetails"]["pairing"]["pairing"];
    assert(recoveredPairing["state"] == "qualified");
    assert(std::string(recoveredPairing["requalifications"].GetString()) == "1");
    blah2::RecordingReader recoveredReader(recoveredPath, options);
    assert(recoveredReader.provenance().contractValid);
    assert(recoveredReader.provenance().continuous);
    assert(!recoveredReader.provenance().exactTime);
    assert(!recoveredReader.provenance().decisionGrade);
    blah2::IqBlock replayed;
    assert(recoveredReader.read(replayed) && replayed.size() == 2 &&
      replayed[0].size() == 4 && replayed[1].size() == 4);
    assert(recoveredReader.read(replayed) && replayed[0].size() == 2 &&
      replayed[1].size() == 2);
    assert(!recoveredReader.read(replayed));
    receiver.stop();
    std::filesystem::remove(path);
    std::filesystem::remove(path + ".provenance.json");
    std::filesystem::remove(recoveredPath);
    std::filesystem::remove(recoveredPath + ".provenance.json");
    std::filesystem::remove(directory);
  }
  unsigned failureCases = 0;
  for (const auto& stage : {"init", "list", "open1", "frequency1", "rate1", "amp1", "lna1", "vga1", "sync1", "clock1", "open0", "frequency0", "rate0", "amp0", "lna0", "vga0", "stream1", "stream0"}) {
    reset(); failAt = stage;
    HackRf receiver("HackRF", 527000000, 6000000, "/unused", &record,
      {"a1", "b2"}, {16, 32}, {20, 40}, {false, true});
    bool failed = false;
    try { receiver.start(); receiver.process(nullptr, nullptr); }
    catch (const std::exception&) { failed = true; }
    assert(failed && opened == closed && initialized == exited && listed == freed);
    receiver.stop(); receiver.stop();
    assert(opened == closed && initialized == exited && listed == freed);
    ++failureCases;
  }
  std::cout << "Unique short suffix settings and 18 SDK failure stages passed.\n" << std::flush;
  struct SerialScenario {
    std::vector<std::string> found, wanted;
    const char* reason;
  };
  const std::vector<SerialScenario> refused = {
    SerialScenario{{"00000000000000000000000000000001", "00000000000000000000000000000002"},
      {"01", "001"}, "same device"},
    SerialScenario{{"00000000000000000000000000000001", "00000000000000000000000000000101",
       "00000000000000000000000000000002"}, {"01", "02"}, "matches multiple"},
    SerialScenario{{"000000000000000000000000000000a1", "000000000000000000000000000000a1",
       "000000000000000000000000000000b2"}, {"a1", "b2"}, "matches multiple"},
    SerialScenario{{"0000000000000000000000000000ABCD", "00000000000000000000000000000002"},
      {"abcd", "02"}, "matches no connected"},
    SerialScenario{{"00000000000000000000000000000001", "00000000000000000000000000000002"},
      {"ff", "02"}, "matches no connected"}
  };
  for (const auto& scenario : refused) {
    reset(scenario.found);
    HackRf receiver("HackRF", 527000000, 6000000, "/unused", &record,
      scenario.wanted, {16, 32}, {20, 40}, {false, true});
    bool rejected = false;
    try { receiver.start(); }
    catch (const std::exception& error) {
      rejected = true;
      if (std::string(error.what()).find(scenario.reason) == std::string::npos)
        std::cerr << "Expected identity refusal '" << scenario.reason
          << "', got '" << error.what() << "'.\n";
      assert(std::string(error.what()).find(scenario.reason) != std::string::npos);
    }
    assert(rejected && opened == 0 && closed == 0 && initialized == exited && listed == freed);
    receiver.stop();
  }
  reset({"000000000000000000000000000000a1", "000000000000000000000000000000b2", ""});
  HackRf withUnknownEntry("HackRF", 527000000, 6000000, "/unused", &record,
    {"a1", "b2"}, {16, 32}, {20, 40}, {false, true});
  withUnknownEntry.start();
  withUnknownEntry.stop();
  assert(opened == closed && initialized == exited && listed == freed);
  reset({"000000000000000000000000000000A1", "000000000000000000000000000000b2"});
  HackRf caseSensitiveSuffixes("HackRF", 527000000, 6000000, "/unused", &record,
    {"A1", "b2"}, {16, 32}, {20, 40}, {false, true});
  caseSensitiveSuffixes.start();
  caseSensitiveSuffixes.stop();
  assert(opened == closed && initialized == exited && listed == freed);
  std::cout << "HackRF mocked SDK: all settings and paired callbacks at 2/6/20 MS/s, "
    << failureCases << " setter/start failures, serial identity refusals and idempotent cleanup passed; no hardware opened.\n";
}
