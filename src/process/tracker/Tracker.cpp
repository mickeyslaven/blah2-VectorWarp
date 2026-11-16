#include "Tracker.h"
#include "NormalizedInnovationGate.h"
#include <algorithm>
#include <iostream>
#include <cmath>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <utility>
#include <unordered_set>

void Tracker::reset(bool lineageDiscontinuity)
{
  *this = Tracker(m, n, nDelete, cpi, maxAccInit, rangeRes, lambda,
    minInitiationSnr, adaptiveGate, dopplerRidge, normalizedInnovationGate,
    associationMaximumNis, birthExclusivity, recovery);
  pendingLineageReset=lineageDiscontinuity;
}

uint64_t Tracker::initiationMultiplicity(double cpi,
  double maximumAcceleration, bool adaptive)
{
  if (!std::isfinite(cpi) || cpi <= 0 ||
      !std::isfinite(maximumAcceleration) || maximumAcceleration < 0)
    throw std::invalid_argument("Invalid initiation geometry");
  if (adaptive) return 1;
  const double resolution = 1 / (cpi * cpi);
  const double bins = std::floor(maximumAcceleration / resolution);
  if (bins > 65535)
    throw std::invalid_argument("Too many initial tracking acceleration hypotheses");
  return 2 * uint64_t(bins) + 1;
}

// constructor
Tracker::Tracker(uint32_t _m, uint32_t _n, uint32_t _nDelete, 
  double _cpi, double _maxAccInit, double _rangeRes, double _lambda,
  double _minInitiationSnr, bool _adaptiveGate,
  DopplerRidgeConfig _dopplerRidge, bool _normalizedInnovationGate,
  double _associationMaximumNis, BirthExclusivityConfig _birthExclusivity,
  TrackerRecoveryConfig _recovery)
{
  if (_m == 0 || _m > _n || _n > 255 || !std::isfinite(_cpi) || _cpi <= 0 ||
      !std::isfinite(_rangeRes) || _rangeRes <= 0 || !std::isfinite(_lambda) ||
      _lambda <= 0 || !std::isfinite(_maxAccInit) || _maxAccInit < 0 ||
      !std::isfinite(_minInitiationSnr) || _minInitiationSnr < -100 ||
      _minInitiationSnr > 100 ||
      !std::isfinite(_associationMaximumNis) || _associationMaximumNis <= 0 ||
      (_birthExclusivity.enabled &&
        (!std::isfinite(_birthExclusivity.delayHalfWidthBins) ||
         !std::isfinite(_birthExclusivity.dopplerHalfWidthBins) ||
         _birthExclusivity.delayHalfWidthBins <= 0 ||
         _birthExclusivity.dopplerHalfWidthBins <= 0)) ||
      (_dopplerRidge.enabled &&
        (_dopplerRidge.persistenceCpis < 2 ||
         !std::isfinite(_dopplerRidge.toleranceHz) ||
         _dopplerRidge.toleranceHz <= 0 ||
         !std::isfinite(_dopplerRidge.minimumDelaySeparationKm) ||
         _dopplerRidge.minimumDelaySeparationKm <= 0)))
    throw std::invalid_argument("Invalid tracking window, frame timing or radar geometry");
  m = _m;
  n = _n;
  nDelete = _nDelete;
  cpi = _cpi;
  maxAccInit = _maxAccInit;
  minInitiationSnr = _minInitiationSnr;
  adaptiveGate = _adaptiveGate;
  normalizedInnovationGate = _normalizedInnovationGate;
  associationMaximumNis = _associationMaximumNis;
  birthExclusivity = _birthExclusivity;
  timestamp = 0;
  rangeRes = _rangeRes;
  lambda = _lambda;
  dopplerRidge = _dopplerRidge;
  dopplerRidgeMinimumDelayBins =
    dopplerRidge.minimumDelaySeparationKm * 1000.0 / rangeRes;
  configureRecovery(_recovery);

  double resolutionAcc = 1/(cpi*cpi);
  const int nAcc = static_cast<int>((initiationMultiplicity(cpi,
    maxAccInit, adaptiveGate) - 1) / 2);
  for (int i = 0; i < 2*nAcc+1; i++)
  {
    accInit.push_back(resolutionAcc*(i-nAcc));
  }

}

Tracker::~Tracker()
{
}

