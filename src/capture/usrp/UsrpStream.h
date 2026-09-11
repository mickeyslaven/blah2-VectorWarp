#pragma once
#include <cstddef>
#include <stdexcept>
#include <string>

inline void verify_usrp_receive_capacity(std::size_t capacity) {
  if (capacity == 0 || capacity > (1u << 20))
    throw std::runtime_error("[USRP] UHD reported an invalid receive block size.");
}

template<class Metadata>
void verify_usrp_receive(const Metadata& metadata, std::size_t received,
                         std::size_t capacity) {
  if (received > capacity)
    throw std::runtime_error("[USRP] UHD returned more samples than the receive buffers can hold.");
  if (metadata.error_code != Metadata::ERROR_CODE_NONE || metadata.out_of_sequence)
    throw std::runtime_error("[USRP] Receive discontinuity: " + metadata.strerror() +
      (metadata.out_of_sequence ? " (out of sequence)" : "") +
      ". Input stopped before accepting this block; check the receiver/USB/network path and explicitly restart. No automatic IQ-gap retry.");
}
