#include "capture/ReceiverLoader.h"
#include "capture/ReceiverLibrary.h"
#include <cassert>
#include <iostream>

int main(int argc, char** argv) {
  assert(argc == 2 || argc == 4);
  const std::string mode = argv[1];
  if (mode == "status") {
    std::cout << blah2::receiver_module_status_json() << '\n';
    return 0;
  }
  bool record = false;
  Blah2ReceiverConfig config;
  config.frequency = 527000000;
  config.sampleRate = 6000000;
  config.recordingPath = "/unused";
  config.saveIq = &record;
  if (mode == "invalid-config") config.channels = 5;
  try {
    blah2::ReceiverSource source;
    if (mode == "local-runtime" || mode == "wrong-owner" || mode == "no-fallback") {
      assert(argc == 4);
      auto library = blah2::open_receiver_library(argv[2],
        mode == "no-fallback" ? nullptr : argv[3],
        "libreceiver-fixture-runtime.so.3",
        mode == "wrong-owner" ? getuid() + 1 : getuid());
      if (!library.handle) throw std::runtime_error(library.error);
      auto entry = reinterpret_cast<Blah2ReceiverEntry>(dlsym(library.handle.get(), "blah2_receiver_api_v1"));
      assert(entry);
      const auto api = entry();
      assert(api && api->abi == BLAH2_RECEIVER_ABI);
      auto instance = api->create(&config, nullptr, 0);
      assert(instance);
      source = blah2::ReceiverSource(instance,
        blah2::ReceiverSourceDeleter{std::move(library.handle), api->destroy});
    } else source = blah2::load_receiver(mode == "unknown" ? "../../Usrp" :
      mode == "rspduo-legacy" ? "RspDuo" : "Usrp", config);
    assert(source);
    assert(blah2::receiver_startup_receipt(source).empty()); // Pre-receipt module.
    IqData first(32), second(32);
    source->start();
    source->process(&first, &second);
    assert(first.get_length() == 1 && second.get_length() == 1);
    assert(first.get_data()[0] == std::complex<double>(6, 1));
    assert(second.get_data()[0] == std::complex<double>(6, -1));
    assert(!source->recording_status().active);
    auto moved = std::move(source);
    assert(!source && moved);
    moved.reset();
    std::cout << "Source lifetime and shared IQ interface passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cout << error.what() << '\n';
    return 42;
  }
}
