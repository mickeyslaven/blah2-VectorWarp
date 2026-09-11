#pragma once
#include <dlfcn.h>
#include <fcntl.h>
#include <memory>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace blah2 {
struct ReceiverLibrary {
  std::shared_ptr<void> handle;
  std::string error;
};

// The production caller supplies a fixed vendor path only for RSPduo. Keeping
// this primitive separate permits relocated fake-runtime tests without writing
// a vendor file into /usr/local or changing the machine's dynamic-loader cache.
inline ReceiverLibrary open_receiver_library(const std::string& modulePath,
    const char* localRuntime = nullptr, const char* runtimeSoname = nullptr,
    uid_t trustedOwner = 0) {
  constexpr int flags = RTLD_NOW | RTLD_LOCAL;
  auto loaderError = [] {
    const char* detail = dlerror();
    return std::string(detail ? detail : "unknown loader error");
  };
  void* module = dlopen(modulePath.c_str(), flags);
  std::string error = module ? "" : loaderError();
  std::shared_ptr<void> runtime;
  // A missing adapter, bad ABI, or unrelated dependency must not load an SDK.
  if (!module && localRuntime && runtimeSoname &&
      error.rfind(std::string(runtimeSoname) + ":", 0) == 0) {
    // O_PATH inspects metadata without reading a device or blocking on a FIFO.
    const int descriptor = open(localRuntime, O_PATH | O_CLOEXEC | O_NOFOLLOW);
    if (descriptor >= 0) {
      struct stat metadata{};
      const bool trusted = fstat(descriptor, &metadata) == 0 &&
        S_ISREG(metadata.st_mode) && metadata.st_uid == trustedOwner &&
        (metadata.st_mode & (S_IWGRP | S_IWOTH)) == 0;
      if (trusted) {
        // Bind the checked inode, not a pathname that could be replaced between
        // validation and dlopen. Its ELF SONAME resolves the module dependency.
        const auto descriptorPath = "/proc/self/fd/" + std::to_string(descriptor);
        void* handle = dlopen(descriptorPath.c_str(), flags);
        if (handle) runtime = std::shared_ptr<void>(handle, [](void* value) { dlclose(value); });
        else error = loaderError();
      } else error = "Local vendor runtime needs a trusted owner and no group/other write access";
      close(descriptor);
      if (runtime) {
        module = dlopen(modulePath.c_str(), flags);
        if (!module) error = loaderError();
      }
    }
  }
  if (!module) return {{}, std::move(error)};
  // The optional SDK remains mapped until after the adapter's destructors.
  return {std::shared_ptr<void>(module, [runtime = std::move(runtime)](void* value) mutable {
    dlclose(value);
    runtime.reset();
  }), {}};
}
}
