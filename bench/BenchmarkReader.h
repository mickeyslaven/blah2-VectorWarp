#pragma once
#include "MchqReader.h"
#include <memory>

// A benchmark-only adapter for retained RSPduo callback recordings. Metadata for
// raw input must be pinned in the campaign manifest; no sample-rate conversion,
// DC removal, quantization, or repeated samples are introduced here.
class BenchmarkReader {
  std::unique_ptr<MchqReader> mchq_;
  std::ifstream raw_;
  std::vector<unsigned char> bytes_;
public:
  uint64_t samples = 0, packets = 0, tailSamples = 0;
  BenchmarkReader(const std::string& path, unsigned channels, double frequency,
                  const std::string& format = "mchq") {
    if (format == "mchq") {
      mchq_ = std::make_unique<MchqReader>(path, channels, frequency);
    } else if (format == "rspduo-s16le") {
      if (channels != 2 || !std::isfinite(frequency) || frequency <= 0)
        throw std::invalid_argument("Raw RSPduo input requires two channels and a frequency");
      raw_.open(path, std::ios::binary | std::ios::ate);
      if (!raw_ || raw_.tellg() <= 0 || uint64_t(raw_.tellg()) % 8)
        throw std::runtime_error("Raw RSPduo input is empty or has a partial sample pair");
      raw_.seekg(0);
    } else {
      throw std::invalid_argument("Unknown benchmark recording format");
    }
  }
  bool read(unsigned count, std::vector<std::deque<std::complex<double>>>& output) {
    if (!count) throw std::invalid_argument("Frame length must be positive");
    if (mchq_) {
      const bool ready = mchq_->read(count, output);
      samples = mchq_->samples; packets = mchq_->packets; tailSamples = mchq_->tailSamples;
      return ready;
    }
    bytes_.resize(size_t(count) * 8);
    raw_.read(reinterpret_cast<char*>(bytes_.data()), bytes_.size());
    const auto got = raw_.gcount();
    if (raw_.bad() || got % 8) throw std::runtime_error("Raw RSPduo recording read failed");
    output.assign(2, {});
    if (got != static_cast<std::streamsize>(bytes_.size())) {
      tailSamples = got / 8;
      return false;
    }
    auto value = [&](size_t offset) {
      const unsigned bits = unsigned(bytes_[offset]) | (unsigned(bytes_[offset + 1]) << 8);
      return (bits >= 32768 ? int(bits) - 65536 : int(bits)) * 1.0;
    };
    for (size_t i = 0; i < count; ++i) {
      output[0].emplace_back(value(i * 8), value(i * 8 + 2));
      output[1].emplace_back(value(i * 8 + 4), value(i * 8 + 6));
    }
    samples += count; ++packets;
    return true;
  }
};