void Tracker::configureRecovery(TrackerRecoveryConfig config)
{
  if (!std::isfinite(config.maximumDopplerHz) || config.maximumDopplerHz < 0 ||
      (config.stationaryClutter && (config.persistenceCpis < 2 ||
       config.persistenceCpis > 65535 || !config.maximumStationaryCells ||
       config.maximumStationaryCells > 1048576)) ||
      ((config.guidedPromote || config.guidedSustain) && !config.guidedShadow) ||
      (config.guidedShadow && (!config.guidedMaximumTracks || config.guidedMaximumTracks > 65535)) ||
      (config.guidedPromote && n < 2 * m - 1))
    throw std::invalid_argument("Invalid shared tracker recovery policy");
  recovery = config;
  // Configuration boundaries cannot reuse prior algorithm evidence.
  track=Track{}; timestamp=0; diagnostics={}; pendingWeakSupport={};
  doNotInitiate.clear(); lineageNoBirth.clear(); frameGuided.clear(); frameHasEvidence=false;
  evidenceLedgers.clear(); weakSupportLineage.clear(); stationaryCells.clear();
  dopplerRidges.clear(); pendingLineageReset=false;
  evidenceEpoch = 0; evidenceEpochInitialized=false; frameSequence = 0; frameOrdinal = 0;
}

bool Tracker::validEvidence(const Detection::Evidence& value, bool requireCurrentEpoch) const
{
  if (!value.newestSequence ||
      (value.depth != 1 && value.depth != 2 && value.depth != 4 && value.depth != 8) ||
      value.newestSequence != value.sequences[0] ||
      (requireCurrentEpoch && evidenceEpochInitialized && value.epoch != evidenceEpoch)) return false;
  for (uint8_t i = 1; i < value.depth; ++i)
    if (!value.sequences[i] || value.sequences[i] >= value.sequences[i-1] ||
        value.sequences[i-1] - value.sequences[i] != 1) return false;
  return true;
}

bool Tracker::freshEvidence(const std::string& id, const Detection::Evidence& value) const
{
  if (!validEvidence(value)) return false;
  const auto found = evidenceLedgers.find(id);
  return found == evidenceLedgers.end() || !found->second.initialized || found->second.epoch != value.epoch ||
    value.sequences[value.depth-1] > found->second.highWaterSequence;
}

bool Tracker::creditEvidence(const std::string& id, const Detection::Evidence* value)
{
  if (!value) return !evidenceEpochInitialized; // Original single-CPI contract only before lineage is established.
  if (!freshEvidence(id, *value)) return false;
  auto& ledger = evidenceLedgers[id];
  if (!ledger.initialized || ledger.epoch != value->epoch) {
    ledger.votes.clear(); ledger.epoch = value->epoch; ledger.initialized=true;
  }
  ledger.highWaterSequence = value->newestSequence;
  ++diagnostics.independentCredits;
  diagnostics.creditedCpis += value->depth;
  return true;
}

void Tracker::appendVote(uint64_t index, bool credit)
{
  if (!frameHasEvidence && !recovery.guidedPromote && !recovery.stationaryClutter &&
      evidenceLedgers.find(track.get_id(index))==evidenceLedgers.end()) return;
  auto& votes = evidenceLedgers[track.get_id(index)].votes;
  votes.push_back(credit);
  if (votes.size() > n) votes.pop_front();
}

void Tracker::promote(uint64_t index, bool credit)
{
  const auto found=evidenceLedgers.find(track.get_id(index));
  if (!frameHasEvidence && !recovery.guidedPromote && !recovery.stationaryClutter &&
      (found==evidenceLedgers.end() || !found->second.initialized)) {
    track.promote(index, m, n);
    return;
  }
  appendVote(index, credit);
  const auto& votes = evidenceLedgers[track.get_id(index)].votes;
  if (credit && std::count(votes.begin(), votes.end(), true) >= m &&
      track.get_state(index) != "COASTING") track.promote(index, 1, 1);
}

static bool stationaryKey(double delay, double doppler, double cpi,
  std::pair<int64_t,int64_t>& key)
{
  // Explicit bounds also keep neighbourhood addition away from int64 overflow.
  if (!std::isfinite(delay) || !std::isfinite(doppler) ||
      std::abs(delay) > 1e9 || std::abs(doppler*cpi) > 1e9) return false;
  key = {int64_t(std::floor(delay+.5)), int64_t(std::floor(doppler*cpi+.5))};
  return true;
}

