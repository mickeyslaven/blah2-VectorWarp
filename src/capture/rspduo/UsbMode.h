#pragma once

#include <cstring>
#include <stdexcept>

// ISOCH is the historical default.  This only selects transport before Init;
// it never changes ADC or output sample rates.
inline bool rspduo_usb_bulk_mode(const char* value) {
  if (!value || std::strcmp(value, "isoch") == 0) return false;
  if (std::strcmp(value, "bulk") == 0) return true;
  throw std::invalid_argument("VECTORWARP_RSPDUO_USB_MODE must be isoch or bulk");
}
