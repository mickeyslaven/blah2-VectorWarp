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

struct RecordingProvenance {
  bool present = false;
  bool contractValid = false;
  bool legacy = true;
  bool decisionGrade = false;
  bool exactTime = false;
  bool uncertaintyBounded = false;
  bool continuous = false;
  bool configurationFingerprintValid = false;
  bool codeFingerprintMatches = false;
  std::string schema = "legacy-unqualified";
  std::string receiver, backend, timeSource, reason;
  std::string configurationFingerprint, codeFingerprint, json;
  std::string clockSource, deviceTimeSource, motherboard, subdevices;
  std::vector<double> perChannelGainDb;
  uint64_t epoch = 0, firstSample = 0, endSample = 0, sourceFirstSample = 0;
  int64_t deviceTimeEpochTick = 0;
  bool sourceSampleAvailable = false, deviceTimeAvailable = false;
};

struct ReplayOptions {
  std::string format = "auto", receiver;
  uint32_t channels = 2, sampleRate = 0, frequency = 0;
  uint32_t legacyBlockSamples = 0;
  // legacy-unqualified preserves old recordings without inventing timing
  // precision. decision-grade requires a valid v2 contract with exact,
  // bounded RF time and rejects every legacy/inexact recording.
  std::string provenanceMode = "legacy-unqualified";
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
  const RecordingProvenance& provenance() const;
  bool read(IqBlock& samples);
  void rewind();
};
}