bool Tracker::stationary(double delay, double doppler) const
{
  if (!recovery.stationaryClutter ||
      (recovery.maximumDopplerHz > 0 && std::abs(doppler) > recovery.maximumDopplerHz)) return false;
  std::pair<int64_t,int64_t> key;
  if (!stationaryKey(delay,doppler,cpi,key)) return true;
  const auto found = stationaryCells.find(key);
  // Exhausted storage is not evidence that this cell is stationary. In
  // particular, it cannot quarantine an existing moving track.
  if (found == stationaryCells.end()) return false;
  const auto& cell = found->second;
  const uint64_t missed = frameOrdinal > cell.last ? frameOrdinal-cell.last-1 : 0;
  return cell.streak >= recovery.persistenceCpis + missed/4;
}

bool Tracker::stationaryCapacityBlocked(double delay, double doppler) const
{
  if (!recovery.stationaryClutter ||
      (recovery.maximumDopplerHz > 0 && std::abs(doppler) > recovery.maximumDopplerHz)) return false;
  std::pair<int64_t,int64_t> key;
  return stationaryKey(delay,doppler,cpi,key) &&
    stationaryCells.size() >= recovery.maximumStationaryCells && !stationaryCells.count(key);
}

void Tracker::pruneStationary()
{
  // Evict before association/birth admission, not one frame after a cell expires.
  for (auto it = stationaryCells.begin(); it != stationaryCells.end();)
    if (frameOrdinal > it->second.last &&
        (frameOrdinal-it->second.last-1)/4 >= it->second.streak)
      it = stationaryCells.erase(it);
    else ++it;
}

void Tracker::learnStationary(Detection& detection)
{
  if (!recovery.stationaryClutter) return;
  const auto delay = detection.get_delay(), doppler = detection.get_doppler();
  for (size_t i = 0; i < delay.size(); ++i) {
    if (recovery.maximumDopplerHz > 0 && std::abs(doppler[i]) > recovery.maximumDopplerHz) continue;
    std::pair<int64_t,int64_t> key;
    if (!stationaryKey(delay[i],doppler[i],cpi,key)) continue;
    // Same 9-delay by 3-Doppler-cell occupancy neighbourhood as the resident policy.
    for (int x=-4; x<=4; ++x) for (int y=-1; y<=1; ++y) {
      const auto cellKey = std::make_pair(key.first+x,key.second+y);
      auto found = stationaryCells.find(cellKey);
      if (found == stationaryCells.end()) {
        if (stationaryCells.size() >= recovery.maximumStationaryCells) {
          ++diagnostics.stationaryCapacityRejected; continue;
        }
        found = stationaryCells.emplace(cellKey,StationaryCell{}).first;
      }
      auto& cell = found->second;
      if (cell.last == frameOrdinal) continue;
      const uint64_t decay = cell.last && frameOrdinal > cell.last ? (frameOrdinal-cell.last-1)/4 : 0;
      cell.streak = uint32_t(std::min<uint64_t>(65535,
        (decay >= cell.streak ? 0 : cell.streak-decay)+1));
      cell.last = frameOrdinal;
    }
  }
  diagnostics.stationaryCells = stationaryCells.size();
}

std::vector<TrackerGuidedPrediction> Tracker::guidedPredictions(uint64_t currentTime)
{
  std::vector<TrackerGuidedPrediction> output;
  if (!recovery.guidedShadow || currentTime <= timestamp) return output;
  const double elapsed = double(currentTime-timestamp)/1000;
  for (uint64_t i=0; i<track.get_n() && output.size()<recovery.guidedMaximumTracks; ++i) {
    auto prior=track.get_current(i), next=predict(prior,track.get_acceleration(i),elapsed);
    output.push_back({track.get_id(i),prior.get_delay().front(),prior.get_doppler().front(),
      next.get_delay().front(),next.get_doppler().front(),
      track.get_state(i)=="ACTIVE" || track.get_state(i)=="COASTING"});
  }
  return output;
}

bool Tracker::guidedRecovery(uint64_t index, Detection& prediction, bool& credit)
{
  if (!recovery.guidedShadow) return false;
  const auto candidate=std::find_if(frameGuided.begin(),frameGuided.end(),[&](const auto& item){
    return item.trackId==track.get_id(index);});
  if (candidate==frameGuided.end()) return false;
  ++diagnostics.guidedCandidates;
  if (!candidate->valid || !candidate->aboveThreshold ||
      (candidate->evidence.depth!=2 && candidate->evidence.depth!=4 && candidate->evidence.depth!=8) ||
      !validEvidence(candidate->evidence)) { ++diagnostics.guidedInvalid; return false; }
  if (stationary(prediction.get_delay().front(),prediction.get_doppler().front())) {
    ++diagnostics.guidedStationaryRejected; return false;
  }
  if (!freshEvidence(track.get_id(index),candidate->evidence)) {
    ++diagnostics.guidedLineageRejected; return false;
  }
  ++diagnostics.guidedWouldRecover;
  const bool operational=track.get_state(index)=="ACTIVE" || track.get_state(index)=="COASTING";
  if (!(operational ? recovery.guidedSustain : recovery.guidedPromote)) return false;
  credit=creditEvidence(track.get_id(index),&candidate->evidence);
  if (operational) ++diagnostics.guidedSustained;
  return credit;
}

