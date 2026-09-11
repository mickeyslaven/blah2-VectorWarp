#include "ReceiverLoader.h"
#include "ReceiverLibrary.h"
#include "ReceiverCohort.h"
#include <array>
#include <cstring>
#include <dlfcn.h>
#include <stdexcept>
#include <unistd.h>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

namespace blah2 {
namespace {
struct ModuleSpec {
  const char* receiver;
  const char* filename;
  bool compiled;
  const char* remedy;
};
const std::array<ModuleSpec, 3> modules{{
  {"Usrp", "blah2-receiver-usrp.so", BLAH2_BUILT_USRP,
    "Install the matching UHD runtime using Receiver setup."},
  {"HackRF", "blah2-receiver-hackrf.so", BLAH2_BUILT_HACKRF,
    "Install the libhackrf runtime using Receiver setup."},
  {"RspDuo", "blah2-receiver-rspduo.so", BLAH2_BUILT_RSPDUO,
    "Install SDRplay API 3.15 locally after reviewing its vendor license; see Receiver setup."}
}};

std::string executable_directory() {
  std::array<char, 4096> path{};
  const auto count = readlink("/proc/self/exe", path.data(), path.size());
  if (count <= 0 || static_cast<std::size_t>(count) == path.size())
    throw std::runtime_error("Cannot resolve the installed receiver module directory");
  const std::string executable(path.data(), static_cast<std::size_t>(count));
  return executable.substr(0, executable.find_last_of('/'));
}

struct LoadedModule {
  std::shared_ptr<void> library;
  const Blah2ReceiverApi* api = nullptr;
};
LoadedModule open_module(const ModuleSpec& module) {
  if (!module.compiled)
    throw std::runtime_error(std::string(module.receiver) +
      " adapter was not compiled into this build. Install the unified VectorWarp package or build with this adapter enabled.");
  const auto path = executable_directory() + "/" + module.filename;
  // Fedora does not put the vendor's standard /usr/local/lib installation in
  // its loader cache. No configurable path, environment search or sibling SDK
  // preload is permitted; only this exact root-owned SDK file is a fallback.
  auto library = open_receiver_library(path, std::strcmp(module.receiver, "RspDuo") == 0 ?
    "/usr/local/lib/libsdrplay_api.so.3.15" : nullptr, "libsdrplay_api.so.3");
  if (!library.handle) {
    throw std::runtime_error(std::string(module.receiver) +
      " adapter could not load: " + library.error +
      ". " + module.remedy + " No other receiver was selected.");
  }
  void* handle = library.handle.get();
  LoadedModule loaded{std::move(library.handle), nullptr};
  dlerror();
  auto entry = reinterpret_cast<Blah2ReceiverEntry>(dlsym(handle, "blah2_receiver_api_v1"));
  const char* symbolError = dlerror();
  if (symbolError || !entry)
    throw std::runtime_error(std::string(module.receiver) + " adapter has no supported factory entry point; reinstall the complete matching package.");
  loaded.api = entry();
  if (!loaded.api || loaded.api->abi != BLAH2_RECEIVER_ABI ||
      loaded.api->size != sizeof(Blah2ReceiverApi) || !loaded.api->cohort ||
      std::strcmp(loaded.api->cohort, BLAH2_RECEIVER_COHORT) != 0 ||
      !loaded.api->receiver || std::strcmp(loaded.api->receiver, module.receiver) != 0 ||
      !loaded.api->create || !loaded.api->destroy)
    throw std::runtime_error(std::string(module.receiver) + " adapter ABI/cohort mismatch; reinstall the complete matching package.");
  return loaded;
}
}

void ReceiverSourceDeleter::operator()(Source* source) const noexcept {
  if (destroy) destroy(source);
  else delete source;
}

ReceiverSource load_receiver(const std::string& receiver,
    const Blah2ReceiverConfig& config) {
  if (config.abi != BLAH2_RECEIVER_ABI || config.size != sizeof(config) ||
      config.channels != 2 || !config.recordingPath || !config.saveIq)
    throw std::invalid_argument("Invalid two-channel receiver module configuration");
  for (const auto& module : modules) if (receiver == module.receiver) {
    auto loaded = open_module(module);
    std::array<char, 2048> error{};
    Source* source = loaded.api->create(&config, error.data(), error.size());
    if (!source) {
      error.back() = '\0';
      throw std::runtime_error(receiver + " adapter creation failed: " +
        (error[0] ? error.data() : "unspecified error"));
    }
    return ReceiverSource(source, ReceiverSourceDeleter{std::move(loaded.library), loaded.api->destroy});
  }
  throw std::invalid_argument("Unknown receiver type; no receiver was opened");
}

std::vector<ReceiverModuleStatus> receiver_module_status() {
  std::vector<ReceiverModuleStatus> result{{"Kraken", true, true, true, ""}};
  for (const auto& module : modules) {
    ReceiverModuleStatus status{module.receiver, false, module.compiled, false, ""};
    try { auto loaded = open_module(module); status.moduleLoadable = true; }
    catch (const std::exception& error) {
      // The UI contract accepts a bounded printable diagnostic. dlerror may
      // contain long paths or arbitrary bytes from the local environment.
      for (const unsigned char character : std::string(error.what())) {
        if (status.error.size() == 240) break;
        status.error += character >= 32 && character <= 126 ? static_cast<char>(character) : ' ';
      }
    }
    result.push_back(std::move(status));
  }
  return result;
}

std::string receiver_module_status_json() {
  rapidjson::Document document(rapidjson::kObjectType);
  auto& allocator = document.GetAllocator();
  document.AddMember("schema", 1, allocator);
  // Loadability is deliberately not called "ready": hardware, coherent channel
  // pairing, service readiness and applied-setting readback are separate gates.
  document.AddMember("hardwareProbed", false, allocator);
  rapidjson::Value receivers(rapidjson::kArrayType);
  for (const auto& status : receiver_module_status()) {
    rapidjson::Value item(rapidjson::kObjectType);
    item.AddMember("receiver", rapidjson::Value(status.receiver.c_str(), allocator), allocator);
    item.AddMember("builtIn", status.builtIn, allocator);
    item.AddMember("compiled", status.compiled, allocator);
    item.AddMember("moduleLoadable", status.moduleLoadable, allocator);
    item.AddMember("error", rapidjson::Value(status.error.c_str(), allocator), allocator);
    receivers.PushBack(item, allocator);
  }
  document.AddMember("receivers", receivers, allocator);
  rapidjson::StringBuffer buffer;
  rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
  document.Accept(writer);
  return buffer.GetString();
}
}
