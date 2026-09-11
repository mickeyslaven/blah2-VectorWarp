#include "capture/usrp/UsrpReadback.h"
#include "capture/usrp/UsrpSettings.h"
#include "capture/usrp/UsrpStream.h"
#include <array>
#include <cassert>
#include <iostream>
#include <limits>
struct Spec { std::string value = "A:A A:B"; std::string to_string() const { return value; } };
struct Receiver {
  size_t count = 2;
  Spec spec;
  std::array<double, 2> rate{2000000, 2000000}, frequency{100000000, 100000000}, gain{10, 20};
  std::array<std::string, 2> antenna{"RX2", "RX2"};
  std::vector<std::string> calls;
  int coerceChannel = -1;
  void set_rx_subdev_spec(Spec value, size_t board) { assert(board == 0); spec = value; calls.push_back("subdev"); }
  void set_rx_rate(double value, size_t channel) {
    rate[channel] = value + (coerceChannel == static_cast<int>(channel) ? 1 : 0);
    calls.push_back("rate" + std::to_string(channel));
  }
  void set_rx_freq(double value, size_t channel) { frequency[channel] = value; calls.push_back("freq" + std::to_string(channel)); }
  void set_rx_gain(double value, size_t channel) { gain[channel] = value; calls.push_back("gain" + std::to_string(channel)); }
  void set_rx_antenna(std::string value, size_t channel) { antenna[channel] = value; calls.push_back("antenna" + std::to_string(channel)); }
  size_t get_rx_num_channels() { return count; }
  Spec get_rx_subdev_spec(size_t) { return spec; }
  double get_rx_rate(size_t channel) { return rate[channel]; }
  double get_rx_freq(size_t channel) { return frequency[channel]; }
  double get_rx_gain(size_t channel) { return gain[channel]; }
  std::string get_rx_antenna(size_t channel) { return antenna[channel]; }
};
void verify(Receiver& value) { verify_usrp_readback(value, 100000000, 2000000, {10, 20}, {"RX2", "RX2"}, "A:A A:B"); }
void rejects(Receiver value) {
  bool rejected = false;
  try { verify(value); } catch (const std::exception&) { rejected = true; }
  assert(rejected);
}
int main() {
  struct Metadata {
    enum Error { ERROR_CODE_NONE, TIMEOUT, OVERFLOW, BROKEN_CHAIN, BAD_PACKET };
    Error error_code = ERROR_CODE_NONE;
    bool out_of_sequence = false;
    std::string strerror() const { return "mock receive error"; }
  } metadata;
  verify_usrp_receive_capacity(4096);
  verify_usrp_receive(metadata, 4096, 4096);
  for (auto error : {Metadata::TIMEOUT, Metadata::OVERFLOW, Metadata::BROKEN_CHAIN, Metadata::BAD_PACKET}) {
    metadata.error_code = error;
    bool rejected = false;
    try { verify_usrp_receive(metadata, 100, 4096); }
    catch (const std::exception& failure) { rejected = true; assert(std::string(failure.what()).find("Input stopped") != std::string::npos); }
    assert(rejected);
  }
  metadata.error_code = Metadata::ERROR_CODE_NONE;
  metadata.out_of_sequence = true;
  try { verify_usrp_receive(metadata, 100, 4096); assert(false); } catch (const std::exception&) {}
  metadata.out_of_sequence = false;
  try { verify_usrp_receive(metadata, 4097, 4096); assert(false); } catch (const std::exception&) {}
  for (std::size_t capacity : {0u, (1u << 20) + 1}) {
    try { verify_usrp_receive_capacity(capacity); assert(false); } catch (const std::exception&) {}
  }
  for (double rate : {2000000.0, 6000000.0, 20000000.0}) {
    Receiver applied;
    apply_usrp_settings(applied, Spec{"A:B A:A"}, 527000000, rate, {11.5, 22.5}, {"RX2", "TX/RX"});
    assert(applied.calls == std::vector<std::string>({"subdev", "antenna0", "rate0", "freq0", "gain0", "antenna1", "rate1", "freq1", "gain1"}));
    assert(applied.rate[0] == rate && applied.rate[1] == rate);
    assert(applied.frequency[0] == 527000000 && applied.frequency[1] == 527000000);
    assert(applied.gain[0] == 11.5 && applied.gain[1] == 22.5);
    assert(applied.antenna[0] == "RX2" && applied.antenna[1] == "TX/RX");
    for (int channel : {0, 1}) {
      Receiver coerced; coerced.coerceChannel = channel;
      bool rejected = false;
      try { apply_usrp_settings(coerced, Spec{}, 527000000, rate, {10, 20}, {"RX2", "RX2"}); }
      catch (const std::exception&) { rejected = true; }
      assert(rejected);
    }
  }
  Receiver matched; verify(matched);
  for (size_t channel = 0; channel < 2; ++channel) {
    for (double invalid : {0.0, -1.0, std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()}) {
      Receiver sampleRate; sampleRate.rate[channel] = invalid; rejects(sampleRate);
      Receiver frequency; frequency.frequency[channel] = invalid; rejects(frequency);
      Receiver gain; gain.gain[channel] = invalid; rejects(gain);
    }
    Receiver coercedRate; coercedRate.rate[channel] += 1; rejects(coercedRate);
    Receiver coercedFrequency; coercedFrequency.frequency[channel] += 2; rejects(coercedFrequency);
    Receiver coercedGain; coercedGain.gain[channel] += 1; rejects(coercedGain);
    Receiver antenna; antenna.antenna[channel] = "TX/RX"; rejects(antenna);
  }
  Receiver count; count.count = 1; rejects(count);
  Receiver mapping; mapping.spec.value = "A:B A:A"; rejects(mapping);
  std::cout << "UHD mocked setter/readback: both channels at2/6/20MS/s, 6 setter-coercion, 34 getter/nonfinite/mapping and 8 discontinuity/capacity faults refused. No hardware opened.\n";
}
