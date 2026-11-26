#pragma once

#include "process/integration/CoherentPathEvidence.h"
#include "process/integration/DirectPathCoherentReplay.h"
#include "process/integration/RangeMigrationReplay.h"
#include "process/ambiguity/GpuResidentWire.h"
#include "capture/Sha256.h"

#include <locale>
#include <optional>
#include <sstream>
#include <string>
#include <utility>

namespace blah2::integration {

// Replay-only adapter from actual complex channel CAFs to the existing path
// integrator. One immutable, unit-norm spatial hypothesis owns each instance.
// The accepted power map is borrowed unchanged; coherent scores are a separate
// uncalibrated statistic and must never be fed into its Gamma(L) CFAR threshold.
class DirectPathCoherentExperiment {
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
    uint32_t fallbackBeamCount = 0, fallbackBeamTolerance = UINT32_MAX;
    std::vector<double> migrationRates; // Reader-only immutable intra-CPI trials.
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
    TemporalEvidence::Result noncoherent;
    // Exact newest-first capture ranges for either selected family. Overlap
    // between rolling banks remains visible to the shared ConfirmationLedger.
    std::array<Frame, 8> sources{};
    uint32_t sourceCount = 0;
    const ComplexMap* powerFallback = nullptr; // Borrowed real FP64 power, not amplitude.
    std::string reason;
    std::string configurationSha256; // Produced by the immutable numerical owner.
    uint64_t declaredPerCellTrials = 0;
    // Calibration includes path/beam maxima, depths, pilot and target-coherence
    // gates, and fallback family. A trial count alone does not establish PFA.
    bool nullCalibrated = false;
    bool usePowerFallback(std::size_t bank, uint64_t cell) const {
      return bank >= banks.size() || !banks[bank].ready ||
        cell >= banks[bank].valid.size() || !banks[bank].valid[cell];
    }
    struct Cell {
      Complex amplitude{};
      double power = 0;
      EvidenceLineage lineage;
      bool coherent = false;
      bool valid = false;
    };
    // Independent uncalibrated families. Consumers must not compare raw power
    // or replace NC merely because a coherent path is numerically available.
    Cell coherentAt(std::size_t bank, uint64_t cell) const {
      if (usePowerFallback(bank, cell)) return {};
      const auto lineage = banks[bank].lineageAt(cell);
      if (!lineage.depth) return {};
      const auto columns = banks[bank].map->get_nCols();
      const auto value = banks[bank].map->data[cell / columns][cell % columns];
      const double power = std::norm(value);
      return std::isfinite(power) ? Cell{value, power, lineage, true, true} : Cell{};
    }
    Cell noncoherentAt(std::size_t bank, uint64_t cell) const {
      if (bank >= banks.size()) return {};
      const auto& fallback = noncoherent.banks[bank + 1];
      auto lineage = fallback.lineageAt(cell);
      if (!lineage.depth) return {};
      attachRanges(lineage);
      const auto columns = fallback.map->get_nCols();
      return {fallback.map->data[cell / columns][cell % columns],
        fallback.detectorPower[cell], lineage, false, true};
    }
    // Legacy availability-based view, not a maximum or a calibrated detector.
    // Retained for exact frozen replay compatibility; new comparisons retain
    // both families and calibrate their full joint statistic independently.
    Cell hybridAt(std::size_t bank, uint64_t cell) const {
      const auto coherent = coherentAt(bank, cell);
      return coherent.valid ? coherent : noncoherentAt(bank, cell);
    }
  private:
    void attachRanges(EvidenceLineage& lineage) const {
      for (uint32_t index = 0; index < lineage.depth; ++index)
        for (uint32_t source = 0; source < sourceCount; ++source)
          if (sources[source].sequence == lineage.sequences[index]) {
            lineage.firstSamples[index] = sources[source].firstSample;
            lineage.endSamples[index] = sources[source].endSample;
            break;
          }
    }
  public:
  };

  // One serialization for selector admission and the producing owner. Axes
  // and per-rate ingress identity are checked separately at the replay seam.
  static std::string configurationSha256(const Config& c) {
    const auto& t = c.temporal;
    std::ostringstream s; s.imbue(std::locale::classic()); s << std::hexfloat;
    s << "direct-path-experiment-v1 " << t.maximumCpis << ' ' << t.delayMotionBins << ' '
      << t.dopplerMotionBins << ' ' << t.cpiHopSeconds << ' ' << t.minimumCoherence << ' '
      << t.phaseDirection << ' ' << t.delayDriftPerHz << ' ' << t.beamCount << ' '
      << t.refineDopplerPhase << ' ' << t.interpolateDelay << ' ' << t.heterodyneCenterHz << ' '
      << t.measureTiming << ' ' << t.sourceDomain.enabled << ' ' << t.sourceDomain.minimumDelay << ' '
      << t.sourceDomain.minimumAbsDopplerHz << ' ' << c.anchorChannelId << ' '
      << c.declaredSpatialTrials << ' ' << c.maximumCells << ' ' << c.minimumPilotCoherence << ' '
      << c.fallbackBeamCount << ' ' << c.fallbackBeamTolerance << " channels ";
    for (auto value : c.channelIds) s << value << ' ';
    s << "weights "; for (auto value : c.spatialCoefficients) s << value.real() << ' ' << value.imag() << ' ';
    s << "rates "; for (double value : c.migrationRates) s << value << ' ';
    const auto text = s.str(); Sha256 hash; hash.update(text.data(), text.size()); return hash.finish();
  }

