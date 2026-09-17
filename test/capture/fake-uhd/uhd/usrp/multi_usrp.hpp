#pragma once
// Deliberately minimal SDK double. ONLY the testUsrpIngress target includes
// this directory; production adapters must compile against the real UHD SDK.
#include <algorithm>
#include <array>
#include <cassert>
#include <complex>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace uhd {
struct time_spec_t {
  double seconds;
  time_spec_t(double s = 0) : seconds(s) {}
  time_spec_t operator+(time_spec_t other) const { return seconds + other.seconds; }
};
struct stream_cmd_t {
  enum Mode { STREAM_MODE_START_CONTINUOUS, STREAM_MODE_STOP_CONTINUOUS };
  Mode stream_mode;
  bool stream_now = true;
  time_spec_t time_spec;
  stream_cmd_t(Mode mode) : stream_mode(mode) {}
};
struct rx_metadata_t {
  enum Error { ERROR_CODE_NONE, ERROR_CODE_OVERFLOW, ERROR_CODE_TIMEOUT, ERROR_CODE_BAD_PACKET };
  Error error_code = ERROR_CODE_NONE;
  bool out_of_sequence = false;
  std::string strerror() const { return "injected UHD error " + std::to_string(error_code); }
};
struct stream_args_t {
  std::vector<size_t> channels;
  stream_args_t(const char* cpu, const char* wire) {
    assert(std::string(cpu) == "fc32" && std::string(wire) == "sc16");
  }
};
struct FakeState {
  size_t capacity = 64, count = 7, calls = 0, failCall = 2;
  rx_metadata_t::Error error = rx_metadata_t::ERROR_CODE_OVERFLOW;
  bool outOfSequence = false, started = false, stopped = false;
  std::function<void()> beforeReceive;
};
inline FakeState fake;
class rx_streamer {
public:
  using sptr = std::shared_ptr<rx_streamer>;
  virtual ~rx_streamer() = default;
  size_t get_max_num_samps() const { return fake.capacity; }
  void issue_stream_cmd(const stream_cmd_t& command) {
    if (command.stream_mode == stream_cmd_t::STREAM_MODE_START_CONTINUOUS) {
      assert(!command.stream_now && command.time_spec.seconds == 10.05);
      fake.started = true;
    } else fake.stopped = true;
  }
  size_t recv(const std::vector<std::complex<float>*>& buffers, size_t capacity,
      rx_metadata_t& metadata) {
    assert(fake.started && !fake.stopped && capacity == fake.capacity);
    ++fake.calls;
    if (fake.beforeReceive) fake.beforeReceive();
    metadata = {};
    if (fake.calls == fake.failCall) {
      metadata.error_code = fake.error;
      metadata.out_of_sequence = fake.outOfSequence;
    }
    for (size_t i = 0; i < std::min(fake.count, capacity); ++i) {
      buffers[0][i] = {float(i) + .25f, -float(i)};
      buffers[1][i] = {float(i) + 100.5f, float(i)};
    }
    return fake.count;
  }
};
namespace usrp {
struct subdev_spec_t {
  std::string value;
  subdev_spec_t(std::string s = "A:A A:B") : value(std::move(s)) {}
  std::string to_string() const { return value; }
};
class multi_usrp {
  subdev_spec_t spec;
  std::array<double, 2> rates{}, frequencies{}, gains{};
  std::array<std::string, 2> antennas{};
public:
  using sptr = std::shared_ptr<multi_usrp>;
  static sptr make(const std::string&) { return std::make_shared<multi_usrp>(); }
  void set_rx_subdev_spec(subdev_spec_t s, size_t) { spec = s; }
  subdev_spec_t get_rx_subdev_spec(size_t) const { return spec; }
  size_t get_rx_num_channels() const { return 2; }
  void set_rx_rate(double v, size_t c) { rates[c] = v; }
  double get_rx_rate(size_t c) const { return rates[c]; }
  void set_rx_freq(double v, size_t c) { frequencies[c] = v; }
  double get_rx_freq(size_t c) const { return frequencies[c]; }
  void set_rx_gain(double v, size_t c) { gains[c] = v; }
  double get_rx_gain(size_t c) const { return gains[c]; }
  void set_rx_antenna(std::string v, size_t c) { antennas[c] = std::move(v); }
  std::string get_rx_antenna(size_t c) const { return antennas[c]; }
  time_spec_t get_time_now() const { return 10; }
  rx_streamer::sptr get_rx_stream(const stream_args_t& args) const;
};
}
}
