#pragma once

// Opt-in diagnostic only. Fixed storage is allocated before acquisition;
// records are formatted/written only when their owning stage is destroyed.
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <time.h>
#if defined(__linux__) || defined(__APPLE__)
#include <sys/resource.h>
#include <unistd.h>
#endif
#if defined(__linux__)
#include <sched.h>
#endif

namespace blah2::stream_profile {
using Clock=std::chrono::steady_clock;
inline double ms(Clock::time_point end,Clock::time_point begin) noexcept
{return std::chrono::duration<double,std::milli>(end-begin).count();}
inline uint64_t micros(Clock::time_point value) noexcept
{return uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(value.time_since_epoch()).count());}
inline long long processId() noexcept {
#if defined(__linux__) || defined(__APPLE__)
  return static_cast<long long>(getpid());
#else
  return 0;
#endif
}
inline unsigned setting(const char* name,unsigned fallback,unsigned maximum) noexcept {
  const auto* value=std::getenv(name);if(!value)return fallback;
  if(!*value)return 0;
  unsigned result=0;
  for(;*value;++value){if(*value<'0'||*value>'9')return 0;
    result=std::min(maximum,result*10+unsigned(*value-'0'));}
  return result;
}
inline unsigned frameLimit() noexcept
{return setting("VECTORWARP_STREAM_PROFILE_FRAMES",0,2048);}

struct Activity {
  double cpuMs=0;
  int64_t minor=0,major=0,voluntary=0,involuntary=0;
  int cpu=-1;
  bool clockKnown=false,faultsKnown=false;
  static Activity capture(bool enabled,bool process=false) noexcept {
    Activity value;if(!enabled)return value;
#if defined(__linux__)
    rusage usage{};
    if(getrusage(process?RUSAGE_SELF:RUSAGE_THREAD,&usage)==0){
      value.cpuMs=1000.*(usage.ru_utime.tv_sec+usage.ru_stime.tv_sec)+
        .001*(usage.ru_utime.tv_usec+usage.ru_stime.tv_usec);
      value.minor=usage.ru_minflt;value.major=usage.ru_majflt;
      value.voluntary=usage.ru_nvcsw;value.involuntary=usage.ru_nivcsw;
      value.clockKnown=value.faultsKnown=true;
    }
    value.cpu=sched_getcpu();
#else
#if defined(CLOCK_THREAD_CPUTIME_ID)
    timespec time{};
    if(clock_gettime(process?CLOCK_PROCESS_CPUTIME_ID:CLOCK_THREAD_CPUTIME_ID,&time)==0){
      value.cpuMs=1000.*time.tv_sec+time.tv_nsec*1e-6;value.clockKnown=true;
    }
#else
    (void)process;
#endif
#endif
    return value;
  }
};
// Build each bounded record before its single stderr submission. Workers share
// the parent's journal stream: emitting one field at a time can mix their lines.
class Line {
  std::array<char,4096> bytes_{};
  size_t used_=0;
public:
  template<class... Args> void append(const char* format,Args... args) noexcept {
    const int count=std::snprintf(bytes_.data()+used_,bytes_.size()-used_,format,args...);
    if(count>0)used_+=std::min(size_t(count),bytes_.size()-used_-1);
  }
  ~Line(){bytes_[used_++]='\n';std::fwrite(bytes_.data(),1,used_,stderr);}
};
inline void activity(Line& line,const Activity& begin,const Activity& end,const char* prefix) noexcept {
  line.append(" %s_cpu_known=%u %s_faults_known=%u %s_cpu_ms=%.6f %s_minor=%lld %s_major=%lld %s_nvcsw=%lld %s_nivcsw=%lld %s_cpu_begin=%d %s_cpu_end=%d %s_endpoint_migration=%u",
    prefix,unsigned(begin.clockKnown&&end.clockKnown),prefix,unsigned(begin.faultsKnown&&end.faultsKnown),
    prefix,end.cpuMs-begin.cpuMs,prefix,static_cast<long long>(end.minor-begin.minor),
    prefix,static_cast<long long>(end.major-begin.major),prefix,static_cast<long long>(end.voluntary-begin.voluntary),
    prefix,static_cast<long long>(end.involuntary-begin.involuntary),prefix,begin.cpu,prefix,end.cpu,
    prefix,unsigned(begin.cpu>=0&&end.cpu>=0&&begin.cpu!=end.cpu));
}

