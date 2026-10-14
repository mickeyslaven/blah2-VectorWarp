#pragma once
// Diagnostic-only monotonic host accounting. No device timing or DSP claims.
// Included inside the backend namespace; OS headers are included globally there.
namespace gpu_host {
#include "GpuSubmitContextProbe.private.h"
struct Entry {const char* label=nullptr;
uint32_t parent=UINT32_MAX,tag=0;
uint64_t begin=0,end=0;};
// Linux thread samples bracket only submission calls, with fixed storage. They
// include boundary sampling overhead; they do not measure driver helper threads.
struct ThreadSample {
 bool valid=false;
 uint64_t cpu=0,user=0,system=0,minorFaults=0,majorFaults=0,voluntary=0,involuntary=0;
 static constexpr bool supported(){
#if defined(__linux__) && defined(RUSAGE_THREAD)
  return true;
#else
  return false;
#endif
 }
 static ThreadSample take()noexcept{
  ThreadSample s;
#if defined(__linux__) && defined(RUSAGE_THREAD)
  timespec time{};
  rusage usage{};
  if(clock_gettime(CLOCK_THREAD_CPUTIME_ID,&time)!=0||getrusage(RUSAGE_THREAD,&usage)!=0||
     time.tv_sec<0||time.tv_nsec<0||time.tv_nsec>=1000000000||uint64_t(time.tv_sec)>UINT64_MAX/1000000000||
     usage.ru_utime.tv_sec<0||usage.ru_utime.tv_usec<0||usage.ru_utime.tv_usec>=1000000||
     usage.ru_stime.tv_sec<0||usage.ru_stime.tv_usec<0||usage.ru_stime.tv_usec>=1000000||
     uint64_t(usage.ru_utime.tv_sec)>UINT64_MAX/1000000||uint64_t(usage.ru_stime.tv_sec)>UINT64_MAX/1000000||
     usage.ru_minflt<0||usage.ru_majflt<0||usage.ru_nvcsw<0||usage.ru_nivcsw<0)return s;
  const uint64_t ns=uint64_t(time.tv_sec)*1000000000,us=uint64_t(usage.ru_utime.tv_sec)*1000000,ss=uint64_t(usage.ru_stime.tv_sec)*1000000;
  if(uint64_t(time.tv_nsec)>UINT64_MAX-ns||uint64_t(usage.ru_utime.tv_usec)>UINT64_MAX-us||uint64_t(usage.ru_stime.tv_usec)>UINT64_MAX-ss)return s;
  s={true,ns+uint64_t(time.tv_nsec),us+uint64_t(usage.ru_utime.tv_usec),ss+uint64_t(usage.ru_stime.tv_usec),
     uint64_t(usage.ru_minflt),uint64_t(usage.ru_majflt),uint64_t(usage.ru_nvcsw),uint64_t(usage.ru_nivcsw)};
#endif
  return s;
 }
};
struct SubmitThread {uint32_t interval=UINT32_MAX;
ThreadSample delta{};};
class Frame {
 static uint64_t now(){return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());}
 std::array<Entry,2048> entries_{};
 std::array<SubmitThread,128> submissions_{};
 uint32_t submissionCount_=0;
 std::unique_ptr<SubmitContextProbe> contextProbe_;
 std::array<SubmitContextRecord,1> contextRecords_{};
 uint32_t contextCount_=0;
 uint32_t count_=0,current_=UINT32_MAX;
 uint64_t generation_=0,begin_=0,end_=0;
 bool active_=false,fault_=false,complete_=false;
 void copyData(const Frame& other){
  if(other.active_)throw std::logic_error("Cannot snapshot an active host timing frame");
  entries_=other.entries_;
  submissions_=other.submissions_;
  submissionCount_=other.submissionCount_;
  contextRecords_=other.contextRecords_;
  contextCount_=other.contextCount_;
  count_=other.count_;
  current_=other.current_;
  generation_=other.generation_;
  begin_=other.begin_;
  end_=other.end_;
  active_=other.active_;
  fault_=other.fault_;
  complete_=other.complete_;
 }
public:
 Frame()=default;
 // Deferred fixture ledgers are data snapshots; they never copy/own live FDs.
 Frame(const Frame& other){copyData(other);}
 Frame& operator=(const Frame& other){if(this!=&other){if(contextProbe_)throw std::logic_error("Cannot overwrite configured host timing collector");
 copyData(other);}return *this;}
 void configureSubmitContextProbe(const char* exactCgroupPath){
  if(active_||contextProbe_)throw std::logic_error("Submit context probe must be configured once while idle");
  contextProbe_=std::make_unique<SubmitContextProbe>(exactCgroupPath);
 }
 SubmitContextProbe* capturedSubmitProbe()const noexcept{
  return active_&&current_<count_&&std::strcmp(entries_[current_].label,"captured.submit")==0?contextProbe_.get():nullptr;
 }
 void submitContext(const SubmitContextRecord& record)noexcept{
  if(contextCount_==contextRecords_.size()||record.interval>=count_||std::strcmp(entries_[record.interval].label,"vkQueueSubmit")!=0||
     record.before.endNs<record.before.beginNs||record.after.endNs<record.after.beginNs||record.after.beginNs<record.before.endNs){fault_=true;
     return;}
  contextRecords_[contextCount_++]=record;
 }
 void begin(uint64_t generation){contextCount_=0;
 submissionCount_=0;
 count_=0;
 current_=UINT32_MAX;
 generation_=generation;
 begin_=now();
 end_=0;
 active_=true;
 fault_=false;
 complete_=false;}
 uint32_t enter(const char* label,uint32_t tag){
  if(!active_||count_==entries_.size()){fault_=true;
  return UINT32_MAX;}
  const uint32_t i=count_++;
  entries_[i]={label,current_,tag,now(),0};
  current_=i;
  return i;
 }
 void leave(uint32_t i)noexcept{
  if(i==UINT32_MAX)return;
  if(i>=count_||current_!=i){fault_=true;
  return;}
  entries_[i].end=now();
  current_=entries_[i].parent;
 }
 void submission(uint32_t interval,const ThreadSample& before,const ThreadSample& after)noexcept{
  if(submissionCount_==submissions_.size()||interval>=count_||std::strcmp(entries_[interval].label,"vkQueueSubmit")!=0){fault_=true;
  return;}
  auto& value=submissions_[submissionCount_++];
  value={interval,{}};
  if(!ThreadSample::supported())return;
  if(!before.valid||!after.valid||after.cpu<before.cpu||after.user<before.user||after.system<before.system||
     after.minorFaults<before.minorFaults||after.majorFaults<before.majorFaults||after.voluntary<before.voluntary||after.involuntary<before.involuntary){fault_=true;
     return;}
  value.delta={true,after.cpu-before.cpu,after.user-before.user,after.system-before.system,after.minorFaults-before.minorFaults,
   after.majorFaults-before.majorFaults,after.voluntary-before.voluntary,after.involuntary-before.involuntary};
 }
 void finish(){
  end_=now();
  active_=false;
  if(fault_||current_!=UINT32_MAX)throw std::runtime_error("Host timing accounting overflow or incomplete scope");
  for(uint32_t i=0;i<count_;++i){const auto& e=entries_[i];
   if(!e.label||e.begin<begin_||e.end<e.begin||e.end>end_||
      (e.parent!=UINT32_MAX&&(e.parent>=i||e.begin<entries_[e.parent].begin||e.end>entries_[e.parent].end)))
    throw std::runtime_error("Host timing accounting invalid interval");
  }
  for(uint32_t i=0;i<contextCount_;++i){const auto& r=contextRecords_[i];
  const auto& api=entries_[r.interval];
   if(api.parent>=count_||std::strcmp(entries_[api.parent].label,"captured.submit")!=0||
      r.before.beginNs<entries_[api.parent].begin||r.before.endNs>api.begin||
      r.after.beginNs<api.end||r.after.endNs>entries_[api.parent].end)
    throw std::runtime_error("Submit context sample lies outside its captured parent or API boundaries");
  }
  complete_=true;
 }
 void abandon()noexcept{active_=false;
 complete_=false;}
 template<class Sink>void exportData(Sink&& sink)const{
  if(!complete_||active_)throw std::logic_error("Cannot export incomplete host ledger");
  sink(generation_,begin_,end_,entries_,count_,submissions_,submissionCount_,contextRecords_,contextCount_,bool(contextProbe_));
 }
 void write(std::ostream& stream,uint32_t call)const{
  if(!complete_)throw std::logic_error("Host timing frame did not complete");
  uint64_t covered=0,last=begin_;
  for(uint32_t i=0;i<count_;++i){const auto& e=entries_[i];
  if(e.parent==UINT32_MAX){
   if(e.begin<last||e.end-e.begin>end_-begin_-covered)throw std::runtime_error("Host timing top-level overlap");
   covered+=e.end-e.begin;
   last=e.end;
  }}
  stream<<"GPU_HOST_FRAME call="<<call<<" frame="<<generation_<<" entries="<<count_<<" capacity="<<entries_.size()
   <<" envelope_ns="<<end_-begin_<<" top_level_ns="<<covered<<" unaccounted_ns="<<(end_-begin_-covered)<<" calibrated_gpu=0\n";
  for(uint32_t i=0;i<count_;++i){const auto& e=entries_[i];
  stream<<"GPU_HOST_INTERVAL call="<<call<<" frame="<<generation_<<" id="<<i
   <<" parent="<<e.parent<<" label="<<e.label<<" tag="<<e.tag<<" begin_ns="<<e.begin-begin_<<" end_ns="<<e.end-begin_<<" elapsed_ns="<<e.end-e.begin<<'\n';}
  for(uint32_t i=0;i<submissionCount_;++i){const auto& value=submissions_[i];
  const auto& delta=value.delta;
  stream
   <<"GPU_HOST_SUBMIT_THREAD call="<<call<<" frame="<<generation_<<" interval="<<value.interval
   <<" supported="<<uint32_t(ThreadSample::supported())<<" valid="<<uint32_t(delta.valid)<<" capacity="<<submissions_.size()<<" storage_bytes="<<sizeof(submissions_)
   <<" thread_cpu_ns="<<delta.cpu<<" thread_user_us="<<delta.user<<" thread_system_us="<<delta.system
   <<" minor_faults="<<delta.minorFaults<<" major_faults="<<delta.majorFaults
   <<" voluntary_switches="<<delta.voluntary<<" involuntary_switches="<<delta.involuntary
   <<" sample_boundary_overhead_included=1 driver_helper_threads_included=0\n";}
  for(uint32_t i=0;i<contextCount_;++i)SubmitContextProbe::write(stream,call,generation_,contextRecords_[i]);

 }
};
class Scope {
 Frame* frame_;
 uint32_t index_;
public:
 Scope(Frame* frame,const char* label,uint32_t tag=0):frame_(frame),index_(frame?frame->enter(label,tag):UINT32_MAX){}
 Scope(const Scope&)=delete;
 Scope& operator=(const Scope&)=delete;
 ~Scope(){if(frame_)frame_->leave(index_);}
 uint32_t index()const noexcept{return index_;}
};
class SubmitProbe {
 Frame* frame_;
 ThreadSample before_{};
 uint32_t interval_=UINT32_MAX;
public:
 SubmitProbe(Frame* frame,const char* label):frame_(frame&&std::strcmp(label,"vkQueueSubmit")==0?frame:nullptr){if(frame_)before_=ThreadSample::take();}
 SubmitProbe(const SubmitProbe&)=delete;
 SubmitProbe& operator=(const SubmitProbe&)=delete;
 void interval(uint32_t value)noexcept{interval_=value;}
 ~SubmitProbe(){if(frame_)frame_->submission(interval_,before_,ThreadSample::take());}
};
class SubmitContextScope {
 Frame* frame_=nullptr;
 SubmitContextProbe* probe_=nullptr;
 SubmitContextRecord record_{};
public:
 SubmitContextScope(Frame* frame,const char* label){
  if(frame&&std::strcmp(label,"vkQueueSubmit")==0){probe_=frame->capturedSubmitProbe();
  if(probe_){frame_=frame;
  record_=probe_->start();}}
 }
 SubmitContextScope(const SubmitContextScope&)=delete;
 SubmitContextScope& operator=(const SubmitContextScope&)=delete;
 void interval(uint32_t value)noexcept{record_.interval=value;}
 ~SubmitContextScope(){if(probe_){record_.after=probe_->take();
 frame_->submitContext(record_);}}
};
// Declaration order matters: sampling starts before the original Scope's wall
// interval and ends after it closes. The enclosing phase/whole timers retain
// all sampling overhead. No per-call allocation or output occurs here.
template<class F> decltype(auto) call(Frame* frame,const char* label,uint32_t tag,F&& fn){SubmitContextScope contextProbe(frame,label);
SubmitProbe probe(frame,label);
Scope scope(frame,label,tag);
probe.interval(scope.index());
contextProbe.interval(scope.index());
return fn();}
} // namespace gpu_host
