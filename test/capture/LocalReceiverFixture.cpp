#include "capture/ReceiverLoader.h"
#include "capture/ReceiverLibrary.h"
#include "capture/LocalReceiverGate.h"
#include <iostream>

int main(int argc, char** argv) {
  // Match the processor's direct use of its shared capture core; --as-needed
  // must not remove the core dependency from this status-only fixture.
  IqData keepCaptureCoreLoaded(1);
  if (argc == 2 && std::string(argv[1]) == "status") {
    std::cout << blah2::receiver_module_status_json() << '\n';
    return 0;
  }
  if (argc == 3 && std::string(argv[1]) == "trust")
    return blah2::local_receiver::trusted(argv[2]) ? 0 : 42;
  const std::string mode = argc > 1 ? argv[1] : "";
  if (argc == 5 && (mode == "pinned" || mode == "pinned-replaced" || mode == "pinned-reused-fd")) {
    if (mode == "pinned-replaced") {
      if (!dlopen(argv[3], RTLD_NOW | RTLD_LOCAL)) return 41;
      std::filesystem::rename(argv[4], argv[3]);
    } else if (mode == "pinned-reused-fd") {
      int old = open(argv[4], O_PATH | O_CLOEXEC);
      const auto descriptor = "/proc/self/fd/" + std::to_string(old);
      if (!dlopen(descriptor.c_str(), RTLD_NOW | RTLD_LOCAL)) return 41;
      close(old);
    } else if (std::string(argv[4]) != "none" &&
        !dlopen(argv[4], RTLD_NOW | RTLD_LOCAL)) return 41;
    auto fd = std::shared_ptr<int>(new int(open(argv[3], O_PATH | O_NOFOLLOW | O_CLOEXEC)),
      [](int* value) { close(*value); delete value; });
    const int descriptor = *fd;
    auto library = blah2::open_pinned_receiver_library(argv[2], fd, "libsdrplay_api.so.3");
    fd.reset();
    if (!library.handle) { std::cout << library.error << '\n'; return 42; }
    if (fcntl(descriptor, F_GETFD) < 0) return 44;
    auto marker = reinterpret_cast<int(*)()>(dlsym(library.handle.get(), "vectorwarp_runtime_marker"));
    const bool correct = marker && marker() == 7;
    library.handle.reset();
    if (fcntl(descriptor, F_GETFD) >= 0 || errno != EBADF) return 45;
    return correct ? 0 : 43;
  }
  return 2;
}