void Tracker::pruneLedgers()
{
  std::unordered_set<std::string> live;
  for (uint64_t i=0; i<track.get_n(); ++i) live.insert(track.get_id(i));
  for (auto it=evidenceLedgers.begin(); it!=evidenceLedgers.end();)
    if (!live.count(it->first)) it=evidenceLedgers.erase(it); else ++it;
  for (auto it=weakSupportLineage.begin(); it!=weakSupportLineage.end();)
    if (!live.count(it->first)) it=weakSupportLineage.erase(it); else ++it;
}

void Tracker::updateDopplerRidges(Detection& detection)
{
  if (!dopplerRidge.enabled) return;
  for (auto& ridge : dopplerRidges) ridge.seen = false;
  const auto& delay = detection.get_delay();
  const auto& doppler = detection.get_doppler();
  std::vector<double> evidence;
  for (size_t i = 0; i < doppler.size(); ++i)
  {
    if (!std::isfinite(delay[i]) || !std::isfinite(doppler[i])) continue;
    bool broad = false;
    for (size_t j = 0; j < doppler.size(); ++j)
      if (i != j && std::isfinite(delay[j]) && std::isfinite(doppler[j]) &&
          std::abs(doppler[i] - doppler[j]) <= dopplerRidge.toleranceHz &&
          std::abs(delay[i] - delay[j]) >= dopplerRidgeMinimumDelayBins)
      {
        broad = true;
        break;
      }
    if (!broad) continue;
    if (std::none_of(evidence.begin(), evidence.end(), [&](double value) {
          return std::abs(value - doppler[i]) <= dopplerRidge.toleranceHz;
        }))
      evidence.push_back(doppler[i]);
  }
  for (double value : evidence)
  {
    auto nearest = dopplerRidges.end();
    double distance = std::numeric_limits<double>::infinity();
    for (auto it = dopplerRidges.begin(); it != dopplerRidges.end(); ++it)
      if (const double candidate = std::abs(it->dopplerHz - value);
          candidate <= dopplerRidge.toleranceHz && candidate < distance)
      {
        nearest = it;
        distance = candidate;
      }
    if (nearest == dopplerRidges.end())
      dopplerRidges.push_back({value, 1, 0, true});
    else
    {
      nearest->dopplerHz = .8 * nearest->dopplerHz + .2 * value;
      nearest->streak = std::min<uint32_t>(UINT32_MAX, nearest->streak + 1);
      nearest->missedCpis = 0;
      nearest->seen = true;
    }
  }
  for (auto& ridge : dopplerRidges)
    if (!ridge.seen && ridge.streak && ++ridge.missedCpis >= 4)
    {
      --ridge.streak;
      ridge.missedCpis = 0;
    }
  dopplerRidges.erase(std::remove_if(dopplerRidges.begin(), dopplerRidges.end(),
    [](const auto& ridge) { return ridge.streak == 0; }), dopplerRidges.end());
}

bool Tracker::blocksDopplerRidgeBirth(double dopplerHz) const
{
  return dopplerRidge.enabled && std::any_of(dopplerRidges.begin(),
    dopplerRidges.end(), [&](const auto& ridge) {
      return ridge.streak >= dopplerRidge.persistenceCpis &&
        std::abs(ridge.dopplerHz - dopplerHz) <= dopplerRidge.toleranceHz;
    });
}

