#pragma once

#include "process/ambiguity/Ambiguity.h"
#include "process/clutter/WienerHopf.h"
#include "process/conditioning/ArrayReferenceSynthesizer.h"
#include <cstddef>
#include <memory>
#include <vector>

namespace blah2::production_replay {

// The CPU stages used by the live processor and its resident startup oracle.
// Callers retain capture ownership, acceleration selection, and publication.
// This object retains the reference learner and FFT workspaces across CPIs;
// resetting only the learner has exactly the live replay-loop semantics.
class RawIqStages {
public:
  struct Config {
    int32_t clutterDelayMin = 0, clutterDelayMax = 0;
    uint32_t samples = 0;
    size_t paths = 0, workers = 1;
    bool borrowed = false;
    ArrayReferenceSynthesizer::Config reference;
  };

  explicit RawIqStages(Config config);
  ArrayReferenceSynthesizer& reference() { return reference_; }
  const ArrayReferenceSynthesizer& reference() const { return reference_; }
  void resetReference();
  bool clutter(IqData& reference, const std::vector<IqData*>& surveillance);
  // The Pi mixed backend's failed map commit retries its one raw CPI here.
  bool fallbackClutter(IqData& reference, IqData& surveillance);
  void ambiguity(const IqData& reference,
    const std::vector<IqData*>& surveillance,
    const std::vector<Ambiguity*>& processors, bool preserveSurveillance);

private:
  Config config_;
  ArrayReferenceSynthesizer reference_;
  std::vector<std::unique_ptr<WienerHopf>> filters_;
  std::unique_ptr<WienerHopf::PreparedReference> prepared_;
};
}
