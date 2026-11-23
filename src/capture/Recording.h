#pragma once
#include <complex>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include "RecordingAcquisition.h"

namespace blah2 {
using IqBlock = std::vector<std::vector<std::complex<float>>>;

struct RecordingMetadata {
  uint32_t channels = 0, sampleRate = 0, frequency = 0;
  uint64_t samples = 0;
  std::string format;
  std::string acquisitionJson = {};
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
  bool acquisitionIntegrityValid = false, acquisitionHashTrusted = false, exactAcquisition = false;
  bool geographicSitesAvailable = false;
  std::string geographicSitesReason = "Recorded geographic sites unavailable; replay geography disabled";
  std::string acquisitionJson, acquisitionSha256, recordingSha256, gainProvenance;
  std::string acquisitionReason = "Recording has no acquisition contract; ordinary playback only";
  RecordingContext acquisitionContext;
  std::vector<RecordingFrameLineage> frames;
  uint64_t epoch = 0, firstSample = 0, endSample = 0, sourceFirstSample = 0;
  int64_t deviceTimeEpochTick = 0;
  bool sourceSampleAvailable = false, deviceTimeAvailable = false;
  const std::vector<double>& requested_gains_for_manifold() const;
};

struct ReplayOptions {
  std::string format = "auto", receiver;
  uint32_t channels = 2, sampleRate = 0, frequency = 0;
  uint32_t legacyBlockSamples = 0;
  // legacy-unqualified preserves old recordings without inventing timing
  // precision. decision-grade requires a valid v2 contract with exact,
  // bounded RF time and rejects every legacy/inexact recording.
  std::string provenanceMode = "legacy-unqualified";
  bool requireExactAcquisition = false;
  // Trusted separately from the file/sidecar; required for exact admission.
  std::string expectedRecordingSha256 = {};
  std::string expectedAcquisitionSha256 = {};
  bool validateAcquisitionContext = false;
  RecordingContext expectedAcquisitionContext = {};
};

// Portable, self-describing float32 complex samples, preserving all input
// channels. A successful close marks the file complete; an interrupted recording
// remains distinguishable from a correctly finished recording.
// Producer/control methods require external serialization; only the internal
// disk worker runs concurrently. Source owns that serialization for live IQ.
class RecordingWriter {
  struct Impl;
  std::unique_ptr<Impl> impl_;
public:
  RecordingWriter(const std::string& path, RecordingMetadata metadata);
  ~RecordingWriter();
  void append(const IqBlock& samples);
  // Append a common aligned range without copying it into a temporary block.
  // This keeps event-clip persistence from competing with live DSP for memory
  // bandwidth while preserving the exact portable on-disk representation.
  void append_slice(const IqBlock& samples, std::size_t offset,
    std::size_t count);
  void close();
  // Stop accepting data without waiting for an in-flight disk operation.
  // Ownership must be retained until control-thread teardown; close refuses
  // every aborted file. Control may retire an idle aborted writer for rollover.
  void abort();
  bool abort_ready() const;
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
  void seek(uint64_t sample);
};
}
