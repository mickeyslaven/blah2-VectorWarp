#pragma once

#include "process/detection/PostClutterBootstrap.h"
#include "process/integration/TemporalEvidence.h"

#include <array>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>

namespace blah2::integration {

// Observation-only post-clutter CAF capture. Every cell is retained, including
// cells discarded by beam fusion, CFAR, temporal admission or tracking. This is
// NOT a detector replay checkpoint: live fusion/reference state and the null
// calibration are intentionally not inferred from these channel CAFs.
//
// All operations belong to the single observer worker. The caller must never
// call this writer on the radar thread. Complete frames have an explicit footer;
// a partial write is retained as an invalid artifact, never silently repaired.
class LiveWeakEvidenceArtifact {
public:
  using ComplexMap = Map<std::complex<double>>;
  struct Config {
    std::string path;
    uint64_t maximumBytes = 512ULL << 20;
    uint32_t maximumFrames = 16;
  };
  static constexpr uint64_t diskReserveBytes = 128ULL << 30;
  static constexpr uint32_t version = 1;
  static constexpr uint32_t cpuRowMajorDouble = 1, residentDelayMajorFloat = 2;

  static void validate(const Config& config) {
    if (config.path.empty()) return;
    if (config.path.size() > 4096 || config.path.find('\0') != std::string::npos ||
        !std::filesystem::path(config.path).is_absolute() ||
        config.maximumBytes < (1ULL << 20) || config.maximumBytes > (1ULL << 30) ||
        !config.maximumFrames || config.maximumFrames > 128)
      throw std::invalid_argument("Live weak-evidence artifact bounds are invalid");
  }

  LiveWeakEvidenceArtifact(Config config, const ComplexMap& geometry,
    const std::vector<uint32_t>& channels,
    const detection::PostClutterBootstrap::Config& bootstrap,
    const TemporalEvidence::Config& temporal)
    : config_(std::move(config)), rows_(uint32_t(geometry.data.size())),
      columns_(rows_ ? uint32_t(geometry.data.front().size()) : 0),
      channels_(uint32_t(channels.size())) {
    validate(config_);
    if (config_.path.empty() || !rows_ || !columns_ || rows_ > 4096 || columns_ > 4096 ||
        uint64_t(rows_) * columns_ > 524288 || !channels_ || channels_ > 8 ||
        geometry.delay.size() != columns_ || geometry.doppler.size() != rows_)
      throw std::invalid_argument("Live weak-evidence artifact geometry is invalid");
    const auto space = std::filesystem::space(std::filesystem::path(config_.path).parent_path());
    if (space.available < diskReserveBytes + config_.maximumBytes)
      throw std::runtime_error("Live weak-evidence artifact disk reserve unavailable");
    stream_.reset(std::fopen(config_.path.c_str(), "wbx"));
    if (!stream_) throw std::runtime_error("Cannot exclusively create live weak-evidence artifact");
    bytes("VWWEAK01", 8);
    u32(version); u32(rows_); u32(columns_); u32(channels_);
    u64(config_.maximumBytes); u32(config_.maximumFrames);
    u32(uint32_t(bootstrap.covariance)); u32(bootstrap.guardCells); u32(bootstrap.trainingCells);
    f64(bootstrap.diagonalLoading); f64(bootstrap.shrinkage); f64(bootstrap.robustClip);
    f64(bootstrap.robustShrinkageFloor); f64(bootstrap.robustShrinkageCeiling);
    u32(temporal.maximumCpis); u32(temporal.delayMotionBins); u32(temporal.dopplerMotionBins);
    for (const auto channel : channels) u32(channel);
    for (const auto delay : geometry.delay) u32(uint32_t(int32_t(delay)));
    for (const auto doppler : geometry.doppler) f64(doppler);
    flush();
  }

  bool complete() const { return complete_; }
  uint64_t frames() const { return frames_; }
  uint64_t size() const { return bytes_; }

