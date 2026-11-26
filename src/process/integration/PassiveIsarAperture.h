#pragma once
#include "process/integration/DirectPathCoherentExperiment.h"

namespace blah2::integration::coherent_replay {

// Offline ownership of unique correlation blocks, not another radar pipeline.
// No truth, tracker, image scaling, autofocus, or detector threshold inputs.
class PassiveIsarAperture {
public:
  using Frame = ExperimentReader::ApertureFrame;
  using Complex = std::complex<double>;
  struct Entry {
    std::shared_ptr<const Frame> frame;
    uint64_t newRawFirst = 0, newRawEnd = 0;
    std::vector<uint8_t> ownedRows; // First observation owns the whole block.
  };
  struct Spectrum {
    std::vector<double> residualDopplerHz;
    std::vector<Complex> amplitude;
    uint64_t uniqueBlocks = 0, complexMacBound = 0, perCellTrials = 0;
    double delayAtFirstMidpoint = 0, translationDopplerHz = 0;
    uint32_t rateIndex = 0;
  };
  void append(std::shared_ptr<const Frame> frame) {
    using resident::wire::require;
    require(frame && entries_.size() < 8 && frame->phaseQualified,
      "Aperture needs qualified phase and at most eight frames; power fallback is forbidden");
    const auto& s = frame->source; const auto& c = frame->config;
    require(!frame->migration.empty() && !frame->migration[0].banks.empty(), "Missing aperture profiles");
    const uint64_t processedEnd = s.firstSample + uint64_t(c.geometry.doppler) * c.correlation;
    require(processedEnd <= s.endSample, "Aperture processed support exceeds raw support");
    if (!entries_.empty()) {
      const auto& old = *entries_.back().frame; const auto& p = old.source;
      require(experiment_detail::same(p, s) && s.identity.logicalFrame == p.identity.logicalFrame + 1 &&
        s.identity.generation == p.identity.generation + 1 &&
        s.identity.consumedSequence > p.identity.consumedSequence && s.firstSample > p.firstSample &&
        s.firstSample <= through_ && processedEnd > through_ &&
        (s.firstSample - entries_[0].frame->source.firstSample) % c.correlation == 0,
        "Aperture epoch/config/gap/order/block-grid discontinuity");
      require(frame->spatialCoefficients == old.spatialCoefficients &&
        frame->anchorChannelId == old.anchorChannelId && frame->minimumPilotCoherence == old.minimumPilotCoherence &&
        frame->declaredSpatialTrials == old.declaredSpatialTrials &&
        frame->migration[0].banks.size() == old.migration[0].banks.size(), "Aperture hypothesis changed");
      for (size_t i = 0; i < frame->migration[0].banks.size(); ++i)
        require(frame->migration[0].banks[i].delayRateBinsPerSecond ==
          old.migration[0].banks[i].delayRateBinsPerSecond, "Aperture rate winner splicing forbidden");
    }
    Entry entry; entry.frame = std::move(frame);
    entry.newRawFirst = entries_.empty() ? s.firstSample : entries_.back().frame->source.endSample;
    entry.newRawEnd = s.endSample;
    uint64_t added = 0;
    for (uint32_t b = 0; b < c.geometry.doppler; ++b) {
      const bool own = entries_.empty() || s.firstSample + uint64_t(b) * c.correlation >= through_;
      entry.ownedRows.push_back(uint8_t(own)); added += own;
    }
    require(uniqueBlocks_ + added <= 1024 && bytes_ + entry.frame->retainedBytes <= (256u << 20) &&
      work_ + entry.frame->complexMacBound <= RangeMigrationReplay::maximumMacs,
      "Aperture exceeds aggregate block/storage/work bound");
    bytes_ += entry.frame->retainedBytes; work_ += entry.frame->complexMacBound;
    uniqueBlocks_ += added; through_ = processedEnd; entries_.push_back(std::move(entry));
  }
  const std::vector<Entry>& entries() const { return entries_; }
  uint64_t uniqueBlocks() const { return uniqueBlocks_; }
  uint64_t complexMacBound() const { return work_; }
  uint64_t remainingWork() const { return RangeMigrationReplay::maximumMacs - work_; }
  uint64_t remainingBytes() const { return (256u << 20) - bytes_; }

