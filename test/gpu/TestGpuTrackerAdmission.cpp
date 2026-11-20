#include "core/GpuTrackerAdmission.private.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <utility>

static void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
static bool adaptiveCompatible(double delayDelta,double dopplerDelta,double span,double elapsed,
 double delayScale,double dopplerMargin){
 const double q=delayScale*.5*elapsed*elapsed;
 const double low=std::max(-span,std::max((dopplerDelta-dopplerMargin)/elapsed,(-1-delayDelta)/q));
 const double high=std::min(span,std::min((dopplerDelta+dopplerMargin)/elapsed,(1-delayDelta)/q));
 return std::isfinite(low)&&std::isfinite(high)&&low<high;
}

static double adaptiveElapsed(double frameElapsed,unsigned inactiveFrames){
 return frameElapsed*double(inactiveFrames+1);
}

static void associationRoundingContract(){
 // ARM Release enables FMA without an extra ISA flag. These exact distances
 // must retain the same separately rounded operations on every architecture.
 // Volatile inputs ensure this checks runtime arithmetic, not constant folding.
 volatile double delay=-1.125,cpi=.5,elapsed=.5,span=8,q=.125;
 for(const auto& sample:{std::pair<double,uint64_t>{0,0x3ff0333333333334ULL},
                        std::pair<double,uint64_t>{3.5,0x3fa9999999999999ULL}}){
  const auto result=tracker_association::associationDecision(delay,sample.first,0,0,cpi,
   true,elapsed,span,q,false,9.210340371976184);
  uint64_t bits=0;std::memcpy(&bits,&result.distance,sizeof(bits));
  require(result.valid&&result.accepted&&result.rejection==0&&bits==sample.second,
   "association binary64 rounding contracted or cross-architecture distance changed");
 }
 volatile double position=.24,velocity=48,acceleration=4,time=.8,wavelength=2,resolution=100;
 const double predictedDelay=tracker_association::predictionDelay(position,velocity,acceleration,time,wavelength,resolution),
  predictedDoppler=tracker_association::predictionDoppler(velocity,acceleration,time);
 uint64_t delayBits=0,dopplerBits=0;
 std::memcpy(&delayBits,&predictedDelay,sizeof(delayBits));
 std::memcpy(&dopplerBits,&predictedDoppler,sizeof(dopplerBits));
 require(delayBits==0xbfe1b71758e21966ULL&&dopplerBits==0x404999999999999aULL,
  "kinematic prediction binary64 rounding changed");
}

