#pragma once

#include "process/pipeline/RawIqStages.h"
#include "process/ambiguity/Acceleration.h"
#include "process/ambiguity/GpuResidentBackend.h"
#include "process/ambiguity/GpuResultJson.private.h"
#include "data/meta/StreamProfile.h"
#include "process/detection/DetectorIntegration.h"
#include "process/fusion/CoherentBeamBank.h"
#include "process/fusion/Noncoherent.h"

#include <chrono>
#include <cmath>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <iostream>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

namespace blah2::pipeline {

// Canonical DSP stages, independent of receiver, OS and ingress.
struct Stages {
  bool clutter = true, detection = true, tracking = true;
  void validate() const {
    if (tracking && !detection)
      throw std::invalid_argument("Tracking requires detection");
  }
};

struct Position {
  uint64_t epoch = 0, first = 0, end = 0, timestampMs = 0;
  bool available = false;
};

inline std::unique_ptr<Ambiguity> makeAmbiguity(int32_t delayMin,
    int32_t delayMax, int32_t dopplerMin, int32_t dopplerMax,
    uint32_t sampleRate, uint32_t samples, bool roundHamming) {
  return std::make_unique<Ambiguity>(delayMin, delayMax, dopplerMin,
    dopplerMax, sampleRate, samples, roundHamming);
}

struct ProductConfig {
  Stages stages;
  double pfa = 1e-5;
  int8_t guard = 2, train = 6;
  int32_t minimumDelay = 1;
  double minimumDoppler = 15, cpiSeconds = .5;
  uint16_t centroidDelay = 6, centroidDoppler = 6;
  uint32_t integrationCpis = 1, integrationMotion = 0;
  uint32_t beamCount = 0, beamTolerance = UINT32_MAX, fineBeamFactor = 1;
  double delayDriftPerHz = 0;
  uint32_t trackerM = 3, trackerN = 6, trackerDelete = 10;
  double maximumAcceleration = 10, rangeResolution = 1, wavelength = 1,
    minimumSnr = 5;
  bool adaptiveGate = true, normalizedInnovation = true;
  DopplerRidgeConfig ridgeBirth;
  TrackerRecoveryConfig recovery;
};

// Resolved policy: adapters translate this snapshot without retuning it.
class RadarPlan {
  const ProductConfig products_;
  const std::string identity_;
  static std::string serialize(const ProductConfig& p) {
    // Ordered, locale-independent doubles identify the resolved policy.
    rapidjson::StringBuffer b; rapidjson::Writer<rapidjson::StringBuffer> w(b);
    w.StartObject(); w.Key("schema"); w.String("vectorwarp-resolved-products-v1");
    w.Key("tracker_policy"); w.String(tracker_policy::identity);
    const auto field = [&](const char* name, auto value) {
      w.Key(name);
      using T = decltype(value);
      if constexpr (std::is_same_v<T, bool>) w.Bool(value);
      else if constexpr (std::is_floating_point_v<T>) w.Double(value);
      else if constexpr (std::is_signed_v<T>) w.Int64(value);
      else w.Uint64(value);
    };
#define PLAN_FIELD(name) field(#name, p.name)
    PLAN_FIELD(stages.clutter); PLAN_FIELD(stages.detection); PLAN_FIELD(stages.tracking);
    PLAN_FIELD(pfa); PLAN_FIELD(guard); PLAN_FIELD(train); PLAN_FIELD(minimumDelay);
    PLAN_FIELD(minimumDoppler); PLAN_FIELD(cpiSeconds); PLAN_FIELD(centroidDelay);
    PLAN_FIELD(centroidDoppler); PLAN_FIELD(integrationCpis); PLAN_FIELD(integrationMotion);
    PLAN_FIELD(beamCount); PLAN_FIELD(beamTolerance); PLAN_FIELD(fineBeamFactor);
    PLAN_FIELD(delayDriftPerHz); PLAN_FIELD(trackerM); PLAN_FIELD(trackerN);
    PLAN_FIELD(trackerDelete); PLAN_FIELD(maximumAcceleration); PLAN_FIELD(rangeResolution);
    PLAN_FIELD(wavelength); PLAN_FIELD(minimumSnr); PLAN_FIELD(adaptiveGate);
    PLAN_FIELD(normalizedInnovation); PLAN_FIELD(ridgeBirth.enabled);
    PLAN_FIELD(ridgeBirth.persistenceCpis); PLAN_FIELD(ridgeBirth.toleranceHz);
    PLAN_FIELD(ridgeBirth.minimumDelaySeparationKm); PLAN_FIELD(recovery.stationaryClutter);
    PLAN_FIELD(recovery.persistenceCpis); PLAN_FIELD(recovery.maximumDopplerHz);
    PLAN_FIELD(recovery.maximumStationaryCells);
#undef PLAN_FIELD
    w.EndObject(); return {b.GetString(), b.GetSize()};
  }
public:
  explicit RadarPlan(ProductConfig products)
    : products_(std::move(products)), identity_(serialize(products_)) {
    const auto& p = products_;
    p.stages.validate();
    if (!std::isfinite(p.cpiSeconds) || p.cpiSeconds <= 0 || p.guard < 0 || p.train < 1 ||
        !std::isfinite(p.minimumDoppler) || p.minimumDoppler < 0 ||
        !std::isfinite(p.pfa) || p.pfa <= 0 || p.pfa >= 1 ||
        !p.integrationCpis || !p.trackerM || p.trackerM > p.trackerN || !p.trackerDelete ||
        !std::isfinite(p.maximumAcceleration) || p.maximumAcceleration < 0 ||
        !std::isfinite(p.rangeResolution) || p.rangeResolution <= 0 ||
        !std::isfinite(p.wavelength) || p.wavelength <= 0 || !std::isfinite(p.minimumSnr) ||
        (p.recovery.stationaryClutter &&
         (p.recovery.persistenceCpis < 2 || p.recovery.persistenceCpis > 65535)) ||
        !std::isfinite(p.recovery.maximumDopplerHz) || p.recovery.maximumDopplerHz < 0)
      throw std::invalid_argument("Resolved radar plan is invalid");
    if (p.stages.tracking)
      tracker_confirmation::validate(p.trackerM, p.trackerN, p.trackerDelete, p.integrationCpis);
  }
  const ProductConfig& products() const { return products_; }
  const std::string& identity() const { return identity_; }
  static double currentCpiPreferenceSnr(const ProductConfig& p) {
    return p.stages.tracking && p.integrationCpis > 2 ?
      std::max(tracker_weak_birth::strongBirthSnr(), p.minimumSnr) : 0;
  }
  resident::DetectorConfig residentDetector() const {
    const auto& p = products_;
    return {p.stages.detection, true, true, p.guard, p.train, p.minimumDelay,
      p.centroidDelay, p.centroidDoppler, p.pfa, p.minimumDoppler, 1 / p.cpiSeconds,
      p.integrationCpis, p.integrationMotion, p.beamTolerance, currentCpiPreferenceSnr(p)};
  }
  resident::TrackerConfig residentTracker(uint32_t capacity) const {
    const auto& p = products_;
    if(p.stages.tracking && p.recovery.stationaryClutter &&
       p.recovery.maximumStationaryCells!=tracker_policy::residentStationaryCapacity)
      throw std::invalid_argument("Resident stationary capacity is unsupported; configured host capacity is preserved");
    return {p.stages.tracking, capacity, p.trackerM, p.trackerN, p.trackerDelete,
      p.adaptiveGate ? resident::TrackerInitiationMode::adaptiveGate :
        resident::TrackerInitiationMode::hypothesisBank,
      p.cpiSeconds, p.maximumAcceleration, p.rangeResolution, p.wavelength, p.minimumSnr,
      p.recovery.stationaryClutter, p.recovery.persistenceCpis, p.recovery.maximumDopplerHz,
      {}, p.normalizedInnovation, 0};
  }
  static double detectorPfa(double configured, double beamTrials,
      uint32_t depth, uint32_t motion) {
    // Four/eight-CPI motion-bank correction belongs to temporal detection;
    // the two-CPI policy applies its nine trials before either backend runs.
    return configured / (beamTrials * (depth == 2 && motion ? 9.0 : 1.0));
  }
};

struct Products {
  std::unique_ptr<Detection> detections;
  std::unique_ptr<Track> tracks;
  std::optional<detection::IntegrationMetrics> integration;
};

// Own one history per canonical signal, independent of how its samples arrived.
// This is also the factory for startup oracles.
class ProductStages {
  const ProductConfig config_;
  const std::string planIdentity_;
  detection::DetectorEvidenceClock singleCpiEvidence_;
  detection::DetectorEvidenceClock temporalClock_;
  temporal_gpu::Config temporalConfig_;
  std::unique_ptr<temporal_gpu::Session> temporal_;
  uint64_t temporalSubmitted_ = 0;
  uint32_t temporalAvailable_ = 0;
  bool temporalQualifying_ = false;
  stream_profile::Worker temporalProfile_{"temporal-products"};
  template <typename MapOwner>
  Products temporalProducts(MapOwner& map, Position position, CfarDetector1D::Trials* trials) {
    if (!map || map->delay.size() != temporalConfig_.delayAxis.size() ||
        map->doppler.size() != temporalConfig_.dopplerAxis.size() ||
        !std::equal(map->delay.begin(), map->delay.end(), temporalConfig_.delayAxis.begin()) ||
        !std::equal(map->doppler.begin(), map->doppler.end(), temporalConfig_.dopplerAxis.begin()))
      throw std::invalid_argument("Temporal GPU canonical map geometry changed");
    Products result; detection::IntegrationMetrics metrics;
    metrics.cpis = config_.integrationCpis; metrics.motionBins = config_.integrationMotion;
    const auto motion = track_before_detect::decodeMotionBins(config_.integrationMotion);
    metrics.delayMotionBins = motion.delay; metrics.dopplerMotionBins = motion.doppler;
    metrics.delayDriftPerHz = config_.delayDriftPerHz; metrics.captureEpoch = position.epoch;
    metrics.backend = "vulkan-temporal";
    const auto budget = detection::temporalIntegrationBudget(map->get_nRows(), map->get_nCols(),
      config_.integrationCpis, config_.integrationMotion);
    metrics.workingBytesBound = budget.workingBytes; metrics.pathSamplesBound = budget.pathSamples;
    const bool available = temporalClock_.observe(position.epoch, position.first, position.end,
      position.available, metrics.historyReset);
    if (metrics.historyReset) {
      temporal_->reset(); temporalAvailable_ = 0;
      // The dormant CPU owner must advance its reset generation too, so a
      // later fallback cannot reuse an earlier GPU evidence namespace.
      if (integration) integration->reset();
      resetTracker();
    }
    if (!available) {
      result.integration = metrics;
      result.detections = std::make_unique<Detection>(std::vector<double>{}, std::vector<double>{}, std::vector<double>{});
      return result;
    }
    stream_profile::WorkScope profile(temporalProfile_, temporalSubmitted_ + 1, position.epoch, 0, false, !temporalQualifying_);
    auto* input = temporal_->input(); size_t offset = 0;
    for (const auto& row : map->data) { std::copy(row.begin(), row.end(), input + offset); offset += row.size(); }
    profile.mark(1);
    const auto& output = temporal_->process(++temporalSubmitted_);
    profile.mark(2);
    temporalAvailable_ = std::min(temporalAvailable_ + 1, config_.integrationCpis);
    if (output.metrics.available != temporalAvailable_)
      throw std::runtime_error("Temporal GPU acknowledged history depth differs");
    const auto identity = temporalClock_.current();
    std::array<uint64_t,8> sequences{};
    for (uint32_t i = 0; i < temporalAvailable_; ++i)
      sequences[i] = identity.newestSequence - temporalAvailable_ + i + 1;
    result.detections = std::make_unique<Detection>(gpu_result_json::hostDetections(
      gpu_result_json::View<temporal_gpu::Record>{output.records, output.metrics.count},
      {output.flags, offset}, {output.cells, output.metrics.count}, identity.epoch,
      {sequences.data(), temporalAvailable_}, config_.integrationCpis));
    metrics.availableCpis = temporalAvailable_; metrics.ready = config_.integrationCpis > 2 || temporalAvailable_ >= 2;
    const uint64_t span = position.end - position.first;
    if (temporalAvailable_ > 1 && span > position.first / (temporalAvailable_ - 1))
      throw std::runtime_error("Temporal GPU support precedes capture origin");
    metrics.firstInputSample = position.first - uint64_t(temporalAvailable_ - 1) * span;
    metrics.endInputSample = position.end; metrics.window.epoch = identity.epoch;
    metrics.window.newestSequence = identity.newestSequence; metrics.window.depth = temporalAvailable_ >= 2 ? 2 : 1;
    for (uint32_t age = 0; age < metrics.window.depth; ++age)
      metrics.window.sequences[age] = identity.newestSequence - age;
    metrics.processingMs = output.metrics.uploadMs + output.metrics.executeMs + output.metrics.readbackMs;
    auto integrated = std::make_unique<Map<std::complex<double>>>(map->get_nRows(), map->get_nCols());
    integrated->delay = map->delay; integrated->doppler = map->doppler;
    offset = 0;
    for (auto& row : integrated->data) for (auto& value : row) value = {output.display[offset++], 0};
    integrated->noisePower = output.metrics.noise; integrated->maxPower = output.metrics.maximum;
    map = std::move(integrated); result.integration = metrics;
    if (trials) { trials->evaluated = output.metrics.evaluated; trials->accepted = output.metrics.cfar; }
    profile.record.stages = {output.metrics.uploadMs, output.metrics.executeMs, output.metrics.readbackMs};
    profile.record.temporalStages = true; profile.record.gpuStages = output.metrics.gpuMs;
    profile.completed();
    return result;
  }
  Tracker makeTracker() const {
    Tracker result(config_.trackerM, config_.trackerN, config_.trackerDelete,
      config_.cpiSeconds, config_.maximumAcceleration, config_.rangeResolution,
      config_.wavelength, config_.minimumSnr, config_.adaptiveGate,
      config_.ridgeBirth, config_.normalizedInnovation);
    result.configureRecovery(config_.recovery);
    result.configureMaximumEvidenceDepth(config_.integrationCpis);
    result.configureSpatialAssociation(config_.beamCount, config_.fineBeamFactor,
      config_.beamTolerance);
    return result;
  }
public:
  CfarDetector1D detector;
  Centroid centroid;
  Interpolate interpolate{true, true};
  Tracker tracker;
  std::unique_ptr<detection::DetectorIntegration> integration;

