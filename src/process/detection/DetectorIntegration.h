#pragma once

#include "process/integration/TemporalEvidence.h"
#include "process/detection/CfarDetector1D.h"
#include "process/detection/Centroid.h"
#include "process/detection/Interpolate.h"
#include "process/tracker/Tracker.h"
#include "../../../core/TrackGuidedShadow.private.h"
#include "rapidjson/stringbuffer.h"
#include "rapidjson/writer.h"
#include <optional>
#include <chrono>
#include <deque>
#include <map>

namespace blah2::detection {

// Tracker kinematics use capture cadence, not wall-clock scheduling jitter.
// Quotient/remainder conversion avoids overflowing endSample * 1000.
inline uint64_t signalTimeMilliseconds(uint64_t endSample, uint32_t rate) {
  if (!rate || endSample / rate > UINT64_MAX / 1000)
    throw std::invalid_argument("Capture sample time cannot be represented");
  const uint64_t whole = (endSample / rate) * 1000;
  const uint64_t fraction = ((endSample % rate) * 1000) / rate;
  if (whole > UINT64_MAX - fraction)
    throw std::overflow_error("Capture sample time exhausted");
  return whole + fraction;
}

struct IntegrationMetrics {
  uint32_t cpis = 1, motionBins = 0, availableCpis = 0;
  uint32_t delayMotionBins = 0, dopplerMotionBins = 0;
  double delayDriftPerHz = 0, processingMs = 0;
  uint64_t workingBytesBound = 0, pathSamplesBound = 0;
  bool ready = false, historyReset = false;
  uint64_t captureEpoch = 0;
  uint64_t firstInputSample = 0, endInputSample = 0;
  integration::EvidenceLineage window;
  std::shared_ptr<integration::TemporalEvidence::Result> evidence;
};

// The same power integration and Gamma-CFAR warm-up contract is used after
// CPU or portable-GPU CAFs, regardless of channelization or receiver. Capture
// intervals are half-open and expressed in original input sample units.
class DetectorIntegration {
  const uint32_t maximumCpis_, motionBins_;
  const track_before_detect::MotionBins motion_;
  const double delayDriftPerHz_;
  integration::TemporalEvidence evidence_;
  uint64_t generation_ = 1, sequence_ = 0, captureEpoch_ = 0, nextSample_ = 0,
    previousFirst_ = 0;
  bool positioned_ = false;
  std::deque<uint64_t> firstSamples_;
public:
  explicit DetectorIntegration(uint32_t motionBins)
    : DetectorIntegration(2, motionBins) {}
  DetectorIntegration(uint32_t maximumCpis, uint32_t motionBins,
      double delayDriftPerHz = 0)
    : maximumCpis_(maximumCpis), motionBins_(motionBins),
      motion_(track_before_detect::decodeMotionBins(motionBins)),
      delayDriftPerHz_(motion_.fractionalDelay ? delayDriftPerHz : 0),
      evidence_({maximumCpis, motion_.delay, motion_.doppler, delayDriftPerHz_, maximumCpis > 2}) {
    if (maximumCpis == 2 && motionBins > 1)
      throw std::invalid_argument("Two-CPI integration motion must be zero or one bin");
    if (motion_.fractionalDelay && !(delayDriftPerHz_ < 0))
      throw std::invalid_argument("Fractional temporal integration requires sample/carrier geometry");
  }
  void reset() {
    evidence_.reset(); sequence_ = 0; positioned_ = false;
    firstSamples_.clear();
    if (++generation_ == 0)
      throw std::overflow_error("Detector integration generation exhausted");
  }
  template <typename MapOwner>
  IntegrationMetrics process(MapOwner& map,
      uint64_t captureEpoch, uint64_t first, uint64_t end, bool positionAvailable) {
    IntegrationMetrics metrics;
    metrics.cpis = maximumCpis_; metrics.motionBins = motionBins_;
    metrics.delayMotionBins = motion_.delay;
    metrics.dopplerMotionBins = motion_.doppler;
    metrics.delayDriftPerHz = delayDriftPerHz_;
    metrics.captureEpoch = captureEpoch;
    if (!map) throw std::invalid_argument("Detector integration requires a map");
    const uint64_t cells = uint64_t(map->get_nRows()) * map->get_nCols();
    // Deterministic pre-admission bounds, independent of OS/receiver/backend.
    // FP64 histories and power maps are never silently downsampled or narrowed.
    if (!cells || cells > (1ULL << 20))
      throw std::invalid_argument("Temporal detector exceeds the 1048576-cell CPU contract");
    metrics.workingBytesBound = cells * (maximumCpis_ * 8 + 192);
    const uint64_t depths = maximumCpis_ == 8 ? 14 : maximumCpis_ == 4 ? 6 : 2;
    metrics.pathSamplesBound = cells * (2 * motion_.delay + 1) *
      (2 * motion_.doppler + 1) * depths;
    if (metrics.workingBytesBound > (256ULL << 20) ||
        metrics.pathSamplesBound > (256ULL << 20))
      throw std::invalid_argument("Temporal detector exceeds its 256-MiB/268435456-path-sample contract; reduce explicit geometry or motion bounds");
    if (!positionAvailable || end <= first) {
      reset(); metrics.historyReset = true; return metrics;
    }
    if (positioned_ && (captureEpoch != captureEpoch_ || first != nextSample_)) {
      reset(); metrics.historyReset = true;
    }
    captureEpoch_ = captureEpoch; nextSample_ = end; positioned_ = true;
    if (sequence_ == UINT64_MAX)
      throw std::overflow_error("Detector integration sequence exhausted");
    const auto started = std::chrono::steady_clock::now();
    auto integrated = std::make_shared<integration::TemporalEvidence::Result>(
      evidence_.process(*map, generation_, ++sequence_));
    metrics.processingMs = std::chrono::duration<double, std::milli>(
      std::chrono::steady_clock::now() - started).count();
    metrics.availableCpis = integrated->availableCpis;
    firstSamples_.push_back(first);
    while (firstSamples_.size() > maximumCpis_) firstSamples_.pop_front();
    auto& bank = integrated->banks[maximumCpis_ > 2 ? 0u : motionBins_ ? 1u : 0u];
    metrics.ready = bank.ready;
    metrics.firstInputSample = maximumCpis_ > 2 ? firstSamples_.front() :
      bank.ready ? previousFirst_ : first;
    metrics.endInputSample = end;
    previousFirst_ = first;
    metrics.window = bank.ready ? bank.window : integrated->lineageAt(0);
    if (maximumCpis_ > 2) {
      map = std::move(integrated->map);
      metrics.evidence = std::move(integrated);
    } else if (bank.ready) map = std::move(bank.map);
    return metrics;
  }
};

inline std::unique_ptr<Detection> detectProducts(Map<std::complex<double>>& map,
    CfarDetector1D& detector, Centroid& centroid, Interpolate& interpolate,
    const std::optional<IntegrationMetrics>& integration,
    const std::vector<std::vector<double>>* background = nullptr) {
  // Never apply a two-CPI threshold to an unintegrated startup/reset map.
  if (integration && !integration->ready)
    return std::make_unique<Detection>(std::vector<double>{},
      std::vector<double>{}, std::vector<double>{});
  if (integration && integration->evidence) {
    // Separate depth-specific Gamma passes; never threshold the maximum of
    // different depths as if it were one Gamma-distributed observation.
    const auto& metrics = *integration;
    std::vector<double> delays, dopplers, snrs;
    std::vector<Detection::Evidence> lineages;
    std::map<std::pair<double, double>, std::size_t> uniqueCells;
    const double trials = track_before_detect::trialCount(metrics.cpis,
      metrics.delayMotionBins, metrics.dopplerMotionBins);
    for (uint8_t index = 0; index < metrics.evidence->banks.size(); ++index) {
      const auto& bank = metrics.evidence->banks[index];
      if (!bank.ready) continue;
      auto pass = detector.forIntegration(bank.depth, index ? 2 * trials : 2);
      auto candidates = pass->process(bank.map.get(), background);
      const auto ds = candidates->get_delay(), fs = candidates->get_doppler();
      std::vector<Detection::Evidence> candidateLineage;
      for (std::size_t candidate = 0; candidate < ds.size(); ++candidate) {
        const auto column = std::find(bank.map->delay.begin(), bank.map->delay.end(), ds[candidate]);
        const auto row = std::find(bank.map->doppler.begin(), bank.map->doppler.end(), fs[candidate]);
        if (column == bank.map->delay.end() || row == bank.map->doppler.end())
          throw std::logic_error("Temporal detector candidate left its native grid");
        const auto lineage = bank.lineageAt(uint64_t(row - bank.map->doppler.begin()) *
          bank.map->get_nCols() + uint64_t(column - bank.map->delay.begin()));
        candidateLineage.push_back({lineage.epoch, lineage.newestSequence,
          lineage.depth, uint8_t(index + 1), lineage.delayMotion,
          lineage.dopplerMotion, lineage.sequences});
      }
      candidates->set_evidence(std::move(candidateLineage));
      auto centered = centroid.process(candidates.get());
      // Interpolation must use the same bank that passed CFAR, never the
      // display maximum across depths (which carries a different statistic).
      auto finalized = interpolate.process(centered.get(), bank.map.get());
      const auto finalDelay = finalized->get_delay(), finalDoppler = finalized->get_doppler(),
        finalSnr = finalized->get_snr();
      for (std::size_t candidate = 0; candidate < finalDelay.size(); ++candidate) {
        const auto nearest = std::min_element(bank.map->doppler.begin(), bank.map->doppler.end(),
          [&](double left, double right) {
            return std::abs(left - finalDoppler[candidate]) < std::abs(right - finalDoppler[candidate]);
          });
        const auto key = std::make_pair(double(std::llround(finalDelay[candidate])), *nearest);
        const auto found = uniqueCells.find(key);
        if (found == uniqueCells.end()) {
          uniqueCells.emplace(key, delays.size());
          delays.push_back(finalDelay[candidate]); dopplers.push_back(finalDoppler[candidate]);
          snrs.push_back(finalSnr[candidate]);
          lineages.push_back(finalized->get_evidence()[candidate]);
        } else if (finalSnr[candidate] > snrs[found->second]) {
          const auto at = found->second;
          delays[at] = finalDelay[candidate]; dopplers[at] = finalDoppler[candidate];
          snrs[at] = finalSnr[candidate]; lineages[at] = finalized->get_evidence()[candidate];
        }
      }
    }
    Detection combined(std::move(delays), std::move(dopplers), std::move(snrs));
    combined.set_evidence(std::move(lineages));
    return centroid.process(&combined);
  }
  auto first = detector.process(&map, background);
  if (integration) {
    const auto& window = integration->window;
    Detection::Evidence evidence{window.epoch, window.newestSequence,
      window.depth, uint8_t(integration->motionBins ? 2 : 1),
      window.delayMotion, window.dopplerMotion, window.sequences};
    first->set_evidence(std::vector<Detection::Evidence>(first->get_nDetections(), evidence));
  }
  auto centered = centroid.process(first.get());
  return interpolate.process(centered.get(), &map);
}

inline std::unique_ptr<Track> trackProducts(Detection& detections,
    Tracker& tracker, uint64_t signalTimeMs,
    const std::optional<IntegrationMetrics>& integration,
    const CfarDetector1D* detector = nullptr) {
  if (integration && integration->historyReset) tracker.reset(true);
  std::vector<TrackerGuidedCandidate> guided;
  if (detector && integration && integration->evidence && integration->ready) {
    const auto& evidence = *integration->evidence;
    const auto& bank = evidence.banks[0];
    if (bank.ready && !evidence.recentPower[1].empty()) {
      auto& map = *bank.map;
      const std::vector<double> doppler(map.doppler.begin(), map.doppler.end());
      for (const auto& prediction : tracker.guidedPredictions(signalTimeMs)) {
        const double columnValue = std::floor(prediction.delay - map.delay.front() + .5);
        TrackerGuidedCandidate candidate;
        candidate.trackId = prediction.trackId;
        const auto& window = bank.window;
        candidate.evidence = {window.epoch, window.newestSequence,
          window.depth, 1, 0, 0, window.sequences};
        if (std::isfinite(columnValue) && columnValue >= 0 && columnValue < map.get_nCols()) {
          const auto value = track_guided_shadow::evaluate(evidence.recentPower[0],
            evidence.recentPower[1], map.get_nRows(), map.get_nCols(), map.delay.front(),
            doppler, prediction.delay, prediction.dopplerHz, prediction.priorDelay,
            prediction.priorDopplerHz, detector->guardCells(), detector->trainingCells(),
            detector->guidedAlpha(map.get_nCols(), uint32_t(columnValue)));
          candidate.valid = value.valid; candidate.aboveThreshold = value.aboveThreshold;
        }
        guided.push_back(std::move(candidate));
      }
    }
  }
  return tracker.process(&detections, signalTimeMs, guided);
}

template <typename Writer>
void writeIntegration(Writer& writer, const IntegrationMetrics& metrics) {
  writer.StartObject();
  writer.Key("cpis"); writer.Uint(metrics.cpis);
  writer.Key("motion_bins"); writer.Uint(metrics.motionBins);
  writer.Key("delay_motion_bins"); writer.Uint(metrics.delayMotionBins);
  writer.Key("doppler_motion_bins"); writer.Uint(metrics.dopplerMotionBins);
  writer.Key("delay_drift_bins_per_hz"); writer.Double(metrics.delayDriftPerHz);
  writer.Key("arithmetic"); writer.String("fp64");
  writer.Key("backend"); writer.String("cpu");
  writer.Key("pfa_allocation"); writer.String(metrics.cpis > 2 ?
    "half_baseline_half_all_configured_motion_paths" : "single_configured_family");
  writer.Key("processing_ms"); writer.Double(metrics.processingMs);
  writer.Key("working_bytes_bound"); writer.Uint64(metrics.workingBytesBound);
  writer.Key("path_samples_bound"); writer.Uint64(metrics.pathSamplesBound);
  writer.Key("bank_ready"); writer.StartArray();
  if (metrics.evidence)
    for (const auto& bank : metrics.evidence->banks) writer.Bool(bank.ready);
  writer.EndArray();
  writer.Key("available_cpis"); writer.Uint(metrics.availableCpis);
  writer.Key("ready"); writer.Bool(metrics.ready);
  writer.Key("history_reset"); writer.Bool(metrics.historyReset);
  writer.Key("capture_epoch"); writer.Uint64(metrics.captureEpoch);
  writer.Key("history_epoch"); writer.Uint64(metrics.window.epoch);
  writer.Key("sequence"); writer.Uint64(metrics.window.newestSequence);
  writer.Key("sample_interval"); writer.String("half-open");
  writer.Key("first_input_sample_exact");
  writer.String(std::to_string(metrics.firstInputSample).c_str());
  writer.Key("end_input_sample_exact");
  writer.String(std::to_string(metrics.endInputSample).c_str());
  writer.Key("support_sequences"); writer.StartArray();
  for (const auto sequence : metrics.window.supportSequences()) writer.Uint64(sequence);
  writer.EndArray(); writer.EndObject();
}

inline std::string annotateIntegration(std::string json,
    const std::optional<IntegrationMetrics>& integration) {
  if (!integration) return json;
  if (json.size() < 2 || json.front() != '{' || json.back() != '}')
    throw std::invalid_argument("Detector product must be a JSON object");
  rapidjson::StringBuffer buffer;
  rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
  writeIntegration(writer, *integration);
  json.pop_back();
  if (json.back() != '{') json += ',';
  json += "\"detector_integration\":";
  json.append(buffer.GetString(), buffer.GetSize());
  json += '}';
  return json;
}
}
