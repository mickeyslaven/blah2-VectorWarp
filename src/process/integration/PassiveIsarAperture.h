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
    uint64_t unprocessedTailSamples = 0, missingBeforeSamples = 0;
    std::vector<uint8_t> ownedRows; // First observation owns the whole block.
  };
  struct Spectrum {
    std::vector<double> residualDopplerHz;
    std::vector<Complex> amplitude;
    uint64_t uniqueBlocks = 0, complexMacBound = 0, perCellTrials = 0;
    double delayAtFirstMidpoint = 0, translationDopplerHz = 0;
    uint32_t rateIndex = 0;
  };
  struct Grid {
    std::vector<double> delayAtFirstMidpoint, residualDopplerHz;
    std::vector<Complex> pixels; // Delay-major; invalid entries are not measurements.
    std::vector<uint8_t> valid;
    uint64_t uniqueBlocks = 0, complexMacBound = 0;
    double translationDopplerHz = 0;
  };
  void append(std::shared_ptr<const Frame> frame) {
    using resident::wire::require;
    require(frame && entries_.size() < 8 && frame->phaseQualified,
      "Aperture needs qualified phase and at most eight frames; power fallback is forbidden");
    const auto& s = frame->source; const auto correlation = frame->correlationSamples;
    require(!frame->migration.empty() && !frame->migration[0].banks.empty(), "Missing aperture profiles");
    const auto rows = frame->migration[0].rows;
    const uint64_t processedEnd = frame->firstSample + uint64_t(rows) * correlation;
    require(processedEnd <= frame->endSample, "Aperture processed support exceeds raw support");
    uint64_t missingBefore = 0;
    if (!entries_.empty()) {
      const auto& old = *entries_.back().frame; const auto& p = old.source;
      require(experiment_detail::same(p, s) && frame->sequence == old.sequence + 1 &&
        frame->firstSample > old.firstSample && frame->firstSample <= old.endSample && processedEnd > through_ &&
        frame->correlationSamples == old.correlationSamples && frame->carrierHz == old.carrierHz &&
        bool(frame->derived) == bool(old.derived) && frame->caf[0].delay == old.caf[0].delay &&
        frame->caf[0].doppler == old.caf[0].doppler,
        "Aperture epoch/config/gap/order/block-grid discontinuity");
      if (frame->derived) {
        require(frame->derived->planHash == old.derived->planHash &&
          frame->derived->receiptHash == old.derived->receiptHash && frame->firstSample == old.endSample &&
          (s.snapshotHash == p.snapshotHash || (s.identity.logicalFrame == p.identity.logicalFrame + 1 &&
            s.identity.generation == p.identity.generation + 1 && s.identity.consumedSequence > p.identity.consumedSequence)),
          "Derived aperture acquisition overlap or parent/receipt discontinuity");
      } else {
        require(s.identity.generation == p.identity.generation + 1 && s.identity.consumedSequence > p.identity.consumedSequence,
          "Native aperture generation discontinuity");
      }
      if (frame->firstSample >= through_) {
        require(frame->firstSample == old.endSample, "Only declared unprocessed tails may separate blocks");
        missingBefore = frame->firstSample - through_;
        require(missingBefore == entries_.back().unprocessedTailSamples, "Unaccounted aperture support gap");
      } else {
        require((frame->firstSample - old.firstSample) % correlation == 0,
          "Partial correlation-block overlap is not independent support");
      }
      require(frame->spatialCoefficients == old.spatialCoefficients &&
        frame->anchorChannelId == old.anchorChannelId && frame->minimumPilotCoherence == old.minimumPilotCoherence &&
        frame->declaredSpatialTrials == old.declaredSpatialTrials &&
        frame->migration[0].banks.size() == old.migration[0].banks.size(), "Aperture hypothesis changed");
      require(frame->hopSamples == old.hopSamples && frame->delayDriftPerHz == old.delayDriftPerHz &&
        frame->producers.size() == old.producers.size() &&
        frame->producerSeal.empty() == old.producerSeal.empty(), "Aperture producer handoff changed");
      for (size_t i = 0; i < frame->producers.size(); ++i)
        require(frame->producers[i].configurationSha256 == old.producers[i].configurationSha256 &&
          frame->producers[i].upstreamPlanSha256 == old.producers[i].upstreamPlanSha256,
          "Aperture producer configuration changed");
      for (size_t i = 0; i < frame->migration[0].banks.size(); ++i)
        require(frame->migration[0].banks[i].delayRateBinsPerSecond ==
          old.migration[0].banks[i].delayRateBinsPerSecond, "Aperture rate winner splicing forbidden");
    }
    Entry entry; entry.frame = std::move(frame);
    entry.newRawFirst = entries_.empty() ? entry.frame->firstSample : entries_.back().frame->endSample;
    entry.newRawEnd = entry.frame->endSample;
    entry.unprocessedTailSamples = entry.frame->endSample - processedEnd;
    entry.missingBeforeSamples = missingBefore;
    uint64_t added = 0;
    for (uint32_t b = 0; b < rows; ++b) {
      const bool own = entries_.empty() || entry.frame->firstSample + uint64_t(b) * correlation >= through_;
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
  uint64_t missingSupportSamples() const {
    uint64_t total = 0; for (const auto& e : entries_) total += e.missingBeforeSamples; return total;
  }
  uint64_t complexMacBound() const { return work_; }
  uint64_t remainingWork() const { return RangeMigrationReplay::maximumMacs - work_; }
  uint64_t remainingBytes() const { return (256u << 20) - bytes_; }

  // Prescribed bounded grid, never selected from a detector winner. The domain
  // is applied to EVERY tap of the rate-corrected profile, matching the temporal
  // owner's post-migration domain. It is not a mask on the original raw CAF.
  Grid grid(uint32_t rateIndex, const std::vector<double>& delays,
      const std::vector<double>& frequencies, const EvidenceSourceDomain& domain,
      uint64_t maximumWork = RangeMigrationReplay::maximumMacs, uint64_t maximumBytes = 1u << 20) const {
    using resident::wire::require;
    require(!entries_.empty() && !delays.empty() && delays.size() <= 65 &&
      frequencies.size() >= 3 && frequencies.size() <= 129 && domain.enabled,
      "Invalid bounded aperture grid/domain");
    const auto& first = *entries_.front().frame;
    const double translation = validateFrequencies(rateIndex, frequencies);
    domain.validate(first.caf[0].delay, first.caf[0].doppler, first.caf[0].rows(), first.caf[0].cols());
    Grid out;
    out.uniqueBlocks = uniqueBlocks_;
    out.complexMacBound = work_ + delays.size() * first.migration[0].banks.size() * uniqueBlocks_ *
      (16 * first.caf.size() + frequencies.size());
    require(out.complexMacBound <= maximumWork && maximumWork <= RangeMigrationReplay::maximumMacs &&
      sizeof(Grid) + sizeof(Spectrum) + (delays.size() + 4 * frequencies.size()) * sizeof(double) +
        delays.size() * frequencies.size() * (sizeof(Complex) + 1) <= maximumBytes,
      "Aperture grid aggregate work/storage admission refused");
    out.delayAtFirstMidpoint = delays; out.residualDopplerHz = frequencies;
    const double rate = first.migration[0].banks[rateIndex].delayRateBinsPerSecond;
    out.translationDopplerHz = translation;
    out.pixels.resize(delays.size() * frequencies.size()); out.valid.resize(out.pixels.size());
    for (size_t d = 0; d < delays.size(); ++d) {
      require(std::isfinite(delays[d]) && (!d || delays[d] > delays[d - 1]), "Unordered aperture delays");
      bool supported = true;
      for (const auto& entry : entries_) {
        const auto& frame = *entry.frame; const auto& g = frame.migration[0];
        const double delay = delays[d] + rate * double(frame.firstSample - first.firstSample) / first.source.sampleRateHz;
        const double column = delay - frame.caf[0].delay[g.firstColumn];
        const auto kernel = RangeMigrationReplay::prepareProfile(column, frame.caf[0].doppler[g.rows / 2], first.source.sampleRateHz);
        supported &= column >= RangeMigrationReplay::radius && column + RangeMigrationReplay::radius < g.columns &&
          double(frame.caf[0].delay[g.firstColumn]) + kernel.firstOffset() >= domain.minimumDelay;
      }
      if (!supported) continue;
      const auto values = spectrum(rateIndex, delays[d], frequencies);
      for (size_t f = 0; f < frequencies.size(); ++f) if (domain.allowsDoppler(frequencies[f] + out.translationDopplerHz)) {
        out.valid[d * frequencies.size() + f] = 1;
        out.pixels[d * frequencies.size() + f] = values.amplitude[f];
      }
    }
    return out;
  }

  // A prescribed constant-translation signature, NOT a metre-scaled image.
  // Output retains Doppler Hz because target rotation/geometry is unidentified.
  // All specified bins are emitted; neither peak selection nor autofocus runs.
  Spectrum spectrum(uint32_t rateIndex, double delayAtFirstMidpoint,
      const std::vector<double>& frequencies) const {
    using resident::wire::require;
    require(std::isfinite(delayAtFirstMidpoint), "Invalid aperture spectrum delay");
    const double translation = validateFrequencies(rateIndex, frequencies);
    const auto& first = *entries_[0].frame;
    const auto correlation = first.correlationSamples;
    const double rate = first.migration[0].banks[rateIndex].delayRateBinsPerSecond;
    Spectrum result; result.residualDopplerHz = frequencies; result.amplitude.resize(frequencies.size());
    result.rateIndex = rateIndex; result.delayAtFirstMidpoint = delayAtFirstMidpoint;
    result.translationDopplerHz = translation; result.uniqueBlocks = uniqueBlocks_;
    result.perCellTrials = first.migration[0].banks.size() * uint64_t(first.declaredSpatialTrials);
    result.complexMacBound = work_ + first.migration[0].banks.size() * uniqueBlocks_ *
      (16 * first.caf.size() + frequencies.size());
    require(result.complexMacBound <= RangeMigrationReplay::maximumMacs, "Aperture diagnostic work limit");
    const double tau = 2 * std::acos(-1.);
    for (const auto& entry : entries_) {
      const auto& frame = *entry.frame; const auto& g = frame.migration[0];
      const double offset = double(frame.firstSample - first.firstSample) / first.source.sampleRateHz;
      const double delay = delayAtFirstMidpoint + rate * offset;
      const double column = delay - frame.caf[0].delay[g.firstColumn];
      const double center = frame.caf[0].doppler[g.rows / 2];
      const auto kernel = RangeMigrationReplay::prepareProfile(column, center, first.source.sampleRateHz);
      for (uint32_t b = 0; b < g.rows; ++b) if (entry.ownedRows[b]) {
        const double local = (double(b) * correlation + .5 * (correlation - 1)) / first.source.sampleRateHz;
        const double time = offset + local;
        Complex z{};
        for (size_t channel = 0; channel < frame.migration.size(); ++channel)
          z += frame.spatialCoefficients[channel] * RangeMigrationReplay::sampleProfile(
            frame.migration[channel].banks[rateIndex].rangeSlowTime, g.columns, b, 0u, kernel);
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
  double validateFrequencies(uint32_t rateIndex, const std::vector<double>& frequencies) const {
    using resident::wire::require;
    require(!entries_.empty() && uniqueBlocks_ >= 3 && frequencies.size() >= 3 && frequencies.size() <= 129,
      "Invalid aperture spectrum frequencies/support");
    const auto& first = *entries_.front().frame;
    require(rateIndex < first.migration[0].banks.size(), "Unknown aperture rate");
    const double rate = first.migration[0].banks[rateIndex].delayRateBinsPerSecond, carrier = first.carrierHz;
    require(rate == 0 || (std::isfinite(carrier) && carrier > 0), "Unknown physical carrier; no translation phase inference");
    const double translation = -carrier * rate / first.source.sampleRateHz;
    const double nyquist = .5 * first.source.sampleRateHz / first.correlationSamples;
    const double center = first.caf[0].doppler[first.migration[0].rows / 2];
    for (size_t i = 0; i < frequencies.size(); ++i)
      require(std::isfinite(frequencies[i]) && std::abs(frequencies[i] + translation - center) < nyquist &&
        (!i || frequencies[i] > frequencies[i - 1]), "Aliased/unordered aperture frequencies");
    return translation;
  }
  std::vector<Entry> entries_;
  uint64_t uniqueBlocks_ = 0, through_ = 0, bytes_ = 0, work_ = 0;
};
} // namespace blah2::integration::coherent_replay