  // A prescribed constant-translation signature, NOT a metre-scaled image.
  // Output retains Doppler Hz because target rotation/geometry is unidentified.
  // All specified bins are emitted; neither peak selection nor autofocus runs.
  Spectrum spectrum(uint32_t rateIndex, double delayAtFirstMidpoint,
      const std::vector<double>& frequencies) const {
    using resident::wire::require;
    require(!entries_.empty() && uniqueBlocks_ >= 3 && frequencies.size() >= 3 && frequencies.size() <= 129 &&
      std::isfinite(delayAtFirstMidpoint), "Invalid aperture spectrum request");
    const auto& first = *entries_[0].frame; const auto& c = first.config;
    require(rateIndex < first.migration[0].banks.size(), "Unknown aperture rate");
    const double rate = first.migration[0].banks[rateIndex].delayRateBinsPerSecond;
    const double carrier = c.temporalCarrierHz();
    require(std::isfinite(carrier) && carrier > 0, "Unknown physical carrier; no translation phase inference");
    const double translation = -carrier * rate / first.source.sampleRateHz;
    const double nyquist = .5 * first.source.sampleRateHz / c.correlation;
    const double centerHz = first.caf[0].doppler[c.geometry.doppler / 2];
    for (size_t i = 0; i < frequencies.size(); ++i)
      require(std::isfinite(frequencies[i]) && std::abs(frequencies[i] + translation - centerHz) < nyquist &&
        (!i || frequencies[i] > frequencies[i - 1]), "Aliased/unordered aperture frequencies");
    Spectrum result; result.residualDopplerHz = frequencies; result.amplitude.resize(frequencies.size());
    result.rateIndex = rateIndex; result.delayAtFirstMidpoint = delayAtFirstMidpoint;
    result.translationDopplerHz = translation; result.uniqueBlocks = uniqueBlocks_;
    result.perCellTrials = first.migration[0].banks.size() * uint64_t(first.declaredSpatialTrials);
    result.complexMacBound = work_ + first.migration[0].banks.size() * uniqueBlocks_ *
      (16 * c.geometry.channels + frequencies.size());
    require(result.complexMacBound <= RangeMigrationReplay::maximumMacs, "Aperture diagnostic work limit");
    const double tau = 2 * std::acos(-1.);
    for (const auto& entry : entries_) {
      const auto& frame = *entry.frame; const auto& g = frame.migration[0];
      const double offset = double(frame.source.firstSample - first.source.firstSample) / first.source.sampleRateHz;
      const double delay = delayAtFirstMidpoint + rate * offset;
      const double column = delay - frame.caf[0].delay[g.firstColumn];
      const double center = frame.caf[0].doppler[g.rows / 2];
      for (uint32_t b = 0; b < g.rows; ++b) if (entry.ownedRows[b]) {
        const double local = (double(b) * c.correlation + .5 * (c.correlation - 1)) / first.source.sampleRateHz;
        const double time = offset + local;
        Complex z{};
        for (size_t channel = 0; channel < frame.migration.size(); ++channel)
          z += frame.spatialCoefficients[channel] * RangeMigrationReplay::sampleProfile(
            frame.migration[channel].banks[rateIndex].rangeSlowTime, g.columns, b, column, center,
            first.source.sampleRateHz);
        // Restore physical block-center phase and remove known range ramp,
        // then remove prescribed bulk Doppler. No per-scatterer phase fitting.
        z *= frame.gaugeCorrection * std::polar(1., tau * (center * (local - delay / first.source.sampleRateHz) - translation * time));
        for (size_t k = 0; k < frequencies.size(); ++k)
          result.amplitude[k] += z * std::polar(1., -tau * frequencies[k] * time);
      }
    }
    for (auto& z : result.amplitude) z /= std::sqrt(double(uniqueBlocks_));
    return result;
  }
private:
  std::vector<Entry> entries_;
  uint64_t uniqueBlocks_ = 0, through_ = 0, bytes_ = 0, work_ = 0;
};
} // namespace blah2::integration::coherent_replay
