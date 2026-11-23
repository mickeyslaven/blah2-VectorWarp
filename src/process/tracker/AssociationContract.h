#pragma once
// One prediction/association policy compiled as C++ and GLSL. The adaptive
// envelope/ranking is the resident acceleration-fit policy, not a larger box.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

// Products feeding sums have explicit binary64 rounding boundaries. C++ volatile
// prevents FMA contraction even in an FMA-enabled Release build; GLSL precise
// gives the same noncontracted operations without changing process-wide flags.
#define BLAH2_ASSOCIATION_FUNCTIONS(Q,P,R) \
 Q double associationProduct(double left,double right) {R double value=left*right;return value;} \
 Q double associationSquaredNorm(double left,double right) { \
   R double a=left*left,b=right*right;R double value=a+b;return value;} \
 Q double predictionDelay(double delay,double doppler,double acceleration,double time,double wavelength,double rangeResolution) { \
   R double correction=wavelength*(associationProduct(doppler,time)+ \
     associationProduct(0.5*acceleration*time,time))/rangeResolution; \
   P double value=delay-correction;return value;} \
 Q double predictionDoppler(double doppler,double acceleration,double time) { \
   P double value=doppler+associationProduct(acceleration,time);return value;} \
 Q double associationElapsed(double gap,double missed) { return gap+missed; } \
 Q double associationMissed(double gap,double missed,bool measured) { \
   return measured?0.0:associationElapsed(gap,missed); } \
 Q bool adaptiveAssociationGate(bool adaptive,bool accelerationEstimated, \
   bool coasting,double missed) { \
   return adaptive&&(!accelerationEstimated||(coasting&&missed>0.0)); } \
 struct AssociationDecision { bool valid; bool accepted; uint rejection; double distance; }; \
 Q uint spatialCandidate(uint spatial,uint candidate,uint beams,uint fineFactor) { \
   if ((spatial&0x80000000u)==0u) {return candidate==0u ? spatial&511u : 0u;} \
   uint fine=(spatial>>(candidate*7u))&127u; \
   return fine==0u || beams==0u || fineFactor==0u ? 0u : \
     (((fine-1u)+fineFactor/2u)/fineFactor)%beams+1u; } \
 Q bool spatialCompatible(uint left,uint right,uint beams,uint fineFactor,uint tolerance) { \
   if(tolerance==0xffffffffu || beams==0u || left==0u || right==0u) {return true;} \
   for(uint i=0u;i<3u;++i) { uint a=spatialCandidate(left,i,beams,fineFactor); if(a==0u){continue;} \
     for(uint j=0u;j<3u;++j) { uint b=spatialCandidate(right,j,beams,fineFactor); if(b==0u){continue;} \
       if(a>beams || b>beams){return false;} uint delta=a>b?a-b:b-a; \
       if(min(delta,beams-delta)<=tolerance){return true;} }} \
   return false; } \
 Q AssociationDecision associationDecision(double delay,double doppler,double dp,double vp, \
   double cpi,bool adaptive,double elapsed,double span,double q,bool normalized,double maximumNis) { \
   AssociationDecision result;result.valid=false;result.accepted=false;result.rejection=1u;result.distance=0.0; \
   P double delayMargin=normalized&&!adaptive?sqrt(maximumNis):1.0; \
   P double dopplerMargin=delayMargin/cpi; \
   if(adaptive){delayMargin+=associationProduct(q,span);dopplerMargin+=associationProduct(span,elapsed);} \
   P double lowDelay=dp-delayMargin,highDelay=dp+delayMargin; \
   P double lowDoppler=vp-dopplerMargin,highDoppler=vp+dopplerMargin; \
   result.valid=!isnan(lowDelay)&&!isinf(lowDelay)&&!isnan(highDelay)&&!isinf(highDelay)&& \
     !isnan(lowDoppler)&&!isinf(lowDoppler)&&!isnan(highDoppler)&&!isinf(highDoppler)&& \
     !isnan(delay)&&!isinf(delay)&&!isnan(doppler)&&!isinf(doppler)&& \
     cpi>0.0 && maximumNis>0.0 && (!adaptive || (elapsed>0.0 && q>0.0 && \
       !isnan(span)&&!isinf(span)&&span>=0.0)); \
   if(!result.valid){return result;} \
   P double dd=delay-dp,dv=doppler-vp,nd=dv*cpi; \
   if(normalized&&!adaptive) {if(!(associationSquaredNorm(dd,nd)<=maximumNis)){result.rejection=8u;return result;}} \
   else {if(!(delay>lowDelay&&delay<highDelay)){result.rejection=2u;return result;} \
     if(!(doppler>lowDoppler&&doppler<highDoppler)){result.rejection=4u;return result;}} \
   P double fit=0.0; \
   if(adaptive&&span>0.0) {P double unit=1.0/cpi; \
     P double low=max(-span,max((dv-unit)/elapsed,(-1.0-dd)/q)); \
     P double high=min(span,min((dv+unit)/elapsed,(1.0-dd)/q)); \
     if(isnan(low)||isinf(low)||isnan(high)||isinf(high)||!(low<high)){result.rejection=8u;return result;} \
     P double denominator=associationProduct(q,q)+associationProduct(elapsed*elapsed*cpi,cpi); \
     fit=clamp((associationProduct(elapsed*cpi*cpi,dv)-associationProduct(q,dd))/denominator,low,high); \
     dd+=associationProduct(q,fit);nd=(doppler-vp-associationProduct(fit,elapsed))*cpi; } \
   result.distance=associationSquaredNorm(dd,nd); \
   if(isnan(result.distance)||isinf(result.distance)){result.valid=false;result.rejection=1u;return result;} \
   result.accepted=true;result.rejection=0u;return result; }

