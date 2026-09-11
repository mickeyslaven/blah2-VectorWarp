#include "HackRf.h"

#include <iostream>
#include <complex>
#include <stdexcept>
#include <unordered_set>

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
  // global hackrf config
  int status;
  status = hackrf_init();
  check_status(status, "Failed to initialise HackRF");
  apiStarted = true;
  hackrf_device_list_t *list;
  list = hackrf_device_list();
  const bool haveTwo = list && list->devicecount >= 2;
  if (list) hackrf_device_list_free(list);
  if (!haveTwo)
  {
    check_status(-1, "Failed to find 2 HackRF devices.");
  }

  // surveillance config
  status = hackrf_open_by_serial(serial[1].c_str(), &dev[1]);
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
  status = hackrf_open_by_serial(serial[0].c_str(), &dev[0]);
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
}

void HackRf::process(IqData *buffer1, IqData *buffer2)
{
  try {
    int status;
    channels[0] = {this, buffer1, 0, 0};
    channels[1] = {this, buffer2, 1, 0};
    status = hackrf_start_rx(dev[1], rx_callback, &channels[1]);
    check_status(status, "Failed to start RX streaming.");
    status = hackrf_start_rx(dev[0], rx_callback, &channels[0]);
    check_status(status, "Failed to start RX streaming.");
  } catch (...) {
    stop();
    throw;
  }
}

int HackRf::rx_callback(hackrf_transfer* transfer)
{
  if (!transfer || !transfer->rx_ctx || !transfer->buffer) return -1;
  auto& channel = *static_cast<ChannelContext*>(transfer->rx_ctx);
  IqData* buffer_blah2 = channel.buffer;
  int8_t* buffer_hackrf = (int8_t*) transfer->buffer;
  if (transfer->valid_length < 0 || transfer->valid_length > transfer->buffer_length || transfer->valid_length % 2) {
    channel.source->recording_discontinuity("HackRF callback has an invalid sample length");
    return -1;
  }

  buffer_blah2->lock();

  for (int i = 0; i < transfer->valid_length; i=i+2)
  {
    double iqi = static_cast<double>(buffer_hackrf[i]);
    double iqq = static_cast<double>(buffer_hackrf[i+1]);
    buffer_blah2->push_back({iqi, iqq});
  }

  buffer_blah2->unlock();

  const auto count = static_cast<unsigned>(transfer->valid_length / 2);
  if (channel.source->is_recording() && count) {
    std::vector<std::complex<float>> samples(count);
    for (unsigned i=0; i<count; ++i)
      samples[i] = {float(buffer_hackrf[i*2]), float(buffer_hackrf[i*2+1])};
    channel.source->record_channel(channel.channel, channel.received, samples);
  }
  channel.received += count;

  return 0;
}
