#include "Tracker.h"
#include "NormalizedInnovationGate.h"
#include "ConfirmationContract.h"
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
  const auto beams=spatialBeams,fineFactor=spatialFineFactor,tolerance=spatialTolerance;
  const auto delay=nativeDelay;const auto doppler=nativeDoppler;
  const auto evidenceDepth=maximumEvidenceDepth;
  *this = Tracker(m, n, nDelete, cpi, maxAccInit, rangeRes, lambda,
    minInitiationSnr, adaptiveGate, dopplerRidge, normalizedInnovationGate,
    associationMaximumNis, birthExclusivity, recovery);
  configureMaximumEvidenceDepth(evidenceDepth);
  configureSpatialAssociation(beams,fineFactor,tolerance);
  configureGrid(delay,doppler);
  pendingLineageReset=lineageDiscontinuity;
}

void Tracker::configureGrid(const std::deque<int>& delay,const std::deque<double>& doppler)
{
  if(delay==nativeDelay&&doppler==nativeDoppler)return;
  if(delay.empty()!=doppler.empty())throw std::invalid_argument("Incomplete tracker grid");
  for(size_t i=0;i<delay.size();++i)
    if(int64_t(delay[i])!=int64_t(delay.front())+int64_t(i))throw std::invalid_argument("Tracker delay grid is not contiguous");
  for(size_t i=0;i<doppler.size();++i)
    if(!std::isfinite(doppler[i])||(i&&doppler[i]<=doppler[i-1]))throw std::invalid_argument("Tracker Doppler grid is not ordered");
  if(frameOrdinal)reset(true);
  nativeDelay=delay;nativeDoppler=doppler;
}

double Tracker::dopplerRow(double value) const
{
  if(nativeDoppler.empty())return std::ceil(value*cpi-.5);
  const auto right=std::lower_bound(nativeDoppler.begin(),nativeDoppler.end(),value);
  if(right==nativeDoppler.begin())return 0;
  if(right==nativeDoppler.end())return double(nativeDoppler.size()-1);
  const auto index=right-nativeDoppler.begin();
  return tracker_policy::nearestAxisLeft(value,nativeDoppler[index-1],*right)?index-1:index;
}

void Tracker::configureSpatialAssociation(uint32_t beams,uint32_t fineFactor,uint32_t tolerance)
{
  if(frameOrdinal || beams>511 || !fineFactor || fineFactor>127 ||
     (!beams && tolerance!=UINT32_MAX) || (beams && tolerance!=UINT32_MAX && tolerance>beams))
    throw std::invalid_argument("Invalid or already-active tracker spatial association contract");
  spatialBeams=beams;spatialFineFactor=fineFactor;spatialTolerance=tolerance;
}

std::vector<TrackerQualificationState> Tracker::qualificationState()
{
  std::vector<TrackerQualificationState> result;result.reserve(track.get_n());
  for(uint64_t i=0;i<track.get_n();++i){TrackerQualificationState value;
    value.id=track.get_id(i);const auto state=track.get_state(i);auto current=track.get_current(i);
    value.state=state=="TENTATIVE"?0u:state=="ASSOCIATED"?1u:state=="ACTIVE"?2u:3u;
    value.delay=current.get_delay().front();value.doppler=current.get_doppler().front();
    value.acceleration=track.get_acceleration(i);value.missedSeconds=track.get_missed_seconds(i);
    value.inactive=track.get_nInactive(i);value.measurements=track.get_nAssociated(i);
    value.updates=track.get_nUpdates(i);value.spatial=spatialForTrack(value.id);
    value.source=uint32_t(track.get_current_source(i));
    if(value.source==uint32_t(Track::Source::detector)&&current.get_evidence().size()==1)
      value.measurementEvidence=current.get_evidence().front();
    value.accelerationEstimated=track.get_acceleration_estimated(i);
    value.everConfirmed=previouslyConfirmed.count(value.id)!=0;
    const auto found=evidenceLedgers.find(value.id);
    if(found!=evidenceLedgers.end()) {const auto& ledger=found->second;
      value.lineageKnown=ledger.initialized;value.epoch=ledger.epoch;
      value.highWaterSequence=ledger.highWaterSequence;
      for(bool vote:ledger.votes)value.confirmationVotes.push_back(vote);
    }
    result.push_back(std::move(value));
  }
  return result;
}

