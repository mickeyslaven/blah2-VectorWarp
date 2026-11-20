#include "process/ambiguity/GpuClutterBatch.h"
#include <iostream>
#include <new>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using blah2::ClutterSubmission;
void require(bool value,const char* why){if(!value)throw std::runtime_error(why);}
template<class Error,class Function> void rejects(Function function,const char* why){
  try{function();}catch(const Error&){return;}
  throw std::runtime_error(why);
}
struct Fence {
  bool pending=false,signaled=false,failSubmit=false,failWait=false,failReset=false;
  std::vector<std::string> events;
  void enqueue(){
    require(!pending&&!signaled,"Fence reused before wait/reset");
    events.push_back("enqueue");pending=true;
    // Deliberately leave uncertain queued work even on a submission error.
    if(failSubmit)throw std::runtime_error("submit failure");
  }
  void wait(){
    require(pending&&!signaled,"Wait without pending work");events.push_back("wait");
    if(failWait)throw std::runtime_error("wait failure");
    pending=false;signaled=true;
  }
  void reset(){
    require(!pending&&signaled,"Reset while work is pending");events.push_back("reset");
    if(failReset)throw std::runtime_error("reset failure");
    signaled=false;
  }
  void teardownDrain(){events.push_back("drain");pending=false;}
};
void complete(ClutterSubmission& submission,Fence& fence){
  submission.complete([&]{fence.wait();},[&]{fence.reset();});
}
template<class Host> void frame(ClutterSubmission& submission,Fence& fence,Host&& host){
  submission.enqueue([&]{fence.enqueue();});
  submission.overlap([&]{
    require(fence.pending,"Host preparation did not overlap outstanding GPU work");
    host();
  },[&]{complete(submission,fence);});
}
void successfulFrames(){
  ClutterSubmission submission;Fence fence;
  for(unsigned iteration=0;iteration<5;++iteration){
    frame(submission,fence,[&]{
      fence.events.push_back("host");
      rejects<std::logic_error>([&]{submission.ensureIdle();},"Pending shared fence looked idle");
    });
    submission.ensureIdle();
    require(!fence.pending&&!fence.signaled&&!submission.needsDrain(),"Completed frame retained fence state");
  }
  for(size_t i=0;i<fence.events.size();i+=4)
    require(fence.events[i]=="enqueue"&&fence.events[i+1]=="host"&&
      fence.events[i+2]=="wait"&&fence.events[i+3]=="reset","Overlap order differs");
}
void hostExceptionRecovery(){
  ClutterSubmission submission;Fence fence;
  for(unsigned failingStage=0;failingStage<3;++failingStage){
    bool original=false;
    try{
      frame(submission,fence,[&]{
        for(unsigned stage=0;stage<3;++stage){
          fence.events.push_back("host stage");
          if(stage==failingStage)throw std::domain_error("reference precision rejection");
        }
      });
    }catch(const std::domain_error& error){original=std::string(error.what())=="reference precision rejection";}
    require(original,"Host numerical exception was replaced or lost");
    require(fence.events[fence.events.size()-2]=="wait"&&fence.events.back()=="reset",
      "Host failure escaped before draining/resetting the fence");
    submission.ensureIdle();
    frame(submission,fence,[]{}); // Recoverable rejection must permit next frame.
  }
  rejects<std::bad_alloc>([&]{frame(submission,fence,[]{throw std::bad_alloc();});},
    "Allocation exception was lost");
  submission.ensureIdle();
  bool original=false;
  try{frame(submission,fence,[]{throw 37;});}catch(int value){original=value==37;}
  require(original,"Non-standard host exception was lost");submission.ensureIdle();
  frame(submission,fence,[]{});
}
void deviceFaults(){
  for(unsigned fault=0;fault<3;++fault){
    ClutterSubmission submission;Fence fence;
    fence.failSubmit=fault==0;fence.failWait=fault==1;fence.failReset=fault==2;
    rejects<std::runtime_error>([&]{frame(submission,fence,[&]{fence.events.push_back("host");});},
      "Device fault was hidden");
    require(submission.needsDrain(),"Faulted queue lost its teardown-drain requirement");
    const size_t count=fence.events.size();
    rejects<std::logic_error>([&]{submission.ensureIdle();},"Poisoned shared fence looked reusable");
    rejects<std::logic_error>([&]{submission.enqueue([&]{fence.enqueue();});},"Poisoned queue reused");
    rejects<std::logic_error>([&]{complete(submission,fence);},"Poisoned completion retried");
    require(fence.events.size()==count,"Poisoned state invoked driver operations");
    if(fault==0)require(count==1,"Host work started after failed enqueue");
    if(fault==1)require(fence.events.back()=="wait","Failed wait reset a pending fence");
    if(submission.needsDrain())fence.teardownDrain();
    require(!fence.pending&&fence.events.back()=="drain","Uncertain work was not drained before teardown");
  }
  for(bool failWait:{false,true}){
    ClutterSubmission submission;Fence fence;
    fence.failWait=failWait;fence.failReset=!failWait;
    bool deviceWon=false;
    try{frame(submission,fence,[]{throw std::domain_error("host precision failure");});}
    catch(const std::runtime_error& error){
      deviceWon=std::string(error.what())==(failWait?"wait failure":"reset failure");
    }
    require(deviceWon&&submission.needsDrain(),"Device fault became recoverable host rejection");
  }
}
void invalidSequencing(){
  ClutterSubmission submission;Fence fence;
  rejects<std::logic_error>([&]{complete(submission,fence);},"Completion without enqueue accepted");
  rejects<std::logic_error>([&]{submission.overlap([]{},[]{});},"Overlap without enqueue accepted");
  require(fence.events.empty(),"Invalid sequencing called the driver");
  submission.enqueue([&]{fence.enqueue();});
  rejects<std::logic_error>([&]{submission.enqueue([&]{fence.enqueue();});},"Duplicate enqueue accepted");
  complete(submission,fence);submission.ensureIdle();
  rejects<std::logic_error>([&]{complete(submission,fence);},"Fence reset repeated after completion");
  submission.enqueue([&]{fence.enqueue();});
  rejects<std::logic_error>([&]{submission.overlap([]{},[]{});},"Incomplete callback hid pending work");
  require(submission.needsDrain(),"Incomplete callback lost pending state");
  complete(submission,fence);submission.ensureIdle();
}
}
int main()try{
  successfulFrames();hostExceptionRecovery();deviceFaults();invalidSequencing();
  std::cout<<"PASS clutter overlap sequencing, host-exception fence recovery, device poisoning and teardown state\n";
  return 0;
}catch(const std::exception& error){std::cerr<<"FAIL clutter submission: "<<error.what()<<'\n';return 1;}
