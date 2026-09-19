#include "MacGpuCompatibility.h"

#ifdef __APPLE__
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#endif

#include <cstdlib>
#include <iostream>

namespace blah2 {

bool applyMacGpuCompatibilityPolicy() {
#if defined(__APPLE__) && defined(__x86_64__)
  @autoreleasepool {
    NSArray<id<MTLDevice>>* devices = MTLCopyAllDevices();
    std::vector<std::string> names;
    for (id<MTLDevice> device in devices) {
      const char* name = [[device name] UTF8String];
      names.emplace_back(name ? name : "");
    }
    [devices release];
    if (macGpuNeedsArgumentBuffersDisabled(true,
        std::getenv("MVK_CONFIG_USE_METAL_ARGUMENT_BUFFERS"), names) &&
        setenv("MVK_CONFIG_USE_METAL_ARGUMENT_BUFFERS", "0", 0) == 0) {
      std::cerr << "GPU compatibility: disabled MoltenVK Metal argument buffers for Apple Paravirtual device\n";
      return true;
    }
  }
#endif
  return false;
}

} // namespace blah2
