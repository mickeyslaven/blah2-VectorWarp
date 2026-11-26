#pragma once

#include "process/analysis/ResidentReplaySnapshot.h"
#include "process/ambiguity/GpuResidentBackend.h"

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
// For a previously authenticated extraction receipt: validates the same sealed
// sidecar against its explicit parent hash, without pretending a slice is a
// native snapshot. The caller must independently authenticate that receipt.
Sidecar decodeBoundSidecar(const uint8_t*, size_t, const resident_replay::Hash& parentHash);

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

struct DerivedIqLineage {
  resident_replay::Hash planHash{}, receiptHash{}, payloadHash{};
  std::array<std::array<uint64_t, 2>, 3> parentByteRanges{}; // raw, reference, filtered.
  std::string planJson, receiptJson;
};
struct AuthenticatedIqSegment {
  Sidecar parent; // Exact original sidecar, never a fabricated derived snapshot.
  DerivedIqLineage lineage;
  uint64_t sequence = 0, firstSample = 0, endSample = 0;
  uint32_t sampleRate = 0, channelId = 0;
  int delayMin = 0, delayMax = 0, dopplerMin = 0, dopplerMax = 0;
  std::vector<std::complex<double>> raw, reference, filtered;
};
// Calibration-only, bounded, hash-pinned ingress for the existing authenticated
// raw/reference/filtered extraction format. No truth or after-sum product input.
AuthenticatedIqSegment loadAuthenticatedIqSegment(const std::filesystem::path& plan,
  const std::filesystem::path& receipt, const std::filesystem::path& segment,
  const resident_replay::Hash& planHash, const resident_replay::Hash& receiptHash);
} // namespace blah2::integration::coherent_replay
