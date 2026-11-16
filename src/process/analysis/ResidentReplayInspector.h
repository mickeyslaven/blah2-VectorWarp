#pragma once
#include "ResidentReplaySnapshot.h"
#include <string>

namespace blah2::resident_replay {
// Expectations must come from an independently retained capture manifest, not
// from the snapshots under inspection. This is validation, not device replay.
Hash parseInspectorHash(const std::string&);
std::vector<Identity> readInspectorManifest(const std::string& path);
std::vector<uint8_t> readInspectorSnapshot(const std::string& path);
struct InspectedFrame {
  Identity identity;
  bool warmup=false, covarianceValid=false, historyReady=false;
  uint32_t historyAvailable=0, detections=0;
  uint64_t tracks=0;
};
class SequenceInspector {
public:
  SequenceInspector(Hash source,Hash rf,std::vector<Identity> expected,size_t warmup);
  InspectedFrame append(const uint8_t* data,size_t bytes);
  void finish() const;
private:
  Hash source_,rf_;
  std::vector<Identity> expected_;
  size_t warmup_,seen_=0;
};
}
