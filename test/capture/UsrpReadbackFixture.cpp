#include "capture/usrp/UsrpReadback.h"
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
  std::cout << "UHD mock readback: matched tuple accepted; 34 coercion/nonfinite/channel/mapping cases refused before streaming.\n";
}
