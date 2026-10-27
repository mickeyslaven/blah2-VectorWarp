#include "capture/AsyncReferenceExporter.h"

#include <asio.hpp>
#include <array>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <thread>
#include <vector>

namespace {
uint32_t get32(const std::array<uint8_t,64>& input, size_t offset) {
  uint32_t value = 0;
  for (unsigned i = 0; i < 4; ++i) value = (value << 8) | input[offset+i];
  return value;
}
uint64_t get64(const std::array<uint8_t,64>& input, size_t offset) {
  uint64_t value = 0;
  for (unsigned i = 0; i < 8; ++i) value = (value << 8) | input[offset+i];
  return value;
}
}

int main() {
  asio::io_context io;
  asio::ip::tcp::acceptor acceptor(io,
    {asio::ip::address_v4::loopback(), 0});
  const auto port = acceptor.local_endpoint().port();
  std::array<uint8_t,64> header{};
  std::vector<int16_t> received(16);
  std::thread server([&] {
    asio::ip::tcp::socket socket(io);
    acceptor.accept(socket);
    asio::read(socket, asio::buffer(header));
    asio::read(socket, asio::buffer(received));
  });

  std::vector<int16_t> paired(32);
  for (unsigned sample = 0; sample < 8; ++sample) {
    paired[4*sample] = int16_t(100 + sample);
    paired[4*sample+1] = int16_t(-100 - int(sample));
    paired[4*sample+2] = int16_t(1000 + sample);
    paired[4*sample+3] = int16_t(-1000 - int(sample));
  }
  {
    AsyncReferenceExporter exporter("127.0.0.1:" + std::to_string(port),
      2'000'000, 551'000'000, 8, 1);
    assert(exporter.try_submit(paired.data(), 8, 17, 123456));
    for (unsigned attempt = 0; attempt < 100 && exporter.stats().sent != 1; ++attempt)
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    const auto stats = exporter.stats();
    assert(stats.submitted == 1 && stats.sent == 1 && !stats.dropped && !stats.failures);
  }
  server.join();

  assert(std::equal(header.begin(), header.begin()+8,
    reinterpret_cast<const uint8_t*>("VWREF001")));
  assert(get32(header, 8) == 1 && get32(header, 12) == 64);
  assert(get64(header, 16) == 0 && get64(header, 24) == 17 &&
    get64(header, 32) == 123456);
  assert(get32(header, 40) == 2'000'000 && get32(header, 44) == 551'000'000 &&
    get32(header, 48) == 8 && get32(header, 52) == 1 &&
    get32(header, 56) == 32 && get32(header, 60) == 1);
  for (unsigned sample = 0; sample < 8; ++sample) {
    assert(received[2*sample] == 1000 + int(sample));
    assert(received[2*sample+1] == -1000 - int(sample));
  }
}