  // The exact original precision/layout is in each record. Native resident
  // input is never reconstructed from display dB or a thresholded candidate list.
  template<class Frame> bool append(const Frame& frame, uint64_t generation,
    const std::vector<ComplexMap*>& maps,
    const std::vector<std::complex<float>>* resident = nullptr,
    const EvidenceWorkBudget* budget = nullptr) {
    if (complete_) return false;
    if (!frame.samplePositionAvailable || !frame.sampleRateHz || !frame.tuningHz ||
        !frame.configurationEpoch || !frame.timestampMs || !frame.mapSampleRateHz ||
        frame.endSample <= frame.firstSample)
      throw std::invalid_argument("Live weak-evidence artifact lacks exact frame identity");
    const uint64_t samples = uint64_t(rows_) * columns_ * channels_;
    const uint32_t encoding = resident ? residentDelayMajorFloat : cpuRowMajorDouble;
    const uint64_t recordBytes = 120 + samples * (resident ? 8 : 16);
    if (recordBytes > config_.maximumBytes || bytes_ > config_.maximumBytes - recordBytes) {
      complete_ = true; return false;
    }
    if (std::filesystem::space(std::filesystem::path(config_.path).parent_path()).available <
        diskReserveBytes + recordBytes)
      throw std::runtime_error("Live weak-evidence artifact disk reserve exhausted");
    if (budget) budget->check();
    if (resident) {
      if (resident->size() != samples ||
          std::any_of(resident->begin(), resident->end(), [](const auto value) {
            return !std::isfinite(value.real()) || !std::isfinite(value.imag());
          }))
        throw std::invalid_argument("Live weak-evidence resident CAF is invalid");
    } else {
      if (maps.size() != channels_)
        throw std::invalid_argument("Live weak-evidence channel count changed");
      for (const auto* map : maps)
        if (!map || map->data.size() != rows_ ||
            std::any_of(map->data.begin(), map->data.end(), [&](const auto& row) {
              return row.size() != columns_ ||
                std::any_of(row.begin(), row.end(), [](const auto value) {
                  return !std::isfinite(value.real()) || !std::isfinite(value.imag());
                });
            }))
          throw std::invalid_argument("Live weak-evidence CPU CAF is invalid");
    }
    bytes("FRMWEAK1", 8); u64(recordBytes);
    u64(frame.captureEpoch); u64(frame.sequence);
    u64(frame.firstSample); u64(frame.endSample);
    u64(frame.tuningHz); u64(frame.configurationEpoch);
    u64(generation); u64(frame.timestampMs);
    u32(frame.sampleRateHz); u32(frame.mapSampleRateHz);
    u32(encoding); u32(0); u64(samples);
    const uint32_t littleEndian = 1;
    const bool nativeEndian = *reinterpret_cast<const uint8_t*>(&littleEndian) == 1;
    static_assert(sizeof(std::complex<float>) == 8 && sizeof(std::complex<double>) == 16);
    static_assert(std::numeric_limits<float>::is_iec559 && std::numeric_limits<double>::is_iec559);
    if (resident && nativeEndian) {
      // The normal little-endian path has no per-cell transposition or packing.
      const auto* source = reinterpret_cast<const uint8_t*>(resident->data());
      for (uint64_t offset = 0; offset < samples * 8; offset += buffer_.size()) {
        if (budget) budget->check();
        bytes(source + offset, std::min<uint64_t>(buffer_.size(), samples * 8 - offset));
      }
    } else if (resident) {
      for (uint64_t i = 0; i < samples; ++i) {
        if (budget && i % 4096 == 0) budget->check();
        f32((*resident)[i].real()); f32((*resident)[i].imag());
      }
    } else {
      for (const auto* map : maps) for (const auto& row : map->data) {
        if (budget) budget->check();
        if (nativeEndian) bytes(row.data(), row.size() * 16);
        else for (const auto value : row) { f64(value.real()); f64(value.imag()); }
      }
    }
    bytes("ENDWEAK1", 8); u64(recordBytes); flush();
    ++frames_; complete_ = frames_ >= config_.maximumFrames;
    return true;
  }

private:
  struct Closer { void operator()(std::FILE* file) const { std::fclose(file); } };
  void bytes(const void* input, std::size_t size) {
    if (size > config_.maximumBytes || bytes_ > config_.maximumBytes - size)
      throw std::runtime_error("Live weak-evidence artifact byte cap reached");
    const auto* source = static_cast<const uint8_t*>(input);
    while (size) {
      const auto count = std::min(size, buffer_.size() - buffered_);
      std::memcpy(buffer_.data() + buffered_, source, count);
      source += count; size -= count; buffered_ += count; bytes_ += count;
      if (buffered_ == buffer_.size()) drain();
    }
  }
  void drain() {
    if (buffered_ && std::fwrite(buffer_.data(), 1, buffered_, stream_.get()) != buffered_)
      throw std::runtime_error("Live weak-evidence artifact write failed");
    buffered_ = 0;
  }
  void flush() {
    drain();
    if (std::fflush(stream_.get()) != 0)
      throw std::runtime_error("Live weak-evidence artifact flush failed");
  }
  void u32(uint32_t value) {
    uint8_t wire[4]; for (unsigned i = 0; i < 4; ++i) wire[i] = uint8_t(value >> (8 * i));
    bytes(wire, sizeof wire);
  }
  void u64(uint64_t value) {
    uint8_t wire[8]; for (unsigned i = 0; i < 8; ++i) wire[i] = uint8_t(value >> (8 * i));
    bytes(wire, sizeof wire);
  }
  void f32(float value) { uint32_t bits; std::memcpy(&bits, &value, 4); u32(bits); }
  void f64(double value) { uint64_t bits; std::memcpy(&bits, &value, 8); u64(bits); }

  const Config config_;
  const uint32_t rows_, columns_, channels_;
  std::unique_ptr<std::FILE, Closer> stream_;
  std::array<uint8_t, 65536> buffer_{};
  std::size_t buffered_ = 0;
  uint64_t bytes_ = 0, frames_ = 0;
  bool complete_ = false;
};
} // namespace blah2::integration
