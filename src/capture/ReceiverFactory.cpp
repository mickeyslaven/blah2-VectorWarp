#include "ReceiverModule.h"
#include "ReceiverCohort.h"
#include "Source.h"
#include <cstdio>
#include <stdexcept>

#if defined(BLAH2_MODULE_USRP)
#include "usrp/Usrp.h"
#define BLAH2_MODULE_NAME "Usrp"
#elif defined(BLAH2_MODULE_HACKRF)
#include "hackrf/HackRf.h"
#define BLAH2_MODULE_NAME "HackRF"
#elif defined(BLAH2_MODULE_RSPDUO)
#include "rspduo/RspDuo.h"
#define BLAH2_MODULE_NAME "RspDuo"
#else
#error A receiver module must select exactly one backend
#endif

namespace {
Source* create(const Blah2ReceiverConfig* config, char* error, std::size_t capacity) noexcept {
  try {
    if (!config || config->abi != BLAH2_RECEIVER_ABI || config->size != sizeof(*config) ||
        config->channels != 2 || !config->recordingPath || !config->saveIq)
      throw std::invalid_argument("Invalid receiver factory configuration");
    const auto& c = *config;
#if defined(BLAH2_MODULE_USRP)
    if (!c.address || !c.subdev || !c.antenna[0] || !c.antenna[1])
      throw std::invalid_argument("Both USRP antenna settings, address and subdevice are required");
    return new Usrp(BLAH2_MODULE_NAME, c.frequency, c.sampleRate, c.recordingPath,
      c.saveIq, c.address, c.subdev, {c.antenna[0], c.antenna[1]}, {c.gain[0], c.gain[1]});
#elif defined(BLAH2_MODULE_HACKRF)
    if (!c.serial[0] || !c.serial[1])
      throw std::invalid_argument("Two distinct HackRF serials are required");
    return new HackRf(BLAH2_MODULE_NAME, c.frequency, c.sampleRate, c.recordingPath,
      c.saveIq, {c.serial[0], c.serial[1]}, {c.gainLna[0], c.gainLna[1]},
      {c.gainVga[0], c.gainVga[1]}, {c.ampEnable[0] != 0, c.ampEnable[1] != 0});
#elif defined(BLAH2_MODULE_RSPDUO)
    return new RspDuo(BLAH2_MODULE_NAME, c.frequency, c.sampleRate, c.recordingPath,
      c.saveIq, c.agcSetPoint, c.bandwidthNumber, c.gainReduction[0],
      c.gainReduction[1], c.lnaState, c.dabNotch != 0, c.rfNotch != 0,
      c.rspduoSerial ? c.rspduoSerial : "");
#endif
  } catch (const std::exception& exception) {
    if (error && capacity) std::snprintf(error, capacity, "%s", exception.what());
  } catch (...) {
    if (error && capacity) std::snprintf(error, capacity, "%s", "Unknown receiver construction failure");
  }
  return nullptr;
}

void destroy(Source* source) noexcept {
  if (!source) return;
  // Capture normally stops explicitly after joining processing. Retain cleanup
  // for failed starts and all other ownership exits, while code is still mapped.
  try { source->stop(); } catch (...) {}
  try { source->close_file(); } catch (...) {}
  delete source;
}
const Blah2ReceiverApi api{BLAH2_RECEIVER_ABI, sizeof(Blah2ReceiverApi),
  BLAH2_RECEIVER_COHORT, BLAH2_MODULE_NAME, create, destroy};
}

extern "C" __attribute__((visibility("default")))
const Blah2ReceiverApi* blah2_receiver_api_v1() noexcept { return &api; }
