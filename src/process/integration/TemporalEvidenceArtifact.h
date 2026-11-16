#ifndef TEMPORAL_EVIDENCE_ARTIFACT_H
#define TEMPORAL_EVIDENCE_ARTIFACT_H

#include "process/integration/TemporalEvidence.h"

#include <algorithm>
#include <complex>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <limits>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

namespace blah2::integration
{

// Bounded, full-precision offline artifact. Axis/window identity is shared per
// record; only per-cell motion is repeated. This avoids the hundreds of MiB of
// redundant JSON lineage that cannot fit the API transport at target geometry.
class TemporalEvidenceArtifact
{
public:
  TemporalEvidenceArtifact(std::string path, uint64_t maximumBytes,
    uint32_t matureCpis, uint32_t delayMotionBins,
    uint32_t dopplerMotionBins, uint64_t captureCpiSamples,
    uint32_t captureSampleStep, std::string inputStatistic,
    std::string covariance)
    : path_(std::move(path)), maximumBytes_(maximumBytes),
      matureCpis_(matureCpis), delayMotionBins_(delayMotionBins),
      dopplerMotionBins_(dopplerMotionBins), captureCpiSamples_(captureCpiSamples),
      captureSampleStep_(captureSampleStep), inputStatistic_(std::move(inputStatistic)),
      covariance_(std::move(covariance))
  {
    if (path_.empty() || maximumBytes_ < (64ULL << 20) ||
        maximumBytes_ > (1ULL << 40) ||
        (matureCpis_ != 4 && matureCpis_ != 8) || delayMotionBins_ > 2 ||
        dopplerMotionBins_ > 16 || !captureCpiSamples_ || !captureSampleStep_)
      throw std::invalid_argument("Temporal artifact bounds are invalid");
    stream_ = std::fopen(path_.c_str(), "wbx");
    if (!stream_) throw std::runtime_error(
      "Cannot exclusively create temporal replay artifact");
    try {
      writeBytes("VWTBD01\0", 8);
      writeUnsigned<uint32_t>(1);
      writeUnsigned<uint64_t>(maximumBytes_);
      writeUnsigned<uint32_t>(matureCpis_);
      writeUnsigned<uint32_t>(delayMotionBins_);
      writeUnsigned<uint32_t>(dopplerMotionBins_);
      writeUnsigned<uint64_t>(captureCpiSamples_);
      writeUnsigned<uint32_t>(captureSampleStep_);
      writeString(inputStatistic_);
      writeString(covariance_);
    } catch (...) {
      std::fclose(stream_); stream_ = nullptr;
      std::remove(path_.c_str());
      throw;
    }
  }

  ~TemporalEvidenceArtifact()
  {
    if (stream_) std::fclose(stream_);
  }

  TemporalEvidenceArtifact(const TemporalEvidenceArtifact&) = delete;
  TemporalEvidenceArtifact& operator=(const TemporalEvidenceArtifact&) = delete;

  const std::string& path() const { return path_; }
  uint64_t bytes() const { return bytes_; }
  uint64_t records() const { return records_; }

