#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>
namespace blah2::mixed {
enum class Choice { cpu, shadow, mixed };
class AutoPolicy {
  unsigned shadowPasses_=0,deferrals_=0,trialIndex_=0;
  uint32_t lastBacklog_=0;
  bool sawBacklog_=false;
  bool selected_=false,disabled_=false;
  unsigned slowWindows_=0;
  std::vector<double> cpu_,mixed_,sustained_;
  std::string reason_="Checking complete mixed-DSP map and CPI cost";
  static double median(std::vector<double> values){
    std::sort(values.begin(),values.end());return values[values.size()/2];
  }
public:
  Choice choose(uint32_t backlog,uint32_t capacity,uint64_t drops){
    if(disabled_)return Choice::cpu;
    if(drops){disable("Capture dropped samples during mixed qualification");return Choice::cpu;}
    const bool growing=sawBacklog_ &&
      uint64_t(backlog)>uint64_t(lastBacklog_)+capacity/4;
    lastBacklog_=backlog;sawBacklog_=true;
    const bool headroom=capacity>0 && uint64_t(backlog)*2<capacity && !growing;
    if(shadowPasses_<2){
      // An oracle executes both full pipelines on one frame. With the two-CPI
      // live queue, the ordinary half-capacity watermark allowed a second
      // oracle while ~0.75 CPI was still queued, dropping a complete CPI.
      // Wait on CPU until almost empty; keep this bounded without increasing
      // queue capacity or changing the acquisition workload.
      const bool oracleHeadroom=headroom && uint64_t(backlog)*16<capacity;
      if(!oracleHeadroom){if(++deferrals_>=16)disable("Insufficient capture headroom for mixed map oracle");return Choice::cpu;}
      deferrals_=0;
      return Choice::shadow;
    }
    if(trialIndex_<6){
      if(!headroom){disable("Capture backlog grew during mixed speed qualification");return Choice::cpu;}
      return trialIndex_%2==0?Choice::cpu:Choice::mixed;
    }
    if(selected_&&!headroom){
      disable("Capture backlog grew during mixed processing");return Choice::cpu;
    }
    return selected_?Choice::mixed:Choice::cpu;
  }
  void shadow(double rms,double peak){
    if(disabled_)return;
    if(!std::isfinite(rms)||!std::isfinite(peak)||rms>1e-4||peak>1e-4){
      disable("Mixed full complex-map accuracy check failed");return;
    }
    ++shadowPasses_;
  }
  void complete(Choice choice,double cpiMs){
    if(disabled_||shadowPasses_<2)return;
    if(!std::isfinite(cpiMs)||cpiMs<=0)throw std::invalid_argument("Invalid complete CPI time");
    if(trialIndex_>=6){
      if(selected_&&choice==Choice::mixed){
        sustained_.push_back(cpiMs);
        if(sustained_.size()==5){
          slowWindows_=median(sustained_)>=median(cpu_)*.95 ? slowWindows_+1 : 0;
          sustained_.clear();
          if(slowWindows_>=2)
            disable("CPU selected after mixed processing lost its complete-CPI speed margin");
        }
      }
      return;
    }
    const Choice expected=trialIndex_%2==0?Choice::cpu:Choice::mixed;
    if(choice!=expected)throw std::logic_error("Mixed speed trial order changed");
    (choice==Choice::cpu?cpu_:mixed_).push_back(cpiMs);
    if(++trialIndex_==6){
      const double cpuMedian=median(cpu_),mixedMedian=median(mixed_);
      selected_=mixedMedian<cpuMedian*.95;
      reason_=selected_?"Mixed V3D selected by complete CPI time":
        "CPU selected by complete CPI time";
    }
  }
  void disable(std::string reason){disabled_=true;selected_=false;reason_=std::move(reason);}
  bool disabled()const{return disabled_;}
  bool selected()const{return selected_&&trialIndex_==6&&!disabled_;}
  unsigned shadowPasses()const{return shadowPasses_;}
  unsigned trials()const{return trialIndex_;}
  double cpuMs()const{return cpu_.empty()?0:median(cpu_);}
  double mixedMs()const{return mixed_.empty()?0:median(mixed_);}
  const std::string& reason()const{return reason_;}
};
}