  explicit ProductStages(const RadarPlan& plan)
    : config_(plan.products()), planIdentity_(plan.identity()),
      detector(config_.pfa, config_.guard, config_.train, config_.minimumDelay,
        config_.minimumDoppler, config_.integrationCpis),
      centroid(config_.centroidDelay, config_.centroidDoppler, 1 / config_.cpiSeconds),
      tracker(makeTracker()) {
    if (config_.stages.detection && config_.integrationCpis > 1)
      integration = std::make_unique<detection::DetectorIntegration>(
        config_.integrationCpis, config_.integrationMotion, config_.delayDriftPerHz,
        config_.beamCount, config_.beamTolerance);
  }
  const ProductConfig& config() const { return config_; }
  Detection::Evidence singleCpiEvidence() const { return singleCpiEvidence_.current(); }
  const std::string& planIdentity() const { return planIdentity_; }
  bool temporalAccelerated() const { return bool(temporal_); }
  const std::string& temporalDevice() const { return temporalConfig_.device; }
  // Before capture, compare CPU/GPU products on the same bounded maps,
  // then clear private history. No runtime shadow detector.
  void prepareTemporal(temporal_gpu::Config config, const GpuProcessOptions& options,
      const std::function<std::unique_ptr<temporal_gpu::Session>(
        const temporal_gpu::Config&, const GpuProcessOptions&)>& factory = temporal_gpu::createProcess) {
    if (!integration || config_.beamCount || temporal_)
      throw std::invalid_argument("Temporal GPU product admission invalid");
    config.detector = RadarPlan(config_).residentDetector();
    config.delayDriftPerHz = config_.delayDriftPerHz;
    temporal_gpu::validate(config);
    // Synthetic qualification uses private clocks; transfer only a reset worker.
    ProductStages candidate{RadarPlan(config_)};
    candidate.temporalConfig_ = std::move(config);
    candidate.temporal_ = factory(candidate.temporalConfig_, options);
    if (!candidate.temporal_) throw std::runtime_error("Temporal GPU session unavailable");
    candidate.temporalQualifying_ = true;
    {
      ProductStages oracle{RadarPlan(config_)};
      const auto rows = uint32_t(candidate.temporalConfig_.dopplerAxis.size()), columns = uint32_t(candidate.temporalConfig_.delayAxis.size());
      for (uint32_t frame = 0; frame < config_.integrationCpis + 2; ++frame) {
        auto raw = std::make_unique<Map<std::complex<double>>>(rows, columns);
        raw->delay.assign(candidate.temporalConfig_.delayAxis.begin(), candidate.temporalConfig_.delayAxis.end());
        raw->doppler.assign(candidate.temporalConfig_.dopplerAxis.begin(), candidate.temporalConfig_.dopplerAxis.end());
        for (uint32_t r = 0; r < rows; ++r) for (uint32_t c = 0; c < columns; ++c) {
          const double background = 1.0 + double((frame * 13u + r * 7u + c * 3u) % 11u) / 16.0;
          raw->data[r][c] = {r == rows * 3 / 4 && c == columns / 2 + frame % 5 ? 1000.0 : background, 0};
        }
        raw->set_metrics(); auto cpu = std::make_unique<Map<std::complex<double>>>(*raw);
        const Position position{1, uint64_t(frame) * 1000, uint64_t(frame + 1) * 1000, uint64_t(frame + 1) * 500, true};
        const auto expected = oracle.process(cpu, position);
        auto actual = candidate.temporalProducts(raw, position, nullptr);
        actual = candidate.track(raw, position, std::move(actual));
        gpu_result_json::compareOracleJson(expected.detections->to_json(position.timestampMs),
          actual.detections->to_json(position.timestampMs), "Temporal map startup detector");
        if (expected.tracks) gpu_result_json::compareOracleJson(expected.tracks->to_json(position.timestampMs),
          actual.tracks->to_json(position.timestampMs), "Temporal map startup tracker");
      }
      candidate.reset("temporal_startup_complete");
      if (!candidate.temporal_) throw std::runtime_error("Temporal GPU startup reset was not acknowledged");
    }
    temporalConfig_ = std::move(candidate.temporalConfig_);
    temporalSubmitted_ = candidate.temporalSubmitted_; // Worker transport sequence remains monotonic.
    temporal_ = std::move(candidate.temporal_);
  }
  void resetTracker() {
    tracker = makeTracker();
  }
  void reset(const char* = "explicit_reset") {
    if (integration) integration->reset();
    if (singleCpiEvidence_.current().depth) singleCpiEvidence_.reset();
    temporalClock_.reset(); temporalAvailable_ = 0;
    if (temporal_) try { temporal_->reset(); }
    catch (const std::exception& error) {
      std::cerr << "GPU temporal fallback phase=reset reason=" << gpuDiagnosticString(error.what()) << '\n';
      temporal_.reset();
    }
    resetTracker();
  }
  template <typename MapOwner>
  Products process(MapOwner& map, Position position, bool integrate = true,
      const std::function<void()>& detected = {},
      const std::vector<uint32_t>* beams = nullptr,
      const std::function<uint32_t(uint32_t, uint32_t)>& spatialAt = {},
      CfarDetector1D::Trials* trials = nullptr,
      utility::FixedExecutor* executor = nullptr) {
    Products output;
    if (trials) *trials = {};
    if (integration && !integrate)
      throw std::logic_error("Configured temporal detection cannot bypass canonical integration");
    if (temporal_) {
      try {
        output = temporalProducts(map, position, trials);
      } catch (const std::exception& error) {
        std::cerr << "GPU temporal fallback reason=" << gpuDiagnosticString(error.what()) << '\n';
        temporal_.reset(); reset("temporal_worker_failed");
      }
      if (temporal_) {
        if (detected) detected();
        return track(map, position, std::move(output));
      }
    }
    if (integration && integrate)
      output.integration = integration->process(map, position.epoch,
        position.first, position.end, position.available, beams, executor);
    const auto detect = [&](const std::vector<std::vector<double>>* background,
        CfarDetector1D::Trials* observed) {
      return detection::detectProducts(*map, detector, centroid, interpolate,
        output.integration, background, spatialAt, observed, RadarPlan::currentCpiPreferenceSnr(config_));
    };
    if (!config_.stages.detection) return output;
    output.detections = detect(nullptr, trials);
    if (!integration) {
      bool changed = false;
      if (singleCpiEvidence_.observe(position.epoch, position.first, position.end, position.available, changed, true)) {
        const auto identity=singleCpiEvidence_.current();
        output.detections->set_evidence(std::vector<Detection::Evidence>(
          output.detections->get_nDetections(), identity));
        output.detections->set_evidence_identity(identity.epoch,identity.newestSequence);
        if (changed) resetTracker();
      }
    } else if (output.integration) {
      output.detections->set_evidence_identity(output.integration->window.epoch,
        output.integration->window.newestSequence);
    }
    if (detected) detected();
    return track(map, position, std::move(output));
  }
  // Qualified detector implementations converge here. Ridge births, recovery,
  // grid association and reset policy remain owned by the same host tracker.
  template <typename MapOwner>
  Products track(MapOwner& map, Position position, Products output) {
    if (config_.stages.tracking) {
      if (!output.detections) throw std::logic_error("Tracking requires detector products");
      tracker.configureGrid(map->delay,map->doppler);
      output.tracks = detection::trackProducts(*output.detections, tracker,
        position.timestampMs, output.integration);
    }
    return output;
  }
};

// One order for CPU and portable accelerator stages. Hooks observe accepted
// boundaries; they do not choose another algorithm or own adaptive DSP state.
inline bool processFrontend(pipeline::RawIqStages& stages,
    Acceleration* acceleration, IqData& reference,
    const std::vector<IqData*>& surveillance, const std::vector<Ambiguity*>& ambiguity,
    bool clutterEnabled, bool preserveSurveillance,
    const std::function<void()>& filtered = {},
    const std::function<void()>& borrowedAmbiguity = {},
    IqData* qualifiedReference = nullptr,
    const std::function<IqData*()>& prepareAmbiguityReference = {}) {
  const auto prepareReference = [&] {
    if (prepareAmbiguityReference) qualifiedReference = prepareAmbiguityReference();
  };
  if (clutterEnabled) {
    const auto clutter = [&] { return stages.clutter(reference, surveillance); };
    if (acceleration) {
      if (!acceleration->processClutter(reference, surveillance, clutter,
          prepareAmbiguityReference ? prepareReference : std::function<void()>{})) return false;
    } else {
      prepareReference();
      if (!clutter()) return false;
    }
  } else prepareReference();
  if (filtered) filtered();
  if (borrowedAmbiguity) borrowedAmbiguity();
  else {
    const auto& selected = qualifiedReference ? *qualifiedReference : reference;
    const auto calculate = [&] { stages.ambiguity(selected, surveillance, ambiguity, preserveSurveillance); };
    if (acceleration) acceleration->process(selected.sample_view(), surveillance, ambiguity, calculate);
    else calculate();
  }
  return true;
}

template <typename MapOwner>
void fuseMaps(const std::vector<Map<std::complex<double>>*>& maps,
    Noncoherent& noncoherent, CoherentBeamBank* coherent, MapOwner& output,
    std::vector<uint32_t>* selectedBeams = nullptr) {
  if (maps.empty() || !maps.front())
    throw std::invalid_argument("At least one ambiguity map is required");
  if (!output || output->get_nRows() != maps.front()->get_nRows() ||
      output->get_nCols() != maps.front()->get_nCols())
    output = std::make_unique<Map<std::complex<double>>>(
      maps.front()->get_nRows(), maps.front()->get_nCols());
  if (coherent) coherent->process_into(maps, *output, selectedBeams);
  else noncoherent.process_into(maps, *output);
  output->set_metrics();
}
// Retained frames are immutable snapshots. Only the sole owner can reuse its
// matrix, for either fusion statistic; direct processing uses unique ownership.
inline void fuseMaps(const std::vector<Map<std::complex<double>>*>& maps,
    Noncoherent& noncoherent, CoherentBeamBank* coherent,
    std::shared_ptr<Map<std::complex<double>>>& output,
    std::vector<uint32_t>* selectedBeams = nullptr) {
  if (maps.empty() || !maps.front())
    throw std::invalid_argument("At least one ambiguity map is required");
  if (!output || output.use_count() != 1 ||
      output->get_nRows() != maps.front()->get_nRows() ||
      output->get_nCols() != maps.front()->get_nCols())
    output = std::make_shared<Map<std::complex<double>>>(
      maps.front()->get_nRows(), maps.front()->get_nCols());
  if (coherent) coherent->process_into(maps, *output, selectedBeams);
  else noncoherent.process_into(maps, *output);
  output->set_metrics();
}

} // namespace blah2::pipeline