namespace tracker_association {
using uint=uint32_t;
using std::min;using std::max;using std::clamp;using std::sqrt;
using std::isnan;using std::isinf;
BLAH2_ASSOCIATION_FUNCTIONS(inline,,volatile)
#define BLAH2_ASSOCIATION_STRINGIFY_IMPL(...) #__VA_ARGS__
#define BLAH2_ASSOCIATION_STRINGIFY(...) BLAH2_ASSOCIATION_STRINGIFY_IMPL(__VA_ARGS__)
inline constexpr const char* shaderSource =
  BLAH2_ASSOCIATION_STRINGIFY(BLAH2_ASSOCIATION_FUNCTIONS(,precise,precise));
#undef BLAH2_ASSOCIATION_STRINGIFY
#undef BLAH2_ASSOCIATION_STRINGIFY_IMPL
}
#undef BLAH2_ASSOCIATION_FUNCTIONS

// Quarantine suppresses publication, not measured state or confirmed lifetime.
// These are the original resident bounds: 75% associated, one delay cell,
// one-and-a-half Doppler cells, over the configured persistence interval.
#define BLAH2_QUARANTINE_FUNCTIONS(Q) \
 Q uint quarantineMinimumAssociated(uint samples) {return max(2u,(3u*samples+3u)/4u);} \
 Q bool quarantineHistory(uint samples,uint capacity,uint available,uint associated, \
   double delaySpan,double dopplerSpan,double cpi) { \
   return samples>=2u&&samples<=capacity&&available>=samples&& \
     associated>=quarantineMinimumAssociated(samples)&& \
     delaySpan<=1.0&&dopplerSpan<=1.5/cpi; } \
 Q uint quarantineAssociationState(bool quarantine,uint previous) { \
   return quarantine?0u:((previous==2u||previous==3u)?2u:1u); }
namespace tracker_quarantine {
using uint=uint32_t;using std::max;
BLAH2_QUARANTINE_FUNCTIONS(inline)
#define BLAH2_QUARANTINE_STRINGIFY_IMPL(...) #__VA_ARGS__
#define BLAH2_QUARANTINE_STRINGIFY(...) BLAH2_QUARANTINE_STRINGIFY_IMPL(__VA_ARGS__)
inline constexpr const char* shaderSource=BLAH2_QUARANTINE_STRINGIFY(BLAH2_QUARANTINE_FUNCTIONS());
#undef BLAH2_QUARANTINE_STRINGIFY
#undef BLAH2_QUARANTINE_STRINGIFY_IMPL
}
#undef BLAH2_QUARANTINE_FUNCTIONS

