#pragma once

#include "process/integration/CoherentPathEvidence.h"

#include <optional>
#include <string>
#include <utility>

namespace blah2::integration {

// Replay-only adapter from actual complex channel CAFs to the existing path
// integrator. One immutable, unit-norm spatial hypothesis owns each instance.
// The accepted power map is borrowed unchanged; coherent scores are a separate
// uncalibrated statistic and must never be fed into its Gamma(L) CFAR threshold.
class DirectPathCoherentShadow {
public:
  using Complex = std::complex<double>;
  using ComplexMap = Map<Complex>;
  struct Config {
    CoherentPathEvidence::Config temporal;
    std::vector<uint32_t> channelIds;
    std::vector<Complex> spatialCoefficients; // z = sum(coefficient[c] * CAF[c]).
    uint32_t anchorChannelId = 0;
    uint32_t declaredSpatialTrials = 1;
    uint64_t maximumCells = 65536;
    double minimumPilotCoherence = .25; // Squared normalized correlation.
  };
  struct Frame {
    uint64_t captureEpoch = 0, sequence = 0;
    uint64_t firstSample = 0, endSample = 0; // Exact [first,end) capture range.
    uint64_t tuningHz = 0, gainCalibrationEpoch = 0;
    uint32_t sampleRateHz = 0;
    bool samplePositionAvailable = false, inputValid = false;
  };
  struct Pilot {
    Frame frame;
    uint32_t anchorChannelId = 0;
    Complex normalizedCorrelation{};
    double coherence = 0;
  };
  struct Result {
    std::array<CoherentPathEvidence::Bank, 3> banks;
    const ComplexMap* powerFallback = nullptr; // Caller keeps this map alive.
    std::string reason;
    uint64_t declaredPerCellTrials = 0;
    // Calibration includes path/beam maxima, depths, pilot and target-coherence
    // gates, and fallback family. A trial count alone does not establish PFA.
    bool nullCalibrated = false;
    bool usePowerFallback(std::size_t bank, uint64_t cell) const {
      return bank >= banks.size() || !banks[bank].ready ||
        cell >= banks[bank].valid.size() || !banks[bank].valid[cell];
    }
  };

  DirectPathCoherentShadow(Config config, bool replayInput)
    : config_(std::move(config)), coherent_(config_.temporal) {
    if (!replayInput)
      throw std::invalid_argument("Direct-path coherent shadow is replay-only");
    if (config_.channelIds.empty() || config_.channelIds.size() > 8 ||
        config_.spatialCoefficients.size() != config_.channelIds.size() ||
        !config_.declaredSpatialTrials || config_.declaredSpatialTrials > 360 ||
        !config_.maximumCells || config_.maximumCells > (1u << 20) ||
        !std::isfinite(config_.minimumPilotCoherence) ||
        config_.minimumPilotCoherence <= 0 || config_.minimumPilotCoherence > 1)
      throw std::invalid_argument("Direct-path coherent shadow configuration invalid");
    auto ids = config_.channelIds;
    std::sort(ids.begin(), ids.end());
    if (std::adjacent_find(ids.begin(), ids.end()) != ids.end() ||
        !std::binary_search(ids.begin(), ids.end(), config_.anchorChannelId))
      throw std::invalid_argument("Direct-path coherent shadow channel identity invalid");
    double norm = 0;
    for (const auto weight : config_.spatialCoefficients) {
      if (!finite(weight)) throw std::invalid_argument("Nonfinite spatial coefficient");
      norm += std::norm(weight);
    }
    if (std::abs(norm - 1) > 1e-9)
      throw std::invalid_argument("Spatial coefficients must have unit norm");
  }

  // Use ONE fixed physical raw channel before cancellation. Multiplying this
  // correlation by the learned eigenvector would cancel the reference gauge
  // that the pilot must measure. No target cell, truth coordinate, or phase
  // maximization participates in estimating this phasor.
  static std::optional<Pilot> estimatePilot(const Complex* reference,
    const Complex* rawAnchor, std::size_t count, const Frame& frame,
    uint32_t anchorChannelId) {
    if (!reference || !rawAnchor || !validFrame(frame) ||
        uint64_t(count) != frame.endSample - frame.firstSample)
      return std::nullopt;
    Complex cross{}; double referencePower = 0, anchorPower = 0;
    for (std::size_t sample = 0; sample < count; ++sample) {
      if (!finite(reference[sample]) || !finite(rawAnchor[sample]))
        return std::nullopt;
      cross += rawAnchor[sample] * std::conj(reference[sample]);
      referencePower += std::norm(reference[sample]);
      anchorPower += std::norm(rawAnchor[sample]);
    }
    const double normalization = std::sqrt(referencePower) * std::sqrt(anchorPower);
    if (!(normalization > 0) || !std::isfinite(normalization) || !finite(cross))
      return std::nullopt;
    const auto value = cross / normalization;
    if (!finite(value) || !(std::norm(value) > 0)) return std::nullopt;
    return Pilot{frame, anchorChannelId, value, std::min(1.0, std::norm(value))};
  }

