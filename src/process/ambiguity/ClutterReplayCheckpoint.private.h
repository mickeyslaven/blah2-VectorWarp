#pragma once
// Authenticated, bounded replay qualification only. No live mode or DSP math.
#include "capture/Sha256.h"
#include <algorithm>
#include <cerrno>
#include <cmath>
#include <complex>
#include <cstring>
#include <functional>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace blah2::clutter_replay {
inline void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
inline bool hex(const std::string& value,size_t count=64){return value.size()==count&&
 value.find_first_not_of("0123456789abcdef")==std::string::npos;}
inline std::string hash(const void* data,size_t size){Sha256 h;h.update(data,size);return h.finish();}
inline std::string hash(const std::string& value){return hash(value.data(),value.size());}
struct Frame {uint64_t frame=0,epoch=0,first=0,end=0;};
struct Plan {
 std::string mode,path,expectedSha,identity,sourceSha,configSha;
 uint64_t warmFrame=0,epoch=0,firstSample=0,warmFirst=0,warmEnd=0,injectionFrame=0,injectionFirst=0;
 uint32_t samples=0,channels=0,taps=0;int32_t delayMin=0;bool injected=false;
 bool active()const{return !mode.empty();}
 bool fixed()const{return mode=="fixed";}
 void validate()const{
  require(mode=="export"||mode=="fixed"||mode=="refit","Unknown replay clutter checkpoint operation");
  require(!path.empty()&&path.front()=='/'&&path.size()<4096&&path.find('\n')==std::string::npos,
   "Replay clutter checkpoint requires an absolute bounded path");
  require(hex(identity)&&hex(configSha)&&hex(sourceSha,40)&&
   (mode=="export"?expectedSha.empty():hex(expectedSha)),"Replay clutter checkpoint identity/digest missing");
  require(samples&&samples<=4096&&channels&&channels<=8&&taps&&taps<=samples&&
   warmFrame&&warmFrame<64&&firstSample<=UINT64_MAX-uint64_t(samples)*64&&
   warmFirst==firstSample+(warmFrame-1)*samples&&warmEnd==warmFirst+samples&&
   injectionFrame==warmFrame+1&&injectionFirst==warmEnd,
   "Replay clutter checkpoint warm/injection boundary or geometry invalid");
  require(mode!="export"||!injected,"Checkpoint export cannot inject a warm-prefix echo");
 }
 std::string encode()const{
  std::ostringstream out;out<<std::quoted(mode)<<' '<<std::quoted(path)<<' '<<std::quoted(expectedSha)
   <<' '<<identity<<' '<<sourceSha<<' '<<configSha<<' '<<warmFrame<<' '<<epoch<<' '<<firstSample
   <<' '<<warmFirst<<' '<<warmEnd<<' '<<injectionFrame<<' '<<injectionFirst<<' '<<samples
   <<' '<<channels<<' '<<taps<<' '<<delayMin<<' '<<injected;return out.str();
 }
 static Plan decode(const std::string& text){Plan p;std::istringstream in(text);std::string extra;
  require(bool(in>>std::quoted(p.mode)>>std::quoted(p.path)>>std::quoted(p.expectedSha)>>p.identity
   >>p.sourceSha>>p.configSha>>p.warmFrame>>p.epoch>>p.firstSample>>p.warmFirst>>p.warmEnd
   >>p.injectionFrame>>p.injectionFirst>>p.samples>>p.channels>>p.taps>>p.delayMin>>p.injected)&&!(in>>extra),
   "Replay clutter checkpoint descriptor malformed");p.validate();return p;}
 void checkFrame(const Frame& f)const{
  require(f.frame&&f.frame<=64&&f.epoch==epoch&&
   f.first==firstSample+(f.frame-1)*samples&&f.end==f.first+samples,
   "Replay clutter checkpoint CPI/sample lineage drift");
 }
 bool inject(const Frame& f)const{checkFrame(f);return injected&&f.frame>=injectionFrame;}
};
template<class Node> Plan declaredPlan(Node node){
 Plan p;node["mode"]>>p.mode;node["path"]>>p.path;node["source_sha"]>>p.sourceSha;
 if(node.has_child("sha256"))node["sha256"]>>p.expectedSha;
 node["warm_frame"]>>p.warmFrame;node["epoch"]>>p.epoch;node["first_sample"]>>p.firstSample;
 node["warm_first_sample"]>>p.warmFirst;node["warm_end_sample"]>>p.warmEnd;
 node["injection_frame"]>>p.injectionFrame;node["injection_first_sample"]>>p.injectionFirst;
 node["injected"]>>p.injected;return p;
}
// Match semantic config scalars, preserving every setting except the explicitly
// isolated runner paths/ports and the authenticated per-arm qualification block.
template<class Node> std::string configBytes(Node node,const std::string& path={}){
 if(node.is_map()){
  std::vector<std::pair<std::string,std::string>> fields;
  for(auto child:node.children()){
   const auto key=child.key();std::string name(key.str,key.len),at=path.empty()?name:path+'.'+name;
   if(at=="capture.replay.qualification"||at=="capture.replay.artifacts"||at=="capture.replay.file"||
      at=="network.ports"||at=="save.path")continue;
   fields.emplace_back(name,configBytes(child,at));
  }
  std::sort(fields.begin(),fields.end());std::string out="{";
  for(const auto& item:fields)out+=std::to_string(item.first.size())+':'+item.first+item.second;
  return out+'}';
 }
 if(node.is_seq()){std::string out="[";for(auto child:node.children())out+=configBytes(child,path);return out+']';}
 const auto value=node.val();return std::to_string(value.len)+':'+std::string(value.str?value.str:"",value.len);
}
inline void put(std::vector<uint8_t>& out,uint64_t value,unsigned bytes){
 for(unsigned i=0;i<bytes;++i)out.push_back(uint8_t(value>>(8*i)));}
