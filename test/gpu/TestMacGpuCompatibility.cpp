#include "process/ambiguity/MacGpuCompatibility.h"

#include <cstdlib>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool value, const char* message) {
  if (!value) throw std::runtime_error(message);
}
}

int main() try {
  using blah2::macGpuNeedsArgumentBuffersDisabled;
  const std::vector<std::string> virtualOnly{"Apple Paravirtual device"};
  require(macGpuNeedsArgumentBuffersDisabled(true, nullptr, virtualOnly),
    "x86 virtual-only devices without an override must select the compatibility default");
  require(!macGpuNeedsArgumentBuffersDisabled(false, nullptr, virtualOnly),
    "ARM must not select the x86 compatibility default");
  require(!macGpuNeedsArgumentBuffersDisabled(true, "", virtualOnly),
    "an explicit empty override must remain untouched");
  require(!macGpuNeedsArgumentBuffersDisabled(true, "0", virtualOnly),
    "an explicit disabled override must remain untouched");
  require(!macGpuNeedsArgumentBuffersDisabled(true, "1", virtualOnly),
    "an explicit enabled override must remain untouched");
  require(!macGpuNeedsArgumentBuffersDisabled(true, nullptr, {}),
    "an empty Metal device list must not select the default");
  require(!macGpuNeedsArgumentBuffersDisabled(true, nullptr, {"Apple Paravirtual device", "Apple M2"}),
    "a mixed Metal device list must not select the default");
  require(!macGpuNeedsArgumentBuffersDisabled(true, nullptr, {"AMD Radeon Pro"}),
    "a physical Metal device must not select the default");
  require(!macGpuNeedsArgumentBuffersDisabled(true, nullptr, {"Unknown"}),
    "an unknown device must not select the default");
  std::cout << "Mac GPU compatibility policy passed\n";
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
