#ifndef BLAH2_RECEIVER_LOADER_H
#define BLAH2_RECEIVER_LOADER_H

#include "ReceiverModule.h"
#include "Source.h"
#include <memory>
#include <string>
#include <vector>

namespace blah2 {
struct ReceiverSourceDeleter {
  // The library stays mapped until *after* the derived deleting destructor has
  // returned. Keeping this handle in Source itself would unload too early.
  std::shared_ptr<void> library;
  void (*destroy)(Source*) noexcept = nullptr;
  void operator()(Source* source) const noexcept;
};
using ReceiverSource = std::unique_ptr<Source, ReceiverSourceDeleter>;

struct ReceiverModuleStatus {
  std::string receiver;
  bool builtIn = false, compiled = false, moduleLoadable = false;
  std::string error;
};

// There is deliberately no caller-configurable library path or receiver-name
// interpolation. Modules are the three known files next to this executable.
ReceiverSource load_receiver(const std::string& receiver,
  const Blah2ReceiverConfig& config);
std::vector<ReceiverModuleStatus> receiver_module_status();
std::string receiver_module_status_json();
}

#endif
