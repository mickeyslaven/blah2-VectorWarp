#pragma once

#include <array>
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
  std::string motherboardId;
  std::string motherboardSerial;
  std::array<std::string, 2> rxIds;
  std::array<std::string, 2> rxSerials;
  std::array<std::string, 2> rxSubdeviceNames;
  bool identityComplete = false;
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
  for (std::size_t channel = 0; channel < 2; ++channel) {
    std::string boardId, boardSerial;
    try {
      const auto info = device.get_usrp_rx_info(channel);
      const auto read = [&](const std::string& key) {
        return info.has_key(key) ? info.get(key) : std::string{};
      };
      boardId = read("mboard_id");
      boardSerial = read("mboard_serial");
      value.rxIds[channel] = read("rx_id");
      value.rxSerials[channel] = read("rx_serial");
      value.rxSubdeviceNames[channel] = read("rx_subdev_name");
    } catch (const std::exception&) {
      // Identity is provenance, not a reason to disable otherwise qualified
      // same-motherboard capture. Missing SDK identity stays explicit below.
    }
    if (channel == 0) {
      value.motherboardId = boardId;
      value.motherboardSerial = boardSerial;
    } else if ((!boardId.empty() && !value.motherboardId.empty() &&
                boardId != value.motherboardId) ||
               (!boardSerial.empty() && !value.motherboardSerial.empty() &&
                boardSerial != value.motherboardSerial)) {
      throw std::runtime_error("[USRP] UHD channel identity reports different motherboards.");
    }
  }
  value.identityComplete = !value.motherboardId.empty() &&
    !value.motherboardSerial.empty() && !value.rxIds[0].empty() &&
    !value.rxIds[1].empty() && !value.rxSubdeviceNames[0].empty() &&
    !value.rxSubdeviceNames[1].empty();
  value.policy = "single-motherboard-b210-v1";
  return value;
}