  Result process(const std::vector<ComplexMap*>& channels,
    const std::vector<uint32_t>& channelIds, const Frame& frame,
    const std::optional<Pilot>& pilot, const ComplexMap& powerFallback) {
    Result result;
    result.powerFallback = &powerFallback;
    result.declaredPerCellTrials = uint64_t(config_.declaredSpatialTrials) *
      (config_.temporal.maximumCpis == 8 ? 3u : 2u) *
      (2u * config_.temporal.delayMotionBins + 1u) *
      (2u * config_.temporal.dopplerMotionBins + 1u);
    const auto fail = [&](const char* reason) {
      reset(); result.reason = reason; return std::move(result);
    };
    if (!validFrame(frame)) return fail("capture_lineage_unavailable");
    if (!pilot || !sameFrame(frame, pilot->frame) ||
        pilot->anchorChannelId != config_.anchorChannelId)
      return fail("pilot_identity_unavailable");
    const double pilotPower = std::norm(pilot->normalizedCorrelation);
    if (!finite(pilot->normalizedCorrelation) || !std::isfinite(pilotPower) ||
        !std::isfinite(pilot->coherence) || pilotPower > 1 + 1e-9 ||
        std::abs(pilot->coherence - std::min(1.0, pilotPower)) > 1e-9 ||
        pilot->coherence < config_.minimumPilotCoherence)
      return fail("pilot_quality");
    if (channelIds != config_.channelIds || channels.size() != channelIds.size() ||
        channels.empty() || !channels.front()) return fail("channel_identity");
    const uint32_t rows = channels.front()->get_nRows();
    const uint32_t columns = channels.front()->get_nCols();
    if (!rows || !columns || uint64_t(rows) * columns > config_.maximumCells)
      return fail("map_capacity");
    const auto validMap = [&](const ComplexMap& map) {
      return map.data.size() == rows && map.delay == channels.front()->delay &&
        map.doppler == channels.front()->doppler &&
        std::all_of(map.data.begin(), map.data.end(), [&](const auto& row) {
          return row.size() == columns &&
            std::all_of(row.begin(), row.end(), [](auto value) { return finite(value); });
        });
    };
    if (!validMap(powerFallback) ||
        std::any_of(channels.begin(), channels.end(), [&](const auto* map) {
          return !map || !validMap(*map);
        }) || (channels.front()->doppler.size() != rows &&
          channels.front()->doppler.size() != 2) ||
        (channels.front()->delay.size() != columns && channels.front()->delay.size() != 2) ||
        std::any_of(channels.front()->doppler.begin(), channels.front()->doppler.end(),
          [](double value) { return !std::isfinite(value); }))
      return fail("map_invalid");
    bool restarted = false;
    if (previous_) {
      const bool sameIdentity = frame.captureEpoch == previous_->captureEpoch &&
        frame.tuningHz == previous_->tuningHz &&
        frame.gainCalibrationEpoch == previous_->gainCalibrationEpoch &&
        frame.sampleRateHz == previous_->sampleRateHz;
      const bool sameSamples = frame.endSample - frame.firstSample ==
        previous_->endSample - previous_->firstSample;
      if (!sameIdentity || !sameSamples || frame.sequence != previous_->sequence + 1 ||
          frame.firstSample <= previous_->firstSample ||
          (hopSamples_ && frame.firstSample - previous_->firstSample != hopSamples_)) {
        reset(); restarted = true;
      }
    }
    ComplexMap spatial(rows, columns);
    spatial.delay = channels.front()->delay; spatial.doppler = channels.front()->doppler;
    const Complex correction = std::conj(pilot->normalizedCorrelation) / std::sqrt(pilotPower);
    for (uint32_t row = 0; row < rows; ++row)
      for (uint32_t column = 0; column < columns; ++column) {
        Complex value{};
        for (std::size_t channel = 0; channel < channels.size(); ++channel)
          value += config_.spatialCoefficients[channel] * channels[channel]->data[row][column];
        spatial.data[row][column] = value * correction;
      }
    try {
      result.banks = coherent_.process(spatial, frame.captureEpoch, frame.sequence,
        frame.firstSample, frame.sampleRateHz);
    } catch (const std::invalid_argument&) {
      return fail("coherent_lineage_or_numeric_rejection");
    }
    if (previous_ && !hopSamples_) hopSamples_ = frame.firstSample - previous_->firstSample;
    previous_ = frame;
    result.reason = restarted ? "lineage_reset" : result.banks[0].ready ?
      "uncalibrated_coherent_scores" : "warmup";
    return result;
  }

  void reset() { coherent_.reset(); previous_.reset(); hopSamples_ = 0; }

private:
  static bool finite(Complex value) {
    return std::isfinite(value.real()) && std::isfinite(value.imag());
  }
  static bool validFrame(const Frame& frame) {
    return frame.samplePositionAvailable && frame.inputValid && frame.sampleRateHz &&
      frame.tuningHz && frame.gainCalibrationEpoch && frame.endSample > frame.firstSample;
  }
  static bool sameFrame(const Frame& left, const Frame& right) {
    return left.captureEpoch == right.captureEpoch && left.sequence == right.sequence &&
      left.firstSample == right.firstSample && left.endSample == right.endSample &&
      left.tuningHz == right.tuningHz && left.gainCalibrationEpoch == right.gainCalibrationEpoch &&
      left.sampleRateHz == right.sampleRateHz &&
      left.samplePositionAvailable == right.samplePositionAvailable && left.inputValid == right.inputValid;
  }
  Config config_;
  CoherentPathEvidence coherent_;
  std::optional<Frame> previous_;
  uint64_t hopSamples_ = 0;
};

} // namespace blah2::integration