std::unique_ptr<Track> Tracker::process(Detection *detection, uint64_t currentTime,
  const std::vector<TrackerGuidedCandidate>& guided)
{
  const auto& evidence=detection->get_evidence();
  uint64_t epoch=0,sequence=0;
  bool haveEpoch=false;
  for (const auto& item:evidence) if (validEvidence(item,false)) {
    epoch=item.epoch; sequence=item.newestSequence; haveEpoch=true; break;
  }
  if (!haveEpoch) for (const auto& item:guided) if(item.valid && validEvidence(item.evidence,false)) {
    epoch=item.evidence.epoch; sequence=item.evidence.newestSequence; haveEpoch=true; break;
  }
  const bool epochReset=haveEpoch && evidenceEpochInitialized && epoch!=evidenceEpoch;
  if (epochReset) reset(true);
  if (haveEpoch) {evidenceEpoch=epoch;evidenceEpochInitialized=true;}
  // Duplicate or reversed timestamps cannot define a velocity update. Do not
  // age tracks or create duplicates from a repeated frame.
  diagnostics = pendingWeakSupport;
  pendingWeakSupport = {};
  diagnostics.detections = detection->get_nDetections();
  diagnostics.tracksBefore = track.get_n();
  diagnostics.lineageResets=pendingLineageReset;
  pendingLineageReset=false;
  diagnostics.stationaryCells=stationaryCells.size();
  if ((frameOrdinal > 0 && currentTime <= timestamp) ||
      (sequence && frameSequence && sequence<=frameSequence))
  {
    diagnostics.noOp = true;
    diagnostics.tracksAfter = track.get_n();
    return std::make_unique<Track>(track);
  }
  if (sequence) frameSequence=sequence;
  ++frameOrdinal;
  pruneStationary();
  frameHasEvidence=!evidence.empty();
  const size_t guidedLimit=recovery.guidedShadow ? recovery.guidedMaximumTracks : 0;
  frameGuided.assign(guided.begin(),guided.begin()+std::min(guided.size(),guidedLimit));
  diagnostics.guidedBudgetRejected=guided.size()-frameGuided.size();
  lineageNoBirth.assign(detection->get_nDetections(),false);
  doNotInitiate.clear();
  for (size_t i = 0; i < detection->get_nDetections(); i++)
  {
    doNotInitiate.push_back(false);
  }

  if (track.get_n() > 0)
  {
    update(detection, currentTime);
  }
  else
  {
    timestamp = currentTime;
  }
  pruneLedgers();
  initiate(detection);
  learnStationary(*detection);
  pruneLedgers();
  diagnostics.tracksAfter = track.get_n();

  return std::make_unique<Track>(track);
}

uint64_t Tracker::applyWeakSupport(
  const std::vector<TrackerWeakSupportCandidate>& candidates)
{
  pendingWeakSupport.weakSupportCandidates += candidates.size();
  std::unordered_set<std::string> liveIds;
  for (uint64_t index = 0; index < track.get_n(); ++index)
    liveIds.insert(track.get_id(index));
  for (auto it = weakSupportLineage.begin(); it != weakSupportLineage.end();)
    if (!liveIds.count(it->first)) it = weakSupportLineage.erase(it);
    else ++it;

  std::vector<bool> claimed(candidates.size(), false);
  uint64_t accepted = 0;
  const double delayGate = 1.0;
  const double dopplerGate = 1.0 / cpi;
  const blah2::tracker::NormalizedInnovationGate innovationGate({
    associationMaximumNis, 100, 1e-9});
  for (uint64_t index = 0; index < track.get_n(); ++index) {
    const auto state = track.get_state(index);
    if (state != "ACTIVE" && state != "COASTING") continue;
    auto current = track.get_current(index);
    const double delay = current.get_delay().front();
    const double doppler = current.get_doppler().front();
    std::size_t best = candidates.size();
    double bestDistance = std::numeric_limits<double>::infinity();
    for (std::size_t candidate = 0; candidate < candidates.size(); ++candidate) {
      if (claimed[candidate]) continue;
      const auto& value = candidates[candidate];
      const double dd = value.delay-delay;
      const double df = (value.dopplerHz-doppler)*cpi;
      const bool inGate = normalizedInnovationGate ?
        innovationGate.evaluate({dd, df}, {0, 0, 0, 0}, {1, 0, 0, 1}).accepted :
        (value.delay > delay-delayGate && value.delay < delay+delayGate &&
          value.dopplerHz > doppler-dopplerGate && value.dopplerHz < doppler+dopplerGate);
      if (!inGate) continue;
      ++pendingWeakSupport.weakSupportInGate;
      const double distance = dd*dd + df*df;
      if (distance < bestDistance) { bestDistance = distance; best = candidate; }
    }
    if (best == candidates.size()) continue;
    const auto& value = candidates[best];
    bool valid = (value.depth == 2 || value.depth == 4 || value.depth == 8) &&
      value.newestSequence != 0 &&
      value.newestSequence == value.sequences[0];
    for (uint8_t i = 1; valid && i < value.depth; ++i)
      valid = value.sequences[i] != 0 &&
        value.sequences[i] + 1 == value.sequences[i-1];
    auto& ledger = weakSupportLineage[track.get_id(index)];
    if (valid && (!ledger.initialized || ledger.epoch != value.epoch)) ledger = {value.epoch, 0, true};
    if (valid && value.newestSequence <= ledger.highWaterSequence) valid = false;
    // A valid candidate is one contiguous interval.  The oldest sequence is
    // therefore sufficient to reject overlap with every previously credited
    // interval; retaining every CPI for the lifetime of a track is unnecessary.
    if (valid && value.sequences[value.depth - 1] <= ledger.highWaterSequence)
      valid = false;
    if (!valid) {
      ++pendingWeakSupport.weakSupportLineageRejected;
      continue;
    }
    Detection::Evidence supportEvidence;
    supportEvidence.epoch=value.epoch;
    supportEvidence.newestSequence=value.newestSequence;
    supportEvidence.depth=value.depth;
    supportEvidence.sequences=value.sequences;
    if (!freshEvidence(track.get_id(index),supportEvidence)) {
      ++pendingWeakSupport.weakSupportLineageRejected;
      continue;
    }
    auto& shared=evidenceLedgers[track.get_id(index)];
    if (!shared.initialized || shared.epoch!=value.epoch) shared.votes.clear();
    shared.epoch=value.epoch; shared.highWaterSequence=value.newestSequence; shared.initialized=true;
    ledger.epoch = value.epoch;
    ledger.highWaterSequence = value.newestSequence;
    // This is a survival credit only. In particular, leave COASTING as
    // COASTING and do not append a measured sample.
    track.set_nInactive(index, 0);
    claimed[best] = true;
    ++accepted;
    pendingWeakSupport.weakSupportCreditedCpis += value.depth;
  }
  pendingWeakSupport.weakSupportAccepted += accepted;
  return accepted;
}