uint64_t Tracker::initiationMultiplicity(double cpi,
  double maximumAcceleration, bool adaptive)
{
  if (!std::isfinite(cpi) || cpi <= 0 ||
      !std::isfinite(maximumAcceleration) || maximumAcceleration < 0)
    throw std::invalid_argument("Invalid initiation geometry");
  const double resolution = 1 / (cpi * cpi);
  const double bins = std::floor(maximumAcceleration / resolution);
  if (!std::isfinite(resolution) || resolution<=0 || !std::isfinite(bins) || bins > 65535)
    throw std::invalid_argument("Too many initial tracking acceleration hypotheses");
  if (adaptive) return 1;
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
       config.maximumStationaryCells > 1048576)))
    throw std::invalid_argument("Invalid shared tracker recovery policy");
  recovery = config;
  // Configuration boundaries cannot reuse prior algorithm evidence.
  track=Track{}; timestamp=0; diagnostics={};
  doNotInitiate.clear(); lineageNoBirth.clear(); frameHasEvidence=false;
  evidenceLedgers.clear(); stationaryCells.clear(); trackSpatial.clear();
  quarantineHistory.clear();previouslyConfirmed.clear();
  dopplerRidges.clear(); pendingLineageReset=false;
  evidenceEpoch = 0; evidenceEpochInitialized=false; frameSequence = 0; frameOrdinal = 0;
}

void Tracker::configureMaximumEvidenceDepth(uint32_t depth)
{
  if (depth != 1 && depth != 2 && depth != 4 && depth != 8)
    throw std::invalid_argument("Invalid tracker evidence depth");
  maximumEvidenceDepth = depth;
}

bool Tracker::validEvidence(const Detection::Evidence& value, bool requireCurrentEpoch) const
{
  return Detection::valid_evidence(value) &&
    !(requireCurrentEpoch && evidenceEpochInitialized && value.epoch != evidenceEpoch);
}

bool Tracker::freshEvidence(const std::string& id, const Detection::Evidence& value) const
{
  if (!validEvidence(value)) return false;
  const auto found = evidenceLedgers.find(id);
  if (found == evidenceLedgers.end() || !found->second.initialized || found->second.epoch != value.epoch)
    return true;
  if (value.newestSequence<=found->second.highWaterSequence) return false;
  const uint64_t gap=value.newestSequence-found->second.highWaterSequence;
  const uint32_t recent=gap<8 ? 1u<<uint32_t(gap-1) : 0;
  return tracker_confirmation::confirmationDisjoint(value.depth,recent);
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
  if (!frameHasEvidence && !recovery.stationaryClutter &&
      evidenceLedgers.find(track.get_id(index))==evidenceLedgers.end()) return;
  auto& votes = evidenceLedgers[track.get_id(index)].votes;
  votes.push_back(credit);
  if (votes.size() > n) votes.pop_front();
}

void Tracker::promote(uint64_t index, bool credit)
{
  const auto found=evidenceLedgers.find(track.get_id(index));
  if (!frameHasEvidence && !recovery.stationaryClutter &&
      (found==evidenceLedgers.end() || !found->second.initialized)) {
    track.promote(index, m, n);
    return;
  }
  appendVote(index, credit);
  const auto& votes = evidenceLedgers[track.get_id(index)].votes;
  if (credit && std::count(votes.begin(), votes.end(), true) >= m &&
      track.get_state(index) != "COASTING") track.promote(index, 1, 1);
}

bool Tracker::stationaryKey(double delay, double doppler,
  std::pair<int64_t,int64_t>& key) const
{
  // Explicit bounds also keep neighbourhood addition away from int64 overflow.
  if (!std::isfinite(delay) || !std::isfinite(doppler) ||
      std::abs(delay) > 1e9 || std::abs(doppler*cpi) > 1e9) return false;
  const double column=tracker_policy::nativeDelayCoordinate(delay,nativeDelay.empty()?0:nativeDelay.front(),uint32_t(nativeDelay.size()));
  key = {int64_t(column), int64_t(dopplerRow(doppler))};
  return true;
}

