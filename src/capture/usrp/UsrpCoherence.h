#pragma once

#include <cstddef>
#include <sstream>
#include <stdexcept>
#include <string>

struct UsrpCoherenceAttestation {
  std::string motherboard;
  std::string clockSource;
  std::string timeSource;
  std::string subdevices;
  std::string policy;
  std::size_t motherboards = 0;
  std::size_t channels = 0;
};

template<class Device>
UsrpCoherenceAttestation attest_usrp_coherence(Device& device,
  const std::string& requestedSubdevices)
{
  UsrpCoherenceAttestation value;
  value.motherboards = device.get_num_mboards();
  value.channels = device.get_rx_num_channels();
  if (value.motherboards != 1)
    throw std::runtime_error("[USRP] Scored coherent capture requires both channels on exactly one motherboard; multi-motherboard synchronization is not attested.");
  if (value.channels < 2)
    throw std::runtime_error("[USRP] Scored coherent capture requires two receive channels.");
  value.motherboard = device.get_mboard_name(0);
  value.clockSource = device.get_clock_source(0);
  value.timeSource = device.get_time_source(0);
  value.subdevices = device.get_rx_subdev_spec(0).to_string();
  if (value.motherboard.empty() || value.clockSource.empty() ||
      value.timeSource.empty() || value.subdevices.empty())
    throw std::runtime_error("[USRP] UHD did not provide complete motherboard, clock, time-source and subdevice attestation.");
  // This bounded policy intentionally rejects otherwise capable USRPs until
  // their clocking and channel topology have an explicit qualification.
  if (value.motherboard != "B210")
    throw std::runtime_error("[USRP] Scored coherent capture is qualified only for a single-motherboard B210 topology.");
  if (value.subdevices != requestedSubdevices)
    throw std::runtime_error("[USRP] UHD coherent topology differs from the requested channel mapping.");
  std::istringstream mapping(value.subdevices);
  std::string first, second, extra;
  if (!(mapping >> first >> second) || (mapping >> extra) || first == second)
    throw std::runtime_error("[USRP] Scored coherent capture requires two distinct B210 subdevices.");
  value.policy = "single-motherboard-b210-v1";
  return value;
}