// Deep temporal processing may lower the numerical birth threshold, but a
// weak candidate must actually carry the accumulated evidence that justified
// doing so. Strong detections and non-temporal pipelines retain their ordinary
// admission behavior; existing-track association never calls this predicate.
#define BLAH2_WEAK_BIRTH_FUNCTIONS(Q) \
 Q bool weakBirthSupportEligible(double snr,uint maximumDepth,uint supportDepth) { \
   return maximumDepth<4u||snr>=7.0||supportDepth>=4u; }
namespace tracker_weak_birth {
using uint=uint32_t;
BLAH2_WEAK_BIRTH_FUNCTIONS(inline)
#define BLAH2_WEAK_BIRTH_STRINGIFY_IMPL(...) #__VA_ARGS__
#define BLAH2_WEAK_BIRTH_STRINGIFY(...) BLAH2_WEAK_BIRTH_STRINGIFY_IMPL(__VA_ARGS__)
inline constexpr const char* shaderSource=
  BLAH2_WEAK_BIRTH_STRINGIFY(BLAH2_WEAK_BIRTH_FUNCTIONS());
#undef BLAH2_WEAK_BIRTH_STRINGIFY
#undef BLAH2_WEAK_BIRTH_STRINGIFY_IMPL
}
#undef BLAH2_WEAK_BIRTH_FUNCTIONS

// Canonical admission policy. Adapters expose the ordered detector population
// and surviving *predictions*; decisions and ties are compiled identically for
// C++ and GLSL. Claimed measurements cannot suppress new births. Every valid
// detector edge is eligible; tracking adds no final-delay-column veto.
#define BLAH2_TRACKER_ADMISSION(Q) \
 Q bool rangeCombAmbiguous(uint i) { \
   for(uint j=0u;j<policyCount();++j) {if(i==j){continue;} \
     if(abs(policyDelay(j)-policyDelay(i))<=0.5&& \
        abs(policyRow(j)-policyRow(i))>=4.0){return true;}}return false;} \
 Q bool combBirthEligible(uint i,bool rangeComb) { \
   if(!policyUnmatched(i)){return false;}uint cluster=1u,winner=i; \
   for(uint j=0u;j<policyCount();++j) {if(i==j||!policyUnmatched(j)){continue;} \
     double dd=abs(policyDelay(j)-policyDelay(i)),dr=abs(policyRow(j)-policyRow(i)); \
     if(rangeComb?(dd>0.5||dr<4.0):(dd<4.0||dr>3.0)){continue;}++cluster; \
     if(policySnr(j)>policySnr(winner)||(policySnr(j)==policySnr(winner)&&j<winner)){winner=j;}} \
   return cluster<3u||winner==i;} \
 Q bool dopplerRidgeHasBirthCapacity(uint i) {uint siblings=0u; \
   for(uint j=0u;j<policyTracks();++j) {if(!policyAlive(j)){continue;} \
     if(abs(policyTrackRow(j)-policyRow(i))<=3.0&& \
        abs(policyTrackDelay(j)-policyDelay(i))>=4.0&&++siblings>=2u){return false;}}return true;}