  DirectPathCoherentExperiment(Config config, bool replayInput)
    : config_(std::move(config)), configurationHash_(configurationSha256(config_)), coherent_(config_.temporal),
      noncoherent_({config_.temporal.maximumCpis, config_.temporal.delayMotionBins,
        config_.temporal.dopplerMotionBins, config_.temporal.delayDriftPerHz,
        config_.fallbackBeamCount, config_.fallbackBeamTolerance, config_.temporal.sourceDomain}) {
    if (!replayInput || !config_.migrationRates.empty())
      throw std::invalid_argument("Direct-path coherent experiment is replay-only");
    if (config_.channelIds.empty() || config_.channelIds.size() > 8 ||
        config_.spatialCoefficients.size() != config_.channelIds.size() ||
        !config_.declaredSpatialTrials || config_.declaredSpatialTrials > 360 ||
        !config_.maximumCells || config_.maximumCells > (1u << 20) ||
        !std::isfinite(config_.minimumPilotCoherence) ||
        config_.minimumPilotCoherence <= 0 || config_.minimumPilotCoherence > 1 ||
        config_.temporal.beamCount)
      throw std::invalid_argument("Direct-path coherent experiment configuration invalid");
    auto ids = config_.channelIds;
    std::sort(ids.begin(), ids.end());
    if (std::adjacent_find(ids.begin(), ids.end()) != ids.end() ||
        !std::binary_search(ids.begin(), ids.end(), config_.anchorChannelId))
      throw std::invalid_argument("Direct-path coherent experiment channel identity invalid");
    double norm = 0;
    for (const auto weight : config_.spatialCoefficients) {
      if (!finite(weight)) throw std::invalid_argument("Nonfinite spatial coefficient");
      norm += std::norm(weight);
    }
    if (std::abs(norm - 1) > 1e-9)
      throw std::invalid_argument("Spatial coefficients must have unit norm");
  }

  TemporalEvidence::PathAudit auditPowerPath(uint64_t cell, uint32_t depth,
    int dr, int dc) const { return noncoherent_.auditPath(cell, depth, dr, dc, false); }

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
    const std::optional<Pilot>& pilot, ComplexMap& powerFallback,
    const std::vector<uint32_t>* fallbackBeams = nullptr) {
    Result result;
    result.configurationSha256 = configurationHash_;
    for (std::size_t bank = 0; bank < result.banks.size(); ++bank)
      result.banks[bank].depth = 2u << bank;
    result.powerFallback = &powerFallback;
    result.declaredPerCellTrials = uint64_t(config_.declaredSpatialTrials) *
      CoherentPathEvidence::totalPhaseTrials(config_.temporal) *
      (2u * config_.temporal.delayMotionBins + 1u) *
      (2u * config_.temporal.dopplerMotionBins + 1u);
    const auto fail = [&](const char* reason) {
      reset(); result.reason = reason; return std::move(result);
    };
    if (!validFrame(frame)) return fail("capture_lineage_unavailable");
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
    if (powerFallback.get_nRows() != rows || powerFallback.get_nCols() != columns ||
        !validMap(powerFallback) ||
        std::any_of(channels.begin(), channels.end(), [&](const auto* map) {
          return !map || !validMap(*map);
        }) || channels.front()->doppler.size() != rows ||
        (channels.front()->delay.size() != columns && channels.front()->delay.size() != 2) ||
        std::any_of(channels.front()->doppler.begin(), channels.front()->doppler.end(),
          [](double value) { return !std::isfinite(value); }))
      return fail("map_invalid");
    bool restarted = false;
    if (previous_) {
      if (frame.captureEpoch == previous_->captureEpoch &&
          (frame.sequence <= previous_->sequence || frame.firstSample <= previous_->firstSample)) {
        coherent_.reset(); noncoherent_.reset(); sources_.clear(); hopSamples_ = 0;
        result.reason = "capture_reordered"; return result;
      }
      const bool sameIdentity = frame.captureEpoch == previous_->captureEpoch &&
        frame.tuningHz == previous_->tuningHz &&
        frame.gainCalibrationEpoch == previous_->gainCalibrationEpoch &&
        frame.sampleRateHz == previous_->sampleRateHz;
      const bool sameSamples = frame.endSample - frame.firstSample ==
        previous_->endSample - previous_->firstSample;
      const double declaredHop = config_.temporal.cpiHopSeconds * frame.sampleRateHz;
      if (!sameIdentity || !sameSamples || frame.sequence != previous_->sequence + 1 ||
          frame.firstSample <= previous_->firstSample ||
          (config_.temporal.cpiHopSeconds > 0 &&
            double(frame.firstSample - previous_->firstSample) != declaredHop) ||
          (hopSamples_ && frame.firstSample - previous_->firstSample != hopSamples_)) {
        reset(); restarted = true;
      }
    }
    try {
      result.noncoherent = noncoherent_.process(powerFallback, frame.captureEpoch,
        frame.sequence, nullptr, fallbackBeams, TemporalEvidence::Input::realPower);
    } catch (const std::exception&) { return fail("power_lineage_or_numeric_rejection"); }
    if (previous_ && !hopSamples_) hopSamples_ = frame.firstSample - previous_->firstSample;
    previous_ = frame;
    sources_.push_front(frame);
    while (sources_.size() > config_.temporal.maximumCpis) sources_.pop_back();
    result.sourceCount = uint32_t(sources_.size());
    std::copy(sources_.begin(), sources_.end(), result.sources.begin());
    const auto phaseFail = [&](const char* reason) {
      coherent_.reset(); result.reason = reason; return std::move(result);
    };
    if (!pilot || !sameFrame(frame, pilot->frame) ||
        pilot->anchorChannelId != config_.anchorChannelId)
      return phaseFail("pilot_identity_unavailable");
    const double pilotPower = std::norm(pilot->normalizedCorrelation);
    if (!finite(pilot->normalizedCorrelation) || !std::isfinite(pilotPower) ||
        !std::isfinite(pilot->coherence) || pilotPower > 1 + 1e-9 ||
        std::abs(pilot->coherence - std::min(1.0, pilotPower)) > 1e-9 ||
        pilot->coherence < config_.minimumPilotCoherence)
      return phaseFail("pilot_quality");
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
        frame.firstSample, frame.sampleRateHz, nullptr, frame.endSample);
    } catch (const std::invalid_argument&) {
      return phaseFail("coherent_lineage_or_numeric_rejection");
    }
    result.reason = restarted ? "lineage_reset" : result.banks[0].ready ?
      "uncalibrated_coherent_scores" : "warmup";
    return result;
  }

  void reset() {
    coherent_.reset(); noncoherent_.reset(); sources_.clear(); previous_.reset(); hopSamples_ = 0;
  }

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
  std::string configurationHash_;
  CoherentPathEvidence coherent_;
  TemporalEvidence noncoherent_;
  std::deque<Frame> sources_;
  std::optional<Frame> previous_;
  uint64_t hopSamples_ = 0;
};

} // namespace blah2::integration

