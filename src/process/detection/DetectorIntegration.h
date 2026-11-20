#pragma once

#include "process/integration/TemporalEvidence.h"
#include "process/detection/CfarDetector1D.h"
#include "process/detection/Centroid.h"
#include "process/detection/Interpolate.h"
#include "process/tracker/Tracker.h"
#include "process/tracker/SignalProgressClock.h"
#include "rapidjson/stringbuffer.h"
#include "rapidjson/writer.h"
#include <optional>
#include <chrono>
#include <deque>
#include <map>
#include <functional>

namespace blah2::detection {

using tracker_clock::signalTimeMilliseconds;
using tracker_clock::SignalProgressClock;

struct IntegrationBudget { uint64_t workingBytes, pathSamples; };
// Admission and execution share native map dimensions, never display decimation.
inline IntegrationBudget temporalIntegrationBudget(uint32_t rows, uint32_t columns,
    uint32_t maximumCpis, uint32_t motionBins) {
  if (maximumCpis != 2 && maximumCpis != 4 && maximumCpis != 8)
    throw std::invalid_argument("Temporal detector depth must be two, four or eight CPIs");
  const auto motion = track_before_detect::decodeMotionBins(motionBins);
  if (maximumCpis == 2 && motionBins > 1)
    throw std::invalid_argument("Two-CPI integration motion must be zero or one bin");
  const uint64_t cells = uint64_t(rows) * columns;
  if (!cells || cells > (1ULL << 20))
    throw std::invalid_argument("Temporal detector exceeds the 1048576-cell CPU contract");
  const uint64_t powerBanks = maximumCpis == 8 ? 4 : maximumCpis == 4 ? 3 : 2;
  const uint64_t depths = maximumCpis == 8 ? 14 : maximumCpis == 4 ? 6 : 2;
  const IntegrationBudget budget{cells * (maximumCpis * 8 + 192 + powerBanks * 8),
    cells * (2 * motion.delay + 1) * (2 * motion.doppler + 1) * depths};
  if (budget.workingBytes > (256ULL << 20) || budget.pathSamples > (256ULL << 20))
    throw std::invalid_argument("Temporal detector exceeds its 256-MiB/268435456-path-sample contract; reduce explicit geometry or motion bounds");
  return budget;
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
  const char* backend = "cpu";
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
      double delayDriftPerHz = 0, uint32_t beamCount = 0,
      uint32_t beamTolerance = UINT32_MAX)
    : maximumCpis_(maximumCpis), motionBins_(motionBins),
      motion_(track_before_detect::decodeMotionBins(motionBins)),
      delayDriftPerHz_(motion_.fractionalDelay ? delayDriftPerHz : 0),
      evidence_({maximumCpis, motion_.delay, motion_.doppler, delayDriftPerHz_,
        beamCount, beamTolerance}) {
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
      uint64_t captureEpoch, uint64_t first, uint64_t end, bool positionAvailable,
      const std::vector<uint32_t>* beams = nullptr) {
    IntegrationMetrics metrics;
    metrics.cpis = maximumCpis_; metrics.motionBins = motionBins_;
    metrics.delayMotionBins = motion_.delay;
    metrics.dopplerMotionBins = motion_.doppler;
    metrics.delayDriftPerHz = delayDriftPerHz_;
    metrics.captureEpoch = captureEpoch;
    if (!map) throw std::invalid_argument("Detector integration requires a map");
    // Deterministic pre-admission bounds, independent of OS/receiver/backend.
    // FP64 histories and power maps are never silently downsampled or narrowed.
    const auto budget = temporalIntegrationBudget(map->get_nRows(), map->get_nCols(),
      maximumCpis_, motionBins_);
    metrics.workingBytesBound = budget.workingBytes;
    metrics.pathSamplesBound = budget.pathSamples;
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
      evidence_.process(*map, generation_, ++sequence_, nullptr, beams));
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
    } else if (bank.ready) {
      map = std::move(bank.map);
      // Keep selected per-cell motion even though this single configured
      // family does not use the multi-bank detector/threshold branch.
      integrated->map.reset();
      for (auto& unused : integrated->banks) {
        unused.map.reset();
        if (&unused != &bank) unused.detectorPower.clear();
      }
      metrics.evidence = std::move(integrated);
    }
    return metrics;
  }
};

