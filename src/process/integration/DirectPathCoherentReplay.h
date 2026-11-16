#pragma once

#include "process/analysis/ResidentReplaySnapshot.h"
#include "process/ambiguity/GpuResidentBackend.h"
#include "process/integration/DirectPathCoherentShadow.h"

#include <filesystem>
#include <memory>
#include <vector>

namespace blah2::integration::coherent_replay {

// Fixed-size, sealed lineage record stored beside exactly one .vwrps export.
// It is deliberately independent of the snapshot ABI so old exports cannot
// accidentally acquire made-up capture positions.  This is replay-only data.
constexpr uint32_t sidecarVersion = 1;
constexpr size_t sidecarBytes = 512;

struct Sidecar {
  resident_replay::Identity identity;
  resident_replay::Hash sourceHash{}, rfHash{}, snapshotHash{};
  uint64_t captureEpoch = 0, firstSample = 0, endSample = 0;
  uint64_t tuningHz = 0, gainCalibrationEpoch = 0;
  uint32_t sampleRateHz = 0;
  std::vector<double> actualGainDb;
};

// Rejects incomplete capture lineage, nonfinite gains, and any unbounded
// encoding.  `snapshotBytes` is hashed into the record to prevent sidecar
// swapping across otherwise similar frames.
std::vector<uint8_t> encodeSidecar(const Sidecar&, const uint8_t* snapshotBytes,
  size_t snapshotSize);
Sidecar decodeSidecar(const uint8_t*, size_t, const uint8_t* snapshotBytes,
  size_t snapshotSize);

std::filesystem::path sidecarPath(const std::filesystem::path& snapshot);
// Exclusive, durable publication.  A failed sidecar leaves an orphan snapshot
// which the reader must reject; it is never inferred from filename or wall time.
void writeSidecarExclusive(const std::filesystem::path& snapshot, const Sidecar&);
std::vector<uint8_t> readSidecar(const std::filesystem::path& snapshot);

// Parses a single sealed snapshot/sidecar pair without accepting any detector
// or device state.  Offline diagnostics reuse this exact validation boundary.
struct ValidatedSnapshot {
  Sidecar sidecar;
  resident::Config config;
  resident_replay::ResidentReplaySnapshot snapshot;
};
ValidatedSnapshot loadValidatedSnapshot(const std::filesystem::path& snapshot);
// Shared decoder preserves the native channel-major, shifted-Doppler layout.
std::vector<DirectPathCoherentShadow::ComplexMap> channelMaps(const ValidatedSnapshot&);

// CPU-only parser and adapter.  No detector/PFA state is accepted or emitted.
class ShadowReader {
public:
  explicit ShadowReader(DirectPathCoherentShadow::Config);
  DirectPathCoherentShadow::Result append(const std::filesystem::path& snapshot);
  DirectPathCoherentShadow::Result append(const ValidatedSnapshot& snapshot);
private:
  DirectPathCoherentShadow shadow_;
  uint32_t anchorChannel_ = 0;
  std::unique_ptr<DirectPathCoherentShadow::ComplexMap> fallback_;
  std::optional<Sidecar> previous_;
};

} // namespace blah2::integration::coherent_replay
