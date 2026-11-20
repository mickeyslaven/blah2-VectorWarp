#pragma once
#include "process/integration/DirectPathCoherentExperiment.h"
#include <cstring>
#include <optional>
#include <stdexcept>
#include <cstdlib>
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
#include "capture/Sha256.h"

namespace blah2::integration::cancellation_replay {
// Canonical little-endian identity includes ALL decoded sidecar fields and the
// snapshot hash. Binding only the snapshot would allow a resealed sidecar to
// change sample lineage between validation and solve without changing the IQ.
inline resident_replay::Hash frameIdentity(const coherent_replay::Sidecar& s) {
  blah2::Sha256 hash;
  const auto integer = [&](uint64_t value) {
    uint8_t bytes[8]; for (unsigned i = 0; i < 8; ++i) bytes[i] = uint8_t(value >> (8 * i));
    hash.update(bytes, sizeof(bytes));
  };
  const auto& id = s.identity;
  for (const auto value : {id.epoch, id.logicalFrame, id.generation, id.consumedSequence,
      id.signalTimestampMs, id.trackerTransaction, s.captureEpoch, s.firstSample,
      s.endSample, s.tuningHz, s.gainCalibrationEpoch, uint64_t(s.sampleRateHz),
      uint64_t(s.actualGainDb.size())}) integer(value);
  for (const auto* value : {&id.configHash, &id.moduleHash, &id.rawIqHash, &id.deviceHash,
      &s.sourceHash, &s.rfHash, &s.snapshotHash}) hash.update(value->data(), value->size());
  for (double gain : s.actualGainDb) {
    uint64_t bits; std::memcpy(&bits, &gain, sizeof(bits)); integer(bits);
  }
  return hash.finishBytes();
}
inline void requireFrozen(const coherent_replay::Sidecar& actual,
    const resident_replay::Hash& frozen) {
  if (frameIdentity(actual) != frozen)
    throw std::invalid_argument("Missing evidence: snapshot or sidecar lineage changed after cohort freeze");
}
inline std::filesystem::path executablePath(const std::string& argument,const std::string& searchPath) {
  const auto executable = [](const std::filesystem::path& path) {
    return std::filesystem::is_regular_file(path) && ::access(path.c_str(), X_OK) == 0;
  };
  const std::filesystem::path requested(argument);
  if (requested.has_parent_path()) {
    if (executable(requested)) return std::filesystem::canonical(requested);
  } else {
    size_t begin = 0;
    while (begin <= searchPath.size()) {
      const auto end = searchPath.find(':', begin);
      const auto directory = searchPath.substr(begin, end == std::string::npos ? end : end - begin);
      const auto candidate = std::filesystem::path(directory.empty() ? "." : directory) / requested;
      if (executable(candidate)) return std::filesystem::canonical(candidate);
      if (end == std::string::npos) break;
      begin = end + 1;
    }
  }
  throw std::runtime_error("Cannot resolve replay executable provenance from argv[0]/PATH");
}
inline std::filesystem::path runningExecutablePath(
    [[maybe_unused]] const std::string& argument,
    [[maybe_unused]] const std::string& searchPath) {
#if defined(__linux__)
  // Opening procfs binds the running inode even if its original path is renamed
  // or replaced. Do not canonicalize this link into a replaceable pathname.
  return "/proc/self/exe";
#elif defined(__APPLE__)
  uint32_t size = 0;
  (void)_NSGetExecutablePath(nullptr, &size);
  if (!size) throw std::runtime_error("Cannot size running executable path");
  std::vector<char> path(size);
  if (_NSGetExecutablePath(path.data(), &size))
    throw std::runtime_error("Cannot locate running executable path");
  return std::filesystem::canonical(path.data());
#else
  // POSIX fallback: skip same-name regular files without execute permission,
  // as PATH execution does. Linux/macOS never trust argv[0] or PATH here.
  return executablePath(argument, searchPath);
#endif
}
// Increasing timestamps alone cannot establish cancellation sample continuity.
class Cohort {
  std::optional<coherent_replay::Sidecar> previous_;
  uint64_t hop_ = 0;
public:
  void append(const coherent_replay::Sidecar& next, uint32_t samples) {
    const auto require = [](bool ok, const char* error) {
      if (!ok) throw std::invalid_argument(error);
    };
    require(samples && next.endSample > next.firstSample &&
      next.endSample - next.firstSample == samples,
      "Missing evidence: exact sample interval does not match cancellation CPI");
    if (previous_) {
      const auto& p = *previous_;
      require(next.captureEpoch == p.captureEpoch && next.identity.epoch == p.identity.epoch &&
        next.identity.configHash == p.identity.configHash &&
        next.identity.moduleHash == p.identity.moduleHash &&
        next.identity.deviceHash == p.identity.deviceHash && next.sourceHash == p.sourceHash &&
        next.rfHash == p.rfHash && next.tuningHz == p.tuningHz &&
        next.gainCalibrationEpoch == p.gainCalibrationEpoch &&
        next.sampleRateHz == p.sampleRateHz && next.actualGainDb == p.actualGainDb,
        "Missing evidence: cancellation cohort provenance changed");
      require(next.identity.logicalFrame == p.identity.logicalFrame + 1 &&
        next.identity.generation == p.identity.generation + 1 &&
        next.identity.consumedSequence == p.identity.consumedSequence + 1 &&
        next.firstSample > p.firstSample && next.endSample > p.endSample,
        "Missing evidence: cancellation cohort is reordered, duplicated or discontinuous");
      const auto hop = next.firstSample - p.firstSample;
      require(hop <= samples && next.endSample - p.endSample == hop && (!hop_ || hop_ == hop),
        "Missing evidence: cancellation sample hop changed or contains an IQ gap");
      hop_ = hop;
    }
    previous_ = next;
  }
};
} // namespace blah2::integration::cancellation_replay
