#pragma once
#include <cmath>
#include <stdexcept>
#include <string>
#include <vector>

// Small SDK-independent gate so coercion, NaN and channel-mapping failures can
// be tested without opening an SDR. Called after setters, before stream creation.
template<class Device>
void verify_usrp_readback(Device& device, double frequency, double sampleRate,
                          const std::vector<double>& gain,
                          const std::vector<std::string>& antenna,
                          const std::string& subdevices)
{
  const auto closeEnough = [](double actual, double expected, double tolerance) {
    return std::isfinite(actual) && std::isfinite(expected) && std::abs(actual - expected) <= tolerance;
  };
  if (gain.size() != 2 || antenna.size() != 2 || device.get_rx_num_channels() < 2)
    throw std::runtime_error("[USRP] Two configured receive channels are required.");
  if (device.get_rx_subdev_spec(0).to_string() != subdevices)
    throw std::runtime_error("[USRP] UHD subdevice readback differs from the requested channel mapping.");
  for (size_t channel = 0; channel < 2; ++channel) {
    if (!closeEnough(device.get_rx_rate(channel), sampleRate, 0.5))
      throw std::runtime_error("[USRP] UHD sample-rate readback differs from the requested rate on channel " + std::to_string(channel));
    if (!closeEnough(device.get_rx_freq(channel), frequency, 1.0))
      throw std::runtime_error("[USRP] UHD frequency readback differs from the requested tuning on channel " + std::to_string(channel));
    if (!closeEnough(device.get_rx_gain(channel), gain[channel], 0.05))
      throw std::runtime_error("[USRP] UHD gain readback differs from the requested gain on channel " + std::to_string(channel));
    if (device.get_rx_antenna(channel) != antenna[channel])
      throw std::runtime_error("[USRP] UHD antenna readback differs from the requested port on channel " + std::to_string(channel));
  }
}
