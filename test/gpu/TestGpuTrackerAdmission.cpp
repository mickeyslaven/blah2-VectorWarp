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
 using gpu_tracker::stationaryRetirementPolicy;
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
 const auto retirement=stationaryRetirementPolicy(true,15);
 require(retirement.samples==100&&retirement.minimumAssociated==90&&
  retirement.maxDopplerHz==1&&retirement.maxDelaySpan==1,
  "stationary retirement policy is not conservative");
 const auto narrow=stationaryRetirementPolicy(true,.25);
 require(narrow.maxDopplerHz==.25,"stationary retirement widened the configured Doppler band");
 const auto disabled=stationaryRetirementPolicy(false,0);
 require(disabled.samples==0&&disabled.minimumAssociated==0&&disabled.maxDopplerHz==0,
  "disabled stationary retirement retained a policy");
 rejected=false;try{stationaryRetirementPolicy(true,-1);}catch(const std::invalid_argument&){rejected=true;}
 require(rejected,"invalid stationary retirement Doppler was accepted");
 std::cout<<"GPU_TRACKER_ADMISSION_PASS saturation_bounded=1 adaptive_single_slot=1 legacy_envelope_preserved=1 delayed_weak_target_retained=1 incompatible_corner_rejected=1 committed_preserved=1 unsaturated_unchanged=1 stationary_retirement_conservative=1\n";
 return 0;
}catch(const std::exception& e){std::cerr<<"FAIL gpu-tracker-admission: "<<e.what()<<'\n';return 1;}