void Tracker::update(Detection *detection, uint64_t current)
{
  std::vector<double> delay = detection->get_delay();
  std::vector<double> doppler = detection->get_doppler();
  std::vector<double> snr = detection->get_snr();
  const auto& evidence = detection->get_evidence();

  // get time between detections
  if (current <= timestamp) return;
  const double T = static_cast<double>(current - timestamp) / 1000;
  timestamp = current;

  // loop over each track
  for (uint64_t i = 0; i < track.get_n();)
  {
    // predict next position
    Detection detectionCurrent = track.get_current(i);
    const double acc = track.get_acceleration(i);
    Detection prediction = predict(detectionCurrent, acc, T);
    const double delayPredict = prediction.get_delay().front();
    const double dopplerPredict = prediction.get_doppler().front();
    const bool needsAccelerationEstimate = adaptiveGate &&
      !track.get_acceleration_estimated(i);
    // First reassociation must use the actual elapsed time since the seed,
    // including all missed updates (not only the most recent CPI).
    const double accelerationTime = needsAccelerationEstimate ?
      T + track.get_missed_seconds(i) : T;
    const double delayGate = needsAccelerationEstimate ? 2.0 : 1.0;
    // A newly initiated adaptive track has no acceleration estimate yet.
    // Include the configured acceleration envelope plus one Doppler bin so a
    // real maneuver can supply the second observation that estimates it.
    const double dopplerGate = needsAccelerationEstimate ?
      maxAccInit * accelerationTime + 1 / cpi : 1 / cpi;
    const blah2::tracker::NormalizedInnovationGate innovationGate({
      associationMaximumNis, 100, 1e-9});
    size_t match = detection->get_nDetections();
    double bestDistance = std::numeric_limits<double>::infinity();
    
    // loop over detections to associate
    for (size_t j = 0; j < detection->get_nDetections(); j++)
    {
      ++diagnostics.pairs;
      if (doNotInitiate[j]) { ++diagnostics.pairClaimed; continue; }
      const double deltaDelay = delay[j] - delayPredict;
      const double deltaDoppler = (doppler[j] - dopplerPredict) * cpi;
      if (normalizedInnovationGate && !needsAccelerationEstimate)
      {
        const auto gate = innovationGate.evaluate({deltaDelay, deltaDoppler},
          {0, 0, 0, 0}, {1, 0, 0, 1});
        if (!gate.accepted) { ++diagnostics.pairInnovationRejected; continue; }
      }
      else
      {
        if (!(delay[j] > delayPredict-delayGate && delay[j] < delayPredict+delayGate))
          { ++diagnostics.pairDelayRejected; continue; }
        if (!(doppler[j] > dopplerPredict-dopplerGate && doppler[j] < dopplerPredict+dopplerGate))
          { ++diagnostics.pairDopplerRejected; continue; }
      }
      if ((evidence.empty() && evidenceEpochInitialized) ||
          (!evidence.empty() && !freshEvidence(track.get_id(i),evidence[j]))) {
        ++diagnostics.pairLineageRejected;
        lineageNoBirth[j]=true;
        continue;
      }
      ++diagnostics.pairInGate;
      // All predicates passed; preserve the original deterministic nearest
      // neighbour ranking and first-candidate tie break.
      {
        const double distance = deltaDelay * deltaDelay + deltaDoppler * deltaDoppler;
        if (distance < bestDistance) { bestDistance = distance; match = j; }
      }
    }

    if (match < detection->get_nDetections())
    {
      const std::string previousState = track.get_state(i);
      if (stationary(delay[match],doppler[match])) {
        doNotInitiate[match]=true;
        ++diagnostics.stationaryQuarantined;
        evidenceLedgers[track.get_id(i)].votes.clear();
        // Quarantined observations are not measured evidence or operational fixes.
        track.set_state(i,"TENTATIVE");
        track.set_predicted_current(i,prediction);
        appendVote(i,false);
        track.set_nInactive(i,track.get_nInactive(i)+1);
        track.set_missed_seconds(i,track.get_missed_seconds(i)+T);
        if (track.get_nUpdates(i)>=n || track.get_nInactive(i)>nDelete) {
          ++diagnostics.tentativeExpired; track.remove(i); continue;
        }
        ++i; continue;
      }
      const bool credit=creditEvidence(track.get_id(i),evidence.empty()?nullptr:&evidence[match]);
      track.set_acceleration(i, (doppler[match] - detectionCurrent.get_doppler().front()) / accelerationTime);
      track.set_acceleration_estimated(i, true);
      track.set_nInactive(i, 0);
      track.set_missed_seconds(i, 0);
      ++diagnostics.associations;
      doNotInitiate[match] = true;
      track.set_state(i, previousState == "ACTIVE" || previousState == "COASTING" ?
        "ACTIVE" : "ASSOCIATED");
      promote(i, credit);
      // Record the measured sample after promotion so its display history is
      // paired with the state actually assigned to that measurement.
      track.set_current(i, Detection(delay[match], doppler[match], snr[match]));
      if (track.get_state(i) == "ACTIVE" && previousState != "ACTIVE" && previousState != "COASTING")
        ++diagnostics.promotions;
      if (track.get_state(i) != "ACTIVE" && track.get_nUpdates(i) >= n)
      {
        ++diagnostics.tentativeExpired;
        track.remove(i);
        continue;
      }
      ++i;
      continue;
    }

    bool guidedCredit=false;
    if (guidedRecovery(i,prediction,guidedCredit)) {
      const std::string previous=track.get_state(i);
      const bool operational=previous=="ACTIVE" || previous=="COASTING";
      // A guided path is a prediction, never a measured location, velocity,
      // association count, or acceleration estimate.
      track.set_predicted_current(i,prediction);
      track.set_state(i,operational ? "COASTING" : "ASSOCIATED");
      track.set_nInactive(i,0);
      track.set_missed_seconds(i,track.get_missed_seconds(i)+T);
      promote(i,guidedCredit);
      if (!operational && track.get_state(i)=="ACTIVE") {
        ++diagnostics.promotions; ++diagnostics.guidedPromotions;
      }
      if (!operational && track.get_state(i)!="ACTIVE" && track.get_nUpdates(i)>=n) {
        ++diagnostics.tentativeExpired; track.remove(i); continue;
      }
      ++i; continue;
    }

    // update state if no detections associated
    // A coast is a kinematic prediction, not a measured association. Keeping
    // it out of measured history is essential to M-of-N expiry and telemetry.
    track.set_predicted_current(i, prediction);
    if (track.get_state(i) == "ACTIVE")
    {
      track.set_state(i, "COASTING");
    }
    else if (track.get_state(i) == "ASSOCIATED")
    {
      track.set_state(i, "TENTATIVE");
    }
    else
    {
      track.set_state(i, track.get_state(i));
    }
    track.set_nInactive(i, track.get_nInactive(i)+1);
    track.set_missed_seconds(i, track.get_missed_seconds(i)+T);
    appendVote(i,false);
    ++diagnostics.misses;

    // remove if tentative or coasting too long
    const bool tentativeExpired = track.get_state(i) != "ACTIVE" &&
      track.get_state(i) != "COASTING" && track.get_nUpdates(i) >= n;
    if (tentativeExpired || track.get_nInactive(i) > nDelete)
    {
      if (tentativeExpired) ++diagnostics.tentativeExpired;
      else ++diagnostics.inactivityDeleted;
      track.remove(i);
    }
    else ++i;
  }
}

