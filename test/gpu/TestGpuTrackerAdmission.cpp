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

int main()try{
 using gpu_tracker::boundInitiations;
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
 a=boundInitiations(4,4,9,1);
 require(a.admittedDetections==0&&a.droppedDetections==9&&a.totalTracks==4,"full tracker did not preserve committed tracks");
 a=boundInitiations(10,2,3,2);
 require(a.admittedDetections==3&&a.droppedDetections==0&&a.totalTracks==8,"unsaturated tracker dropped detections");
 bool rejected=false;try{boundInitiations(3,4,1,1);}catch(const std::invalid_argument&){rejected=true;}
 require(rejected,"invalid retained count was accepted");
 rejected=false;try{boundInitiations(3,0,1,0);}catch(const std::invalid_argument&){rejected=true;}
 require(rejected,"zero hypotheses were accepted");
 std::cout<<"GPU_TRACKER_ADMISSION_PASS saturation_bounded=1 adaptive_single_slot=1 legacy_envelope_preserved=1 incompatible_corner_rejected=1 committed_preserved=1 unsaturated_unchanged=1\n";
 return 0;
}catch(const std::exception& e){std::cerr<<"FAIL gpu-tracker-admission: "<<e.what()<<'\n';return 1;}
