#pragma once

#include <string>
#include <vector>

namespace blah2 {

// Kept free of Metal so the exact policy can be exercised without a physical
// GPU. An environment value is represented by nullptr only when it is absent;
// an explicitly empty value is an override and must remain untouched.
inline bool macGpuNeedsArgumentBuffersDisabled(const bool appleX86,
    const char* argumentBuffersSetting, const std::vector<std::string>& deviceNames) {
  if (!appleX86 || argumentBuffersSetting != nullptr || deviceNames.empty()) return false;
  for (const auto& name : deviceNames)
    if (name != "Apple Paravirtual device") return false;
  return true;
}

// Enumerates Metal devices only in the isolated GPU worker. Returns true only
// when this process applied the narrowly-scoped MoltenVK compatibility default.
#ifdef __APPLE__
bool applyMacGpuCompatibilityPolicy();
#else
inline bool applyMacGpuCompatibilityPolicy() { return false; }
#endif

} // namespace blah2
