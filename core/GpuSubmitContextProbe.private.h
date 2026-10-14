#pragma once
// Private qualification diagnostics. Included inside gpu_host, after global OS headers.
// No configuration/policy writes, path search, tracing, allocation or output in take().
struct SubmitContextSnapshot {
 uint64_t beginNs=0,endNs=0;
 std::array<uint64_t,3> scheduler{};
 std::array<uint64_t,2> process{};
 std::array<uint64_t,6> cgroup{};
 int schedulerError=0,processError=0,cgroupError=0;
};
struct SubmitContextRecord {
 uint32_t interval=UINT32_MAX;
 int schedulerEnabled=-1,schedulerFlagError=0;
 bool supported=false;
 SubmitContextSnapshot before{},after{};
};
class SubmitContextProbe {
 int schedulerFd_=-1,cgroupFd_=-1,schedulerOpenError_=0;
 int schedulerEnabled_=-1,schedulerFlagError_=0;
 uint64_t tid_=0;
 static uint64_t now()noexcept{
  return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
 }
 static bool space(char c)noexcept{return c==' '||c=='\t'||c=='\r'||c=='\n';}
 static bool number(const char*& p,const char* end,uint64_t& value)noexcept{
  while(p<end&&space(*p))++p;
  if(p==end||*p<'0'||*p>'9')return false;
  value=0;
  do{const uint64_t digit=uint64_t(*p-'0');
  if(value>(UINT64_MAX-digit)/10)return false;
  value=value*10+digit;
  ++p;}while(p<end&&*p>='0'&&*p<='9');
  return p==end||space(*p);
 }
 template<size_t N> static int sequence(const char* p,size_t bytes,std::array<uint64_t,N>& values)noexcept{
  const char* end=p+bytes;
  for(auto& value:values)if(!number(p,end,value))return EINVAL;
  while(p<end&&space(*p)){++p;}
  return p==end?0:EINVAL;
 }
 static int cpuStat(const char* p,size_t bytes,std::array<uint64_t,6>& values)noexcept{
  static constexpr const char* keys[]={"usage_usec","user_usec","system_usec","nr_periods","nr_throttled","throttled_usec"};
  const char* end=p+bytes;
  uint32_t seen=0;
  while(p<end){
   const char* line=p;
   while(p<end&&*p!='\n')++p;
   const char* lineEnd=p;
   if(p<end)++p;
   if(line==lineEnd)continue;
   const char* keyEnd=line;
   while(keyEnd<lineEnd&&!space(*keyEnd))++keyEnd;
   for(size_t i=0;i<6;++i)if(size_t(keyEnd-line)==std::strlen(keys[i])&&std::memcmp(line,keys[i],size_t(keyEnd-line))==0){
    if(seen&(1U<<i))return EINVAL;
    const char* value=keyEnd;
    if(!number(value,lineEnd,values[i]))return EINVAL;
    while(value<lineEnd&&space(*value)){++value;}
    if(value!=lineEnd){return EINVAL;}
    seen|=1U<<i;
   }
  }
  return seen==63?0:ENODATA;
 }
#if defined(__linux__)
 template<size_t N> static int readFixed(int fd,char (&buffer)[N],size_t& bytes)noexcept{
  bytes=0;
  if(fd<0)return EBADF;
  const ssize_t n=pread(fd,buffer,N,0);
  if(n<0)return errno?errno:EIO;
  if(size_t(n)==N){return EOVERFLOW;}
  if(n==0){return ENODATA;}
  bytes=size_t(n);
  return 0;
 }
 static int readPath(const char* path,char* buffer,size_t capacity,size_t& bytes)noexcept{
  bytes=0;
  const int fd=open(path,O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
  if(fd<0)return errno?errno:EIO;
  const ssize_t n=pread(fd,buffer,capacity,0);
  const int saved=errno;
  close(fd);
  if(n<0){return saved?saved:EIO;}
  if(size_t(n)==capacity){return EOVERFLOW;}
  if(n==0){return ENODATA;}
  bytes=size_t(n);
  return 0;
 }
#endif
 template<size_t N> static bool delta(const std::array<uint64_t,N>& before,const std::array<uint64_t,N>& after,std::array<uint64_t,N>& result)noexcept{
  for(size_t i=0;i<N;++i)if(after[i]<before[i])return false;
  for(size_t i=0;i<N;++i){result[i]=after[i]-before[i];}
  return true;
 }
public:
 static constexpr bool supported(){
#if defined(__linux__)
  return true;
#else
  return false;
#endif
 }
 explicit SubmitContextProbe(const char* exactCgroupPath){
#if defined(__linux__)
  const std::string path=exactCgroupPath?exactCgroupPath:"";
  const std::string prefix="/sys/fs/cgroup/system.slice/";
  if(path.compare(0,prefix.size(),prefix)!=0||path.size()<=prefix.size()+8||path.size()>512||
     path.find('/',prefix.size())!=std::string::npos||path.find("..")!=std::string::npos||path.substr(path.size()-8)!=".service")
   throw std::invalid_argument("Exact owned service cgroup path required for submit diagnostics");
  char membership[4096];
  size_t bytes=0;
  const int membershipError=readPath("/proc/self/cgroup",membership,sizeof(membership),bytes);
  const std::string expected="0::"+path.substr(std::strlen("/sys/fs/cgroup"))+"\n";
  if(membershipError||bytes!=expected.size()||std::memcmp(membership,expected.data(),bytes)!=0)
   throw std::runtime_error("Submit diagnostics cgroup membership differs");
  const long id=syscall(SYS_gettid);
  if(id<=0)throw std::runtime_error("Submit diagnostics thread identity unavailable");
  tid_=uint64_t(id);
  char schedulerPath[128];
  const int length=std::snprintf(schedulerPath,sizeof(schedulerPath),"/proc/self/task/%llu/schedstat",static_cast<unsigned long long>(tid_));
  if(length<=0||size_t(length)>=sizeof(schedulerPath))throw std::runtime_error("Submit diagnostics thread path overflow");
  const int directory=open(path.c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
  if(directory<0)throw std::runtime_error("Submit diagnostics owned cgroup unavailable");
  cgroupFd_=openat(directory,"cpu.stat",O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
  close(directory);
  if(cgroupFd_<0)throw std::runtime_error("Submit diagnostics owned cpu.stat unavailable");
  schedulerFd_=open(schedulerPath,O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
  if(schedulerFd_<0)schedulerOpenError_=errno?errno:EIO;
  char flag[32];
  bytes=0;
  schedulerFlagError_=readPath("/proc/sys/kernel/sched_schedstats",flag,sizeof(flag),bytes);
  if(!schedulerFlagError_){std::array<uint64_t,1> value{};
  schedulerFlagError_=sequence(flag,bytes,value);
   if(!schedulerFlagError_&&value[0]<=1)schedulerEnabled_=int(value[0]);
   else{schedulerFlagError_=EINVAL;
   schedulerEnabled_=-1;}}
#else
  (void)exactCgroupPath;
#endif
 }
 SubmitContextProbe(const SubmitContextProbe&)=delete;
 SubmitContextProbe& operator=(const SubmitContextProbe&)=delete;
 ~SubmitContextProbe(){
#if defined(__linux__)
  if(schedulerFd_>=0){close(schedulerFd_);}
  if(cgroupFd_>=0){close(cgroupFd_);}
#endif
 }
 SubmitContextSnapshot take()const noexcept{
  SubmitContextSnapshot s;
  s.beginNs=now();
#if defined(__linux__)
  if(uint64_t(syscall(SYS_gettid))!=tid_){s.schedulerError=s.processError=s.cgroupError=EINVAL;
  s.endNs=now();
  return s;}
  char buffer[4096];
  size_t bytes=0;
  s.schedulerError=schedulerEnabled_==1?schedulerOpenError_:(schedulerFlagError_?schedulerFlagError_:ENODATA);
  if(!s.schedulerError){s.schedulerError=readFixed(schedulerFd_,buffer,bytes);
  if(!s.schedulerError)s.schedulerError=sequence(buffer,bytes,s.scheduler);}
  rusage usage{};
  if(getrusage(RUSAGE_SELF,&usage)!=0)s.processError=errno?errno:EIO;
  else if(usage.ru_utime.tv_sec<0||usage.ru_utime.tv_usec<0||usage.ru_utime.tv_usec>=1000000||usage.ru_stime.tv_sec<0||usage.ru_stime.tv_usec<0||usage.ru_stime.tv_usec>=1000000||
          uint64_t(usage.ru_utime.tv_sec)>(UINT64_MAX-uint64_t(usage.ru_utime.tv_usec))/1000000||uint64_t(usage.ru_stime.tv_sec)>(UINT64_MAX-uint64_t(usage.ru_stime.tv_usec))/1000000)s.processError=EOVERFLOW;
  else s.process={uint64_t(usage.ru_utime.tv_sec)*1000000+uint64_t(usage.ru_utime.tv_usec),uint64_t(usage.ru_stime.tv_sec)*1000000+uint64_t(usage.ru_stime.tv_usec)};
  s.cgroupError=readFixed(cgroupFd_,buffer,bytes);
  if(!s.cgroupError)s.cgroupError=cpuStat(buffer,bytes,s.cgroup);
#else
  s.schedulerError=s.processError=s.cgroupError=ENOSYS;
#endif
  s.endNs=now();
  return s;
 }
 SubmitContextRecord start()const noexcept{
  SubmitContextRecord r;
  r.schedulerEnabled=schedulerEnabled_;
  r.schedulerFlagError=schedulerFlagError_;
  r.supported=supported();
  r.before=take();
  return r;
 }
 static void write(std::ostream& stream,uint32_t call,uint64_t generation,const SubmitContextRecord& record){
  const auto& b=record.before;
  const auto& a=record.after;
  std::array<uint64_t,3> scheduler{};
  std::array<uint64_t,2> process{};
  std::array<uint64_t,6> cgroup{};
  const bool sv=!b.schedulerError&&!a.schedulerError&&delta(b.scheduler,a.scheduler,scheduler);
  const bool pv=!b.processError&&!a.processError&&delta(b.process,a.process,process);
  const bool cv=!b.cgroupError&&!a.cgroupError&&delta(b.cgroup,a.cgroup,cgroup);
  stream<<"GPU_HOST_SUBMIT_CONTEXT call="<<call<<" frame="<<generation<<" interval="<<record.interval<<" supported="<<uint32_t(record.supported)
   <<" scheduler_enabled="<<record.schedulerEnabled<<" scheduler_flag_errno="<<record.schedulerFlagError<<" scheduler_valid="<<uint32_t(sv)
   <<" scheduler_before_errno="<<b.schedulerError<<" scheduler_after_errno="<<a.schedulerError
   <<" process_valid="<<uint32_t(pv)<<" process_before_errno="<<b.processError<<" process_after_errno="<<a.processError
   <<" cgroup_valid="<<uint32_t(cv)<<" cgroup_before_errno="<<b.cgroupError<<" cgroup_after_errno="<<a.cgroupError
   <<" before_begin_ns="<<b.beginNs<<" before_end_ns="<<b.endNs<<" after_begin_ns="<<a.beginNs<<" after_end_ns="<<a.endNs
   <<" probe_ns="<<(b.endNs-b.beginNs)+(a.endNs-a.beginNs)
   <<" scheduler_execution_ns="<<scheduler[0]<<" scheduler_runqueue_ns="<<scheduler[1]<<" scheduler_timeslices="<<scheduler[2]
   <<" process_user_us="<<process[0]<<" process_system_us="<<process[1]
   <<" cgroup_usage_us="<<cgroup[0]<<" cgroup_user_us="<<cgroup[1]<<" cgroup_system_us="<<cgroup[2]
   <<" cgroup_periods="<<cgroup[3]<<" cgroup_throttled_periods="<<cgroup[4]<<" cgroup_throttled_us="<<cgroup[5]
   <<" own_membership_verified="<<uint32_t(record.supported)<<" capacity=1 storage_bytes="<<sizeof(SubmitContextRecord)<<" overhead_in_whole=1 causal_claim=0\n";
 }
};
