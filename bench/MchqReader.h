#pragma once
#include "capture/kraken/HeimdallFrame.h"
#include <deque>
#include <fstream>
#include <stdexcept>
#include <cmath>

// Shared lossless packet adapter. Both benchmark engines receive exactly the
// same decoded complex samples, including the fork's packet-wise DC removal.
class MchqReader {
  std::ifstream stream_;
  unsigned channels_;
  double frequency_;
  std::vector<std::vector<std::complex<float>>> packet_;
  size_t offset_ = 0;
  bool initialized_ = false;
  uint32_t tuning_ = 0;
public:
  uint64_t samples = 0, packets = 0, tailSamples = 0;
  MchqReader(const std::string& path, unsigned channels, double frequency)
    : stream_(path, std::ios::binary), channels_(channels), frequency_(frequency) {
    if (channels < 2 || channels > 8 || !std::isfinite(frequency))
      throw std::invalid_argument("Invalid recording configuration");
    if (!stream_) throw std::runtime_error("Cannot open recorded IQ");
  }
  bool read(unsigned count, std::vector<std::deque<std::complex<double>>>& output) {
    if (!count) throw std::invalid_argument("Frame length must be positive");
    output.assign(channels_, {});
    unsigned copied = 0;
    while (copied < count) {
      if (packet_.empty() || offset_ == packet_.front().size()) {
        std::array<uint8_t, 32> bytes{};
        stream_.read(reinterpret_cast<char*>(bytes.data()), bytes.size());
        if (!stream_.gcount() && stream_.eof()) { tailSamples = copied; return false; }
        if (stream_.gcount() != 32) throw std::runtime_error("Truncated MCHQ header");
        const auto header = HeimdallFrame::decode_header(bytes);
        if (header.numChannels != channels_) throw std::runtime_error("Recorded channel count differs");
        std::vector<uint8_t> metadata(HeimdallFrame::metadata_size(header));
        stream_.read(reinterpret_cast<char*>(metadata.data()), metadata.size());
        if (stream_.gcount() != static_cast<std::streamsize>(metadata.size()))
          throw std::runtime_error("Truncated MCHQ metadata");
        auto decoded = header;
        HeimdallFrame::decode_metadata(decoded, metadata);
        if (!HeimdallFrame::is_synchronized_data(decoded))
          throw std::runtime_error("Recording contains uncalibrated or retuning data");
        if (initialized_ && tuning_ != decoded.frequencyChangeCounter)
          throw std::runtime_error("Recording changes receiver tuning");
        for (float value : decoded.frequencies)
          if (!std::isfinite(value) || std::abs(value - frequency_) > 256)
            throw std::runtime_error("Recording frequency differs from benchmark");
        initialized_ = true; tuning_ = decoded.frequencyChangeCounter;
        std::vector<uint8_t> payload(HeimdallFrame::payload_size(decoded));
        stream_.read(reinterpret_cast<char*>(payload.data()), payload.size());
        if (stream_.gcount() != static_cast<std::streamsize>(payload.size()))
          throw std::runtime_error("Truncated MCHQ samples");
        packet_ = HeimdallFrame::decode_payload(decoded, payload);
        offset_ = 0; ++packets;
      }
      const size_t take = std::min<size_t>(count - copied, packet_.front().size() - offset_);
      for (unsigned ch = 0; ch < channels_; ++ch)
        output[ch].insert(output[ch].end(), packet_[ch].begin() + offset_, packet_[ch].begin() + offset_ + take);
      offset_ += take; copied += take;
    }
    samples += count;
    return true;
  }
};
