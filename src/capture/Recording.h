#pragma once
#include <complex>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace blah2 {
using IqBlock = std::vector<std::vector<std::complex<float>>>;

struct RecordingMetadata {
  uint32_t channels = 0, sampleRate = 0, frequency = 0;
  uint64_t samples = 0;
  std::string format;
};

struct ReplayOptions {
  std::string format = "auto", receiver;
  uint32_t channels = 2, sampleRate = 0, frequency = 0;
  uint32_t legacyBlockSamples = 0;
};

// Portable, self-describing float32 complex samples, preserving all input
// channels. A successful close marks the file complete; an interrupted recording
// remains distinguishable from a correctly finished recording.
class RecordingWriter {
  struct Impl;
  std::unique_ptr<Impl> impl_;
public:
  RecordingWriter(const std::string& path, RecordingMetadata metadata);
  ~RecordingWriter();
  void append(const IqBlock& samples);
  void close();
  uint64_t samples() const;
};

// Native recordings, legacy paired RSPduo/HackRF samples, explicitly sized old
// USRP blocks, and Kraken MCHQ. Never guesses the undocumented old USRP block size.
class RecordingReader {
  struct Impl;
  std::unique_ptr<Impl> impl_;
public:
  RecordingReader(const std::string& path, ReplayOptions options);
  ~RecordingReader();
  const RecordingMetadata& metadata() const;
  bool read(IqBlock& samples);
  void rewind();
};
}
