// Real HackRf.cpp with local libhackrf symbols: no USB enumeration or radio IO.
#include "capture/hackrf/HackRf.h"
#include <array>
#include <cassert>
#include <cstring>
#include <iostream>
#include <stdexcept>

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
std::array<Applied, 2> applied;
hackrf_device devices[2]{{0}, {1}};
hackrf_device_list_t deviceList{};
std::string failAt;
unsigned opened = 0, closed = 0, initialized = 0, exited = 0;
int result(const std::string& operation) { return operation == failAt ? HACKRF_ERROR_OTHER : HACKRF_SUCCESS; }
int channelResult(const char* operation, hackrf_device* device) { return result(std::string(operation) + std::to_string(device->channel)); }
void reset() {
  applied = {}; failAt.clear();
  opened = closed = initialized = exited = 0;
  deviceList = {}; deviceList.devicecount = 2;
}
}
int hackrf_init() { const int r = result("init"); if (!r) ++initialized; return r; }
int hackrf_exit() { ++exited; return HACKRF_SUCCESS; }
hackrf_device_list_t* hackrf_device_list() { return failAt == "list" ? nullptr : &deviceList; }
void hackrf_device_list_free(hackrf_device_list_t*) {}
int hackrf_open_by_serial(const char* serial, hackrf_device** output) {
  const unsigned channel = std::strcmp(serial, "first") == 0 ? 0 : 1;
  assert(std::strcmp(serial, channel == 0 ? "first" : "second") == 0);
  const auto r = channelResult("open", &devices[channel]);
  if (!r) { *output = &devices[channel]; ++opened; }
  return r;
}
int hackrf_close(hackrf_device*) { ++closed; return HACKRF_SUCCESS; }
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
  for (uint32_t rate : {2000000u, 6000000u, 20000000u}) {
    reset();
    HackRf receiver("HackRF", 527000000, rate, "/unused", &record,
      {"first", "second"}, {16, 32}, {20, 40}, {false, true});
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
      uint8_t bytes[4]{1, 2, 3, 4};
      hackrf_transfer transfer{};
      transfer.device = &devices[channel]; transfer.buffer = bytes;
      transfer.buffer_length = transfer.valid_length = sizeof(bytes);
      transfer.rx_ctx = applied[channel].context;
      assert(applied[channel].callback(&transfer) == 0);
      transfer.valid_length = 3;
      assert(applied[channel].callback(&transfer) != 0);
    }
    assert(first.get_length() == 2 && second.get_length() == 2);
    assert(first.get_data()[1] == std::complex<double>(3, 4));
    receiver.stop(); receiver.stop();
    assert(opened == closed && initialized == exited);
  }
  unsigned failureCases = 0;
  for (const auto& stage : {"init", "list", "open1", "frequency1", "rate1", "amp1", "lna1", "vga1", "sync1", "clock1", "open0", "frequency0", "rate0", "amp0", "lna0", "vga0", "stream1", "stream0"}) {
    reset(); failAt = stage;
    HackRf receiver("HackRF", 527000000, 6000000, "/unused", &record,
      {"first", "second"}, {16, 32}, {20, 40}, {false, true});
    bool failed = false;
    try { receiver.start(); receiver.process(nullptr, nullptr); }
    catch (const std::exception&) { failed = true; }
    assert(failed && opened == closed && initialized == exited);
    receiver.stop(); receiver.stop();
    assert(opened == closed && initialized == exited);
    ++failureCases;
  }
  std::cout << "HackRF mocked SDK: all settings and paired callbacks at 2/6/20 MS/s, "
    << failureCases << " setter/start failures and idempotent cleanup passed; no hardware opened.\n";
}
