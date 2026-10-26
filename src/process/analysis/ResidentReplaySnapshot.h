#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace blah2::resident { struct Config; }
namespace blah2::resident_replay {
using Hash = std::array<uint8_t,32>;
constexpr uint32_t version=1,headerBytes=2048;
constexpr uint64_t byteLimit=256ULL<<20;
constexpr uint32_t frameLimit=128;

// No implicit enablement: all provenance and both bounds are mandatory.
struct ExportConfig {
  uint64_t maxBytes=0;
  uint32_t maxFrames=0;
  double frequencyHz=0,configuredPfa=0;
  Hash sourceHash{},rfHash{};
  std::vector<double> actualGainDb;
};
enum class BlockKind:uint32_t {
  referenceCheckpoint=1,covariance=2,fusionWeights=3,refinementWeights=4,
  postprocessMetadata=5,powerHistory=6,angleHistory=7,currentPower=8,
  temporalBanks=9,beamPower=10,cfarStatistics=11,fused=12,channelMaps=13,
  rawIq=14,referenceIq=15,filteredIq=16,decisionFlags=17,
  detections=18,trackerCheckpoint=19,configuration=20
};
struct Block { BlockKind kind{}; std::vector<uint8_t> bytes; };
struct Identity {
  uint64_t epoch=0,logicalFrame=0,generation=0,consumedSequence=0,
    signalTimestampMs=0,trackerTransaction=0;
  Hash configHash{},moduleHash{},rawIqHash{},deviceHash{};
};
struct ResidentReplaySnapshot {
  Identity identity;
  bool covarianceValid=false;
  // Blocks are ordered and complete, including zero-length disabled blocks.
  std::vector<Block> blocks;
};
// Upper bound includes every channel/cell/beam and complete temporal history.
uint64_t requiredBytes(const resident::Config&);
void validateExportConfig(const resident::Config&);
void validate(const ResidentReplaySnapshot&,const resident::Config&);
std::vector<uint8_t> encode(const ResidentReplaySnapshot&,const resident::Config&);
// Decodes into a temporary value and validates everything before returning it.
// There is deliberately no device-state restore API in this export-only ABI.
ResidentReplaySnapshot decode(const uint8_t*,size_t,const resident::Config&,
  const Identity& expected);
// Offline inspection only: recover the embedded config/axes, then run the same
// canonical contract decoder. External source/RF identity must still be bound
// by the caller; an internally valid envelope is not provenance by itself.
ResidentReplaySnapshot decodeSelfContained(const uint8_t*,size_t);
}