// Half-ms upper-bound quantiles, exact maximum and repeated tail counts.
// Overflow quantiles report the exact maximum, never a falsely small bound.
struct Histogram {
  std::array<uint64_t,2049> bins{};
  uint64_t count=0,over1=0,over10=0,over100=0,over500=0;
  double total=0,maximum=0;
  void observe(double value) noexcept {
    value=std::max(0.,value);++count;total+=value;maximum=std::max(maximum,value);
    ++bins[std::min<size_t>(2048,size_t(std::ceil(value*2)))];
    over1+=value>1;over10+=value>10;over100+=value>100;over500+=value>500;
  }
  double percentile(double q) const noexcept {
    if(!count)return 0;
    const uint64_t rank=uint64_t(std::ceil(q*count));uint64_t seen=0;
    for(size_t i=0;i<bins.size();++i){seen+=bins[i];if(seen>=rank)return i==2048?maximum:i*.5;}
    return maximum;
  }
  void dump(const char* role,const char* phase)const noexcept {
    std::fprintf(stderr,"STREAM_PROFILE_HIST pid=%lld role=%s phase=%s samples=%llu mean_ms=%.6f p50_upper_ms=%.6f p95_upper_ms=%.6f p99_upper_ms=%.6f max_ms=%.6f over1=%llu over10=%llu over100=%llu over500=%llu bin_ms=0.5\n",
      processId(),role,phase,static_cast<unsigned long long>(count),count?total/count:0,
      percentile(.5),percentile(.95),percentile(.99),maximum,
      static_cast<unsigned long long>(over1),static_cast<unsigned long long>(over10),
      static_cast<unsigned long long>(over100),static_cast<unsigned long long>(over500));
  }
};

struct ReceiveRecord {
  uint64_t when=0,accepted=0,count=0,queued=0,capacity=0;
  std::array<double,6> phases{}; // service gap, recv, wait, append A/B, commit
  Activity begin,end;
  bool sampled=false;
};
class Receiver {
  struct State {
    std::array<Histogram,7> hist;
    Histogram sampledCpu,sampledOffCpu,probe;
    std::unique_ptr<ReceiveRecord[]> tails;
    ReceiveRecord maximum{};
    unsigned limit=0,size=0,stride=64;
    uint64_t calls=0,sampled=0;
    explicit State(unsigned count):tails(new ReceiveRecord[count]),limit(count){}
  };
  std::unique_ptr<State> state_;
public:
  Receiver(){const auto limit=frameLimit();if(limit){state_=std::make_unique<State>(limit);
    state_->stride=std::max(1u,setting("VECTORWARP_STREAM_PROFILE_CPU_STRIDE",64,65536));}}
  bool enabled()const noexcept{return bool(state_);}
  bool sample()const noexcept{return state_&&state_->calls%state_->stride==0;}
  void record(ReceiveRecord value,double probeMs)noexcept {
    if(!state_)return;
    auto& s=*state_;++s.calls;
    for(size_t i=0;i<value.phases.size();++i)s.hist[i].observe(value.phases[i]);
    const auto write=value.phases[3]+value.phases[4]+value.phases[5];s.hist[6].observe(write);
    if(write>s.maximum.phases[3]+s.maximum.phases[4]+s.maximum.phases[5])s.maximum=value;
    if(value.sampled){++s.sampled;s.probe.observe(probeMs);
      if(value.begin.clockKnown&&value.end.clockKnown){
        const double cpu=value.end.cpuMs-value.begin.cpuMs;s.sampledCpu.observe(cpu);
        s.sampledOffCpu.observe(std::max(0.,write-cpu));}}
    if(write>1&&s.size<s.limit)s.tails[s.size++]=value;
  }
  ~Receiver(){if(!state_)return;const auto& s=*state_;
    const char* names[]={"service_gap","receive","lock_wait","append_a","append_b","commit","queue_write"};
    for(size_t i=0;i<s.hist.size();++i)s.hist[i].dump("receive",names[i]);
    s.sampledCpu.dump("receive","sampled_thread_cpu");s.sampledOffCpu.dump("receive","sampled_offcpu");
    s.probe.dump("receive","sampled_probe_overhead");
    std::fprintf(stderr,"STREAM_PROFILE_RECEIVE_META pid=%lld calls=%llu sampled=%llu cpu_stride=%u tails_stored=%u tail_limit=%u\n",
      processId(),static_cast<unsigned long long>(s.calls),static_cast<unsigned long long>(s.sampled),s.stride,s.size,s.limit);
    const auto dump=[](const ReceiveRecord& r,const char* kind){
      Line line;
      line.append("STREAM_PROFILE_RECEIVE pid=%lld kind=%s mono_us=%llu accepted_before=%llu incoming=%llu producer_queued=%llu producer_capacity=%llu sampled=%u",
        processId(),kind,static_cast<unsigned long long>(r.when),static_cast<unsigned long long>(r.accepted),
        static_cast<unsigned long long>(r.count),static_cast<unsigned long long>(r.queued),
        static_cast<unsigned long long>(r.capacity),unsigned(r.sampled));
      const char* fields[]={"service_gap_ms","receive_ms","lock_wait_ms","append_a_ms","append_b_ms","commit_ms"};
      for(size_t i=0;i<r.phases.size();++i)line.append(" %s=%.6f",fields[i],r.phases[i]);
      activity(line,r.begin,r.end,"thread");
    };
    for(unsigned i=0;i<s.size;++i)dump(s.tails[i],"tail");
    dump(s.maximum,"maximum");
  }
};

