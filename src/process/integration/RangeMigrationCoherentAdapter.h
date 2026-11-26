#pragma once

#include "process/integration/CoherentPathEvidence.h"
#include "process/integration/RangeMigrationReplay.h"

#include <array>
#include <optional>

namespace blah2::integration {

// Offline-only bridge from a sealed full complex CAF to phase-coherent CPI
// banks.  It deliberately exposes every declared delay-rate trial: selecting
// a rate upstream would create an uncalibrated search winner and would merge
// incompatible phase references.  Each trial owns its own temporal history.
class RangeMigrationCoherentAdapter {
public:
  using Complex = std::complex<double>;
  struct Frame {
    uint64_t epoch = 0, sequence = 0, firstSample = 0, endSample = 0;
    uint32_t sampleRateHz = 0;
    std::array<uint8_t, 32> configHash{};
  };
  struct RateResult {
    uint32_t rateIndex = 0;
    double delayRateBinsPerSecond = 0;
    // The migration output's phase is explicitly de-referenced from this
    // heterodyne centre before temporal integration.
    double heterodyneCenterHz = 0;
    int64_t delayAxisOriginSamples = 0;
    double interpolationWhiteNoiseGain = 1;
    std::array<uint8_t, 32> configHash{};
    std::array<CoherentPathEvidence::Bank, 3> temporal;
  };

  RangeMigrationCoherentAdapter(CoherentPathEvidence::Config temporal,
    std::vector<double> rates)
    : temporal_(temporal), rates_(std::move(rates))
  {
    // Process performs the bounded numerical validation too; retaining it
    // here makes the owner count and history layout immutable.
    if (rates_.empty() || rates_.size() > RangeMigrationReplay::maximumTrials)
      throw std::invalid_argument("Migration coherent adapter rate contract invalid");
    engines_.reserve(rates_.size());
    for (std::size_t index = 0; index < rates_.size(); ++index)
      engines_.emplace_back(temporal_);
  }

  std::vector<RateResult> append(const Map<Complex>& caf, const Frame& frame,
    uint32_t correlationSamples)
  {
    if (!frame.epoch || !frame.sequence || !frame.sampleRateHz ||
        frame.endSample <= frame.firstSample)
      throw std::invalid_argument("Migration coherent adapter lineage invalid");
    const uint64_t expectedSpan = uint64_t(caf.data.size()) * correlationSamples;
    if (!expectedSpan || frame.endSample - frame.firstSample != expectedSpan)
      throw std::invalid_argument("Migration coherent adapter raw span does not match CAF");
    if (previous_ && (frame.epoch != previous_->epoch ||
        frame.sequence != previous_->sequence + 1 ||
        frame.firstSample != previous_->endSample ||
        frame.configHash != previous_->configHash))
      throw std::invalid_argument("Migration coherent adapter immutable lineage changed");
    const auto migrated = RangeMigrationReplay::process(caf, frame.sampleRateHz,
      correlationSamples, rates_);
    const double centre = caf.doppler.at(caf.data.size() / 2);
    constexpr double tau = 6.283185307179586476925286766559;
    std::vector<RateResult> result;
    result.reserve(migrated.banks.size());
    for (std::size_t index = 0; index < migrated.banks.size(); ++index) {
      const auto& source = migrated.banks[index];
      Map<Complex> corrected(migrated.rows, migrated.columns);
      corrected.doppler = caf.doppler;
      corrected.delay.assign(caf.delay.begin() + migrated.firstColumn,
        caf.delay.begin() + migrated.firstColumn + migrated.columns);
      for (uint32_t row = 0; row < migrated.rows; ++row)
        for (uint32_t column = 0; column < migrated.columns; ++column) {
          const auto delay = corrected.delay[column];
          // RangeMigrationReplay re-ramps by +centre*delay/fs.  Remove that
          // frame-dependent reference so CoherentPathEvidence's physical
          // Doppler rotation is the sole inter-CPI phase convention.
          corrected.data[row][column] = source.cells[uint64_t(row) * migrated.columns + column] *
            std::polar(1.0, -tau * centre * double(delay) / frame.sampleRateHz);
        }
      RateResult item;
      item.rateIndex = uint32_t(index); item.delayRateBinsPerSecond = source.delayRateBinsPerSecond;
      item.heterodyneCenterHz = centre; item.delayAxisOriginSamples = corrected.delay.front();
      item.interpolationWhiteNoiseGain = source.whiteNoiseGain; item.configHash = frame.configHash;
      item.temporal = engines_[index].process(corrected, frame.epoch, frame.sequence,
        frame.firstSample, frame.sampleRateHz, nullptr, frame.endSample);
      result.push_back(std::move(item));
    }
    previous_ = frame;
    return result;
  }

private:
  CoherentPathEvidence::Config temporal_;
  std::vector<double> rates_;
  std::vector<CoherentPathEvidence> engines_;
  std::optional<Frame> previous_;
};

} // namespace blah2::integration
