#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace blah2 {
// Supplied once before acquisition. Geometry is a declaration, not a hardware
// measurement; its confirmations and content identity remain explicit.
struct RecordingContext {
  std::string referenceMode;
  std::vector<uint32_t> referenceChannels, surveillanceChannels;
  bool coherent = false, geometryConfirmed = false;
  std::string geometryJson = "null";
  // Operator-declared acquisition sites, not surveyed/hardware-attested fixes.
  // Only an externally authenticated recording may supply replay geography.
  std::string geographicSitesJson = "null";
  std::map<std::string, std::string> manifoldSha256;
  std::string configurationSha256, codeSha256;
  // Acquisition-time requested total gain, distinct from sample-aligned
  // readback. Empty when the device has no declared scalar dB gain contract.
  std::vector<double> requestedChannelGainDb;
};

struct RecordingBlockMetadata {
  RecordingContext context;
  std::string receiverDetailsJson = "{}";
  std::vector<double> perChannelGainDb;
  // Only independently read-back, sample-aligned values may use this strength.
  // Requested SDK settings remain retained in receiverDetails, not promoted.
  std::string gainProvenance = "requested-settings-only";
};

struct RecordingFrameLineage {
  uint64_t ingressSequence = 0, sourceFirst = 0, sourceCount = 0;
  uint64_t sliceOffset = 0, count = 0;
  // Native raw clips retain the original full-frame ADC I/Q means, even when
  // a clip edge trims that frame. Empty for already canonical float IQ.
  std::vector<double> originalMeanIq = {};
  // Lossless run of consecutive, equal-size complete canonical float frames.
  // Native ADC frames and trimmed slices retain repeat=1 and their own means.
  uint64_t repeat = 1;
};
} // namespace blah2