inline std::unique_ptr<Detection> detectProducts(Map<std::complex<double>>& map,
    CfarDetector1D& detector, Centroid& centroid, Interpolate& interpolate,
    const std::optional<IntegrationMetrics>& integration,
    const std::vector<std::vector<double>>* background = nullptr,
    const std::function<uint32_t(uint32_t, uint32_t)>& spatialAt = {},
    CfarDetector1D::Trials* trials = nullptr) {
  // Never apply a two-CPI threshold to an unintegrated startup/reset map.
  if (integration && !integration->ready)
    return std::make_unique<Detection>(std::vector<double>{},
      std::vector<double>{}, std::vector<double>{});
  if (integration && integration->cpis > 2 && integration->evidence) {
    // Separate depth-specific Gamma passes; never threshold the maximum of
    // different depths as if it were one Gamma-distributed observation.
    const auto& metrics = *integration;
    std::vector<double> delays, dopplers, snrs;
    std::vector<Detection::Evidence> lineages;
    std::vector<uint32_t> spatial;
    std::map<std::pair<double, double>, std::size_t> uniqueCells;
    const double motionTrials = track_before_detect::trialCount(metrics.cpis,
      metrics.delayMotionBins, metrics.dopplerMotionBins);
    for (uint8_t index = 0; index < metrics.evidence->banks.size(); ++index) {
      const auto& bank = metrics.evidence->banks[index];
      if (!bank.ready) continue;
      auto pass = detector.forIntegration(bank.depth, index ? 2 * motionTrials : 2);
      auto candidates = pass->process(bank.map.get(), bank.validCells, background,
        nullptr, std::numeric_limits<std::size_t>::max(), nullptr, trials,
        &bank.detectorPower);
      const auto ds = candidates->get_delay(), fs = candidates->get_doppler();
      std::vector<Detection::Evidence> candidateLineage;
      std::vector<uint32_t> candidateSpatial;
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
        if (spatialAt) candidateSpatial.push_back(spatialAt(
          uint32_t(row - bank.map->doppler.begin()), uint32_t(column - bank.map->delay.begin())));
      }
      candidates->set_evidence(std::move(candidateLineage));
      if (spatialAt) candidates->set_spatial(std::move(candidateSpatial));
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
        const auto key = std::make_pair(*nearest, double(std::llround(finalDelay[candidate])));
        const auto found = uniqueCells.find(key);
        if (found == uniqueCells.end()) {
          uniqueCells.emplace(key, delays.size());
          delays.push_back(finalDelay[candidate]); dopplers.push_back(finalDoppler[candidate]);
          snrs.push_back(finalSnr[candidate]);
          lineages.push_back(finalized->get_evidence()[candidate]);
          if (spatialAt) spatial.push_back(finalized->get_spatial()[candidate]);
        } else if (finalSnr[candidate] > snrs[found->second]) {
          const auto at = found->second;
          delays[at] = finalDelay[candidate]; dopplers[at] = finalDoppler[candidate];
          snrs[at] = finalSnr[candidate]; lineages[at] = finalized->get_evidence()[candidate];
          if (spatialAt) spatial[at] = finalized->get_spatial()[candidate];
        }
      }
    }
    // Canonical row-major grid-key order matches resident stable compaction.
    // Winner selection above remains highest SNR, then first originating bank.
    std::vector<double> orderedDelays, orderedDopplers, orderedSnrs;
    std::vector<Detection::Evidence> orderedLineages;
    std::vector<uint32_t> orderedSpatial;
    for (const auto& cell : uniqueCells) {
      const auto at = cell.second;
      orderedDelays.push_back(delays[at]); orderedDopplers.push_back(dopplers[at]);
      orderedSnrs.push_back(snrs[at]); orderedLineages.push_back(lineages[at]);
      if (spatialAt) orderedSpatial.push_back(spatial[at]);
    }
    Detection combined(std::move(orderedDelays), std::move(orderedDopplers), std::move(orderedSnrs));
    combined.set_evidence(std::move(orderedLineages));
    if (spatialAt) combined.set_spatial(std::move(orderedSpatial));
    return centroid.process(&combined);
  }
  const auto* bank = integration && integration->evidence ?
    &integration->evidence->banks[integration->motionBins ? 1u : 0u] : nullptr;
  auto first = bank ? detector.process(&map, bank->validCells, background, nullptr,
    std::numeric_limits<std::size_t>::max(), nullptr, trials, &bank->detectorPower) :
    detector.process(&map, background, nullptr,
      std::numeric_limits<std::size_t>::max(), nullptr, trials);
  if (spatialAt) {
    std::vector<uint32_t> spatial;
    const auto delays = first->get_delay(), dopplers = first->get_doppler();
    for (size_t candidate = 0; candidate < delays.size(); ++candidate) {
      const auto column = std::find(map.delay.begin(), map.delay.end(), delays[candidate]);
      const auto row = std::find(map.doppler.begin(), map.doppler.end(), dopplers[candidate]);
      if (column == map.delay.end() || row == map.doppler.end())
        throw std::logic_error("Spatial detector candidate left its native grid");
      spatial.push_back(spatialAt(uint32_t(row - map.doppler.begin()),
        uint32_t(column - map.delay.begin())));
    }
    first->set_spatial(std::move(spatial));
  }
  if (integration) {
    const auto& window = integration->window;
    Detection::Evidence evidence{window.epoch, window.newestSequence,
      window.depth, uint8_t(integration->motionBins ? 2 : 1),
      window.delayMotion, window.dopplerMotion, window.sequences};
    std::vector<Detection::Evidence> lineages(first->get_nDetections(), evidence);
    if (integration->evidence) {
      const uint8_t index = integration->motionBins ? 1u : 0u;
      const auto& bank = integration->evidence->banks[index];
      const auto delays = first->get_delay(), dopplers = first->get_doppler();
      for (size_t candidate = 0; candidate < lineages.size(); ++candidate) {
        const auto column = std::find(map.delay.begin(), map.delay.end(), delays[candidate]);
        const auto row = std::find(map.doppler.begin(), map.doppler.end(), dopplers[candidate]);
        if (column == map.delay.end() || row == map.doppler.end())
          throw std::logic_error("Two-CPI detector candidate left its native grid");
        const auto lineage = bank.lineageAt(uint64_t(row - map.doppler.begin()) *
          map.get_nCols() + uint64_t(column - map.delay.begin()));
        if (lineage.depth != 2)
          throw std::logic_error("Two-CPI detector selected unavailable source lineage");
        lineages[candidate] = {lineage.epoch, lineage.newestSequence, lineage.depth,
          uint8_t(index + 1), lineage.delayMotion, lineage.dopplerMotion, lineage.sequences};
      }
    }
    first->set_evidence(std::move(lineages));
  }
  auto centered = centroid.process(first.get());
  return interpolate.process(centered.get(), &map);
}

inline std::unique_ptr<Track> trackProducts(Detection& detections,
    Tracker& tracker, uint64_t signalTimeMs,
    const std::optional<IntegrationMetrics>& integration) {
  if (integration && integration->historyReset) tracker.reset(true);
  return tracker.process(&detections, signalTimeMs);
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
  writer.Key("backend"); writer.String(metrics.backend);
  writer.Key("pfa_allocation"); writer.String(metrics.cpis > 2 ?
    "half_baseline_half_all_configured_motion_paths" : "single_configured_family");
  writer.Key("processing_ms"); writer.Double(metrics.processingMs);
  writer.Key("working_bytes_bound"); writer.Uint64(metrics.workingBytesBound);
  writer.Key("path_samples_bound"); writer.Uint64(metrics.pathSamplesBound);
  writer.Key("bank_ready"); writer.StartArray();
  if (metrics.evidence)
    for (const auto& bank : metrics.evidence->banks) writer.Bool(bank.ready);
  else if (metrics.availableCpis)
    for (const uint32_t depth : {2u, 2u, 4u, 8u})
      writer.Bool(depth <= metrics.cpis && depth <= metrics.availableCpis);
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
