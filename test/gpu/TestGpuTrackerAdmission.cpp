#include "core/GpuTrackerAdmission.private.h"
#include <iostream>
#include <stdexcept>

static void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}

int main()try{
 using gpu_tracker::boundInitiations;
 auto a=boundInitiations(1024,1005,33,5);
 require(a.admittedDetections==3&&a.droppedDetections==30,"live saturation case was not bounded");
 require(a.initiatedTracks==15&&a.totalTracks==1020,"live saturation capacity arithmetic differs");
 a=boundInitiations(4,4,9,1);
 require(a.admittedDetections==0&&a.droppedDetections==9&&a.totalTracks==4,"full tracker did not preserve committed tracks");
 a=boundInitiations(10,2,3,2);
 require(a.admittedDetections==3&&a.droppedDetections==0&&a.totalTracks==8,"unsaturated tracker dropped detections");
 bool rejected=false;try{boundInitiations(3,4,1,1);}catch(const std::invalid_argument&){rejected=true;}
 require(rejected,"invalid retained count was accepted");
 rejected=false;try{boundInitiations(3,0,1,0);}catch(const std::invalid_argument&){rejected=true;}
 require(rejected,"zero hypotheses were accepted");
 std::cout<<"GPU_TRACKER_ADMISSION_PASS saturation_bounded=1 committed_preserved=1 unsaturated_unchanged=1\n";
 return 0;
}catch(const std::exception& e){std::cerr<<"FAIL gpu-tracker-admission: "<<e.what()<<'\n';return 1;}
