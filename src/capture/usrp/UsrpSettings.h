#pragma once
#include "UsrpReadback.h"

// The same setter sequence is exercised with a fake device and the actual UHD
// device. Device creation/address selection remains in Usrp::process(). No
// external UHD application or second settings UI is involved.
template<class Device, class SubdeviceSpec>
void apply_usrp_settings(Device& device, const SubdeviceSpec& subdevices,
                         double frequency, double sampleRate,
                         const std::vector<double>& gain,
                         const std::vector<std::string>& antenna) {
  if (!std::isfinite(frequency) || frequency <= 0 ||
      !std::isfinite(sampleRate) || sampleRate <= 0 ||
      gain.size() != 2 || antenna.size() != 2)
    throw std::invalid_argument("[USRP] Positive frequency/rate and two channel settings are required.");
  for (unsigned channel = 0; channel < 2; ++channel)
    if (!std::isfinite(gain[channel]) || antenna[channel].empty())
      throw std::invalid_argument("[USRP] Finite gain and an antenna port are required on both channels.");
  device.set_rx_subdev_spec(subdevices, 0);
  if (device.get_rx_num_channels() < 2)
    throw std::runtime_error("[USRP] The selected subdevices do not provide two receive channels.");
  for (unsigned channel = 0; channel < 2; ++channel) {
    device.set_rx_antenna(antenna[channel], channel);
    device.set_rx_rate(sampleRate, channel);
    device.set_rx_freq(frequency, channel);
    device.set_rx_gain(gain[channel], channel);
  }
  verify_usrp_readback(device, frequency, sampleRate, gain, antenna, subdevices.to_string());
}
