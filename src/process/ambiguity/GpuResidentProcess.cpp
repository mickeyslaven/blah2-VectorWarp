#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "GpuResidentWire.h"
#include "GpuResidentProof.h"
#include <complex>
#include "data/meta/Constants.h"
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <spawn.h>
#include <stdexcept>
#include <sys/mman.h>
#ifdef __APPLE__
#include <sys/random.h>
#include <mach-o/dyld.h>
#include <sys/event.h>
#else
#include <sys/prctl.h>
#endif
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

extern char** environ;
namespace blah2::resident {
namespace transport {
#include "GpuResidentTransport.private.h"
void sendPacket(int fd,const Packet& p){const auto bytes=encode(p);
size_t sent=0;
 while(sent<bytes.size()){ssize_t n=send(fd,bytes.data()+sent,bytes.size()-sent,MSG_NOSIGNAL|MSG_DONTWAIT);
 if(n<0&&errno==EINTR)continue;
 if(n<=0)throw std::runtime_error("Resident worker send failed");
#ifndef __APPLE__
  if(size_t(n)!=bytes.size())throw std::runtime_error("Resident worker short packet write");
#endif
  sent+=size_t(n);
 }}
Packet receive(int fd,Clock::time_point deadline){std::array<uint8_t,packetBytes> bytes{};
size_t got=0;
 while(got<bytes.size()){auto left=std::chrono::duration_cast<std::chrono::milliseconds>(deadline-Clock::now()).count();
 if(left<=0)throw std::runtime_error("Resident worker deadline exceeded");
 pollfd poller{fd,POLLIN,0};
 int ready=poll(&poller,1,int(std::min<int64_t>(left,INT_MAX)));
 if(ready<0&&errno==EINTR)continue;
 if(ready<0)throw std::runtime_error("Resident worker poll failed");
 if(!ready)continue;
#ifdef __APPLE__
  ssize_t n=recv(fd,bytes.data()+got,bytes.size()-got,MSG_DONTWAIT);
#else
  ssize_t n=recv(fd,bytes.data(),bytes.size(),MSG_TRUNC|MSG_DONTWAIT);
#endif
  if(n<0&&(errno==EINTR||errno==EAGAIN))continue;
  if(n<=0)throw std::runtime_error("Resident worker stopped");
#ifndef __APPLE__
  if(size_t(n)!=bytes.size())throw std::runtime_error("Resident worker packet length mismatch");
#endif
  got+=size_t(n);
 }return decode({bytes.data(),bytes.size()});}
}
namespace {
using Clock=transport::Clock;
using wire::require;
struct StageRejected:std::domain_error{Status status;
StageRejected(const char* message,Status value):std::domain_error(message),status(value){}};
uint64_t pageSize(){long page=sysconf(_SC_PAGESIZE);
require(page>0,"Resident page size unavailable");
return uint64_t(page);}
void sameIdentity(const Packet& p,const Packet& request){require(p.session==request.session&&p.requestId==request.requestId&&p.epoch==request.epoch&&p.frame.logicalId==request.frame.logicalId&&p.frame.consumedSequence==request.frame.consumedSequence&&p.frame.signalTimestampMs==request.frame.signalTimestampMs&&p.mappedBytes==request.mappedBytes&&p.descriptorCount==request.descriptorCount&&p.presence==request.presence&&p.diagnosticPresence==request.diagnosticPresence,"Resident response identity mismatch");}
class Process final:public Session {
 Config config_;
 Layout layout_;
 GpuProcessOptions options_;
 Nonce nonce_{};
 std::unique_ptr<transport::Child> child_;
 Mapping map_;
 Phase phase_=Phase::idle;
 uint64_t sequence_=0,lastLogical_=0,lastConsumed_=0,controlEvent_=0;
 bool haveFrame_=false;
 Packet active_{};
 Token token_{};
 Clock::time_point deadline_{};
 std::vector<double> delayKm_;
 Packet request(Op op){require(child_&&sequence_!=UINT64_MAX,"Resident session unavailable/exhausted");
 Packet p=active_;
 p.op=op;
 p.requestId=++sequence_;
 p.session=nonce_;
 p.epoch=config_.epoch;
 p.mappedBytes=layout_.bytes;
 p.descriptorCount=layout_.count;
 p.presence=config_.presence;
 p.status=Status::success;
 p.phase=phase_;
 p.restoreTrackerSlot=0;
 p.errorCode=0;
 p.reasonLength=0;
 return p;}
 Packet response(const Packet& p,Op expected,Phase state){auto result=transport::receive(child_->fd(),deadline_);
 sameIdentity(result,p);
 if(result.op==Op::failure||result.op==Op::rejected){auto reason=map_.read(Section::reason);
 require(result.reasonLength<=reason.size,"Resident reason bounds");
 throw std::runtime_error(std::string(reinterpret_cast<const char*>(reason.data),result.reasonLength));}require(result.op==expected&&result.status==Status::success&&result.phase==state,"Resident response phase mismatch");
 validateMapping(map_,config_,nonce_);
 if(expected!=Op::referenceReady&&expected!=Op::framePrepared)controlEvent_=validateControl(config_,p,result,map_.read(Section::controlEvidence),map_.read(Section::device),controlEvent_);
 return result;}
 OutputViews outputs(const Packet& p){validateEvidence(config_,p,map_.read(Section::readyEvidence),map_.read(Section::preparedEvidence),map_.read(Section::device));
 auto v=views(map_,config_,p);
 const auto cp=referenceState(v.referenceCheckpoint);
 require(cp.epoch==config_.epoch&&cp.logicalFrame==p.frame.logicalId&&cp.samples==config_.geometry.clutterSamples&&cp.channels==config_.geometry.channels&&cp.weightCount==config_.reference.channels.size(),"Resident acknowledged reference state differs");
 for(size_t i=cp.weightCount;i<cp.weights.size();++i)require(cp.weights[i]==std::complex<double>{},"Resident unused reference weight nonzero");
 if(p.phase==Phase::prepared){const auto proof=preparedProof(v.preparedEvidence);
 require(v.cfarCount==proof.postprocessStatus[6]&&v.centroidCount==proof.postprocessStatus[8]&&v.detectionCount==proof.postprocessStatus[11],"Resident metrics/device counts differ");
 if(config_.tracker.enabled)require(v.trackerCheckpoint.size==proof.trackerStatus[24]&&v.trackerNoOp==(proof.trackerStatus[2]==1),"Resident tracker result/status differs");}v.delayKm={delayKm_.data(),delayKm_.size()};
 return v;}
 template<class F>decltype(auto) guarded(F&& f){try{return f();}catch(...){stop();
 throw;}}
 void inactive()const{require(child_&&(phase_==Phase::idle||phase_==Phase::settled),"Resident input/state still owned by frame");}
public:
 Process(Config config,GpuProcessOptions options):config_(std::move(config)),layout_(makeLayout(config_,pageSize())),options_(std::move(options)){
  require(options_.argument.empty()||options_.argument=="--resident-v5","Resident worker route must be explicit");
  options_.argument="--resident-v5";
  require(options_.startupMs&&options_.frameMs,"Resident timeout zero");
  if(getentropy(nonce_.data(),sizeof(nonce_))||!(nonce_[0]||nonce_[1]))throw std::runtime_error("Resident session nonce unavailable");
  delayKm_.reserve(config_.delayAxis.size());
  for(auto delay:config_.delayAxis)delayKm_.push_back(1.0*delay*(Constants::c/double(config_.sampleRate))/1000);
  child_=std::make_unique<transport::Child>(layout_.bytes,options_,[&](uint8_t* data,uint64_t size){map_={data,size_t(size),layout_};initializeMapping(map_,config_,nonce_);});
  guarded([&]{deadline_=Clock::now()+std::chrono::milliseconds(options_.startupMs);auto init=request(Op::initialize);transport::sendPacket(child_->fd(),init);response(init,Op::initialized,Phase::idle);});
 }
 ~Process()override{
  // Explicit Quit lets the healthy worker destroy its GPU objects before the
  // bounded reap. Error paths use stop() directly and never claim cleanup.
  if(child_&&(phase_==Phase::idle||phase_==Phase::settled))try{
   deadline_=Clock::now()+std::chrono::milliseconds(options_.startupMs);
   auto p=request(Op::quit);
   transport::sendPacket(child_->fd(),p);
   response(p,Op::settled,Phase::settled);
  }catch(...){}
  stop();
 }
 const Config& config()const override{return config_;}const Layout& layout()const override{return layout_;}Phase phase()const override{return phase_;}
 Span<std::complex<double>> input()override{inactive();
 auto b=map_.bytes(Section::canonical);
 return {reinterpret_cast<std::complex<double>*>(b.data),b.size/16};}
 void begin(Frame frame,uint64_t diagnosticPresence)override{inactive();
 require(!(diagnosticPresence&~diagnosticMask)&&!(diagnosticPresence&~config_.presence),"Resident requested diagnostic mask invalid");
 require(frame.logicalId&&(!haveFrame_||(frame.logicalId>lastLogical_&&frame.consumedSequence>lastConsumed_)),"Resident frame identity not advancing");
 guarded([&]{active_=request(Op::begin);active_.frame=frame;active_.tracker={};active_.diagnosticPresence=diagnosticPresence;deadline_=Clock::now()+std::chrono::milliseconds(options_.frameMs);transport::sendPacket(child_->fd(),active_);phase_=Phase::begun;});}
 OutputViews receiveReferenceReady()override{require(phase_==Phase::begun,"Resident reference acknowledgment out of order");
 return guarded([&]{auto reply=response(active_,Op::referenceReady,Phase::referenceReady);require(wire::same(reply.tracker,{}),"ReferenceReady unexpectedly carries tracker token");phase_=Phase::referenceReady;return outputs(reply);});}
 OutputViews receivePrepared()override{require(phase_==Phase::referenceReady,"Resident prepared acknowledgment out of order");
 return guarded([&]{auto reply=response(active_,Op::framePrepared,Phase::prepared);auto v=outputs(reply);token_=reply.tracker;phase_=Phase::prepared;return v;});}
 void commit(Token token)override{require(phase_==Phase::prepared||phase_==Phase::committed,"Resident commit outside pending frame");
 require(wire::same(token,token_),"Resident commit token differs");
 guarded([&]{auto p=request(Op::commit);p.tracker=token;transport::sendPacket(child_->fd(),p);auto r=response(p,Op::committed,Phase::committed);require(wire::same(r.tracker,token),"Resident commit acknowledgment token differs");phase_=Phase::committed;});}
 void abort(Token token)override{require(phase_==Phase::prepared&&wire::same(token,token_),"Resident abort token/state differs");
 guarded([&]{auto p=request(Op::abort);p.tracker=token;transport::sendPacket(child_->fd(),p);response(p,Op::aborted,Phase::rejected);phase_=Phase::rejected;});}
 void published()override{require(phase_==Phase::committed,"Resident publication before commit");
 guarded([&]{auto p=request(Op::published);p.tracker=token_;transport::sendPacket(child_->fd(),p);response(p,Op::settled,Phase::settled);phase_=Phase::settled;haveFrame_=true;lastLogical_=active_.frame.logicalId;lastConsumed_=active_.frame.consumedSequence;});}
 void restore(ConstBytes reference,Span<const uint32_t> tracker)override{inactive();
 require(reference.data&&reference.size==256,"Resident restore reference shape");
  // Views returned by this Session name mutable shared storage. Reject aliases
  // before clearing either destination, including cross-section aliases.
  auto independent=[&](const void* source,size_t count,size_t width){if(!source){require(!count,"Resident restore null source");
  return;}require(count<=SIZE_MAX/width,"Resident restore source overflow");
  const uintptr_t first=reinterpret_cast<uintptr_t>(source),base=reinterpret_cast<uintptr_t>(map_.data);
  const size_t size=count*width;
  require(size<=UINTPTR_MAX-first&&map_.size<=UINTPTR_MAX-base,"Resident restore address overflow");
  require(!(first<base+map_.size&&base<first+size),"Resident restore source aliases Session mapping; take a checkpoint copy first");};
  independent(reference.data,reference.size,1);
  independent(tracker.data,tracker.size,4);
  guarded([&]{auto r=map_.bytes(Section::referenceCheckpoint);std::memcpy(r.data,reference.data,256);auto p=request(Op::restore);p.frame={};p.tracker={};p.restoreTrackerSlot=0;p.diagnosticPresence=0;if(config_.tracker.enabled){const size_t recordWords=trackerRecordWords(config_.tracker);auto b=map_.bytes(Section::tracker0);require(tracker.data&&tracker.size>=trackerHeaderWords&&tracker.size<=b.size/4&&(tracker.size-trackerHeaderWords)%recordWords==0,"Resident restore checkpoint shape");wire::Reader bank{{reinterpret_cast<const uint8_t*>(tracker.data),tracker.size*4}};require(bank.u64(16)<=config_.tracker.capacity&&tracker.size==trackerHeaderWords+recordWords*bank.u64(16),"Resident restore checkpoint count differs");std::memset(b.data,0,b.size);std::memcpy(b.data,tracker.data,tracker.size*4);p.tracker.generation=bank.u64(48);}else require(!tracker.data&&!tracker.size,"Disabled tracker restore input");deadline_=Clock::now()+std::chrono::milliseconds(options_.startupMs);transport::sendPacket(child_->fd(),p);response(p,Op::restored,Phase::idle);phase_=Phase::idle;haveFrame_=false;lastLogical_=lastConsumed_=0;token_={};});}
 void stop()noexcept override{phase_=Phase::poisoned;
 if(child_)child_->stop();
 child_.reset();
 map_.data=nullptr;
 map_.size=0;}
};
}
std::unique_ptr<Session> createProcess(const Config& c,const GpuProcessOptions& o){return std::make_unique<Process>(c,o);}

int runWorker(Factory create){
 if(!create||!transport::protectWorker())return 2;
 void* address=MAP_FAILED;
 size_t bytes=0;
 Mapping map;
 std::unique_ptr<Backend> backend;
 Packet current{};
 bool havePacket=false;
 Phase phase=Phase::idle;
 try{
  // No caller-controlled descriptor or count is dereferenced before the sealed
  // size and fixed setup prefix have been checked. The complete table is then
  // compared with the locally derived layout, including all unused bytes.
  struct stat info{};
  require(fstat(4,&info)==0&&info.st_size>=1792&&uint64_t(info.st_size)<=sharedLimit,"Resident shared descriptor size invalid");
#ifndef __APPLE__
  const int seals=fcntl(4,F_GET_SEALS);
  require(seals>=0&&(seals&(F_SEAL_GROW|F_SEAL_SHRINK|F_SEAL_SEAL))==(F_SEAL_GROW|F_SEAL_SHRINK|F_SEAL_SEAL),"Resident memory descriptor is not size sealed");
#endif
  bytes=size_t(info.st_size);
  address=mmap(nullptr,bytes,PROT_READ|PROT_WRITE,MAP_SHARED,4,0);
  require(address!=MAP_FAILED,"Resident worker mapping failed");
  close(4);
  auto* data=static_cast<uint8_t*>(address);
  wire::Reader prefix{{data,bytes}},setup{{data+1280,512}};
  const uint64_t delays=setup.u64(64),doppler=setup.u64(56);
  require(delays&&delays<=65535&&doppler&&doppler<=65535,"Resident setup axis bounds invalid");
  const uint64_t delayOffset=1792,dopplerOffset=wire::align(wire::add(delayOffset,wire::mul(delays,4)),64);
  require(dopplerOffset<=bytes&&wire::mul(doppler,8)<=bytes-dopplerOffset,"Resident setup axes outside sealed mapping");
  Config config=decodeConfig({data+1280,512},{data+delayOffset,size_t(delays*4)},{data+dopplerOffset,size_t(doppler*8)});
  const Nonce nonce{prefix.u64(32),prefix.u64(40)};
  require(nonce[0]||nonce[1],"Resident mapping nonce zero");
  map={data,bytes,makeLayout(config,pageSize())};
  require(map.layout.bytes==bytes,"Resident mapped capacity differs");
  validateMapping(map,config,nonce);
  uint64_t lastRequest=0,lastLogical=0,lastConsumed=0;
  bool completed=false;
  Frame frame{};
  Token token{};
  uint64_t frameDiagnostics=0;
  const auto forever=[](){return Clock::now()+std::chrono::hours(24);};
  auto read=[&](){auto p=transport::receive(3,forever());
  current=p;
  havePacket=true;
   require(p.session==nonce&&p.epoch==config.epoch&&p.mappedBytes==bytes&&p.descriptorCount==map.layout.count&&p.presence==config.presence,"Resident request identity mismatch");
   require(p.requestId>lastRequest&&p.status==Status::success&&!p.errorCode&&!p.reasonLength,"Resident request sequence/status invalid");
   lastRequest=p.requestId;
   require(p.phase==phase,"Resident request phase differs");
   require(config.tracker.enabled||!p.restoreTrackerSlot,"Disabled tracker restore selector");
   validateMapping(map,config,nonce);
   return p;};
  auto reply=[&](Packet p,Op op,Phase next){p.op=op;
  p.phase=next;
  p.restoreTrackerSlot=0;
  p.status=Status::success;
  p.errorCode=p.reasonLength=0;
  transport::sendPacket(3,p);
  phase=next;};
  auto init=read();
  require(init.op==Op::initialize&&!init.frame.logicalId&&!init.frame.consumedSequence&&!init.frame.signalTimestampMs&&!init.diagnosticPresence&&wire::same(init.tracker,{}),"Resident first request must initialize");
  backend=create(config,map);
  require(bool(backend),"Resident module factory returned null");
  backend->initializeEvidence(init);
  reply(init,Op::initialized,Phase::idle);
  for(;;){auto p=read();
   if(p.op==Op::quit){require(phase==Phase::idle||phase==Phase::settled,"Resident Quit while frame owned");
   backend->shutdown(p);
   backend.reset();
   reply(p,Op::settled,Phase::settled);
   break;}
   if(p.op==Op::restore){require(phase==Phase::idle||phase==Phase::settled,"Resident Restore while frame owned");
   require(!p.frame.logicalId&&!p.frame.consumedSequence&&!p.frame.signalTimestampMs&&!p.tracker.transaction&&!p.tracker.consumedSequence&&!p.tracker.epoch&&!p.diagnosticPresence,"Resident Restore packet contains frame token");
   backend->restore(p);
   frame={};
   token={};
   completed=false;
   lastLogical=lastConsumed=0;
   reply(p,Op::restored,Phase::idle);
   continue;}
   if(p.op==Op::begin){require((phase==Phase::idle||phase==Phase::settled)&&p.frame.logicalId&&(!completed||(p.frame.logicalId>lastLogical&&p.frame.consumedSequence>lastConsumed))&&wire::same(p.tracker,{}),"Resident Begin frame/state invalid");
   frame=p.frame;
   frameDiagnostics=p.diagnosticPresence;
   phase=Phase::begun;
    if(!backend->prepareReference(p))throw StageRejected("Resident GPU reference rejected the frame",backend->rejectionStatus());
    reply(p,Op::referenceReady,Phase::referenceReady);
    // Ready bytes are immutable after its acknowledgment. Radar may now write
    // only Prepared/output storage; the canonical parent input stays owned.
    if(!backend->finishRadar(p))throw StageRejected("Resident GPU radar rejected the frame",backend->rejectionStatus());
    token=backend->preparedToken();
    require(config.tracker.enabled?(token.epoch==config.epoch&&token.transaction&&token.generation&&token.consumedSequence==frame.consumedSequence):wire::same(token,{}),"Resident prepared token invalid");
    p.tracker=token;
    reply(p,Op::framePrepared,Phase::prepared);
    continue;
   }
   require(p.frame.logicalId==frame.logicalId&&p.frame.consumedSequence==frame.consumedSequence&&p.frame.signalTimestampMs==frame.signalTimestampMs&&p.diagnosticPresence==frameDiagnostics&&wire::same(p.tracker,token),"Resident transaction identity differs");
   if(p.op==Op::commit){require(phase==Phase::prepared||phase==Phase::committed,"Resident Commit without prepared transaction");
   backend->commit(p);
   reply(p,Op::committed,Phase::committed);}
   else if(p.op==Op::abort){require(phase==Phase::prepared,"Resident Abort without prepared transaction");
   backend->abort(p);
   reply(p,Op::aborted,Phase::rejected);}
   else if(p.op==Op::published){require(phase==Phase::committed,"Resident Published before commit");
   backend->settle(p);
   completed=true;
   lastLogical=frame.logicalId;
   lastConsumed=frame.consumedSequence;
   reply(p,Op::settled,Phase::settled);}
   else throw std::invalid_argument("Resident operation out of order");
  }
  munmap(address,bytes);
  close(3);
  return 0;
 }catch(const std::exception& error){
  if(havePacket&&map.data)try{auto reason=map.bytes(Section::reason);
  const size_t length=std::min(reason.size,std::strlen(error.what()));
  std::memset(reason.data,0,reason.size);
  std::memcpy(reason.data,error.what(),length);
  current.op=dynamic_cast<const std::domain_error*>(&error)?Op::rejected:Op::failure;
  const auto* rejected=dynamic_cast<const StageRejected*>(&error);
  current.status=rejected?rejected->status:(current.op==Op::rejected?Status::numeric:Status::runtime);
  current.phase=Phase::poisoned;
  current.errorCode=1;
  current.reasonLength=uint32_t(length);
  current.restoreTrackerSlot=0;
  transport::sendPacket(3,current);}catch(...){}
  backend.reset();
  if(address!=MAP_FAILED)munmap(address,bytes);
  close(3);
  close(4);
  return 1;
 }catch(...){backend.reset();
 if(address!=MAP_FAILED)munmap(address,bytes);
 close(3);
 close(4);
 return 1;}
}
} // namespace blah2::resident