Detection Tracker::predict(Detection current, double acc, double T)
{
  double delayTrack = current.get_delay().front();
  double dopplerTrack = current.get_doppler().front();
  // Positive Doppler is decreasing bistatic path length. Acceleration is in
  // Hz/s, so both terms need wavelength before conversion to delay bins.
  double delayPredict = delayTrack - lambda * (dopplerTrack*T +
    0.5*acc*T*T) / rangeRes;
  double dopplerPredict = dopplerTrack+(acc*T);
  Detection prediction(delayPredict, dopplerPredict, 0);
  return prediction;
}

void Tracker::initiate(Detection *detection)
{  
  std::vector<double> delay = detection->get_delay();
  std::vector<double> doppler = detection->get_doppler();
  std::vector<double> snr = detection->get_snr();
  const auto& evidence=detection->get_evidence();
  uint64_t index;
  updateDopplerRidges(*detection);

  std::vector<size_t> order(detection->get_nDetections());
  std::iota(order.begin(), order.end(), 0);
  if (birthExclusivity.enabled)
    std::stable_sort(order.begin(), order.end(), [&](size_t left, size_t right) {
      return snr[left] > snr[right];
    });
  std::vector<std::pair<double, double>> reservedBirthNeighborhoods;
  const bool boundedRecovery=frameHasEvidence || recovery.guidedShadow || recovery.stationaryClutter;
  std::unordered_set<std::string> liveLineageIds;
  if (boundedRecovery) for(uint64_t i=0;i<track.get_n();++i) liveLineageIds.insert(track.get_id(i));
  if (birthExclusivity.enabled)
    for (size_t i = 0; i < detection->get_nDetections(); ++i)
      if (doNotInitiate.at(i))
        reservedBirthNeighborhoods.emplace_back(delay[i], doppler[i]);

  // Existing tracks have already seen every raw candidate. Exclusivity acts
  // only on unmatched births, around actual claimed measurements (never an
  // unmeasured coast prediction) and stronger births selected this frame.
  for (const size_t i : order)
  {
    // skip if detection used in update
    if (doNotInitiate.at(i)) continue;
    ++diagnostics.birthCandidates;
    if (boundedRecovery &&
        track.get_n()+accInit.size()>65535) {
      ++diagnostics.birthCapacityRejected; continue;
    }
    if (lineageNoBirth[i] || (evidence.empty() && evidenceEpochInitialized) ||
        (!evidence.empty() && !validEvidence(evidence[i]))) {
      ++diagnostics.birthLineageRejected; continue;
    }
    if (snr[i] < minInitiationSnr)
    {
      ++diagnostics.birthSnrRejected;
      continue;
    }
    if (blocksDopplerRidgeBirth(doppler[i]))
    {
      ++diagnostics.birthRidgeRejected;
      continue;
    }
    if (stationary(delay[i],doppler[i])) {
      ++diagnostics.stationaryBirthRejected; continue;
    }
    if (stationaryCapacityBlocked(delay[i],doppler[i])) {
      ++diagnostics.stationaryBirthCapacityRejected; continue;
    }
    if (birthExclusivity.enabled &&
        std::any_of(reservedBirthNeighborhoods.begin(),
          reservedBirthNeighborhoods.end(), [&](const auto& reserved) {
            return std::abs(delay[i] - reserved.first) <=
                birthExclusivity.delayHalfWidthBins &&
              std::abs(doppler[i] - reserved.second) * cpi <=
                birthExclusivity.dopplerHalfWidthBins;
          }))
    {
      ++diagnostics.birthExclusiveRejected;
      continue;
    }
    // add tentative detection for each acc
    for (size_t j = 0; j < accInit.size(); j++)
    {
      Detection detectionCurrent(delay[i], doppler[i], snr[i]);
      index = track.add(detectionCurrent);
      // Public 16-bit IDs wrap. A still-live identity must never lend its
      // evidence votes to a newly seeded track after that wrap.
      if (boundedRecovery && !liveLineageIds.insert(track.get_id(index)).second) {
        track.remove(index);++diagnostics.birthCapacityRejected;continue;
      }
      track.set_acceleration(index, accInit[j]);
      const bool credit=creditEvidence(track.get_id(index),evidence.empty()?nullptr:&evidence[i]);
      promote(index,credit);
      ++diagnostics.initiated;
      if (track.get_state(index) == "ACTIVE") ++diagnostics.promotions;
    }
    if (birthExclusivity.enabled)
      reservedBirthNeighborhoods.emplace_back(delay[i], doppler[i]);
  }
}
