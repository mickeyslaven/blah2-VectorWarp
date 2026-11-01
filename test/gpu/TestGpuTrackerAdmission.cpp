#include "core/GpuTrackerAdmission.private.h"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>

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

int main()try{
 using gpu_tracker::boundInitiations;
 using gpu_tracker::DopplerCombBirth;
 using gpu_tracker::dopplerCombBirthEligible;
 using gpu_tracker::rangeCombBirthEligible;
 using gpu_tracker::rangeCombAmbiguous;
 using gpu_tracker::validQuarantineCoverage;
 using gpu_tracker::dopplerRidgeHasBirthCapacity;
 using gpu_tracker::advancePersistentStreak;
 using gpu_tracker::persistentCell;
 using gpu_tracker::persistentEvidence;
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
 uint32_t streak=0;uint16_t frame=1;
 for(uint32_t observation=0;observation<60;++observation,frame+=2)
  streak=advancePersistentStreak(streak,frame);
 require((streak&65535u)==60,
  "persistent cell did not learn intermittent recurring evidence");
 const uint16_t last=uint16_t(frame-2);
 const auto duplicate=advancePersistentStreak(streak,last);
 require(duplicate==streak,"duplicate detections strengthened one frame twice");
 uint32_t crossing=advancePersistentStreak(0,1);
 crossing=advancePersistentStreak(crossing,6);
 require((crossing&65535u)==1,
  "isolated transient crossing accumulated recurring evidence");
 require(persistentEvidence(streak,uint16_t(last+13))==57,
  "bounded absence did not leak evidence deterministically");
 require(persistentEvidence(streak,uint16_t(last+241))==0,
  "stale persistent evidence did not expire");
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
