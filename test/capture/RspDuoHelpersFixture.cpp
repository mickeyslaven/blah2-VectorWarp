#include "capture/rspduo/SdkSampleClock.h"
#include "capture/rspduo/UsbMode.h"
#include "capture/rspduo/OverloadStatus.h"

#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <atomic>

int main() {
  RspDuoOverloadStatus overload;
  assert(!overload.snapshot().monitoring);
  overload.monitoring(true);
  overload.event(0, false, 100); // Unpaired correction during startup.
  assert(overload.snapshot().channels[0].correctedCount == 0);
  overload.event(0, true, 101);
  overload.event(0, true, 102); // Duplicate callback is one onset.
  overload.event(1, true, 103);
  overload.event(0, false, 104);
  overload.event(0, false, 105);
  auto state = overload.snapshot();
  assert(!state.channels[0].active && state.channels[1].active);
  assert(state.channels[0].detectedCount == 1 && state.channels[0].correctedCount == 1);
  assert(state.channels[0].lastDetectedAt == 101 && state.channels[0].lastCorrectedAt == 104);
  assert(state.channels[1].detectedCount == 1 && state.channels[1].correctedCount == 0);
  overload.ackFailed();
  overload.monitoring(false);
  state = overload.snapshot();
  assert(!state.monitoring && state.channels[1].active && state.ackFailures == 1);
  overload.reset();
  assert(overload.snapshot().channels[1].detectedCount == 0);
  assert(overload.snapshot().ackFailures == 0);
  std::atomic<bool> finished{false};
  std::thread events([&] {
    for (uint64_t i = 1; i <= 10000; ++i) {
      overload.event(0, true, 2 * i); overload.event(0, false, 2 * i + 1);
    }
    finished = true;
  });
  while (!finished) {
    const auto c = overload.snapshot().channels[0];
    assert(c.detectedCount == c.correctedCount + (c.active ? 1 : 0));
    assert((c.detectedCount == 0) == (c.lastDetectedAt == 0));
  }
  events.join();
  assert(overload.snapshot().channels[0].detectedCount == 10000);

  assert(!rspduo_usb_bulk_mode(nullptr));
  assert(!rspduo_usb_bulk_mode("isoch"));
  assert(rspduo_usb_bulk_mode("bulk"));
  try { (void)rspduo_usb_bulk_mode("interrupt"); assert(false); }
  catch (const std::invalid_argument&) {}

  const SdkSampleClock::ScaledDuoConfig valid{6000000.0, 2000000, 1, true, true};
  assert(SdkSampleClock::supportsRatio3(valid));
  assert(!SdkSampleClock::supportsRatio3({6000000.0, 1000000, 2, true, true}));
  assert(!SdkSampleClock::supportsRatio3({6000000.0, 2000000, 1, true, false}));

  SdkSampleClock output;
  assert(!output.observe(UINT32_MAX - 1, 2).sequenceBreak);
  const auto outputWrap = output.observe(0, 2);
  assert(!outputWrap.sequenceBreak && outputWrap.wrapped);

  SdkSampleClock scaled(3);
  assert(!scaled.observe((UINT32_MAX - 1) / 3, 1).sequenceBreak);
  const auto scaledWrap = scaled.observe(0, 1);
  assert(!scaledWrap.sequenceBreak && scaledWrap.wrapped);
  assert(scaled.observe(9, 1).sequenceBreak);
  std::cout << "RSPduo transport and guarded SDK counter helpers passed.\n";
}
