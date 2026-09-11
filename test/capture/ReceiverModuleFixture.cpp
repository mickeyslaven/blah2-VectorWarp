#include "capture/ReceiverLoader.h"
#include <cassert>
#include <iostream>

int main(int argc, char** argv) {
  assert(argc == 2);
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
    auto source = blah2::load_receiver(mode == "unknown" ? "../../Usrp" : "Usrp", config);
    assert(source);
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
