#ifndef ARRAY_REFERENCE_SYNTHESIZER_H
#define ARRAY_REFERENCE_SYNTHESIZER_H

#include "data/IqData.h"

#include <complex>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

class ArrayReferenceSynthesizer
{
public:
  struct Config
  {
    uint32_t analysisSamples = 32768;
    uint32_t analysisInterval = 10;
    uint32_t powerIterations = 12;
    double covarianceSmoothing = 0.8;
    double diagonalLoading = 0.001;
    // Replay-only target protection: relative channel response of a candidate
    // surveillance direction with respect to the learned direct-path beam.
    // Empty preserves the production eigenbeam exactly.
    std::vector<std::complex<double>> protectionRelativeSteering;
    // Minimum fraction of the learned unit-vector energy that must remain
    // after projection. Replay arms that nearly erase the reference fail
    // closed instead of normalizing numerical noise.
    double minimumProtectionRetention = 0.05;
    // Explicit shadow helper only.  Zero preserves surveillance samples
    // exactly; values above .5 are rejected so this cannot become a full
    // projection/MVDR substitute.
    double commonModeDecorrelation = 0;
    double commonModeMinimumCoherence = .25;
  };

  struct Metrics
  {
    double coherentFraction = 0;
    double coherentGainDb = 0;
    double directPathCoherentFraction = 0;
    double directPathCoherentGainDb = 0;
    uint64_t updates = 0;
    bool hasSignal = false;
    bool fallback = false;
    std::string reason;
    // Unprojected learned direct-path manifold.  Target protection may alter
    // weights, but must never feed that projected vector back into covariance
    // smoothing or consumers that calibrate surveillance beams.
    std::vector<std::complex<double>> directPathWeights;
    std::vector<std::complex<double>> weights;
    double commonModeCoherence = 0;
    double commonModeFraction = 0;
  };

  ArrayReferenceSynthesizer();
  explicit ArrayReferenceSynthesizer(Config config);
  std::unique_ptr<IqData> process(const std::vector<IqData *>& channels);
  /// Reuse caller-owned sample storage. Output must not alias an input channel.
  void process_into(const std::vector<IqData *>& channels, IqData& output);
  // Bounded rank-one reference/surveillance decorrelation. This is not part
  // of process()/process_into(), so existing reference synthesis is unchanged.
  void decorrelate_surveillance_into(const IqData& reference,
    const IqData& surveillance, IqData& output);
  void reset();
  const Metrics& get_metrics() const { return metrics; }

private:
  Config config;
  Metrics metrics;
  uint64_t frames = 0;
};

#endif
