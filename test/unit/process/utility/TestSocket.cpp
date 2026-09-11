#include <catch2/catch_test_macros.hpp>
#include "process/utility/WriteAll.h"
#include <algorithm>

struct PartialStream {
  std::string received;
  size_t failAfter = 100000;
  template<class Buffers>
  size_t write_some(const Buffers& buffers, asio::error_code& error) {
    if (received.size() >= failAfter) {
      error = asio::error::connection_reset;
      return 0;
    }
    const size_t count = std::min<size_t>(7, asio::buffer_size(buffers));
    std::string part(count, '\0');
    asio::buffer_copy(asio::buffer(part), buffers, count);
    received += part;
    error.clear();
    return count;
  }
};

TEST_CASE("Partial TCP writes never skip frame data", "[socket]")
{
  PartialStream stream;
  const std::string payload = "{\"frame\":\"" + std::string(10000, 'x') + "\"}\r\n";
  vectorwarp::writeAll(stream, payload);
  CHECK(stream.received == payload);
}

TEST_CASE("A broken frame connection reaches the processor error handler", "[socket]")
{
  PartialStream stream;
  stream.failAfter = 7;
  CHECK_THROWS_AS(vectorwarp::writeAll(stream, std::string(50, 'x')), asio::system_error);
  CHECK(stream.received.size() == 7);
}
