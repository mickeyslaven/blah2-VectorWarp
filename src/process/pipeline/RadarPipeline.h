#pragma once

#include "process/pipeline/RawIqStages.h"
#include "process/ambiguity/Acceleration.h"
#include "process/ambiguity/GpuResidentBackend.h"
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
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

namespace blah2::pipeline {

// Normalized processing behavior. Receiver, operating system and ingress
// implementation are deliberately absent: those identities cannot select DSP.
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

// Resolved numerical policy. Only the canonical resolver constructs this
// snapshot; execution adapters can translate it, never retune it.
class RadarPlan {
  const ProductConfig products_;
  const std::string identity_;
  static std::string serialize(const ProductConfig& p) {
    // Ordered, locale-independent round-trip doubles; no backend or scheduling
    // inputs. This is observability of the resolved policy, never another plan.
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
  resident::DetectorConfig residentDetector() const {
    const auto& p = products_;
    return {p.stages.detection, true, true, p.guard, p.train, p.minimumDelay,
      p.centroidDelay, p.centroidDoppler, p.pfa, p.minimumDoppler, 1 / p.cpiSeconds,
      p.integrationCpis, p.integrationMotion, p.beamTolerance};
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
  Tracker makeTracker() const {
    Tracker result(config_.trackerM, config_.trackerN, config_.trackerDelete,
      config_.cpiSeconds, config_.maximumAcceleration, config_.rangeResolution,
      config_.wavelength, config_.minimumSnr, config_.adaptiveGate,
      config_.ridgeBirth, config_.normalizedInnovation);
    result.configureRecovery(config_.recovery);
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
  const std::string& planIdentity() const { return planIdentity_; }
  void resetTracker() {
    tracker = makeTracker();
  }
  void reset(const char* = "explicit_reset") {
    if (integration) integration->reset();
    resetTracker();
  }
  template <typename MapOwner>
  Products process(MapOwner& map, Position position, bool integrate = true,
      const std::function<void()>& detected = {},
      const std::vector<uint32_t>* beams = nullptr,
      const std::function<uint32_t(uint32_t, uint32_t)>& spatialAt = {},
      CfarDetector1D::Trials* trials = nullptr) {
    Products output;
    if (trials) *trials = {};
    if (integration && !integrate)
      throw std::logic_error("Configured temporal detection cannot bypass canonical integration");
    if (integration && integrate)
      output.integration = integration->process(map, position.epoch,
        position.first, position.end, position.available, beams);
    const auto detect = [&](const std::vector<std::vector<double>>* background,
        CfarDetector1D::Trials* observed) {
      return detection::detectProducts(*map, detector, centroid, interpolate,
        output.integration, background, spatialAt, observed);
    };
    if (!config_.stages.detection) return output;
    output.detections = detect(nullptr, trials);
    if (detected) detected();
    if (config_.stages.tracking) {
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
