#pragma once
// Offline qualification only. No environment variable enables the parent.
// A validated replay configuration creates a private, unlinked control file;
// workers receive only its read-only descriptor and an explicit per-spawn recipe.
#include "ClutterReplayCheckpoint.private.h"
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <sstream>
#include <spawn.h>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace blah2::replay_gpu_qualification {
inline constexpr const char* confirmation = "offline-physical-gpu-qualification-v1";
inline constexpr const char* environmentKey = "BLAH2_REPLAY_GPU_QUALIFICATION=";
struct Config { bool enabled=false; std::string confirmation,kind,route; uint64_t frame=0; uint32_t lane=0; };
using Binding=clutter_replay::Frame;
struct Control { uint64_t magic=0x56575155414c3031ULL,nonce=0,parent=0; Binding binding; uint64_t check=0; };
static_assert(sizeof(Control)==64);
inline uint64_t checksum(const Control& c) { return c.magic^c.nonce^c.parent^c.binding.frame^c.binding.epoch^c.binding.first^c.binding.end; }
struct State {
 Config config; int writer=-1,reader=-1; Control control; bool gapPending=false,cpuResetFired=false;
 clutter_replay::Plan clutter;clutter_replay::Proof clutterProof;std::string clutterTrailer;
 ~State(){if(writer>=0)close(writer);if(reader>=0)close(reader);}
};
inline State& state(){static State value;return value;}
inline bool enabled(){return state().config.enabled;}
inline void validate(const Config& c,bool replay,const std::string& format){
 if(!c.enabled||!replay||(format!="blah2"&&format!="mchq")||c.confirmation!=confirmation)
  throw std::invalid_argument("GPU qualification requires explicit native offline replay confirmation");
 if(c.kind.empty()) { if(!c.route.empty()||c.frame||c.lane)throw std::invalid_argument("Incomplete GPU qualification fault");return; }
 const bool startup=c.kind=="startup"||c.kind=="allocation";
 if(c.kind!="startup"&&c.kind!="allocation"&&c.kind!="prepare"&&c.kind!="commit"&&
    c.kind!="worker-exit"&&c.kind!="timeout"&&c.kind!="numeric-rejection"&&c.kind!="input-gap"&&c.kind!="cpu-oracle-reset")
  throw std::invalid_argument("Unknown GPU qualification fault");
 if((startup?c.frame!=0:(!c.frame||c.frame>256))||c.lane>=8||
   (c.kind=="input-gap"?(c.route!="capture"||c.lane!=0):c.kind=="cpu-oracle-reset"?c.route!="host":
    (c.route!="portable"&&c.route!="resident"))||
   ((c.kind=="prepare"||c.kind=="commit")&&c.route!="resident"))
  throw std::invalid_argument("Invalid GPU qualification fault boundary");
}
inline void validateRoute(const Config& c,bool multi,uint32_t lanes,const std::string& mode){
 if(c.kind.empty()||c.kind=="input-gap")return;
 if(c.lane>=lanes)throw std::invalid_argument("GPU qualification fault has no requested lane/backend route");
 if(c.kind=="cpu-oracle-reset"){
  if(!multi||mode!="cpu"||c.route!="host")
   throw std::invalid_argument("Qualification CPU oracle reset requires explicit CPU multi-lane replay");
  return;
 }
 const bool resident=mode=="gpu-resident";
 if(mode=="cpu"||(c.route=="portable"&&resident)||
    (c.route=="resident"&&!resident&&(!multi||mode!="auto")))
  throw std::invalid_argument("GPU qualification fault has no requested lane/backend route");
}
inline void configure(const Config& c,bool replay,const std::string& format){
 validate(c,replay,format);auto& s=state();
 if(s.writer>=0)throw std::logic_error("GPU qualification configured twice");
 char path[]="/tmp/vectorwarp-gpu-qualification-XXXXXX";
 int writer=mkstemp(path);
 if(writer<0)throw std::runtime_error("Cannot create private GPU qualification control");
 const int reader=open(path,O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
 const int removed=unlink(path);
 if(reader<0||removed){close(writer);if(reader>=0)close(reader);throw std::runtime_error("Cannot isolate GPU qualification control");}
 s.writer=fcntl(writer,F_DUPFD_CLOEXEC,10);s.reader=fcntl(reader,F_DUPFD_CLOEXEC,10);
 close(writer);close(reader);
 if(s.writer<0||s.reader<0||getentropy(&s.control.nonce,sizeof(s.control.nonce))||!s.control.nonce)
  throw std::runtime_error("Cannot initialize GPU qualification control");
 s.control.parent=uint64_t(getpid());s.control.check=checksum(s.control);
 if(pwrite(s.writer,&s.control,sizeof(s.control),0)!=ssize_t(sizeof(s.control)))
  throw std::runtime_error("Cannot initialize GPU qualification binding");
 s.config=c;
}
inline Binding current(){return state().control.binding;}
inline void configureClutter(clutter_replay::Plan plan){
 auto& s=state();plan.validate();
 if(!enabled()||s.writer<0||!s.config.kind.empty()||s.clutter.active()||s.control.binding.frame)
  throw std::invalid_argument("Clutter checkpoint requires unused authenticated fault-free replay control");
 s.clutterTrailer=plan.encode();
 if(s.clutterTrailer.size()>8192||pwrite(s.writer,s.clutterTrailer.data(),s.clutterTrailer.size(),sizeof(Control))!=
    ssize_t(s.clutterTrailer.size()))throw std::runtime_error("Cannot bind replay clutter checkpoint contract");
 s.clutter=std::move(plan);
}
inline bool clutterActive(){return enabled()&&state().clutter.active();}
inline bool injectEcho(){return !clutterActive()||state().clutter.inject(current());}
template<class Acceleration> void requireClutterSteady(const Acceleration& a,bool completed){
 if(!clutterActive())return;
 if(!a.available()||a.startupPending()||a.status().state!="ready"||a.clutterStatus().state!="ready"||
    (completed&&(!a.clutterTiming().gpuExecuted||a.clutterTiming().cpuExecuted||
     !a.ambiguityGpuExecuted()||a.ambiguityCpuExecuted()||state().clutterProof.frame.frame!=current().frame)))
  throw std::runtime_error("Replay clutter checkpoint requires qualified GPU execution; fallback/reset is forbidden");
}
inline void acceptClutterProof(const std::string& text){
 if(!clutterActive()){return;}
 auto proof=clutter_replay::Proof::decode(text);const auto b=current();
 if(proof.frame.frame!=b.frame||proof.frame.epoch!=b.epoch||proof.frame.first!=b.first||proof.frame.end!=b.end||
    proof.fixed!=(state().clutter.fixed()&&b.frame>state().clutter.warmFrame)||
    (b.frame>=state().clutter.warmFrame&&(!clutter_replay::hex(proof.checkpoint)||!clutter_replay::hex(proof.prefix)))||
    (b.frame>=state().clutter.warmFrame&&state().clutter.mode!="export"&&proof.checkpoint!=state().clutter.expectedSha))
  throw std::runtime_error("Replay clutter execution proof differs from authenticated frame/checkpoint");
 state().clutterProof=std::move(proof);
}
inline void bind(uint64_t frame,uint64_t epoch,uint64_t first,uint64_t end){
 if(!enabled())return;
 if(!frame||frame>256||first>=end)throw std::runtime_error("GPU qualification exceeds positioned 256-frame replay bound");
 auto& s=state();s.control.binding={frame,epoch,first,end};s.control.check=checksum(s.control);
 if(clutterActive()){s.clutter.checkFrame(s.control.binding);s.clutterProof={};}
 if(pwrite(s.writer,&s.control,sizeof(s.control),0)!=ssize_t(sizeof(s.control)))
  throw std::runtime_error("Cannot bind GPU qualification capture");
}
inline std::string quoted(const std::string& value){
 std::string out="\"";const char* hex="0123456789abcdef";
 for(unsigned char c:value){if(c=='"'||c=='\\'){out+='\\';out+=char(c);}else if(c<32){out+="\\u00";out+=hex[c>>4];out+=hex[c&15];}else out+=char(c);}
 return out+'"';
}
inline void emit(const char* boundary,const char* route,uint32_t lane,const Binding& b,
 const std::string& kind={},const std::string& device={},uint64_t logical=0,uint64_t consumed=0,pid_t worker=0,
 uint64_t allocationBytes=0,uint64_t allocationLimit=0,const std::string& clutterProof={}){
 std::ostringstream out;out<<"VECTORWARP_GPU_QUALIFICATION {\"schema\":1,\"boundary\":"<<quoted(boundary)
  <<",\"route\":"<<quoted(route)<<",\"lane\":"<<lane<<",\"pid\":"<<getpid()<<",\"workerPid\":"<<worker
  <<",\"frame\":"<<b.frame<<",\"captureEpoch\":"<<b.epoch<<",\"captureFirstEpochSample\":"<<b.first
  <<",\"captureEndEpochSample\":"<<b.end<<",\"logicalFrame\":"<<logical<<",\"consumedSequence\":"<<consumed
  <<",\"fault\":"<<quoted(kind)<<",\"device\":"<<quoted(device)
  <<",\"allocationBytes\":"<<allocationBytes<<",\"allocationLimit\":"<<allocationLimit;
 if(!clutterProof.empty())out<<",\"clutterCheckpoint\":"<<quoted(clutterProof);
 out<<"}\n";
 const auto line=out.str();if(line.size()>4096||write(STDERR_FILENO,line.data(),line.size())!=ssize_t(line.size()))
  throw std::runtime_error("GPU qualification evidence write failed");
}
inline void event(const char* boundary){if(enabled())emit(boundary,"capture",0,current(),state().config.kind);}
inline bool discardGap(){auto& s=state();if(!enabled()||s.config.kind!="input-gap"||current().frame!=s.config.frame)return false;
 event("input-gap-discarded");s.gapPending=true;return true;}
inline bool takeGapReset(){auto& s=state();const bool pending=s.gapPending;s.gapPending=false;return pending;}
inline bool takeCpuOracleReset(uint32_t lane){auto& s=state();
 if(!enabled()||s.config.kind!="cpu-oracle-reset"||s.config.route!="host"||s.config.lane!=lane||
    s.config.frame!=current().frame||s.cpuResetFired)return false;
 s.cpuResetFired=true;return true;
}
inline bool inherited(const std::string& item){return item.rfind(environmentKey,0)==0;}
inline void spawnActions(posix_spawn_file_actions_t& actions){if(enabled()&&
 posix_spawn_file_actions_adddup2(&actions,state().reader,5))throw std::runtime_error("Cannot isolate GPU qualification descriptor");}
inline int closeFrom(){return enabled()?6:5;}
inline bool allocationFault(const char* route,uint32_t lane){const auto& c=state().config;
 return enabled()&&c.kind=="allocation"&&c.route==route&&c.lane==lane;}
inline void appendEnvironment(std::vector<std::string>& env,const char* route,uint32_t lane,const std::string& executable){
 if(!enabled())return;
 if(!executable.empty())throw std::invalid_argument("GPU qualification prohibits replacement worker executables");
 const auto& s=state();std::ostringstream value;
 value<<environmentKey<<s.control.nonce<<' '<<s.control.parent<<' '<<route<<' '<<lane<<' '
  <<(s.config.kind.empty()?"none":s.config.kind)<<' '<<(s.config.route.empty()?"none":s.config.route)<<' '<<s.config.lane<<' '<<s.config.frame;
 if(s.clutter.active()){
  if(std::string(route)!="portable"||lane)throw std::invalid_argument("Frozen clutter only supports single-lane portable replay");
  value<<' '<<clutter_replay::hash(s.clutterTrailer);
 }
 env.push_back(value.str());
}
class Worker {
 bool enabled_=false,fired_=false;uint64_t nonce_=0,parent_=0,targetFrame_=0;uint32_t lane_=0,targetLane_=0;
 std::string route_,kind_,targetRoute_,device_;
 clutter_replay::Plan clutter_;
public:
 explicit Worker(const char* route):route_(route){
  const char* value=std::getenv("BLAH2_REPLAY_GPU_QUALIFICATION");if(!value)return;
  std::string actual,trailerHash,extra;std::istringstream in(value);
  if(!(in>>nonce_>>parent_>>actual>>lane_>>kind_>>targetRoute_>>targetLane_>>targetFrame_)||actual!=route_||parent_!=uint64_t(getppid()))
   throw std::invalid_argument("GPU qualification worker recipe invalid");
  if(in>>trailerHash){if(!clutter_replay::hex(trailerHash)||(in>>extra))
   throw std::invalid_argument("GPU qualification clutter recipe invalid");}
  struct stat info{};const int flags=fcntl(5,F_GETFL);
  if(flags<0||(flags&O_ACCMODE)!=O_RDONLY||fstat(5,&info)||!S_ISREG(info.st_mode)||info.st_uid!=getuid()||info.st_nlink!=0||
     (trailerHash.empty()?info.st_size!=static_cast<off_t>(sizeof(Control)):
      (info.st_size<=static_cast<off_t>(sizeof(Control))||info.st_size>static_cast<off_t>(sizeof(Control)+8192))))
   throw std::invalid_argument("GPU qualification requires private read-only control descriptor");
  enabled_=true;(void)binding();
  if(!trailerHash.empty()){
   std::string bytes(size_t(info.st_size)-sizeof(Control),'\0');
   if(route_!="portable"||lane_||kind_!="none"||pread(5,bytes.data(),bytes.size(),sizeof(Control))!=ssize_t(bytes.size())||
      clutter_replay::hash(bytes)!=trailerHash)throw std::invalid_argument("GPU qualification clutter descriptor differs");
   clutter_=clutter_replay::Plan::decode(bytes);
  }
 }
 bool active()const{return enabled_;}
 const clutter_replay::Plan& clutter()const{return clutter_;}
 void clutterComplete(const clutter_replay::Proof& proof){
  if(clutter_.active()&&proof.frame.frame)emit("clutter-checkpoint","portable",lane_,binding(),{},device_,0,0,getpid(),0,0,proof.encode());
 }
 Binding binding()const{if(!enabled_)return {};Control control;
  if(pread(5,&control,sizeof(control),0)!=ssize_t(sizeof(control))||control.magic!=Control{}.magic||control.nonce!=nonce_||control.parent!=parent_||control.check!=checksum(control))
   throw std::runtime_error("GPU qualification capture binding invalid");
  return control.binding;
 }
 void device(const std::string& value){device_=value;}
 bool hit(const char* boundary,uint64_t logical=0,uint64_t consumed=0){
  if(!enabled_||fired_||route_!=targetRoute_||lane_!=targetLane_||kind_!=boundary)return false;
  const auto b=binding();if(b.frame!=targetFrame_)return false;
  fired_=true;emit(boundary,route_.c_str(),lane_,b,kind_,device_,logical,consumed,getpid());return true;
 }
 void beforeFrame(uint64_t logical=0,uint64_t consumed=0){
  if(hit("worker-exit",logical,consumed))_exit(86);
  if(hit("timeout",logical,consumed)){for(;;)pause();}
 }
 void complete(const char* boundary,uint64_t logical=0,uint64_t consumed=0){if(enabled_&&binding().frame)
  emit(boundary,route_.c_str(),lane_,binding(),{},device_,logical,consumed,getpid());}
 void allocationRejected(uint64_t bytes,uint64_t limit){
  if(enabled_&&route_==targetRoute_&&lane_==targetLane_&&kind_=="allocation"&&binding().frame==0)
   emit("allocation-budget-rejected",route_.c_str(),lane_,binding(),kind_,device_,0,0,getpid(),bytes,limit);
 }
};
inline void reaped(pid_t worker)noexcept{if(enabled())try{emit("worker-reaped","parent",0,current(),{}, {},0,0,worker);}catch(...) {}}
inline void reaped(pid_t worker,const Binding& binding,bool active)noexcept{if(active)try{
 emit("worker-reaped","parent",0,binding,{}, {},0,0,worker);}catch(...) {}}
}
