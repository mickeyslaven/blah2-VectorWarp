#include "HackRf.h"

#include <iostream>
#include <complex>
#include <array>
#include <memory>
#include <stdexcept>
#include <unordered_set>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

// constructor
HackRf::HackRf(std::string _type, uint32_t _fc, uint32_t _fs, 
  std::string _path, bool *_saveIq, std::vector<std::string> _serial,
  std::vector<uint32_t> _gainLna, std::vector<uint32_t> _gainVga, 
  std::vector<bool> _ampEnable)
    : Source(_type, _fc, _fs, _path, _saveIq)
{
  serial = _serial;
  ampEnable = _ampEnable;
  if (serial.size() != 2 || _gainLna.size() != 2 || _gainVga.size() != 2 ||
      ampEnable.size() != 2 || serial[0].empty() || serial[1].empty() || serial[0] == serial[1])
    throw std::invalid_argument("[HackRF] Two distinct serials and two gain/amplifier values are required.");

  // validate LNA gain
  std::unordered_set<uint32_t> validLna;
  for (uint32_t gain = 0; gain <= 40; gain += 8) {
    validLna.insert(gain);
  }
  for (uint32_t gain : _gainLna) {
    if (validLna.find(gain) == validLna.end()) {
      throw std::invalid_argument("Invalid LNA gain value");
    }
  }
  gainLna = _gainLna;

  // validate VGA gain
  std::unordered_set<uint32_t> validVga;
  for (uint32_t gain = 0; gain <= 62; gain += 2) {
    validVga.insert(gain);
  }
  for (uint32_t gain : _gainVga) {
    if (validVga.find(gain) == validVga.end()) {
      throw std::invalid_argument("Invalid VGA gain value");
    }
  }
  gainVga = _gainVga;
}

void HackRf::check_status(uint8_t status, std::string message)
{
  if (status != HACKRF_SUCCESS)
  {
    throw std::runtime_error("[HackRF] " + message);
  }
}

void HackRf::start()
{
  try {
  startupAccepted = false;
  // global hackrf config
  int status;
  status = hackrf_init();
  check_status(status, "Failed to initialise HackRF");
  apiStarted = true;
  std::unique_ptr<hackrf_device_list_t, decltype(&hackrf_device_list_free)>
    list(hackrf_device_list(), hackrf_device_list_free);
  if (!list || list->devicecount < 2 || !list->serial_numbers)
    throw std::runtime_error("[HackRF] Failed to find 2 HackRF devices with readable serials.");

  // libhackrf_open_by_serial accepts a suffix and chooses the first match.
  // Resolve each configured suffix uniquely against one enumeration snapshot,
  // then open its exact list index so USB enumeration order cannot swap roles.
  std::array<int, 2> selected{{-1, -1}};
  for (unsigned role = 0; role < 2; ++role) {
    unsigned matches = 0;
    for (int index = 0; index < list->devicecount; ++index) {
      if (!list->serial_numbers[index]) continue;
      const std::string found(list->serial_numbers[index]);
      if (found.size() >= serial[role].size() &&
          found.compare(found.size() - serial[role].size(),
            serial[role].size(), serial[role]) == 0) {
        selected[role] = index;
        ++matches;
      }
    }
    const char* name = role == 0 ? "reference" : "surveillance";
    if (matches == 0)
      throw std::runtime_error(std::string("[HackRF] Configured ") + name +
        " serial matches no connected HackRF.");
    if (matches != 1)
      throw std::runtime_error(std::string("[HackRF] Configured ") + name +
        " serial matches multiple HackRF devices; use a longer serial.");
  }
  if (selected[0] == selected[1])
    throw std::runtime_error("[HackRF] Reference and surveillance serials select the same device.");

  // surveillance config
  status = hackrf_device_list_open(list.get(), selected[1], &dev[1]);
  check_status(status, "Failed to open device.");
  status = hackrf_set_freq(dev[1], fc);
  check_status(status, "Failed to set frequency.");
  status = hackrf_set_sample_rate(dev[1], fs);
  check_status(status, "Failed to set sample rate.");
  status = hackrf_set_amp_enable(dev[1], ampEnable[1] ? 1 : 0);
  check_status(status, "Failed to set AMP status.");
  status = hackrf_set_lna_gain(dev[1], gainLna[1]);
  check_status(status, "Failed to set LNA gain.");
  status = hackrf_set_vga_gain(dev[1], gainVga[1]);
  check_status(status, "Failed to set VGA gain.");
  status = hackrf_set_hw_sync_mode(dev[1], 1);
  check_status(status, "Failed to enable hardware synchronising.");
  status = hackrf_set_clkout_enable(dev[1], 1); 
  check_status(status, "Failed to set CLKOUT on survillance device");


  // reference config
  status = hackrf_device_list_open(list.get(), selected[0], &dev[0]);
  check_status(status, "Failed to open device.");
  status = hackrf_set_freq(dev[0], fc);
  check_status(status, "Failed to set frequency.");
  status = hackrf_set_sample_rate(dev[0], fs);
  check_status(status, "Failed to set sample rate.");
  status = hackrf_set_amp_enable(dev[0], ampEnable[0] ? 1 : 0);
  check_status(status, "Failed to set AMP status.");
  status = hackrf_set_lna_gain(dev[0], gainLna[0]);
  check_status(status, "Failed to set LNA gain.");
  status = hackrf_set_vga_gain(dev[0], gainVga[0]);
  check_status(status, "Failed to set VGA gain.");
  startupAccepted = true;
  } catch (...) {
    stop();
    throw;
  }
}

