#include "process/tracker/Tracker.h"
#include <iostream>
#include <stdexcept>
int main()try{
 for(bool adaptive:{false,true}){
  for(unsigned axis=0;axis<2;++axis){Tracker first(4,6,3,.5,0,1,1,5,adaptive,{},false);
   Detection seed(4,0,8);first.process(&seed,1000);
   // The shared zero-span adaptive and fixed gates both use one strict bin.
   Detection boundary(axis==0?5:4,axis==1?2:0,1);first.process(&boundary,1500);
   if(first.frame_diagnostics().associations)throw std::runtime_error("first-association boundary widened");}
  Tracker tracker(4,6,3,.5,0,1,1,5,adaptive,{},false);
  for(unsigned i=0;i<4;++i){Detection d(4,0,8);auto t=tracker.process(&d,1000+500*i);
   if(t->get_n()!=1||(i==3&&t->get_state(0)!="ACTIVE"))throw std::runtime_error("zero acceleration confirmation");}
  Detection delayBoundary(5,0,1);tracker.process(&delayBoundary,3000);
  if(tracker.frame_diagnostics().associations)throw std::runtime_error("delay boundary widened");
  Detection dopplerBoundary(4,2,1);tracker.process(&dopplerBoundary,3500);
  if(tracker.frame_diagnostics().associations)throw std::runtime_error("Doppler boundary widened");
 }
 std::cout<<"PASS CPU zero-acceleration repeated-hit confirmation and strict boundary negatives\n";return 0;
}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}