bool Tracker::stationary(double delay, double doppler) const
{
  if (!recovery.stationaryClutter ||
      !tracker_policy::stationaryDopplerEligible(doppler,recovery.maximumDopplerHz)) return false;
  std::pair<int64_t,int64_t> key;
  if (!stationaryKey(delay,doppler,key)) return true;
  // Match the resident ridge lookup over the learned 9-by-3 neighborhood.
  // Missing/exhausted storage is never evidence of stationary clutter.
  for(int x=-4;x<=4;++x)for(int y=-1;y<=1;++y){
    const auto found=stationaryCells.find({key.first+x,key.second+y});
    if(found==stationaryCells.end())continue;
    const auto& cell=found->second;
    if(tracker_policy::persistentEvidence(cell.streak,cell.last,frameOrdinal)>=recovery.persistenceCpis)return true;
  }
  return false;
}

bool Tracker::quarantine(uint64_t index,double delay,double doppler) const
{
  if(!stationary(delay,doppler))return false;
  const auto found=quarantineHistory.find(track.get_id(index));
  const uint32_t samples=recovery.persistenceCpis,capacity=std::max(100u,n);
  if(found==quarantineHistory.end() || samples<2 || samples>capacity || found->second.size()+1<samples)return false;
  double lowDelay=delay,highDelay=delay,lowDoppler=doppler,highDoppler=doppler;
  uint32_t associated=1;
  const auto& history=found->second;
  for(size_t i=history.size()-(samples-1);i<history.size();++i){const auto& value=history[i];
    lowDelay=std::min(lowDelay,value.delay);highDelay=std::max(highDelay,value.delay);
    lowDoppler=std::min(lowDoppler,value.doppler);highDoppler=std::max(highDoppler,value.doppler);
    associated+=value.associated;
  }
  return tracker_quarantine::quarantineHistory(samples,capacity,uint32_t(history.size()+1),
    associated,highDelay-lowDelay,highDoppler-lowDoppler,cpi);
}

bool Tracker::stationaryCapacityBlocked(double delay, double doppler) const
{
  if (!recovery.stationaryClutter ||
      !tracker_policy::stationaryDopplerEligible(doppler,recovery.maximumDopplerHz)) return false;
  std::pair<int64_t,int64_t> key;
  return stationaryKey(delay,doppler,key) &&
    tracker_policy::stationaryCapacityBlocked(uint32_t(stationaryCells.size()),recovery.maximumStationaryCells,stationaryCells.count(key)!=0);
}

void Tracker::pruneStationary()
{
  // Evict before association/birth admission, not one frame after a cell expires.
  for (auto it = stationaryCells.begin(); it != stationaryCells.end();)
    if (tracker_policy::persistentEvidence(it->second.streak,it->second.last,frameOrdinal)==0)
      it = stationaryCells.erase(it);
    else ++it;
}

void Tracker::learnStationary(Detection& detection)
{
  if (!recovery.stationaryClutter) return;
  const auto delay = detection.get_delay(), doppler = detection.get_doppler();
  for (size_t i = 0; i < delay.size(); ++i) {
    if (!tracker_policy::stationaryDopplerEligible(doppler[i],recovery.maximumDopplerHz)) continue;
    std::pair<int64_t,int64_t> key;
    if (!stationaryKey(delay[i],doppler[i],key)) continue;
    // Same 9-delay by 3-Doppler-cell occupancy neighbourhood as the resident policy.
    for (int y=-1; y<=1; ++y) for (int x=-4; x<=4; ++x) {
      const auto cellKey = std::make_pair(key.first+x,key.second+y);
      if(!nativeDelay.empty()&&(cellKey.first<0||cellKey.second<0||
         cellKey.first>=int64_t(nativeDelay.size())||cellKey.second>=int64_t(nativeDoppler.size())))continue;
      auto found = stationaryCells.find(cellKey);
      if (found == stationaryCells.end()) {
        if (stationaryCells.size() >= recovery.maximumStationaryCells) {
          ++diagnostics.stationaryCapacityRejected; continue;
        }
        found = stationaryCells.emplace(cellKey,StationaryCell{}).first;
      }
      auto& cell = found->second;
      if (cell.last == frameOrdinal) continue;
      cell.streak = tracker_policy::advancePersistentStreak(cell.streak,cell.last,frameOrdinal);
      cell.last = frameOrdinal;
    }
  }
  diagnostics.stationaryCells = stationaryCells.size();
}