// Explicit offline replay reader. No installed runtime entrypoint includes this
// numerical owner; the live binary uses only DirectPathCoherentReplay sidecars.
namespace blah2::integration::coherent_replay {
// Shared decoder preserves the native channel-major, shifted-Doppler layout.
inline std::vector<DirectPathCoherentExperiment::ComplexMap> channelMaps(const ValidatedSnapshot&);

// CPU-only parser and adapter.  No detector/PFA state is accepted or emitted.
class ExperimentReader {
public:
  // An immutable, pre-winner/pre-sum linear data product. Raw CAF and the
  // optional range/slow-time representation share the canonical transform.
  struct ApertureInput {
    Sidecar source; // Original sealed parent identity/range, including for slices.
    std::optional<resident::Config> sourceConfig; // Absent, never invented, for derived IQ.
    std::optional<DerivedIqLineage> derived;
    uint64_t sequence = 0, firstSample = 0, endSample = 0;
    uint32_t correlationSamples = 0;
    double carrierHz = 0, referenceConditioningBandwidthHz = NAN;
    std::vector<DirectPathCoherentExperiment::ComplexMap> caf;
    std::vector<resident_replay::Hash> blockHashes; // Ordered BlockKind 1..20.
    std::optional<DirectPathCoherentExperiment::Pilot> pilot;
  };
  struct ApertureFrame : ApertureInput {
    std::vector<uint32_t> channelIds;
    std::vector<RangeMigrationReplay::Result> migration;
    std::vector<std::complex<double>> spatialCoefficients;
    std::complex<double> gaugeCorrection{};
    bool phaseQualified = false;
    uint32_t anchorChannelId = 0, declaredSpatialTrials = 0;
    uint64_t retainedBytes = 0, complexMacBound = 0;
    double minimumPilotCoherence = 0;
  };
  struct RateResult {
    uint32_t rateIndex = 0, firstColumn = 0;
    double delayRateBinsPerSecond = 0, heterodyneCenterHz = 0, whiteNoiseGain = 1;
    uint64_t declaredPerCellTrials = 0, combinedPerCellTrials = 0;
    uint32_t correlationSamples = 0;
    uint64_t hopSamples = 0; // Zero until the second sealed frame locks the hop.
    double delayDriftPerHz = 0;
    Sidecar source; // Original, uncropped raw sample span and sealed config hash.
    std::optional<DirectPathCoherentExperiment::Pilot> measuredPilot;
    bool rateCorrectedFallback = false;
    // Recorded native beam choices on the common crop, retained as provenance.
    // Migration fallback is one fixed spatial projection, not these choices.
    std::vector<uint32_t> recordedBeams;
    std::unique_ptr<DirectPathCoherentExperiment::ComplexMap> fallback;
    DirectPathCoherentExperiment::Result result;
    RateResult() = default;
    RateResult(RateResult&&) = default;
    RateResult& operator=(RateResult&&) = default;
    RateResult(const RateResult&) = delete;
    RateResult& operator=(const RateResult&) = delete;
  };
  explicit ExperimentReader(DirectPathCoherentExperiment::Config);
  DirectPathCoherentExperiment::Result append(const std::filesystem::path& snapshot);
  DirectPathCoherentExperiment::Result append(const ValidatedSnapshot& snapshot);
  std::vector<RateResult> appendAll(const std::filesystem::path& snapshot);
  std::vector<RateResult> appendAll(const ValidatedSnapshot& snapshot);
  // Stateless extraction; imaging aperture ownership is separate from the
  // detector's rolling evidence histories. No detector state is advanced.
  std::shared_ptr<const ApertureFrame> aperture(const ValidatedSnapshot&,
    uint64_t maximumWork = RangeMigrationReplay::maximumMacs, uint64_t maximumBytes = 128u << 20) const;
  // One pre-temporal/pre-winner boundary, shared by native decoding and
  // authenticated derived-IQ replay. This method performs no temporal search.
  std::shared_ptr<const ApertureFrame> aperture(ApertureInput,
    uint64_t maximumWork = RangeMigrationReplay::maximumMacs, uint64_t maximumBytes = 128u << 20) const;
  std::shared_ptr<const ApertureFrame> aperture(const std::filesystem::path& path,
      uint64_t maximumWork = RangeMigrationReplay::maximumMacs, uint64_t maximumBytes = 128u << 20) const {
    resident::wire::require(std::filesystem::file_size(path) <= (128u << 20), "Aperture snapshot exceeds 128 MiB");
    return aperture(loadValidatedSnapshot(path), maximumWork, maximumBytes);
  }
private:
  struct DecodedFrame {
    std::vector<DirectPathCoherentExperiment::ComplexMap> maps;
    std::unique_ptr<DirectPathCoherentExperiment::ComplexMap> power;
    std::vector<uint32_t> beams;
    DirectPathCoherentExperiment::Frame frame;
    std::optional<DirectPathCoherentExperiment::Pilot> pilot;
  };
  DecodedFrame decode(const ValidatedSnapshot&) const;
  DirectPathCoherentExperiment::Config config_;
  std::vector<std::unique_ptr<DirectPathCoherentExperiment>> experiments_;
  // First-frame prepared inputs only, released as soon as the immutable raw
  // hop is known. No raw IQ or upstream rate winner is retained here.
  std::vector<DecodedFrame> bootstrap_;
  uint64_t hopSamples_ = 0;
  std::unique_ptr<DirectPathCoherentExperiment::ComplexMap> fallback_;
  std::optional<Sidecar> previous_;
};

} // namespace blah2::integration::coherent_replay

