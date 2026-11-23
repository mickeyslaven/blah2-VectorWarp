#pragma once
// Offline artifact evidence only. Inert without validated replay configuration;
// no changes to module exports, backend vtables, geometry or wire protocols.
#include "capture/Sha256.h"
#include "GpuProcess.h"
#include "GpuBackend.h"
#include "ReplayGpuQualification.h"
#include <climits>
#include <dlfcn.h>
#include <memory>
#include <atomic>
#include <map>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

namespace blah2::gpu_artifact_binding {
inline constexpr const char* confirmation="offline-gpu-artifact-binding-v1";
inline constexpr int workerDescriptor=6; // protocol 3/4; qualification control 5
inline constexpr size_t maximumReceipt=4096,maximumReceipts=512;
struct Config {bool enabled=false;std::string confirmation,nonce;int descriptor=-1;};
struct State {
 int descriptor=-1;std::map<std::string,std::string> pins;std::atomic<size_t> emitted{0};
 ~State(){if(descriptor>=0)close(descriptor);}
};
inline State& state(){static State value;return value;}
inline std::string inheritedValue(const std::string& suffix) {
 const char* value=std::getenv(("VECTORWARP_GPU_ARTIFACT_"+suffix).c_str());
 return value?value:"";
}
inline std::string env(const std::string& suffix){
 const auto& pins=state().pins;const auto found=pins.find(suffix);return found==pins.end()?"":found->second;
}
inline bool active(){return state().descriptor>=0;}
inline bool inherited(const std::string& entry){return entry.rfind("VECTORWARP_GPU_ARTIFACT_",0)==0;}
inline bool hex(const std::string& text,size_t length){return text.size()==length&&
 text.find_first_not_of("0123456789abcdef")==std::string::npos;}
inline std::string channelIdentity(int fd){
 struct stat info{};int type=0;socklen_t size=sizeof(type);
 sockaddr_un local{},peer{};socklen_t localSize=sizeof(local),peerSize=sizeof(peer);
 const char unnamed[sizeof(local.sun_path)]{};
 if(fd<3||fstat(fd,&info)||!S_ISSOCK(info.st_mode)||info.st_uid!=getuid()||
    getsockopt(fd,SOL_SOCKET,SO_TYPE,&type,&size)||type!=SOCK_DGRAM||
    getsockname(fd,reinterpret_cast<sockaddr*>(&local),&localSize)||local.sun_family!=AF_UNIX||
    getpeername(fd,reinterpret_cast<sockaddr*>(&peer),&peerSize)||peer.sun_family!=AF_UNIX||
    std::memcmp(local.sun_path,unnamed,sizeof(unnamed))||std::memcmp(peer.sun_path,unnamed,sizeof(unnamed)))
  throw std::invalid_argument("GPU artifact evidence requires a private connected UNIX datagram descriptor");
 return std::to_string(info.st_dev)+":"+std::to_string(info.st_ino);
}
inline std::map<std::string,std::string> readPins(const std::string& nonce){
 if(!hex(nonce,32)||inheritedValue("NONCE")!=nonce)
  throw std::invalid_argument("GPU artifact launch nonce differs from replay configuration");
 std::map<std::string,std::string> pins{{"NONCE",nonce}};
 for(const auto* role:{"PROCESSOR","WORKER","PORTABLE","RESIDENT","WIDEBAND"}){
  const std::string prefix=std::string(role)+"_",path=inheritedValue(prefix+"PATH"),
   hash=inheritedValue(prefix+"SHA256"),stamp=inheritedValue(prefix+"STAT");
  if(path.empty()&&hash.empty()&&stamp.empty()&&std::string(role)!="PROCESSOR")continue;
  if(path.empty()||path.front()!='/'||path.size()>=PATH_MAX||!hex(hash,64)||stamp.empty()||
     stamp.size()>200||stamp.find_first_not_of("0123456789:")!=std::string::npos)
   throw std::invalid_argument("Invalid GPU artifact launch pin");
  pins[prefix+"PATH"]=path;pins[prefix+"SHA256"]=hash;pins[prefix+"STAT"]=stamp;
 }
 return pins;
}
inline void configure(const Config& config,bool replay,const std::string& format){
 if(!config.enabled||!replay||(format!="auto"&&format!="blah2"&&format!="mchq")||config.confirmation!=confirmation)
  throw std::invalid_argument("GPU artifact binding requires explicit offline replay confirmation");
 if(active())throw std::logic_error("GPU artifact binding configured twice");
 auto pins=readPins(config.nonce);(void)channelIdentity(config.descriptor);
 const int descriptor=fcntl(config.descriptor,F_DUPFD_CLOEXEC,10);
 if(descriptor<0)throw std::runtime_error("Cannot isolate GPU artifact receipt descriptor");
 close(config.descriptor);state().pins=std::move(pins);state().descriptor=descriptor;
}
inline void spawnActions(posix_spawn_file_actions_t& actions,bool qualification=replay_gpu_qualification::enabled()){if(active()){
 if(posix_spawn_file_actions_adddup2(&actions,state().descriptor,workerDescriptor)||
    (!qualification&&posix_spawn_file_actions_addclose(&actions,5)))
  throw std::runtime_error("Cannot isolate GPU artifact worker descriptor");
}}
inline int closeFrom(){return active()?workerDescriptor+1:replay_gpu_qualification::closeFrom();}
inline void appendEnvironment(std::vector<std::string>& environment){if(active()){
 for(const auto& pin:state().pins)environment.push_back("VECTORWARP_GPU_ARTIFACT_"+pin.first+'='+pin.second);
 environment.push_back("VECTORWARP_GPU_ARTIFACT_CHANNEL="+std::to_string(getpid())+' '+channelIdentity(state().descriptor));
}}
inline std::string resolve(const std::string& path){
 char resolved[PATH_MAX];
 if(!realpath(path.c_str(),resolved))throw std::runtime_error("Cannot resolve pinned GPU artifact: "+path);
 return resolved;
}
inline std::string executable(){
 char path[PATH_MAX];
#ifdef __APPLE__
 uint32_t size=sizeof(path);
 if(_NSGetExecutablePath(path,&size))throw std::runtime_error("Cannot locate GPU executable image");
#else
 const auto size=readlink("/proc/self/exe",path,sizeof(path)-1);
 if(size<0||size_t(size)==sizeof(path)-1)throw std::runtime_error("Cannot locate GPU executable image");
 path[size]=0;
#endif
 return resolve(path);
}
inline std::string stamp(const struct stat& value){
 std::ostringstream out;out<<value.st_dev<<':'<<value.st_ino<<':'<<value.st_size<<':';
#ifdef __APPLE__
 out<<value.st_mtimespec.tv_sec<<':'<<value.st_mtimespec.tv_nsec<<':'
    <<value.st_ctimespec.tv_sec<<':'<<value.st_ctimespec.tv_nsec;
#else
 out<<value.st_mtim.tv_sec<<':'<<value.st_mtim.tv_nsec<<':'
    <<value.st_ctim.tv_sec<<':'<<value.st_ctim.tv_nsec;
#endif
 return out.str();
}
struct Artifact {std::string path,sha256,stat;};
inline Artifact inspect(const std::string& path){
 Artifact result;result.path=resolve(path);
 const int fd=open(result.path.c_str(),O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
 if(fd<0)throw std::runtime_error("Cannot read pinned GPU artifact");
 struct Close {int fd;~Close(){close(fd);}} closeFile{fd};
 struct stat before{},after{},named{};
 if(fstat(fd,&before)||!S_ISREG(before.st_mode)||before.st_size<=0||before.st_size>(512LL<<20))
  throw std::runtime_error("GPU artifact is not a bounded regular file");
 result.stat=stamp(before);blah2::Sha256 hash;std::array<char,65536> buffer{};uint64_t bytes=0;
 for(;;){const auto count=read(fd,buffer.data(),buffer.size());
  if(count<0&&errno==EINTR)continue;
  if(count<0)throw std::runtime_error("Cannot hash GPU artifact");
  if(!count)break;
  if(uint64_t(count)>uint64_t(before.st_size)-bytes)throw std::runtime_error("GPU artifact grew while hashing");
  bytes+=uint64_t(count);
  hash.update(buffer.data(),size_t(count));}
 if(bytes!=uint64_t(before.st_size)||fstat(fd,&after)||::stat(result.path.c_str(),&named)||stamp(after)!=result.stat||stamp(named)!=result.stat)
  throw std::runtime_error("GPU artifact changed while hashing");
 for(auto byte:hash.finishBytes()){result.sha256+="0123456789abcdef"[byte>>4];result.sha256+="0123456789abcdef"[byte&15];}
 return result;
}
inline void verify(const Artifact& actual,const std::string& role){
 if(actual.path!=env(role+"_PATH")||actual.sha256!=env(role+"_SHA256")||actual.stat!=env(role+"_STAT"))
  throw std::runtime_error("GPU artifact path/hash/stat differs from launch pin: "+role);
}
inline std::string json(const Artifact& value){
 using replay_gpu_qualification::quoted;
 return "{\"path\":"+quoted(value.path)+",\"sha256\":"+quoted(value.sha256)+",\"stat\":"+quoted(value.stat)+"}";
}
inline void emit(const char* event,const std::string& fields={}){
 if(!active())return;
 using replay_gpu_qualification::quoted;
 const std::string line="VECTORWARP_GPU_ARTIFACT {\"schema\":1,\"nonce\":"+quoted(env("NONCE"))+
  ",\"pid\":"+std::to_string(getpid())+",\"parent\":"+std::to_string(getppid())+
  ",\"event\":"+quoted(event)+fields+"}\n";
 // Datagram boundaries prevent interleaving; a stalled runner can never block
 // a worker. Failed delivery invalidates this explicitly authorized replay.
 int flags=MSG_DONTWAIT;
#ifdef MSG_NOSIGNAL
 flags|=MSG_NOSIGNAL;
#endif
 if(line.size()>maximumReceipt||state().emitted.fetch_add(1)>=maximumReceipts||
    send(state().descriptor,line.data(),line.size(),flags)!=ssize_t(line.size()))
  throw std::runtime_error("GPU artifact evidence write failed");
}
inline void workerStart(){
 const auto recipe=inheritedValue("CHANNEL");if(recipe.empty())return;
 if(active())throw std::logic_error("GPU artifact worker initialized twice");
 uint64_t parent=0;std::string identity,extra;std::istringstream input(recipe);
 if(!(input>>parent>>identity)||(input>>extra)||parent!=uint64_t(getppid())||
    identity!=channelIdentity(workerDescriptor))throw std::invalid_argument("Invalid GPU artifact worker binding");
 auto pins=readPins(inheritedValue("NONCE"));
 if(fcntl(workerDescriptor,F_SETFD,FD_CLOEXEC))throw std::runtime_error("Cannot isolate GPU artifact worker channel");
 state().pins=std::move(pins);state().descriptor=workerDescriptor;
 const auto image=inspect(executable());verify(image,"WORKER");
 emit("worker-start",",\"worker\":"+json(image));
}
inline void spawned(pid_t pid,const std::string& path){if(active()){
 const auto actual=resolve(path);
 if(actual!=env("WORKER_PATH"))throw std::runtime_error("Spawned GPU worker path differs from pin");
 emit("worker-spawn",",\"workerPid\":"+std::to_string(pid)+",\"executablePath\":"+
  replay_gpu_qualification::quoted(actual));
}}
class Module {
 void* handle_=nullptr;
 std::string role_,key_,path_,mode_;
public:
 Module(const char* filename,const char* role,const char* mode=""):role_(role),key_(role),mode_(mode){
  for(auto& c:key_)if(c>='a'&&c<='z')c=char(c-'a'+'A');
  if(active())emit("module-attempt",",\"role\":"+replay_gpu_qualification::quoted(role_));
  path_=gpuSiblingPath(filename);
  if(active()){const auto image=inspect(path_);verify(image,key_);path_=image.path;}
  handle_=dlopen(path_.c_str(),RTLD_NOW|RTLD_LOCAL);
  if(!handle_)throw std::runtime_error("GPU module load failed: "+path_);
 }
 ~Module(){if(handle_)dlclose(handle_);}
 Module(const Module&)=delete;Module& operator=(const Module&)=delete;
 template<class Create> Create factory(const char* symbol,unsigned abi,const std::string& device){
  void* address=dlsym(handle_,symbol);
  if(!address)throw std::runtime_error("GPU module ABI/export mismatch: "+path_);
  if(active()){
   Dl_info loaded{};
   if(!dladdr(address,&loaded)||!loaded.dli_fname)throw std::runtime_error("Loaded GPU module identity unavailable");
   const auto image=inspect(loaded.dli_fname);verify(image,key_);
   const auto status=reinterpret_cast<GpuDriverStatus>(dlsym(handle_,"blah2_gpu_driver_status"));
   if(!status)throw std::runtime_error("Loaded GPU module lacks driver identity");
   Dl_info diagnostics{};
   if(!dladdr(reinterpret_cast<void*>(status),&diagnostics)||!diagnostics.dli_fname||
       resolve(diagnostics.dli_fname)!=image.path)
    throw std::runtime_error("GPU driver identity belongs to another module");
   const auto identity=status(1);
   verify(inspect(path_),key_); // Detect replacement during load/enumeration.
   const auto role=",\"role\":"+replay_gpu_qualification::quoted(role_);
   const auto interface=",\"factory\":"+replay_gpu_qualification::quoted(symbol)+
    ",\"workerMode\":"+replay_gpu_qualification::quoted(mode_);
   emit("module-loaded",role+",\"module\":"+json(image)+",\"abi\":"+std::to_string(abi)+
    interface+",\"deviceRequest\":"+replay_gpu_qualification::quoted(device)+",\"driverStatus\":"+identity);
   emit("backend-start",role+interface);
  }
  return reinterpret_cast<Create>(address);
 }
};
}