  void write(const Map<std::complex<double>>& input,
    const TemporalEvidence::Result& result,
    const std::vector<uint8_t>& statisticFamily,
    uint64_t captureFirstSample, uint64_t captureEndSample)
  {
    const auto& geometry = *result.map;
    const uint32_t rows = static_cast<uint32_t>(geometry.data.size());
    const uint32_t columns = rows ?
      static_cast<uint32_t>(geometry.data.front().size()) : 0;
    const uint64_t cells = uint64_t(rows) * columns;
    if (!rows || !columns || geometry.delay.size() != columns ||
        geometry.doppler.size() < rows ||
        std::any_of(geometry.doppler.begin(), geometry.doppler.begin() + rows,
          [](double value) { return !std::isfinite(value); }) ||
        input.data.size() != rows || input.delay != geometry.delay ||
        input.doppler != geometry.doppler ||
        std::any_of(input.data.begin(), input.data.end(),
          [&](const auto& row) {
            return row.size() != columns || std::any_of(row.begin(), row.end(),
              [](const auto value) {
                return !std::isfinite(value.real()) || !std::isfinite(value.imag());
              });
          }) ||
        (!statisticFamily.empty() && statisticFamily.size() != cells))
      throw std::runtime_error("Temporal artifact geometry is invalid");
    if (captureEndSample < captureFirstSample ||
        captureEndSample - captureFirstSample + captureSampleStep_ !=
          captureCpiSamples_)
      throw std::runtime_error("Temporal artifact sample range is invalid");
    if (std::any_of(statisticFamily.begin(), statisticFamily.end(),
          [](uint8_t family) { return family > 2; }))
      throw std::runtime_error("Temporal artifact statistic family is invalid");
    for (const auto& bank : result.banks)
      if (bank.ready && (!bank.map || bank.motion.size() != cells))
        throw std::runtime_error("Temporal artifact bank is incomplete");
    uint64_t recordBytes = 4 + 8 + 8 + 8 + 8 + 8 + 4 + 4 + 4 + 4 +
      uint64_t(columns) * 4 + uint64_t(rows) * 8 + 1 +
      cells * 8 + (statisticFamily.empty() ? 0 : cells);
    for (const auto& bank : result.banks)
      recordBytes += 4 + (bank.ready ? bank.depth * 8 + cells * 10 : 0);
    if (recordBytes > maximumBytes_ || bytes_ > maximumBytes_ - recordBytes)
      throw std::runtime_error("Temporal replay artifact byte bound exceeded");
    writeUnsigned<uint32_t>(0x314d5246); // FRM1
    writeUnsigned<uint64_t>(recordBytes);
    writeUnsigned<uint64_t>(result.epoch);
    writeUnsigned<uint64_t>(result.sequence);
    writeUnsigned<uint64_t>(captureFirstSample);
    writeUnsigned<uint64_t>(captureEndSample);
    writeUnsigned<uint32_t>(result.availableCpis);
    writeUnsigned<uint32_t>(rows);
    writeUnsigned<uint32_t>(columns);
    writeUnsigned<uint32_t>(static_cast<uint32_t>(result.banks.size()));
    for (const auto delay : geometry.delay) writeSigned<int32_t>(delay);
    for (uint32_t row = 0; row < rows; ++row) writeDouble(geometry.doppler[row]);
    for (const auto& row : input.data)
      for (const auto value : row) writeDouble(std::norm(value));
    writeUnsigned<uint8_t>(statisticFamily.empty() ? 0 : 1);
    if (!statisticFamily.empty())
      writeBytes(statisticFamily.data(), statisticFamily.size());
    for (const auto& bank : result.banks)
    {
      writeUnsigned<uint8_t>(bank.ready ? 1 : 0);
      writeUnsigned<uint8_t>(static_cast<uint8_t>(bank.depth));
      writeUnsigned<uint16_t>(0);
      if (!bank.ready) continue;
      for (uint32_t age = 0; age < bank.depth; ++age)
        writeUnsigned<uint64_t>(result.sequence - age);
      for (const auto& row : bank.map->data)
        for (const auto value : row) writeDouble(std::norm(value));
      for (const auto motion : bank.motion) writeSigned<int8_t>(motion.delay);
      for (const auto motion : bank.motion) writeSigned<int8_t>(motion.doppler);
    }
    if (std::fflush(stream_) != 0)
      throw std::runtime_error("Temporal replay artifact write failed");
    ++records_;
  }

private:
  template <class T> void writeUnsigned(T value)
  {
    static_assert(std::is_unsigned_v<T>);
    unsigned char bytes[sizeof(T)];
    for (std::size_t index = 0; index < sizeof(T); ++index)
      bytes[index] = static_cast<unsigned char>(value >> (8 * index));
    writeBytes(bytes, sizeof(bytes));
  }

  template <class T> void writeSigned(T value)
  {
    using U = std::make_unsigned_t<T>;
    writeUnsigned<U>(static_cast<U>(value));
  }

  void writeDouble(double value)
  {
    static_assert(sizeof(double) == sizeof(uint64_t));
    uint64_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    writeUnsigned<uint64_t>(bits);
  }

  void writeString(const std::string& value)
  {
    if (value.size() > std::numeric_limits<uint16_t>::max())
      throw std::invalid_argument("Temporal artifact identity is too long");
    writeUnsigned<uint16_t>(static_cast<uint16_t>(value.size()));
    writeBytes(value.data(), value.size());
  }

  void writeBytes(const void* data, std::size_t size)
  {
    if (size && (!data || bytes_ > maximumBytes_ - size))
      throw std::runtime_error("Temporal replay artifact byte bound exceeded");
    if (size && std::fwrite(data, 1, size, stream_) != size)
      throw std::runtime_error("Temporal replay artifact write failed");
    bytes_ += size;
  }

  std::string path_;
  uint64_t maximumBytes_;
  uint32_t matureCpis_;
  uint32_t delayMotionBins_;
  uint32_t dopplerMotionBins_;
  uint64_t captureCpiSamples_;
  uint32_t captureSampleStep_;
  std::string inputStatistic_;
  std::string covariance_;
  std::FILE* stream_ = nullptr;
  uint64_t bytes_ = 0;
  uint64_t records_ = 0;
};

} // namespace blah2::integration

#endif