struct CycleRecord {
  uint64_t sequence=0,epoch=0,first=0,end=0,publishedFirst=0;
  uint64_t queued=0,pending=0,capacity=0,rate=0;
  // start, locked, unlocked, capture installed, spectrum start/end, staging end,
  // pipeline start, radar start/end, serialization end, output end, finish end,
  // publication end, next iteration (after local-owner destruction).
  std::array<Clock::time_point,15> time{};
  Activity threadBegin,threadEnd,processBegin,processEnd;
};
class Consumer {
  unsigned limit_=frameLimit(),size_=0;
  std::unique_ptr<CycleRecord[]> records_;
  bool pending_=false;
public:
  Consumer(){if(limit_)records_=std::make_unique<CycleRecord[]>(limit_);}
  bool enabled()const noexcept{return size_<limit_;}
  void record(CycleRecord value)noexcept {if(enabled()){records_[size_]=value;pending_=true;}}
  void complete(Clock::time_point end)noexcept {
    if(!pending_)return;
    auto& value=records_[size_];value.time[14]=end;
    value.threadEnd=Activity::capture(true);value.processEnd=Activity::capture(true,true);
    pending_=false;++size_;
  }
  ~Consumer(){complete(Clock::now());
    const char* phases[]={"queue_wait_ms","queue_locked_ms","capture_install_ms","pre_spectrum_ms",
      "spectrum_ms","capture_staging_ms","pre_pipeline_ms","pre_radar_ms","radar_ms",
      "serialize_ms","output_ms","finish_next_ms","publication_ms","destruction_ms"};
    for(unsigned n=0;n<size_;++n){const auto& r=records_[n];
      Line line;
      line.append("STREAM_PROFILE_CYCLE pid=%lld sequence=%llu epoch=%llu first=%llu end=%llu published_first=%llu start_us=%llu retired_us=%llu producer_queued=%llu pending_queued=%llu producer_capacity=%llu sample_rate=%llu",
        processId(),static_cast<unsigned long long>(r.sequence),static_cast<unsigned long long>(r.epoch),
        static_cast<unsigned long long>(r.first),static_cast<unsigned long long>(r.end),
        static_cast<unsigned long long>(r.publishedFirst),static_cast<unsigned long long>(micros(r.time[0])),
        static_cast<unsigned long long>(micros(r.time[14])),static_cast<unsigned long long>(r.queued),
        static_cast<unsigned long long>(r.pending),static_cast<unsigned long long>(r.capacity),
        static_cast<unsigned long long>(r.rate));
      for(size_t i=0;i<14;++i)line.append(" %s=%.6f",phases[i],ms(r.time[i+1],r.time[i]));
      line.append(" processing_ms=%.6f pipeline_ms=%.6f completion_ms=%.6f idle_ms=%.6f",
        ms(r.time[14],r.time[0]),ms(r.time[12],r.time[7]),
        n?ms(r.time[14],records_[n-1].time[14]):0,
        n?ms(r.time[0],records_[n-1].time[14]):0);
      activity(line,r.threadBegin,r.threadEnd,"thread");activity(line,r.processBegin,r.processEnd,"process");
    }
  }
};