inline uint64_t get(const std::vector<uint8_t>& in,size_t& at,unsigned bytes){
 require(at<=in.size()&&bytes<=in.size()-at,"Truncated replay clutter checkpoint");
 uint64_t value=0;for(unsigned i=0;i<bytes;++i)value|=uint64_t(in[at++])<<(8*i);return value;}
template<class T> void scalar(std::vector<uint8_t>& out,T value){
 static_assert(sizeof(T)==4||sizeof(T)==8);require(std::isfinite(value),"Nonfinite replay clutter coefficient");
 uint64_t bits=0;std::memcpy(&bits,&value,sizeof(T));put(out,bits,sizeof(T));}
template<class T> T scalar(const std::vector<uint8_t>& in,size_t& at){
 const auto bits=get(in,at,sizeof(T));T value;std::memcpy(&value,&bits,sizeof(T));
 require(std::isfinite(value),"Nonfinite replay clutter coefficient");return value;}
struct Checkpoint {
 std::string identity,prefix;
 std::vector<std::complex<double>> initial;
 std::vector<std::complex<float>> correction;
 std::vector<uint8_t> encode(const Plan& p)const{
  require(identity==p.identity&&hex(prefix)&&initial.size()==size_t(p.channels)*p.taps&&
   correction.size()==initial.size(),"Incomplete replay clutter checkpoint");
  const std::string header="VWCLUTTER1"+identity+prefix;std::vector<uint8_t> out(header.begin(),header.end());
  for(auto value:{p.warmFrame,p.epoch,p.firstSample,p.warmFirst,p.warmEnd})put(out,value,8);
  for(auto value:{p.samples,p.channels,p.taps,uint32_t(p.delayMin)})put(out,value,4);
  for(auto v:initial){scalar(out,v.real());scalar(out,v.imag());}
  for(auto v:correction){scalar(out,v.real());scalar(out,v.imag());}return out;
 }
 static Checkpoint decode(const std::vector<uint8_t>& bytes,const Plan& p){
  require(bytes.size()==10+128+40+16+size_t(p.channels)*p.taps*24,
   "Replay clutter checkpoint geometry/length differs");
  require(hash(bytes.data(),bytes.size())==p.expectedSha,"Replay clutter checkpoint digest differs");
  require(std::string(bytes.begin(),bytes.begin()+10)=="VWCLUTTER1","Replay clutter checkpoint schema differs");
  Checkpoint c;c.identity.assign(bytes.begin()+10,bytes.begin()+74);c.prefix.assign(bytes.begin()+74,bytes.begin()+138);
  require(c.identity==p.identity&&hex(c.prefix),"Replay clutter checkpoint artifact/config/backend binding differs");
  size_t at=138;for(auto expected:{p.warmFrame,p.epoch,p.firstSample,p.warmFirst,p.warmEnd})
   require(get(bytes,at,8)==expected,"Replay clutter checkpoint warm lineage differs");
  for(auto expected:{p.samples,p.channels,p.taps,uint32_t(p.delayMin)})
   require(get(bytes,at,4)==expected,"Replay clutter checkpoint topology differs");
  c.initial.resize(size_t(p.channels)*p.taps);c.correction.resize(c.initial.size());
  for(auto& value:c.initial){const auto real=scalar<double>(bytes,at),imag=scalar<double>(bytes,at);value={real,imag};}
  for(auto& value:c.correction){const auto real=scalar<float>(bytes,at),imag=scalar<float>(bytes,at);value={real,imag};}return c;
 }
};
inline std::vector<uint8_t> read(const std::string& path){
 const int fd=open(path.c_str(),O_RDONLY|O_CLOEXEC|O_NOFOLLOW);require(fd>=0,"Missing replay clutter checkpoint");
 struct Close{int fd;~Close(){close(fd);}} guard{fd};struct stat info{};
 require(!fstat(fd,&info)&&S_ISREG(info.st_mode)&&info.st_size>0&&info.st_size<=(1<<20),
  "Invalid replay clutter checkpoint file");std::vector<uint8_t> bytes(size_t(info.st_size));
 size_t at=0;while(at<bytes.size()){const auto n=::read(fd,bytes.data()+at,bytes.size()-at);
  if(n<0&&errno==EINTR){continue;}
  require(n>0,"Replay clutter checkpoint read failed");at+=size_t(n);}
 uint8_t extra;require(::read(fd,&extra,1)==0,"Replay clutter checkpoint grew during read");return bytes;
}
inline void write(const std::string& path,const std::vector<uint8_t>& bytes){
 const int fd=open(path.c_str(),O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0600);
 require(fd>=0,"Replay clutter checkpoint output already exists or cannot be created");
 struct Close{int fd;~Close(){close(fd);}} guard{fd};size_t at=0;
 while(at<bytes.size()){const auto n=::write(fd,bytes.data()+at,bytes.size()-at);
  if(n<0&&errno==EINTR){continue;}
  require(n>0,"Replay clutter checkpoint write failed");at+=size_t(n);}
 require(!fsync(fd),"Replay clutter checkpoint sync failed");
}
struct Proof {
 Frame frame;bool fixed=false;std::string checkpoint,prefix;
 uint32_t covariance=0,factor=0,solve=0;
 std::string encode()const{std::ostringstream out;out<<frame.frame<<' '<<frame.epoch<<' '<<frame.first<<' '
  <<frame.end<<' '<<fixed<<' '<<(checkpoint.empty()?"-":checkpoint)<<' '<<(prefix.empty()?"-":prefix)
  <<' '<<covariance<<' '<<factor<<' '<<solve;return out.str();}
 static Proof decode(const std::string& text){Proof p;std::string extra;std::istringstream in(text);
  require(bool(in>>p.frame.frame>>p.frame.epoch>>p.frame.first>>p.frame.end>>p.fixed>>p.checkpoint>>p.prefix
   >>p.covariance>>p.factor>>p.solve)&&!(in>>extra),
   "Missing authenticated replay clutter execution proof");
  if(p.checkpoint=="-"){p.checkpoint.clear();}
  if(p.prefix=="-"){p.prefix.clear();}
  require(!p.fixed||(!p.covariance&&!p.factor&&!p.solve),"Frozen replay performed covariance/factor/solve work");return p;}
};
class State {
 Plan plan_;std::function<Frame()> binding_;Checkpoint checkpoint_,pending_;
 Sha256 prefix_;Frame frame_{};uint64_t previous_=0;uint32_t completed_=0;
 bool inFrame_=false,ready_=false,poisoned_=false;std::string digest_,prefixDigest_;
 uint32_t covariance_=0,factor_=0,solve_=0;
public:
 State(Plan plan,std::function<Frame()> binding):plan_(std::move(plan)),binding_(std::move(binding)){
  plan_.validate();if(plan_.mode!="export")checkpoint_=Checkpoint::decode(read(plan_.path),plan_);
 }
 bool fixed()const{return inFrame_&&ready_&&plan_.fixed()&&frame_.frame>plan_.warmFrame;}
 const Checkpoint& checkpoint()const{require(fixed(),"Replay clutter checkpoint not ready");return checkpoint_;}
 void begin(const std::complex<float>* reference,size_t references,const std::complex<float>* surveillance,size_t count){
  require(!poisoned_&&!inFrame_,"Replay clutter checkpoint frame reused/poisoned");frame_=binding_();
  if(!frame_.frame){return;}
  plan_.checkFrame(frame_);
  require(frame_.frame==previous_+1&&references==plan_.samples&&count==size_t(plan_.samples)*plan_.channels,
   "Replay clutter checkpoint prefix skipped or shape drifted");
  require(reference&&surveillance,"Replay clutter checkpoint input missing");inFrame_=true;completed_=0;
  covariance_=factor_=solve_=0;
  if(frame_.frame<=plan_.warmFrame){prefix_.update(&frame_,sizeof(frame_));
   prefix_.update(reference,references*sizeof(*reference));prefix_.update(surveillance,count*sizeof(*surveillance));}
  if(frame_.frame==plan_.warmFrame){pending_.identity=plan_.identity;
   pending_.initial.resize(size_t(plan_.channels)*plan_.taps);pending_.correction.resize(pending_.initial.size());}
 }
 void work(uint32_t covariance,uint32_t factor,uint32_t solve){
  if(!frame_.frame)return;
  require(inFrame_&&!fixed(),"Frozen replay attempted covariance/factor/solve work");
  covariance_+=covariance;factor_+=factor;solve_+=solve;
 }
 void batch(uint32_t first,uint32_t active,const std::complex<double>* initial,const std::complex<float>* correction){
  if(!frame_.frame)return;
  require(inFrame_&&first==completed_&&active&&active<=plan_.channels-first,
   "Replay clutter checkpoint channel/batch order differs");
  if(frame_.frame==plan_.warmFrame){require(initial&&correction,"Replay clutter checkpoint coefficients missing");
   std::copy_n(initial,size_t(active)*plan_.taps,pending_.initial.begin()+size_t(first)*plan_.taps);
   std::copy_n(correction,size_t(active)*plan_.taps,pending_.correction.begin()+size_t(first)*plan_.taps);}
  completed_+=active;
 }
 void complete(bool accepted){
  if(!frame_.frame)return;
  require(accepted&&inFrame_&&completed_==plan_.channels,"Replay clutter checkpoint incomplete/rejected frame");
  if(frame_.frame==plan_.warmFrame){pending_.prefix=prefix_.finish();const auto bytes=pending_.encode(plan_);
   digest_=hash(bytes.data(),bytes.size());prefixDigest_=pending_.prefix;
   if(plan_.mode=="export"){write(plan_.path,bytes);checkpoint_=pending_;}
   else {require(digest_==plan_.expectedSha&&pending_.prefix==checkpoint_.prefix,
    "Replay clutter checkpoint warm prefix/solved coefficients differ");}
   ready_=true;}
  previous_=frame_.frame;inFrame_=false;
 }
 void poison(){poisoned_=true;}
 Proof proof()const{return {frame_,ready_&&plan_.fixed()&&frame_.frame>plan_.warmFrame,digest_,prefixDigest_,
  covariance_,factor_,solve_};}
};
// This private optional interface is configured exclusively inside the worker
// after its read-only qualification descriptor and artifact pins are verified.
class Backend {
public:
 virtual ~Backend()=default;
 virtual void configureClutterReplay(const Plan&,std::function<Frame()>)=0;
 virtual Proof clutterReplayProof()const=0;
};
}