void Tracker::pruneLedgers()
{
  std::unordered_set<std::string> live;
  for (uint64_t i=0; i<track.get_n(); ++i) live.insert(track.get_id(i));
  for (auto it=evidenceLedgers.begin(); it!=evidenceLedgers.end();)
    if (!live.count(it->first)) it=evidenceLedgers.erase(it); else ++it;
  for (auto it=trackSpatial.begin(); it!=trackSpatial.end();)
    if (!live.count(it->first)) it=trackSpatial.erase(it); else ++it;
  for(auto it=quarantineHistory.begin();it!=quarantineHistory.end();)
    if(!live.count(it->first))it=quarantineHistory.erase(it);else ++it;
  for(auto it=previouslyConfirmed.begin();it!=previouslyConfirmed.end();)
    if(!live.count(*it))it=previouslyConfirmed.erase(it);else ++it;
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

std::unique_ptr<Track> Tracker::process(Detection *detection, uint64_t currentTime)
{
  const auto& evidence=detection->get_evidence();
  const auto& spatial=detection->get_spatial();
  if(spatialBeams && spatial.size()!=detection->get_nDetections())
    throw std::invalid_argument("Beam-aware tracking requires explicit spatial evidence");
  for(uint32_t word:spatial) {
    if(word&0x80000000u) {
      const uint32_t a=word&127u,b=(word>>7u)&127u,c=(word>>14u)&127u;
      const uint64_t fineCount=uint64_t(spatialBeams)*spatialFineFactor;
      if(!spatialBeams || !a || a>fineCount || b>fineCount || c>fineCount)
        throw std::invalid_argument("Invalid fine spatial evidence");
    } else if((word&511u)>spatialBeams || ((word>>9u)&2047u)>spatialBeams*4u ||
              ((word&511u)==0u && ((word>>9u)&2047u)!=0u))
      throw std::invalid_argument("Invalid coarse spatial evidence");
  }
  // Refuse impossible temporal births before changing any state. The pipeline
  // also validates the deepest enabled bank before its first warm-up frame.
  for (const auto& item:evidence) if (validEvidence(item,false))
    tracker_confirmation::validate(m,n,nDelete,item.depth);
  uint64_t epoch=0,sequence=0;
  bool haveEpoch=false;
  for (const auto& item:evidence) if (validEvidence(item,false)) {
    epoch=item.epoch; sequence=item.newestSequence; haveEpoch=true; break;
  }
  const bool epochReset=haveEpoch && evidenceEpochInitialized && epoch!=evidenceEpoch;
  if (epochReset) reset(true);
  if (haveEpoch) {evidenceEpoch=epoch;evidenceEpochInitialized=true;}
  // Duplicate or reversed timestamps cannot define a velocity update. Do not
  // age tracks or create duplicates from a repeated frame.
  diagnostics = {};
  diagnostics.detections = detection->get_nDetections();
  diagnostics.tracksBefore = track.get_n();
  diagnostics.lineageResets=pendingLineageReset;
  pendingLineageReset=false;
  diagnostics.stationaryCells=stationaryCells.size();
  if (tracker_policy::frameClockNoOp(frameOrdinal>0,currentTime,timestamp,sequence,frameSequence))
  {
    diagnostics.noOp = true;
    diagnostics.tracksAfter = track.get_n();
    return std::make_unique<Track>(track);
  }
  if (sequence) frameSequence=sequence;
  ++frameOrdinal;
  pruneStationary();
  frameHasEvidence=!evidence.empty();
  lineageNoBirth.assign(detection->get_nDetections(),false);
  policyCandidates.clear();policyPredictions.clear();
  const auto delays=detection->get_delay(),dopplers=detection->get_doppler(),snrs=detection->get_snr();
  for(size_t i=0;i<delays.size();++i)policyCandidates.push_back({delays[i],dopplerRow(dopplers[i]),snrs[i],true});
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
  for(size_t i=0;i<policyCandidates.size();++i)policyCandidates[i].unmatched=!doNotInitiate[i]&&!lineageNoBirth[i];
  // Resident ordering: association sees prior occupancy; births see the
  // current accepted endpoint's occupancy update as well.
  learnStationary(*detection);
  initiate(detection);
  pruneLedgers();
  for(uint64_t i=0;i<track.get_n();++i){const auto state=track.get_state(i);
    if(state=="ACTIVE"||state=="COASTING")previouslyConfirmed.insert(track.get_id(i));
    if(recovery.stationaryClutter){auto value=track.get_current(i);auto& history=quarantineHistory[track.get_id(i)];
      history.push_back({value.get_delay().front(),value.get_doppler().front(),track.get_current_source(i)==Track::Source::detector});
      if(history.size()>std::max(100u,n))history.pop_front();}
  }
  diagnostics.tracksAfter = track.get_n();

  return std::make_unique<Track>(track);
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
    const std::string trackState = track.get_state(i);
    const bool adaptiveAssociation = tracker_association::adaptiveAssociationGate(
      adaptiveGate, track.get_acceleration_estimated(i),
      trackState == "COASTING", track.get_missed_seconds(i));
    // First reassociation and confirmed-track reacquisition must use the actual
    // elapsed time since the last measurement, including all missed updates.
    const double accelerationTime = adaptiveAssociation ?
      tracker_association::associationElapsed(T,track.get_missed_seconds(i)) : T;
    const double resolution=1/(cpi*cpi);
    const double span=resolution*std::floor(maxAccInit/resolution);
    const double q=lambda*.5*accelerationTime*accelerationTime/rangeRes;
    size_t match = detection->get_nDetections();
    double bestDistance = std::numeric_limits<double>::infinity();
    
    // loop over detections to associate
    for (size_t j = 0; j < detection->get_nDetections(); j++)
    {
      ++diagnostics.pairs;
      if (doNotInitiate[j]) { ++diagnostics.pairClaimed; continue; }
      const auto gate=tracker_association::associationDecision(delay[j],doppler[j],
        delayPredict,dopplerPredict,cpi,adaptiveAssociation,accelerationTime,
        span,q,normalizedInnovationGate,associationMaximumNis);
      if(!gate.accepted) {
        if(gate.rejection==2)++diagnostics.pairDelayRejected;
        else if(gate.rejection==4)++diagnostics.pairDopplerRejected;
        else ++diagnostics.pairInnovationRejected;
        continue;
      }
      if(!tracker_association::spatialCompatible(spatialForTrack(track.get_id(i)),
          detection->get_spatial().empty()?0u:detection->get_spatial()[j],
          spatialBeams,spatialFineFactor,spatialTolerance)) {
        ++diagnostics.pairSpatialRejected;continue;
      }
      // A new endpoint can share older source CPIs with the last measurement.
      // That overlap forbids another confirmation vote, not a measured update.
      if ((evidence.empty() && evidenceEpochInitialized) ||
          (!evidence.empty() && (!validEvidence(evidence[j]) ||
           evidence[j].newestSequence!=frameSequence))) {
        ++diagnostics.pairLineageRejected;
        lineageNoBirth[j]=true;
        continue;
      }
      ++diagnostics.pairInGate;
      // All predicates passed; preserve the original deterministic nearest
      // neighbour ranking and first-candidate tie break.
      {
        const double distance = gate.distance;
        if (distance < bestDistance) { bestDistance = distance; match = j; }
      }
    }

    if (match < detection->get_nDetections())
    {
      const std::string previousState = track.get_state(i);
      const bool quarantined=quarantine(i,delay[match],doppler[match]);
      if(quarantined)++diagnostics.stationaryQuarantined;
      const bool credit=creditEvidence(track.get_id(i),evidence.empty()?nullptr:&evidence[match]);
      const double updatedAcceleration = adaptiveAssociation &&
          track.get_missed_seconds(i) > 0 ?
        acc + (doppler[match] - dopplerPredict) / accelerationTime :
        (doppler[match] - detectionCurrent.get_doppler().front()) / accelerationTime;
      track.set_acceleration(i, updatedAcceleration);
      track.set_acceleration_estimated(i, true);
      track.set_nInactive(i, 0);
      track.set_missed_seconds(i,tracker_association::associationMissed(T,track.get_missed_seconds(i),true));
      ++diagnostics.associations;
      doNotInitiate[match] = true;
      const uint32_t oldState=previousState=="ACTIVE"?2u:previousState=="COASTING"?3u:previousState=="ASSOCIATED"?1u:0u;
      const auto nextState=tracker_quarantine::quarantineAssociationState(quarantined,oldState);
      track.set_state(i,nextState==0?"TENTATIVE":nextState==2?"ACTIVE":"ASSOCIATED");
      if(quarantined||tracker_policy::View{policyCandidates,policyPredictions}.rangeCombAmbiguous(uint32_t(match)))
        appendVote(i,credit);else promote(i, credit);
      // Record the measured sample after promotion so its display history is
      // paired with the state actually assigned to that measurement.
      Detection measured(delay[match], doppler[match], snr[match]);
      if (!evidence.empty()) measured.set_evidence({evidence[match]});
      track.set_current(i, std::move(measured));
      if(spatialBeams)trackSpatial[track.get_id(i)]=detection->get_spatial()[match];
      if (track.get_state(i) == "ACTIVE" && previousState != "ACTIVE" && previousState != "COASTING")
        ++diagnostics.promotions;
      if (track.get_state(i) != "ACTIVE" && !previouslyConfirmed.count(track.get_id(i)) && track.get_nUpdates(i) >= n)
      {
        ++diagnostics.tentativeExpired;
        track.remove(i);
        continue;
      }
      policyPredictions.push_back({delayPredict,dopplerRow(dopplerPredict),0,true});
      ++i;
      continue;
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
    track.set_missed_seconds(i,tracker_association::associationMissed(T,track.get_missed_seconds(i),false));
    appendVote(i,false);
    ++diagnostics.misses;

    // remove if tentative or coasting too long
    const bool tentativeExpired = track.get_state(i) != "ACTIVE" &&
      track.get_state(i) != "COASTING" && !previouslyConfirmed.count(track.get_id(i)) && track.get_nUpdates(i) >= n;
    if (tentativeExpired || track.get_nInactive(i) > nDelete)
    {
      if (tentativeExpired) ++diagnostics.tentativeExpired;
      else ++diagnostics.inactivityDeleted;
      track.remove(i);
    }
    else {policyPredictions.push_back({delayPredict,dopplerRow(dopplerPredict),0,true});++i;}
  }
}

Detection Tracker::predict(Detection current, double acc, double T)
{
  double delayTrack = current.get_delay().front();
  double dopplerTrack = current.get_doppler().front();
  // Positive Doppler is decreasing bistatic path length. Acceleration is in
  // Hz/s, so both terms need wavelength before conversion to delay bins.
  double delayPredict = tracker_association::predictionDelay(delayTrack,dopplerTrack,acc,T,lambda,rangeRes);
  double dopplerPredict = tracker_association::predictionDoppler(dopplerTrack,acc,T);
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
  const bool boundedRecovery=frameHasEvidence || recovery.stationaryClutter;
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
        (!evidence.empty() && (!validEvidence(evidence[i]) ||
         evidence[i].newestSequence!=frameSequence))) {
      ++diagnostics.birthLineageRejected; continue;
    }
    const uint32_t supportDepth = evidence.empty() ? 1u :
      (validEvidence(evidence[i]) ? evidence[i].depth : 0u);
    if (snr[i] < minInitiationSnr ||
        !tracker_weak_birth::weakBirthSupportEligible(
          snr[i], maximumEvidenceDepth, supportDepth))
    {
      ++diagnostics.birthSnrRejected;
      continue;
    }
    if (blocksDopplerRidgeBirth(doppler[i]))
    {
      ++diagnostics.birthRidgeRejected;
      continue;
    }
    tracker_policy::View policy{policyCandidates,policyPredictions};
    if(!policy.dopplerRidgeHasBirthCapacity(uint32_t(i))||!policy.combBirthEligible(uint32_t(i),false)) {
      ++diagnostics.birthDopplerCombRejected;continue;
    }
    if(!policy.combBirthEligible(uint32_t(i),true)) {++diagnostics.birthRangeCombRejected;continue;}
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
      if (!evidence.empty()) detectionCurrent.set_evidence({evidence[i]});
      index = track.add(detectionCurrent);
      // Public 16-bit IDs wrap. A still-live identity must never lend its
      // evidence votes to a newly seeded track after that wrap.
      if (boundedRecovery && !liveLineageIds.insert(track.get_id(index)).second) {
        track.remove(index);++diagnostics.birthCapacityRejected;continue;
      }
      track.set_acceleration(index, accInit[j]);
      if(spatialBeams)trackSpatial[track.get_id(index)]=detection->get_spatial()[i];
      const bool credit=creditEvidence(track.get_id(index),evidence.empty()?nullptr:&evidence[i]);
      if(policy.rangeCombAmbiguous(uint32_t(i)))appendVote(index,credit);else promote(index,credit);
      ++diagnostics.initiated;
      if (track.get_state(index) == "ACTIVE") ++diagnostics.promotions;
    }
    if (birthExclusivity.enabled)
      reservedBirthNeighborhoods.emplace_back(delay[i], doppler[i]);
  }
}
