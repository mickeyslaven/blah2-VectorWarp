#ifndef COMPLEX_CUTOUT_RECORDER_H
#define COMPLEX_CUTOUT_RECORDER_H

#include "data/Map.h"
#include "NativeProductIdentity.h"
#include <complex>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <vector>

namespace blah2::analysis {

class ComplexCutoutRecorder {
public:
  struct Config {
    bool enabled = false;
    std::string path;
    double delayKm = 0;
    double dopplerHz = 0;
    uint32_t delayRadiusBins = 3;
    uint32_t dopplerRadiusBins = 3;
    uint32_t maxFrames = 32;
    uint64_t maximumBytes = 64ULL << 20;
  };

  ComplexCutoutRecorder(const Config& config, uint32_t sampleRate,
    bool replayInput);
  ~ComplexCutoutRecorder();
  ComplexCutoutRecorder(const ComplexCutoutRecorder&) = delete;
  ComplexCutoutRecorder& operator=(const ComplexCutoutRecorder&) = delete;
  bool enabled() const { return config_.enabled; }
  uint32_t frames() const { return frames_; }
  uint64_t bytes() const { return bytes_; }
  bool complete() const { return capacityReached_ || frames_ >= config_.maxFrames; }
  std::string statusJson() const;
  void append(uint64_t timestamp,
    const std::vector<Map<std::complex<double>>*>& maps,
    const std::vector<std::complex<double>>& referenceWeights = {},
    std::optional<std::complex<double>> pilotReference = std::nullopt,
    std::optional<uint32_t> pilotAnchorChannelId = std::nullopt);
  void appendNative(uint64_t timestamp,
    const std::vector<const Map<std::complex<double>>*>& maps,
    const std::vector<std::complex<double>>& referenceWeights,
    std::optional<std::complex<double>> pilotReference,
    const NativeProductIdentity* identity = nullptr,
    std::optional<uint32_t> pilotAnchorChannelId = std::nullopt);

private:
  Config config_;
  uint32_t sampleRate_ = 0;
  uint32_t frames_ = 0;
  uint64_t bytes_ = 0;
  bool capacityReached_ = false;
  FILE* output_ = nullptr;
};

}

#endif