namespace blah2::integration::coherent_replay {
using resident::wire::Reader;
using resident::wire::require;
namespace experiment_detail {
inline bool same(const Sidecar& a, const Sidecar& b) {
  return a.captureEpoch == b.captureEpoch && a.identity.epoch == b.identity.epoch &&
    a.identity.configHash == b.identity.configHash &&
    a.identity.moduleHash == b.identity.moduleHash &&
    a.identity.deviceHash == b.identity.deviceHash &&
    a.sourceHash == b.sourceHash && a.rfHash == b.rfHash &&
    a.tuningHz == b.tuningHz &&
    a.gainCalibrationEpoch == b.gainCalibrationEpoch &&
    a.sampleRateHz == b.sampleRateHz && a.actualGainDb == b.actualGainDb;
}
inline DirectPathCoherentExperiment::ComplexMap channelMap(const resident_replay::ResidentReplaySnapshot& snapshot,
  const resident::Config& config, uint32_t channel) {
  const uint32_t rows = config.geometry.doppler, columns = config.geometry.delays;
  const uint64_t cells = uint64_t(rows) * columns;
  require(channel < config.geometry.channels && cells <= (1u << 20), "Replay channel map bounds invalid");
  const auto& bytes = snapshot.blocks.at(uint32_t(resident_replay::BlockKind::channelMaps) - 1).bytes;
  const Reader r{{bytes.data(), bytes.size()}};
  DirectPathCoherentExperiment::ComplexMap result(rows, columns);
  result.delay.assign(config.delayAxis.begin(), config.delayAxis.end());
  result.doppler.assign(config.dopplerAxis.begin(), config.dopplerAxis.end());
  for (uint32_t row = 0; row < rows; ++row) for (uint32_t column = 0; column < columns; ++column) {
    const uint64_t index = uint64_t(channel) * cells + uint64_t(column) * rows + ((row + rows / 2 + 1) % rows);
    const auto real = r.f32(size_t(index) * 8), imag = r.f32(size_t(index) * 8 + 4);
    require(std::isfinite(real) && std::isfinite(imag), "Replay complex channel map is nonfinite");
    result.data[row][column] = {real, imag};
  }
  return result;
}
}
inline ExperimentReader::ExperimentReader(DirectPathCoherentExperiment::Config config)
  : config_(std::move(config)) {
  if (!config_.migrationRates.empty()) {
    require(config_.migrationRates.size() <= RangeMigrationReplay::maximumTrials &&
      std::find(config_.migrationRates.begin(), config_.migrationRates.end(), 0.) != config_.migrationRates.end(),
      "Migration rates require a bounded bank including zero");
    for (size_t i = 0; i < config_.migrationRates.size(); ++i)
      require(std::isfinite(config_.migrationRates[i]) &&
        std::find(config_.migrationRates.begin(), config_.migrationRates.begin() + i, config_.migrationRates[i]) == config_.migrationRates.begin() + i,
        "Migration rates must be finite and unique");
  }
}
inline DirectPathCoherentExperiment::Result ExperimentReader::append(const std::filesystem::path& path) {
  return append(loadValidatedSnapshot(path));
}
inline std::vector<DirectPathCoherentExperiment::ComplexMap> channelMaps(const ValidatedSnapshot& frame) {
  std::vector<DirectPathCoherentExperiment::ComplexMap> result;
  result.reserve(frame.config.geometry.channels);
  for (uint32_t channel = 0; channel < frame.config.geometry.channels; ++channel)
    result.push_back(experiment_detail::channelMap(frame.snapshot, frame.config, channel));
  return result;
}
inline DirectPathCoherentExperiment::Result ExperimentReader::append(const ValidatedSnapshot& validated) {
  require(config_.migrationRates.empty(), "Migration requires appendAll; upstream rate winners are forbidden");
  auto all = appendAll(validated);
  fallback_ = std::move(all.front().fallback);
  return std::move(all.front().result);
}
inline std::vector<ExperimentReader::RateResult> ExperimentReader::appendAll(const std::filesystem::path& path) {
  return appendAll(loadValidatedSnapshot(path));
}
inline ExperimentReader::DecodedFrame ExperimentReader::decode(const ValidatedSnapshot& validated) const {
  const auto& sidecar = validated.sidecar; const auto& config = validated.config;
  const auto& snapshot = validated.snapshot;
  if (previous_) { require(experiment_detail::same(*previous_, sidecar), "Replay coherent lineage identity changed"); require(sidecar.identity.logicalFrame == previous_->identity.logicalFrame + 1 && sidecar.identity.generation == previous_->identity.generation + 1 && sidecar.identity.consumedSequence > previous_->identity.consumedSequence && sidecar.firstSample > previous_->firstSample, "Replay coherent sidecars reordered or discontinuous"); }
  require(config_.migrationRates.empty() || !previous_ || sidecar.firstSample <= previous_->endSample,
    "Migration history cannot cross a raw-IQ gap");
  require(!previous_ || !hopSamples_ || sidecar.firstSample - previous_->firstSample == hopSamples_,
    "Replay coherent raw-sample hop changed");
  require(sidecar.endSample > sidecar.firstSample &&
    sidecar.endSample - sidecar.firstSample == config.geometry.clutterSamples &&
    sidecar.sampleRateHz == config.sampleRate &&
    sidecar.identity.configHash == resident::wire::configHash(config), "Replay raw span or configuration lineage differs");
  require(config.surveillanceChannels == config_.channelIds, "Replay spatial channel identity differs");
  DecodedFrame decoded; decoded.maps = channelMaps(validated);
  decoded.power = std::make_unique<DirectPathCoherentExperiment::ComplexMap>(config.geometry.doppler, config.geometry.delays);
  decoded.power->delay.assign(config.delayAxis.begin(), config.delayAxis.end());
  decoded.power->doppler.assign(config.dopplerAxis.begin(), config.dopplerAxis.end());
  const auto& currentPower = snapshot.blocks.at(
    uint32_t(resident_replay::BlockKind::currentPower) - 1).bytes;
  const Reader powerReader{{currentPower.data(), currentPower.size()}};
  const auto& fused = snapshot.blocks.at(uint32_t(resident_replay::BlockKind::fused) - 1).bytes;
  const Reader fusedReader{{fused.data(), fused.size()}};
  decoded.beams.resize(size_t(config.geometry.doppler) * config.geometry.delays);
  for (uint32_t row = 0; row < config.geometry.doppler; ++row)
    for (uint32_t column = 0; column < config.geometry.delays; ++column) {
      const size_t cell = size_t(row) * config.geometry.delays + column;
      decoded.power->data[row][column] = DirectPathCoherentExperiment::Complex(
        powerReader.f64(cell * 8), 0);
      decoded.beams[cell] = config.fusion.beams ? uint32_t(fusedReader.f64(cell * 16 + 8)) + 1 : 0;
    }
  decoded.frame = {sidecar.captureEpoch,sidecar.identity.logicalFrame,sidecar.firstSample,sidecar.endSample,sidecar.tuningHz,sidecar.gainCalibrationEpoch,sidecar.sampleRateHz,true,true};
  const auto anchor = std::find(config.surveillanceChannels.begin(), config.surveillanceChannels.end(), config_.anchorChannelId);
  require(anchor != config.surveillanceChannels.end(), "Replay experiment anchor channel unavailable");
  const auto anchorIndex = size_t(std::distance(config.surveillanceChannels.begin(), anchor));
  const auto& raw = snapshot.blocks.at(uint32_t(resident_replay::BlockKind::rawIq) - 1).bytes;
  const auto& reference = snapshot.blocks.at(uint32_t(resident_replay::BlockKind::referenceIq) - 1).bytes;
  const auto samples = config.geometry.clutterSamples; require(raw.size() == size_t(samples) * config.geometry.channels * 16 && reference.size() == size_t(samples) * 16, "Replay IQ pilot layout differs");
  const Reader rawReader{{raw.data(), raw.size()}}, referenceReader{{reference.data(), reference.size()}}; std::vector<DirectPathCoherentExperiment::Complex> pilotReference(samples), pilotAnchor(samples);
  for (uint32_t sample = 0; sample < samples; ++sample) { pilotReference[sample] = {referenceReader.f64(size_t(sample) * 16), referenceReader.f64(size_t(sample) * 16 + 8)}; const size_t offset = (anchorIndex * size_t(samples) + sample) * 16; pilotAnchor[sample] = {rawReader.f64(offset), rawReader.f64(offset + 8)}; }
  decoded.pilot = DirectPathCoherentExperiment::estimatePilot(pilotReference.data(), pilotAnchor.data(), samples,
    decoded.frame, config_.anchorChannelId);
  return decoded;
}
inline std::vector<ExperimentReader::RateResult> ExperimentReader::appendAll(const ValidatedSnapshot& validated) {
  auto decoded = decode(validated);
  const auto& config = validated.config;
  const bool migration = !config_.migrationRates.empty();
  const size_t count = migration ? config_.migrationRates.size() : 1;
  std::vector<RangeMigrationReplay::Result> corrected;
  if (migration) {
    const uint64_t processed = uint64_t(config.geometry.doppler) * config.correlation;
    require(processed && processed <= config.geometry.clutterSamples &&
      config.geometry.clutterSamples - processed < config.geometry.doppler,
      "Migration raw sample span is incompatible with correlation blocks");
    for (const auto& map : decoded.maps)
      corrected.push_back(RangeMigrationReplay::process(map, validated.sidecar.sampleRateHz,
        config.correlation, config_.migrationRates));
  }
  const auto motion = track_before_detect::decodeMotionBins(config.detector.integrationMotionBins);
  const uint64_t observedHop = previous_ ? validated.sidecar.firstSample - previous_->firstSample : 0;
  require(observedHop <= UINT32_MAX, "Replay coherent sample hop exceeds drift bounds");
  const double drift = motion.fractionalDelay && observedHop ?
    track_before_detect::delayDriftPerHz(uint32_t(observedHop), config.temporalCarrierHz()) : 0;
  const bool bindHop = previous_ && !hopSamples_;
  if (experiments_.empty() || bindHop) {
    auto bound = config_; bound.migrationRates.clear();
    // CPI span is not the hop when capture windows overlap. The first call
    // has no multi-CPI output; seed final immutable engines once lineage locks
    // the hop, preserving both complex and canonical nearest-cell histories.
    bound.temporal.delayDriftPerHz = drift;
    bound.temporal.interpolateDelay = motion.fractionalDelay;
    // Composed migration maps below already remove the complete range ramp.
    // Legacy maps retain it; never de-ramp the same gauge twice.
    bound.temporal.heterodyneCenterHz = migration ? 0 : config.dopplerAxis.at(config.geometry.doppler / 2);
    // A corrected fixed projection has no native selected-beam decisions.
    bound.fallbackBeamCount = migration ? 0 : config.fusion.beams;
    bound.fallbackBeamTolerance = config.detector.integrationBeamTolerance;
    std::vector<std::unique_ptr<DirectPathCoherentExperiment>> initialized;
    if (bindHop) require(bootstrap_.size() == count, "Replay coherent first-frame bootstrap unavailable");
    for (size_t index = 0; index < count; ++index) {
      auto engine = std::make_unique<DirectPathCoherentExperiment>(bound, true);
      if (bindHop) {
        auto& first = bootstrap_[index];
        std::vector<DirectPathCoherentExperiment::ComplexMap*> pointers;
        for (auto& map : first.maps) pointers.push_back(&map);
        engine->process(pointers, config.surveillanceChannels, first.frame, first.pilot,
          *first.power, migration ? nullptr : &first.beams);
      }
      initialized.push_back(std::move(engine));
    }
    experiments_ = std::move(initialized);
  }
  std::vector<RateResult> results;
  try { for (size_t index = 0; index < count; ++index) {
    RateResult item; item.rateIndex = uint32_t(index); item.source = validated.sidecar;
    item.measuredPilot = decoded.pilot;
    item.hopSamples = observedHop; item.delayDriftPerHz = drift;
    item.correlationSamples = config.correlation; item.rateCorrectedFallback = migration;
    auto maps = migration ? std::vector<DirectPathCoherentExperiment::ComplexMap>{} : std::move(decoded.maps);
    if (migration) {
      const auto& geometry = corrected.front();
      item.delayRateBinsPerSecond = config_.migrationRates[index]; item.firstColumn = geometry.firstColumn;
      item.heterodyneCenterHz = config.dopplerAxis.at(geometry.rows / 2);
      item.whiteNoiseGain = geometry.banks[index].whiteNoiseGain;
      for (const auto& channel : corrected) {
        maps.emplace_back(geometry.rows, geometry.columns); auto& map = maps.back();
        map.doppler.assign(config.dopplerAxis.begin(), config.dopplerAxis.end());
        map.delay.assign(config.delayAxis.begin() + geometry.firstColumn,
          config.delayAxis.begin() + geometry.firstColumn + geometry.columns);
        for (uint32_t row = 0; row < geometry.rows; ++row)
          for (uint32_t col = 0; col < geometry.columns; ++col)
            map.data[row][col] = channel.banks[index].cells[uint64_t(row) * geometry.columns + col] *
              std::polar(1., -2 * std::acos(-1.) * item.heterodyneCenterHz * map.delay[col] / validated.sidecar.sampleRateHz);
      }
      item.fallback = std::make_unique<DirectPathCoherentExperiment::ComplexMap>(geometry.rows, geometry.columns);
      item.fallback->delay = maps.front().delay; item.fallback->doppler = maps.front().doppler;
      for (uint32_t row = 0; row < geometry.rows; ++row)
        for (uint32_t col = 0; col < geometry.columns; ++col) {
          DirectPathCoherentExperiment::Complex spatial{};
          for (size_t channel = 0; channel < maps.size(); ++channel)
            spatial += config_.spatialCoefficients[channel] * maps[channel].data[row][col];
          item.fallback->data[row][col] = {std::norm(spatial), 0};
          item.recordedBeams.push_back(decoded.beams[size_t(row) * config.geometry.delays + geometry.firstColumn + col]);
        }
    } else { item.fallback = std::move(decoded.power); item.recordedBeams = std::move(decoded.beams); }
    std::vector<DirectPathCoherentExperiment::ComplexMap*> pointers;
    for (auto& map : maps) pointers.push_back(&map);
    item.result = experiments_[index]->process(pointers, config.surveillanceChannels, decoded.frame,
      decoded.pilot, *item.fallback, migration ? nullptr : &item.recordedBeams);
    if (!previous_) {
      require(item.result.sourceCount == 1, "Replay coherent first-frame input rejected");
      DecodedFrame first; first.maps = std::move(maps);
      first.power = std::make_unique<DirectPathCoherentExperiment::ComplexMap>(*item.fallback);
      first.beams = item.recordedBeams; first.frame = decoded.frame; first.pilot = decoded.pilot;
      bootstrap_.push_back(std::move(first));
    }
    // Native fallback may already maximize over recorded beams; corrected
    // fallback uses the declared fixed spatial hypothesis instead.
    const uint64_t fallbackSpatialTrials = migration ? config_.declaredSpatialTrials : std::max(1u, config.fusion.beams);
    item.declaredPerCellTrials = item.result.declaredPerCellTrials +
      (config_.temporal.maximumCpis == 8 ? 3u : config_.temporal.maximumCpis == 4 ? 2u : 1u) *
      (2u * config_.temporal.delayMotionBins + 1u) *
      (2u * config_.temporal.dopplerMotionBins + 1u) * fallbackSpatialTrials;
    item.combinedPerCellTrials = count * item.declaredPerCellTrials;
    results.push_back(std::move(item));
  }} catch (...) { experiments_.clear(); bootstrap_.clear(); throw; }
  if (bindHop) { hopSamples_ = observedHop; bootstrap_.clear(); }
  previous_ = validated.sidecar;
  return results;
}

inline std::shared_ptr<const ExperimentReader::ApertureFrame>
ExperimentReader::aperture(const ValidatedSnapshot& validated, uint64_t maximumWork, uint64_t maximumBytes) const {
  const auto& c = validated.config;
  const uint64_t rows = c.geometry.doppler, columns = c.geometry.delays;
  const uint64_t cells = rows * columns, channels = c.geometry.channels;
  require(channels > 0 && channels <= 5 && rows >= 3 && rows <= 511 &&
    columns <= 4096 && cells <= 65536, "Aperture frame dimensions exceed bounds");
  uint64_t snapshotBytes = resident_replay::headerBytes;
  for (const auto& block : validated.snapshot.blocks) {
    require(block.bytes.size() <= (128u << 20) - snapshotBytes, "Aperture snapshot exceeds byte bound");
    snapshotBytes += block.bytes.size();
  }
  resident_replay::validate(validated.snapshot, c);
  const auto& a = validated.sidecar.identity; const auto& b = validated.snapshot.identity;
  require(a.epoch == b.epoch && a.logicalFrame == b.logicalFrame && a.generation == b.generation &&
    a.consumedSequence == b.consumedSequence && a.signalTimestampMs == b.signalTimestampMs &&
    a.trackerTransaction == b.trackerTransaction && a.rawIqHash == b.rawIqHash &&
    a.configHash == b.configHash && a.moduleHash == b.moduleHash && a.deviceHash == b.deviceHash &&
    validated.sidecar.sourceHash == c.replay.sourceHash && validated.sidecar.rfHash == c.replay.rfHash &&
    validated.sidecar.actualGainDb == c.replay.actualGainDb,
    "Aperture snapshot identity differs from sidecar");
  auto decoded = decode(validated);
  ApertureInput input;
  input.source = validated.sidecar; input.sourceConfig = c;
  input.sequence = input.source.identity.logicalFrame;
  input.firstSample = input.source.firstSample; input.endSample = input.source.endSample;
  input.correlationSamples = c.correlation; input.carrierHz = c.temporalCarrierHz();
  input.referenceConditioningBandwidthHz = c.reference.bandwidthHz;
  input.caf = std::move(decoded.maps); input.pilot = decoded.pilot;
  for (const auto& block : validated.snapshot.blocks) {
    Sha256 hash; hash.update(block.bytes.data(), block.bytes.size());
    input.blockHashes.push_back(hash.finishBytes());
  }
  return aperture(std::move(input), maximumWork, maximumBytes);
}

inline std::shared_ptr<const ExperimentReader::ApertureFrame>
ExperimentReader::aperture(ApertureInput input, uint64_t maximumWork, uint64_t maximumBytes) const {
  require(!previous_ && !input.caf.empty(), "Aperture needs pre-temporal channel products");
  require(!config_.migrationRates.empty(), "Aperture requires explicit rates including zero");
  auto admissionConfig = config_; admissionConfig.migrationRates.clear();
  DirectPathCoherentExperiment admission(admissionConfig, true);
  const uint64_t rows = input.caf[0].data.size(), columns = input.caf[0].delay.size();
  const uint64_t channels = input.caf.size(), cells = rows * columns;
  require(channels == config_.channelIds.size() && channels <= 5 && rows >= 3 && rows <= 511 &&
    columns <= 4096 && cells <= 65536 && input.correlationSamples > 0 && input.source.sampleRateHz > 0 &&
    std::isfinite(input.carrierHz) && input.carrierHz >= 0 && input.sequence > 0 &&
    input.firstSample >= input.source.firstSample && input.endSample <= input.source.endSample &&
    input.endSample > input.firstSample, "Aperture product/parent support invalid");
  const uint64_t raw = input.endSample - input.firstSample, processed = rows * input.correlationSamples;
  require(processed <= raw && raw - processed < rows, "Aperture declared tail differs from correlation geometry");
  require(bool(input.sourceConfig) != bool(input.derived), "Aperture origin must be native OR authenticated derived IQ");
  if (input.sourceConfig) {
    const auto& c = *input.sourceConfig;
    require(resident::wire::configHash(c) == input.source.identity.configHash &&
      c.surveillanceChannels == config_.channelIds && c.correlation == input.correlationSamples &&
      input.carrierHz == c.temporalCarrierHz() && input.carrierHz > 0 &&
      input.firstSample == input.source.firstSample && input.endSample == input.source.endSample &&
      input.sequence == input.source.identity.logicalFrame && input.blockHashes.size() == 20,
      "Native aperture configuration/source binding differs");
  } else {
    const auto sealed = [](const std::string& bytes, const resident_replay::Hash& expected) {
      if (bytes.empty() || bytes.size() > (1u << 20)) return false;
      Sha256 hash; hash.update(bytes.data(), bytes.size()); return hash.finishBytes() == expected;
    };
    require(input.blockHashes.empty() && sealed(input.derived->planJson, input.derived->planHash) &&
      sealed(input.derived->receiptJson, input.derived->receiptHash), "Derived aperture manifest seal differs");
  }
  for (const auto& map : input.caf)
    require(map.data.size() == rows && map.delay == input.caf[0].delay && map.doppler == input.caf[0].doppler,
      "Aperture channel geometry differs");
  const uint64_t bytes = cells * channels * 16 * (2 + 2 * config_.migrationRates.size()) +
    rows * channels * config_.migrationRates.size() * sizeof(RangeMigrationReplay::Kernel) + (4u << 20);
  const uint64_t work = channels * (rows * rows * columns +
    config_.migrationRates.size() * rows * columns * (rows + 16));
  require(bytes <= (128u << 20) && work <= RangeMigrationReplay::maximumMacs &&
    work <= maximumWork && bytes <= maximumBytes, "Aperture exceeds frame/remaining work/storage bounds");
  auto frame = std::make_shared<ApertureFrame>();
  static_cast<ApertureInput&>(*frame) = std::move(input);
  frame->channelIds = config_.channelIds;
  frame->spatialCoefficients = config_.spatialCoefficients;
  frame->anchorChannelId = config_.anchorChannelId;
  frame->declaredSpatialTrials = config_.declaredSpatialTrials;
  frame->minimumPilotCoherence = config_.minimumPilotCoherence;
  frame->retainedBytes = bytes; frame->complexMacBound = work;
  if (frame->pilot) {
    const auto& p = frame->pilot->frame;
    require(p.captureEpoch == frame->source.captureEpoch && p.sequence == frame->sequence &&
      p.firstSample == frame->firstSample && p.endSample == frame->endSample &&
      p.sampleRateHz == frame->source.sampleRateHz && p.tuningHz == frame->source.tuningHz &&
      p.gainCalibrationEpoch == frame->source.gainCalibrationEpoch && p.samplePositionAvailable && p.inputValid &&
      frame->pilot->anchorChannelId == config_.anchorChannelId,
      "Aperture pilot support differs");
    const auto z = frame->pilot->normalizedCorrelation;
    const double amplitude = std::abs(z);
    require(std::isfinite(amplitude) && amplitude <= 1 + 1e-9 &&
      std::isfinite(frame->pilot->coherence) && frame->pilot->coherence >= 0 && frame->pilot->coherence <= 1 &&
      std::abs(frame->pilot->coherence - std::min(1., std::norm(z))) < 1e-9, "Aperture pilot normalization differs");
    frame->phaseQualified = amplitude > 0 && frame->pilot->coherence >= config_.minimumPilotCoherence;
    if (frame->phaseQualified) frame->gaugeCorrection = std::conj(z) / amplitude;
  }
  for (const auto& map : frame->caf)
    frame->migration.push_back(RangeMigrationReplay::process(map, frame->source.sampleRateHz,
      frame->correlationSamples, config_.migrationRates, true));
  return frame;
}
} // namespace blah2::integration::coherent_replay