int main()try{
 associationRoundingContract();
 require(gpu_tracker::persistenceBytes(false,0)==48&&
  gpu_tracker::persistenceBytes(false,1000)==48&&
  gpu_tracker::persistenceBytes(true,1000)==24056,
  "disabled clutter buffer must hold its complete retirement and capacity header");
 require(!tracker_policy::frameClockNoOp(false,0,0,0,0)&&
  tracker_policy::frameClockNoOp(true,0,0,2,1)&&
  tracker_policy::frameClockNoOp(true,9,10,3,2)&&
  tracker_policy::frameClockNoOp(true,11,10,2,2)&&
  !tracker_policy::frameClockNoOp(true,11,10,3,2),
  "accepted-frame clock must handle first zero timestamp, empty duplicates and stale sequences");
 for(uint32_t depth:{1u,2u,4u,8u}){
  const auto minimum=tracker_confirmation::confirmationMinimumWindow(4,depth);
  require(!tracker_confirmation::confirmationFeasible(4,minimum-1,depth),"impossible confirmation accepted");
  require(tracker_confirmation::confirmationFeasible(4,minimum,depth),"feasible confirmation rejected");
  require(tracker_confirmation::confirmationCoastFeasible(depth-1,depth),"coast boundary rejected");
  if(depth>1)require(!tracker_confirmation::confirmationCoastFeasible(depth-2,depth),"short coast admitted");
 }
 using gpu_tracker::boundInitiations;
 using DopplerCombBirth=tracker_policy::Candidate;
 const auto view=[](const DopplerCombBirth* p,uint32_t n){return std::vector<DopplerCombBirth>(p,p+n);};
 const auto dopplerCombBirthEligible=[&](const auto* p,uint32_t n,uint32_t i){auto v=view(p,n);return tracker_policy::View{v,{}}.combBirthEligible(i,false);};
 const auto rangeCombBirthEligible=[&](const auto* p,uint32_t n,uint32_t i){auto v=view(p,n);return tracker_policy::View{v,{}}.combBirthEligible(i,true);};
 const auto rangeCombAmbiguous=[&](const auto* p,uint32_t n,uint32_t i){auto v=view(p,n);return tracker_policy::View{v,{}}.rangeCombAmbiguous(i);};
 using gpu_tracker::validQuarantineCoverage;
 const auto dopplerRidgeHasBirthCapacity=[&](const DopplerCombBirth* p,uint32_t n,const DopplerCombBirth& c){
  const auto tracks=view(p,n);return tracker_policy::View{{c},tracks}.dopplerRidgeHasBirthCapacity(0);};
 using tracker_policy::advancePersistentStreak;
 using gpu_tracker::persistentCell;
 using tracker_policy::persistentEvidence;
 using gpu_tracker::persistentRidgeContains;
 using gpu_tracker::stationaryRetirementPolicy;
 using gpu_tracker::validFineSpatialEvidence;
 require(validFineSpatialEvidence(1u,60),"primary-only fine spatial evidence was rejected");
 require(validFineSpatialEvidence(1u|(2u<<7u),60),"missing tertiary fine peak was rejected");
 require(validFineSpatialEvidence(1u|(2u<<7u)|(60u<<14u),60),"bounded three-peak evidence was rejected");
 require(!validFineSpatialEvidence(0u,60),"missing primary fine peak was accepted");
 require(!validFineSpatialEvidence(61u,60),"out-of-range primary fine peak was accepted");
 require(!validFineSpatialEvidence(1u|(61u<<7u),60),"out-of-range secondary fine peak was accepted");
 require(!validFineSpatialEvidence(1u|(61u<<14u),60),"out-of-range tertiary fine peak was accepted");
 auto a=boundInitiations(1024,1005,33,5);
 require(a.admittedDetections==3&&a.droppedDetections==30,"live saturation case was not bounded");
 require(a.initiatedTracks==15&&a.totalTracks==1020,"live saturation capacity arithmetic differs");
 auto adaptive=boundInitiations(1024,1005,33,1);
 require(adaptive.admittedDetections==19&&adaptive.droppedDetections==14,
  "adaptive initiation did not use one slot per detection");
 require(adaptive.initiatedTracks==19&&adaptive.totalTracks==1024,
  "adaptive initiation capacity arithmetic differs");
 constexpr double span=8,elapsed=.5,delayScale=.4,dopplerMargin=2;
 for(double acceleration=-span;acceleration<=span;acceleration+=4)
  for(double delayResidual:{-.9,0.,.9})for(double dopplerResidual:{-1.9,0.,1.9})
   require(adaptiveCompatible(delayResidual-delayScale*.5*acceleration*elapsed*elapsed,
    acceleration*elapsed+dopplerResidual,span,elapsed,delayScale,dopplerMargin),
    "adaptive gate excluded a legacy acceleration hypothesis");
 require(!adaptiveCompatible(1+delayScale*.5*span*elapsed*elapsed-.01,
  span*elapsed+dopplerMargin-.01,span,elapsed,delayScale,dopplerMargin),
  "adaptive gate accepted an incompatible delay/Doppler corner");
 constexpr unsigned missed=3;
 const double gap=adaptiveElapsed(elapsed,missed),weakAcceleration=4;
 const double delayedDelay=-delayScale*.5*weakAcceleration*gap*gap;
 const double delayedDoppler=weakAcceleration*gap;
 require(!adaptiveCompatible(delayedDelay,delayedDoppler,span,elapsed,delayScale,dopplerMargin),
  "single-CPI gate unexpectedly retained the delayed weak target");
 const DopplerCombBirth comb[]={{8,100,8,true},{20,102,12,true},{34,99,10,true}};
 require(!dopplerCombBirthEligible(comb,3,0)&&dopplerCombBirthEligible(comb,3,1)&&
  !dopplerCombBirthEligible(comb,3,2),"Doppler comb did not retain only its strongest birth");
 const DopplerCombBirth isolated[]={{8,100,8,true},{9,101,12,true},{34,120,10,true}};
 require(dopplerCombBirthEligible(isolated,3,0)&&dopplerCombBirthEligible(isolated,3,1)&&
  dopplerCombBirthEligible(isolated,3,2),"isolated or same-range births were suppressed");
 const DopplerCombBirth rangeComb[]={{14.0,80,8,true},{14.2,88,12,true},{13.9,96,10,true}};
 require(!rangeCombBirthEligible(rangeComb,3,0)&&rangeCombBirthEligible(rangeComb,3,1)&&
  !rangeCombBirthEligible(rangeComb,3,2),"same-range Doppler comb did not retain only its strongest birth");
 require(rangeCombAmbiguous(rangeComb,3,0)&&rangeCombAmbiguous(rangeComb,3,1)&&
  rangeCombAmbiguous(rangeComb,3,2),"same-range Doppler ambiguity was not retained");
 const DopplerCombBirth rangePair[]={{14.0,80,8,false},{14.2,88,12,false}};
 require(rangeCombAmbiguous(rangePair,2,0)&&rangeCombAmbiguous(rangePair,2,1),
  "two-member same-range ambiguity was published as resolved");
 const DopplerCombBirth rangeIsolated[]={{14.0,80,8,true},{14.7,88,12,true},{13.9,82,10,true}};
 require(rangeCombBirthEligible(rangeIsolated,3,0)&&rangeCombBirthEligible(rangeIsolated,3,1)&&
  rangeCombBirthEligible(rangeIsolated,3,2),"isolated range/Doppler births were suppressed");
 require(!rangeCombAmbiguous(rangeIsolated,3,0)&&!rangeCombAmbiguous(rangeIsolated,3,1)&&
  !rangeCombAmbiguous(rangeIsolated,3,2),"isolated detections were marked ambiguous");
 const DopplerCombBirth existing[]={{8,100,20,false},{20,101,12,true},{34,99,10,true}};
 require(dopplerCombBirthEligible(existing,3,1)&&dopplerCombBirthEligible(existing,3,2),
  "an established track participated in new-birth comb suppression");
 const DopplerCombBirth ridge[]={{8,100,20,true},{20,102,12,true}};
 require(!dopplerRidgeHasBirthCapacity(ridge,2,{34,99,10,true})&&
  dopplerRidgeHasBirthCapacity(ridge,2,{34,120,10,true})&&
  dopplerRidgeHasBirthCapacity(ridge,2,{10,99,10,true}),
  "Doppler-ridge birth capacity did not isolate distant same-Doppler siblings");
 require(adaptiveCompatible(delayedDelay,delayedDoppler,span,gap,delayScale,dopplerMargin),
  "adaptive gate did not retain a weak target across missed CPIs");
 a=boundInitiations(4,4,9,1);
 require(a.admittedDetections==0&&a.droppedDetections==9&&a.totalTracks==4,"full tracker did not preserve committed tracks");
 a=boundInitiations(10,2,3,2);
 require(a.admittedDetections==3&&a.droppedDetections==0&&a.totalTracks==8,"unsaturated tracker dropped detections");
 bool rejected=false;try{boundInitiations(3,4,1,1);}catch(const std::invalid_argument&){rejected=true;}
 require(rejected,"invalid retained count was accepted");
 rejected=false;try{boundInitiations(3,0,1,0);}catch(const std::invalid_argument&){rejected=true;}
 require(rejected,"zero hypotheses were accepted");
 const auto retirement=stationaryRetirementPolicy(true,.5,12);
 require(retirement.samples==12&&retirement.minimumAssociated==9&&
  retirement.maxDelaySpan==1&&retirement.maxDopplerSpanHz==3,
  "stationary quarantine policy does not follow persistence");
 require(validQuarantineCoverage(1,1)&&validQuarantineCoverage(3,2)&&
  !validQuarantineCoverage(0,1),"stationary quarantine coverage is not association-bounded");
 require(persistentCell(7,3,10)==37,"persistent map did not retain Doppler identity");
 require(persistentRidgeContains(37,27,10,8)&&persistentRidgeContains(37,47,10,8)&&
  persistentRidgeContains(37,33,10,8)&&persistentRidgeContains(37,48,10,8),
  "bounded delay walk and one-row Doppler jitter did not join the local neighborhood");
 require(!persistentRidgeContains(37,32,10,8)&&
  !persistentRidgeContains(37,57,10,8)&&!persistentRidgeContains(37,67,10,8),
  "persistent neighborhood exceeded its delay or Doppler radius");
 constexpr uint32_t hpRows=601,hpColumns=511,hpCenter=300*hpColumns+255;
 require(persistentRidgeContains(hpCenter,hpCenter+hpColumns+4,hpColumns,hpRows)&&
  !persistentRidgeContains(hpCenter,hpCenter+hpRows+4,hpColumns,hpRows),
  "non-square persistence exchanged delay and Doppler dimensions");
 for(uint32_t row:{0u,hpRows-1})for(uint32_t column:{0u,hpColumns-1}){
  const uint32_t cell=row*hpColumns+column;
  require(persistentRidgeContains(cell,cell,hpColumns,hpRows),
   "non-square persistence lost a corner cell");
 }
 require(!persistentRidgeContains(hpColumns-1,hpColumns,hpColumns,hpRows),
  "persistent neighborhood wrapped across a delay-axis edge");
 // T04: full-width recurrence, duplicates, stale reappearance and both word boundaries.
 uint32_t streak=0;uint64_t last=0;
 for(uint64_t frame=1;frame<120;frame+=2){streak=advancePersistentStreak(streak,last,frame);last=frame;}
 require(streak==60,"intermittent recurrence did not accumulate");
 require(advancePersistentStreak(streak,last,last)==streak,"duplicate detection strengthened evidence");
 require(persistentEvidence(streak,last,last+13)==57,"bounded absence decay differs");
 for(uint64_t generation:{301ull,65595ull,65596ull,65597ull,131132ull,4294967356ull}){
  require(persistentEvidence(60,60,generation)==0,"stale occupancy resurrected");
  require(advancePersistentStreak(60,60,generation)==1,"reappearance inherited stale evidence");}
 for(uint64_t generation:{65536ull,131072ull,4294967296ull}){
  require(advancePersistentStreak(60,generation-1,generation)==61,"live occupancy lost at rollover");
  require(advancePersistentStreak(65535,generation-1,generation)==65535,"maximum persistence overflowed");}
 require(persistentEvidence(60,61,60)==0,"reversed generation revived evidence");
 require(advancePersistentStreak(0,0,65596)==1,"reset did not cold start");
 // T01/T05: ordered ties and the inclusive configured cutoff.
 const DopplerCombBirth ties[]={{8,100,10,true},{20,102,10,true},{34,99,10,true}};
 for(uint32_t i=0;i<3;++i)require(dopplerCombBirthEligible(ties,3,i)==(i==0),"equal-SNR tie differs");
 for(double sign:{-1.,1.}){
  require(tracker_policy::stationaryDopplerEligible(sign*2,2),"inclusive Doppler boundary rejected");
  for(double value:{std::nextafter(2.,3.),2.01}){
   require(!tracker_policy::stationaryDopplerEligible(sign*value,2),"outside Doppler cutoff accepted");
   require(tracker_policy::stationaryDopplerEligible(sign*value,0),"all-Doppler control rejected");}}
 const auto disabled=stationaryRetirementPolicy(false,0,0);
 require(disabled.samples==0&&disabled.minimumAssociated==0&&disabled.maxDopplerSpanHz==0,
  "disabled stationary retirement retained a policy");
 rejected=false;try{stationaryRetirementPolicy(true,0,12);}catch(const std::invalid_argument&){rejected=true;}
 require(rejected,"invalid stationary retirement CPI was accepted");
 rejected=false;try{stationaryRetirementPolicy(true,.5,1);}catch(const std::invalid_argument&){rejected=true;}
 require(rejected,"invalid stationary quarantine persistence was accepted");
 std::cout<<"GPU_TRACKER_ADMISSION_PASS saturation_bounded=1 adaptive_single_slot=1 legacy_envelope_preserved=1 delayed_weak_target_retained=1 incompatible_corner_rejected=1 doppler_comb_strongest_retained=1 range_comb_ambiguity_held=1 doppler_ridge_capacity_bounded=1 established_track_exempt=1 committed_preserved=1 unsaturated_unchanged=1 stationary_quarantine_bounded=1 persistent_cell_locality=1 intermittent_recurrence=1 stale_expiry=1\n";
 return 0;
}catch(const std::exception& e){std::cerr<<"FAIL gpu-tracker-admission: "<<e.what()<<'\n';return 1;}
