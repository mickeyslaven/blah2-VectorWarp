#include "capture/rspduo/SdkSampleClock.h"
#include "capture/rspduo/UsbMode.h"

#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <stdexcept>

int main() {
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
