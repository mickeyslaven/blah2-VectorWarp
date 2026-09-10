#pragma once
#include <asio.hpp>
#include <string>

namespace vectorwarp {
// A TCP write can succeed after sending only part of the JSON. Complete the
// transfer or throw to the processor's status/error handler; never skip bytes.
template<class SyncWriteStream>
void writeAll(SyncWriteStream& stream, const std::string& data)
{
  asio::write(stream, asio::buffer(data));
}
}