void HackRf::stop()
{
  for (auto& device : dev) if (device) {
    hackrf_stop_rx(device); hackrf_close(device); device = nullptr;
  }
  if (apiStarted) { hackrf_exit(); apiStarted = false; }
  std::lock_guard<std::mutex> lock(pairingMutex);
  pairing.accepting = false;
  pairing.monitoring = false;
}

void HackRf::process(IqData *buffer1, IqData *buffer2)
{
  try {
    if (!buffer1 || !buffer2 || buffer1 == buffer2)
      throw std::invalid_argument("[HackRF] Two distinct IQ queues are required.");
    int status;
    channels[0] = {this, buffer1, 0};
    channels[1] = {this, buffer2, 1};
    reset_pairing(buffer1, buffer2);
    status = hackrf_start_rx(dev[1], rx_callback, &channels[1]);
    check_status(status, "Failed to start RX streaming.");
    status = hackrf_start_rx(dev[0], rx_callback, &channels[0]);
    check_status(status, "Failed to start RX streaming.");
    std::lock_guard<std::mutex> lock(pairingMutex);
    pairing.monitoring = true;
  } catch (...) {
    stop();
    throw;
  }
}

Source::SampleTimePosition HackRf::sample_time_position() const
{
  if (!qualified.load(std::memory_order_acquire)) return {};
  return {true, epoch.load(std::memory_order_acquire),
    nextSample.load(std::memory_order_acquire), 0, fs};
}

std::string HackRf::startup_receipt_json() const
{
  rapidjson::Document document(rapidjson::kObjectType);
  auto& allocator = document.GetAllocator();
  const bool accepted = startupAccepted.load();
  document.AddMember("schema", 1, allocator);
  document.AddMember("receiver", "HackRF", allocator);
  document.AddMember("status", rapidjson::Value(accepted ? "accepted" : "pending", allocator), allocator);
  rapidjson::Value requested(rapidjson::kObjectType);
  requested.AddMember("frequency", fc, allocator);
  requested.AddMember("sampleRate", fs, allocator);
  rapidjson::Value serials(rapidjson::kArrayType), lna(rapidjson::kArrayType),
    vga(rapidjson::kArrayType), amplifiers(rapidjson::kArrayType);
  for (unsigned channel = 0; channel < 2; ++channel) {
    serials.PushBack(rapidjson::Value(serial[channel].c_str(), allocator), allocator);
    lna.PushBack(gainLna[channel], allocator);
    vga.PushBack(gainVga[channel], allocator);
    amplifiers.PushBack(ampEnable[channel], allocator);
  }
  requested.AddMember("serials", serials, allocator);
  requested.AddMember("lnaGainDb", lna, allocator);
  requested.AddMember("vgaGainDb", vga, allocator);
  requested.AddMember("ampEnabled", amplifiers, allocator);
  document.AddMember("requested", requested, allocator);
  if (accepted) {
    rapidjson::Value synchronization(rapidjson::kObjectType);
    synchronization.AddMember("policy", "shared-clock-hardware-trigger-v1", allocator);
    synchronization.AddMember("clockOutputRole", "surveillance", allocator);
    synchronization.AddMember("hardwareTriggerRole", "surveillance", allocator);
    synchronization.AddMember("startOrder", "surveillance-then-reference", allocator);
    synchronization.AddMember("physicalWiringVerified", false, allocator);
    synchronization.AddMember("hardwareTimestampAvailable", false, allocator);
    document.AddMember("synchronization", synchronization, allocator);
  } else document.AddMember("synchronization", rapidjson::Value(rapidjson::kNullType), allocator);
  rapidjson::StringBuffer buffer;
  rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
  document.Accept(writer);
  return {buffer.GetString(), buffer.GetSize()};
}