#define BLAH2_STATIONARY_FUNCTIONS(Q) \
 Q bool clockAfter(uint low,uint high,uint previousLow,uint previousHigh) { \
   return high>previousHigh||(high==previousHigh&&low>previousLow); } \
 Q bool frameClockNoOp(bool accepted,uint low,uint high,uint previousLow,uint previousHigh, \
   uint sequenceLow,uint sequenceHigh,uint previousSequenceLow,uint previousSequenceHigh) { \
   return (accepted&&!clockAfter(low,high,previousLow,previousHigh))|| \
    ((sequenceLow!=0u||sequenceHigh!=0u)&&(previousSequenceLow!=0u||previousSequenceHigh!=0u)&& \
     !clockAfter(sequenceLow,sequenceHigh,previousSequenceLow,previousSequenceHigh)); } \
 Q bool nearestAxisLeft(double value,double left,double right) {return abs(value-left)<=abs(right-value);} \
 Q double nativeDelayCoordinate(double value,double first,uint count) { \
   double cell=ceil(value-first-0.5);return count==0u?cell:clamp(cell,0.0,double(count-1u));} \
 Q bool stationaryCapacityBlocked(uint occupied,uint capacity,bool known) {return !known&&occupied>=capacity;} \
 Q bool stationaryDopplerEligible(double doppler,double maximum) {return maximum==0.0||abs(doppler)<=maximum;} \
 Q uint persistentEvidence(uint streak,uint lastLow,uint lastHigh,uint nowLow,uint nowHigh) { \
   if(streak==0u||nowHigh<lastHigh||(nowHigh==lastHigh&&nowLow<lastLow)){return 0u;} \
   uint low=nowLow-lastLow,high=nowHigh-lastHigh-uint(nowLow<lastLow); \
   if(high!=0u){return 0u;}uint leaked=(low>0u?low-1u:0u)/4u;return leaked>=streak?0u:streak-leaked;} \
 Q uint advancePersistentStreak(uint streak,uint lastLow,uint lastHigh,uint nowLow,uint nowHigh) { \
   if(streak!=0u&&lastLow==nowLow&&lastHigh==nowHigh){return streak;} \
   return min(65535u,persistentEvidence(streak,lastLow,lastHigh,nowLow,nowHigh)+1u);}

namespace tracker_policy {
using uint=uint32_t;using std::abs;using std::min;using std::ceil;using std::clamp;
inline constexpr uint32_t residentStationaryCapacity=65536;
inline constexpr const char* identity="comb-v1-weak-depth4-stationary-u64-ordered-capacity-transaction-v3";
struct Candidate {double delay=0,row=0,snr=0;bool unmatched=true;};
struct View {
 const std::vector<Candidate>& candidates;
 const std::vector<Candidate>& predictions;
 uint policyCount() const {return uint(candidates.size());}
 uint policyTracks() const {return uint(predictions.size());}
 double policyDelay(uint i) const {return candidates[i].delay;}
 double policyRow(uint i) const {return candidates[i].row;}
 double policySnr(uint i) const {return candidates[i].snr;}
 bool policyUnmatched(uint i) const {return candidates[i].unmatched;}
 bool policyAlive(uint i) const {return predictions[i].unmatched;}
 double policyTrackDelay(uint i) const {return predictions[i].delay;}
 double policyTrackRow(uint i) const {return predictions[i].row;}
 BLAH2_TRACKER_ADMISSION(inline)
};
BLAH2_STATIONARY_FUNCTIONS(inline)
inline bool frameClockNoOp(bool accepted,uint64_t timestamp,uint64_t previous,
 uint64_t sequence,uint64_t previousSequence) {
 return frameClockNoOp(accepted,uint32_t(timestamp),uint32_t(timestamp>>32),
  uint32_t(previous),uint32_t(previous>>32),uint32_t(sequence),uint32_t(sequence>>32),
  uint32_t(previousSequence),uint32_t(previousSequence>>32));
}
inline uint32_t persistentEvidence(uint32_t streak,uint64_t last,uint64_t now) {
 return persistentEvidence(streak,uint32_t(last),uint32_t(last>>32),uint32_t(now),uint32_t(now>>32));
}
inline uint32_t advancePersistentStreak(uint32_t streak,uint64_t last,uint64_t now) {
 return advancePersistentStreak(streak,uint32_t(last),uint32_t(last>>32),uint32_t(now),uint32_t(now>>32));
}
#define BLAH2_POLICY_STRINGIFY_IMPL(...) #__VA_ARGS__
#define BLAH2_POLICY_STRINGIFY(...) BLAH2_POLICY_STRINGIFY_IMPL(__VA_ARGS__)
inline constexpr const char* admissionShaderSource=BLAH2_POLICY_STRINGIFY(BLAH2_TRACKER_ADMISSION());
inline constexpr const char* stationaryShaderSource=BLAH2_POLICY_STRINGIFY(BLAH2_STATIONARY_FUNCTIONS());
#undef BLAH2_POLICY_STRINGIFY
#undef BLAH2_POLICY_STRINGIFY_IMPL
}
#undef BLAH2_TRACKER_ADMISSION
#undef BLAH2_STATIONARY_FUNCTIONS
