#ifndef BLAH2_RECEIVER_MODULE_H
#define BLAH2_RECEIVER_MODULE_H

#include <cstddef>
#include <cstdint>

class Source;

// Private, same-package ABI; this is not a third-party plugin interface. No
// STL/YAML object or exception crosses the factory boundary. Source/IqData are
// shared C++ interfaces, so the loader also verifies the generated cohort hash.
constexpr uint32_t BLAH2_RECEIVER_ABI = 1;
struct Blah2ReceiverConfig {
  uint32_t abi = BLAH2_RECEIVER_ABI;
  uint32_t size = sizeof(Blah2ReceiverConfig);
  uint32_t frequency = 0, sampleRate = 0, channels = 2;
  const char* recordingPath = nullptr;
  bool* saveIq = nullptr;
  const char* address = nullptr;
  const char* subdev = nullptr;
  const char* antenna[2]{};
  double gain[2]{};
  const char* serial[2]{};
  const char* rspduoSerial = nullptr;
  uint32_t gainLna[2]{}, gainVga[2]{};
  uint8_t ampEnable[2]{};
  int32_t agcSetPoint = 0, bandwidthNumber = 0;
  int32_t gainReduction[2]{}, lnaState = 0;
  uint8_t dabNotch = 0, rfNotch = 0;
};

struct Blah2ReceiverApi {
  uint32_t abi;
  uint32_t size;
  const char* cohort;
  const char* receiver;
  Source* (*create)(const Blah2ReceiverConfig*, char*, std::size_t) noexcept;
  void (*destroy)(Source*) noexcept;
};
using Blah2ReceiverEntry = const Blah2ReceiverApi* (*)() noexcept;

#endif