std::string HackRf::health_json() const
{
  std::lock_guard<std::mutex> lock(pairingMutex);
  rapidjson::Document document(rapidjson::kObjectType);
  auto& allocator = document.GetAllocator();
  document.AddMember("schema", 1, allocator);
  document.AddMember("receiver", "HackRF", allocator);
  document.AddMember("monitoring", pairing.monitoring, allocator);
  rapidjson::Value state(rapidjson::kObjectType);
  state.AddMember("policy", "paired-callback-fail-closed-v1", allocator);
  state.AddMember("qualified", pairing.qualified, allocator);
  const auto decimal = [&](uint64_t value) {
    const auto text = std::to_string(value);
    return rapidjson::Value(text.c_str(), allocator);
  };
  state.AddMember("epoch", decimal(pairing.epoch), allocator);
  rapidjson::Value callbacks(rapidjson::kArrayType), samples(rapidjson::kArrayType),
    discarded(rapidjson::kArrayType);
  for (unsigned channel = 0; channel < 2; ++channel) {
    callbacks.PushBack(decimal(pairing.callbacks[channel]), allocator);
    samples.PushBack(decimal(pairing.samples[channel]), allocator);
    discarded.PushBack(decimal(pairing.discarded[channel]), allocator);
  }
  state.AddMember("callbacks", callbacks, allocator);
  state.AddMember("samples", samples, allocator);
  state.AddMember("pairedCallbacks", decimal(pairing.pairedCallbacks), allocator);
  state.AddMember("pairedSamples", decimal(pairing.pairedSamples), allocator);
  state.AddMember("discontinuities", decimal(pairing.discontinuities), allocator);
  state.AddMember("discardedSamples", discarded, allocator);
  state.AddMember("reason", rapidjson::Value(pairing.reason.c_str(), allocator), allocator);
  document.AddMember("pairing", state, allocator);
  rapidjson::StringBuffer buffer;
  rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
  document.Accept(writer);
  return {buffer.GetString(), buffer.GetSize()};
}

std::string HackRf::recording_provenance_json() const
{
  rapidjson::Document document(rapidjson::kObjectType);
  auto& allocator = document.GetAllocator();
  document.AddMember("schema", "vectorwarp-hackrf-recording-provenance-v1", allocator);
  for (const auto& item : {std::pair<const char*, std::string>{"startup", startup_receipt_json()},
                           {"pairing", health_json()}}) {
    rapidjson::Document parsed;
    parsed.Parse(item.second.c_str());
    if (parsed.HasParseError() || !parsed.IsObject())
      throw std::runtime_error("[HackRF] Could not serialize recording provenance.");
    rapidjson::Value copied;
    copied.CopyFrom(parsed, allocator);
    document.AddMember(rapidjson::Value(item.first, allocator), copied, allocator);
  }
  rapidjson::StringBuffer buffer;
  rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
  document.Accept(writer);
  return {buffer.GetString(), buffer.GetSize()};
}

void HackRf::reset_pairing(IqData* first, IqData* second)
{
  std::lock_guard<std::mutex> state(pairingMutex);
  std::unique_lock<IqData> firstLock(*first), secondLock(*second);
  pairing.discarded[0] += first->get_length();
  pairing.discarded[1] += second->get_length();
  first->clear(); second->clear();
  pairing.pending = {};
  pairing.callbacks = {};
  pairing.samples = {};
  pairing.pairedCallbacks = 0;
  pairing.pairedSamples = 0;
  pairing.initialDrops = {first->get_dropped_samples(), second->get_dropped_samples()};
  if (pairing.epoch == UINT64_MAX)
    throw std::overflow_error("[HackRF] Pairing epoch exhausted.");
  ++pairing.epoch;
  pairing.accepting = true;
  pairing.qualified = false;
  pairing.monitoring = false;
  pairing.reason = "awaiting first matched hardware-triggered callback pair";
  epoch.store(pairing.epoch, std::memory_order_release);
  nextSample.store(0, std::memory_order_release);
  qualified.store(false, std::memory_order_release);
}