struct WorkRecord {
  uint64_t sequence=0,first=0,identity=0;
  long long peer=0;
  int outcome=0; // 0 legacy/unknown, 1 completed, -1 exception before completion.
  std::array<Clock::time_point,4> time{};
  Activity begin,end;
  bool processAccounting=false;
  Activity processBegin,processEnd;
  std::array<double,3> stages{};
};
class Worker {
  const char* role_;
  unsigned limit_,size_=0;
  std::unique_ptr<WorkRecord[]> records_;
public:
  // Portable lanes perform at most two profiled transactions per CPI (clutter
  // and ambiguity). Keep the same observation horizon, still <=4096 records.
  explicit Worker(const char* role,unsigned recordsPerFrame=1)
    :role_(role),limit_(frameLimit()*std::min(recordsPerFrame,2u)) {
    if(limit_)records_=std::make_unique<WorkRecord[]>(limit_);
  }
  bool enabled()const noexcept{return size_<limit_;}
  void record(WorkRecord value)noexcept {
    if(!enabled())return;
    value.end=Activity::capture(true);
    if(value.processAccounting)value.processEnd=Activity::capture(true,true);
    records_[size_++]=value;
  }
  ~Worker(){for(unsigned i=0;i<size_;++i){const auto& r=records_[i];
    Line line;
    line.append("STREAM_PROFILE_WORK pid=%lld role=%s sequence=%llu first=%llu identity=%llu start_us=%llu end_us=%llu phase0_ms=%.6f phase1_ms=%.6f phase2_ms=%.6f wall_ms=%.6f",
      processId(),role_,static_cast<unsigned long long>(r.sequence),static_cast<unsigned long long>(r.first),
      static_cast<unsigned long long>(r.identity),static_cast<unsigned long long>(micros(r.time[0])),
      static_cast<unsigned long long>(micros(r.time[3])),ms(r.time[1],r.time[0]),
      ms(r.time[2],r.time[1]),ms(r.time[3],r.time[2]),ms(r.time[3],r.time[0]));
    line.append(" peer_pid=%lld outcome=%d phase1_us=%llu phase2_us=%llu",r.peer,r.outcome,
      static_cast<unsigned long long>(micros(r.time[1])),
      static_cast<unsigned long long>(micros(r.time[2])));
    line.append(" clutter_ms=%.6f ambiguity_ms=%.6f products_ms=%.6f",
      r.stages[0],r.stages[1],r.stages[2]);
    activity(line,r.begin,r.end,"thread");
    if(r.processAccounting)activity(line,r.processBegin,r.processEnd,"process");
  }}
};
// Memory-only, exception-safe transaction probes. Disabled calls take no clock
// samples and allocate nothing; an exception preserves its unfinished interval.
class WorkScope {
  Worker& worker_;
  bool enabled_;
  unsigned marked_=0;
public:
  WorkRecord record;
  WorkScope(Worker& worker,uint64_t sequence,uint64_t identity,long long peer=0,bool process=false)
    :worker_(worker),enabled_(worker.enabled()) {
    if(enabled_){record.sequence=sequence;record.identity=identity;record.peer=peer;
      record.outcome=-1;record.begin=Activity::capture(true);record.time.fill(Clock::now());
      record.processAccounting=process;record.processBegin=Activity::capture(process,true);}
  }
  void mark(unsigned phase)noexcept {
    if(!enabled_)return;
    const auto now=Clock::now();
    while(marked_<std::min(phase,3u))record.time[++marked_]=now;
  }
  void completed()noexcept {record.outcome=1;}
  ~WorkScope(){if(enabled_){mark(3);worker_.record(record);}}
};
class Duration {
  bool enabled_;double& total_;Clock::time_point start_;
public:
  Duration(bool enabled,double& total):enabled_(enabled),total_(total),start_(enabled?Clock::now():Clock::time_point{}){}
  ~Duration(){if(enabled_)total_+=ms(Clock::now(),start_);}
};
} // namespace blah2::stream_profile