void HackRf::invalidate_pair_locked(const std::string& reason)
{
  auto* first = channels[0].buffer;
  auto* second = channels[1].buffer;
  std::unique_lock<IqData> firstLock(*first), secondLock(*second);
  pairing.discarded[0] += first->get_length() + pairing.pending[0].size();
  pairing.discarded[1] += second->get_length() + pairing.pending[1].size();
  first->clear(); second->clear();
  pairing.pending = {};
  if (pairing.epoch == UINT64_MAX)
    pairing.reason = "pairing epoch exhausted after discontinuity";
  else ++pairing.epoch;
  ++pairing.discontinuities;
  pairing.accepting = false;
  pairing.qualified = false;
  pairing.monitoring = false;
  pairing.reason = reason;
  epoch.store(pairing.epoch, std::memory_order_release);
  nextSample.store(0, std::memory_order_release);
  qualified.store(false, std::memory_order_release);
}

int HackRf::rx_callback(hackrf_transfer* transfer)
{
  if (!transfer || !transfer->rx_ctx || !transfer->buffer) return -1;
  auto& channel = *static_cast<ChannelContext*>(transfer->rx_ctx);
  auto& source = *channel.source;
  const bool validLength = transfer->buffer_length > 0 &&
    transfer->valid_length == transfer->buffer_length &&
    transfer->valid_length % 2 == 0;
  const auto count = transfer->valid_length > 0 ?
    static_cast<uint64_t>(transfer->valid_length / 2) : 0;
  std::vector<std::complex<float>> incoming;
  if (validLength) {
    incoming.resize(count);
    const auto* bytes = reinterpret_cast<const int8_t*>(transfer->buffer);
    for (uint64_t i = 0; i < count; ++i)
      incoming[i] = {float(bytes[i*2]), float(bytes[i*2+1])};
  }

  std::string discontinuity;
  blah2::IqBlock recorded;
  {
    std::lock_guard<std::mutex> state(source.pairingMutex);
    auto& pairing = source.pairing;
    if (!pairing.accepting) return -1;
    ++pairing.callbacks[channel.channel];
    pairing.samples[channel.channel] += count;
    if (!validLength)
      discontinuity = "HackRF callback was short or had an invalid sample length";
    else if (!pairing.pending[channel.channel].empty())
      discontinuity = "HackRF callback progression diverged before its paired callback arrived";
    else pairing.pending[channel.channel] = std::move(incoming);

    if (discontinuity.empty() && !pairing.pending[0].empty() &&
        !pairing.pending[1].empty()) {
      if (pairing.pending[0].size() != pairing.pending[1].size() ||
          pairing.callbacks[0] != pairing.callbacks[1] ||
          pairing.samples[0] != pairing.samples[1]) {
        discontinuity = "HackRF paired callback sample progression diverged";
      } else {
        auto* first = source.channels[0].buffer;
        auto* second = source.channels[1].buffer;
        std::unique_lock<IqData> firstLock(*first), secondLock(*second);
        const auto currentFirst = first->get_length();
        const auto currentSecond = second->get_length();
        const auto pairSamples = pairing.pending[0].size();
        const bool dropChanged =
          first->get_dropped_samples() != pairing.initialDrops[0] ||
          second->get_dropped_samples() != pairing.initialDrops[1];
        if (dropChanged || currentFirst != currentSecond ||
            pairSamples > first->get_n() - std::min<uint64_t>(currentFirst, first->get_n()) ||
            pairSamples > second->get_n() - std::min<uint64_t>(currentSecond, second->get_n())) {
          discontinuity = "HackRF paired FIFO overflow or asymmetric queue progression";
        } else {
          first->append_unlocked(pairing.pending[0]);
          second->append_unlocked(pairing.pending[1]);
          ++pairing.pairedCallbacks;
          pairing.pairedSamples += pairSamples;
          pairing.qualified = true;
          pairing.reason = "paired hardware-triggered callbacks are aligned";
          source.nextSample.store(pairing.pairedSamples, std::memory_order_release);
          source.qualified.store(true, std::memory_order_release);
          if (source.is_recording()) {
            recorded.resize(2);
            recorded[0] = pairing.pending[0];
            recorded[1] = pairing.pending[1];
          }
          pairing.pending = {};
        }
      }
    }
    if (!discontinuity.empty()) source.invalidate_pair_locked(discontinuity);
  }
  if (!discontinuity.empty()) {
    source.recording_discontinuity(discontinuity);
    return -1;
  }
  if (!recorded.empty()) source.record_block(recorded);
  return 0;
}
