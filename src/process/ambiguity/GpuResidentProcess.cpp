#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "GpuResidentWire.h"
#include "GpuResidentBackend.h"
#include "GpuResidentProof.h"
#include "capture/Sha256.h"
#include "GpuArtifactBinding.h"
#include "ReplayGpuQualification.h"
#include "GpuWorkerLifetime.h"
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
#include "GpuWorkerTransport.private.h"
namespace blah2::resident {
namespace transport {
using Clock=std::chrono::steady_clock;
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
 while(got<bytes.size()){
 gpu_receive_deadline::waitReadable(fd,deadline,"Resident worker deadline exceeded or poll failed");
#ifdef __APPLE__
  ssize_t n=recv(fd,bytes.data()+got,bytes.size()-got,MSG_DONTWAIT);
#else
  ssize_t n=recv(fd,bytes.data(),bytes.size(),MSG_TRUNC|MSG_DONTWAIT);
#endif
 if(n<0&&(errno==EINTR||errno==EAGAIN||errno==EWOULDBLOCK))continue;
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
 std::unique_ptr<gpu_worker_transport::Child> child_;
 Mapping map_;
 Phase phase_=Phase::idle;
 uint64_t sequence_=0,lastLogical_=0,lastConsumed_=0,controlEvent_=0;
 bool haveFrame_=false;
 Packet active_{};
 Token token_{};
 Hash rawIqHash_{};
 uint32_t snapshotCount_=0;
 uint64_t snapshotLogical_=0;
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
 const auto acknowledged=qualificationIdentity();const auto ready=map_.read(Section::readyEvidence);
 require(std::memcmp(acknowledged.data()+32,ready.data+160,32)==0,
  "Resident frame module image differs from initialized identity");
 auto v=views(map_,config_,p);
 const auto cp=referenceState(v.referenceCheckpoint);
 const bool array=config_.reference.mode==ReferenceMode::capturedArray;
 require(cp.epoch==config_.epoch&&cp.logicalFrame==p.frame.logicalId&&cp.samples==config_.geometry.clutterSamples&&cp.channels==config_.inputChannels&&cp.weightCount==(array?config_.reference.channels.size():0),"Resident acknowledged reference state differs");
 require(array||(!cp.frames&&!cp.updates&&cp.coherentFraction==0&&cp.coherentGainDb==0&&cp.scale==0),"Resident dedicated reference state is not inert");
 for(size_t i=cp.weightCount;i<cp.weights.size();++i)require(cp.weights[i]==std::complex<double>{},"Resident unused reference weight nonzero");
 if(p.phase==Phase::prepared){const auto proof=preparedProof(v.preparedEvidence);
 require(v.cfarCount==proof.postprocessStatus[6]&&v.centroidCount==proof.postprocessStatus[8]&&v.detectionCount==proof.postprocessStatus[11],"Resident metrics/device counts differ");
 require(proof.postprocessStatus[28]==uint32_t(config_.detector.integrationCpis>1&&v.availableCpis==config_.detector.integrationCpis),
  "Resident acknowledged temporal readiness/device history differ");
 if(v.cfar.data)require(v.cfar.size==proof.postprocessStatus[16],"Resident CFAR diagnostic union/status differ");
 if(v.centroid.data)require(v.centroid.size==proof.postprocessStatus[17],"Resident centroid diagnostic union/status differ");
 if(config_.tracker.enabled){require(v.trackerCheckpoint.size==proof.trackerStatus[24]&&v.trackerNoOp==(proof.trackerStatus[2]==1),"Resident tracker result/status differs");
  v.trackerDiagnostics=proof.trackerDiagnostics;}}v.delayKm={delayKm_.data(),delayKm_.size()};
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
  child_=std::make_unique<gpu_worker_transport::Child>(gpu_worker_transport::resident,layout_.bytes,options_,[&](uint8_t* data,uint64_t size){map_={data,size_t(size),layout_};initializeMapping(map_,config_,nonce_);});
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
  if(child_&&options_.offlineTransportTrace)child_->stop(true);
  stop();
 }
 const Config& config()const override{return config_;}const Layout& layout()const override{return layout_;}Phase phase()const override{return phase_;}
 std::array<uint8_t,96> qualificationIdentity()const override{
  std::array<uint8_t,96> identity{};const auto config=wire::configHash(config_);
  std::copy(config.begin(),config.end(),identity.begin());
  const auto control=map_.read(Section::controlEvidence);
  require(control.size==controlEvidenceBytes,"Resident qualification identity lacks acknowledged module hash");
  std::copy_n(control.data+112,16,identity.begin()+32);
  std::copy_n(control.data+488,16,identity.begin()+48);
  const auto device=map_.read(Section::device);blah2::Sha256 hash;
  hash.update(device.data,device.size);const auto digest=hash.finishBytes();
  std::copy(digest.begin(),digest.end(),identity.begin()+64);return identity;
 }
 Span<std::complex<double>> input()override{inactive();
 auto b=map_.bytes(Section::canonical);
 return {reinterpret_cast<std::complex<double>*>(b.data),b.size/16};}
 void begin(Frame frame,uint64_t diagnosticPresence)override{inactive();
 require(!(diagnosticPresence&~diagnosticMask)&&!(diagnosticPresence&~config_.presence),"Resident requested diagnostic mask invalid");
 require(frame.logicalId&&(!haveFrame_||(frame.logicalId>lastLogical_&&frame.consumedSequence>lastConsumed_)),"Resident frame identity not advancing");
 if(config_.replay.maxBytes){const auto input=map_.read(Section::canonical);blah2::Sha256 hash;
  hash.update(input.data,input.size);rawIqHash_=hash.finishBytes();}
 guarded([&]{active_=request(Op::begin);active_.frame=frame;active_.tracker={};active_.diagnosticPresence=diagnosticPresence;deadline_=Clock::now()+std::chrono::milliseconds(options_.frameMs);transport::sendPacket(child_->fd(),active_);phase_=Phase::begun;});}
 OutputViews receiveReferenceReady()override{require(phase_==Phase::begun,"Resident reference acknowledgment out of order");
 return guarded([&]{auto reply=response(active_,Op::referenceReady,Phase::referenceReady);require(wire::same(reply.tracker,{}),"ReferenceReady unexpectedly carries tracker token");phase_=Phase::referenceReady;return outputs(reply);});}
 OutputViews receivePrepared()override{require(phase_==Phase::referenceReady,"Resident prepared acknowledgment out of order");
 return guarded([&]{auto reply=response(active_,Op::framePrepared,Phase::prepared);auto v=outputs(reply);token_=reply.tracker;phase_=Phase::prepared;return v;});}
 void commit(Token token)override{require(phase_==Phase::prepared||phase_==Phase::committed,"Resident commit outside pending frame");
 require(wire::same(token,token_),"Resident commit token differs");
 // Prepared has completed the GPU work. Host oracles/publication may take
 // arbitrarily longer; each new control request owns a bounded ACK window.
 guarded([&]{auto p=request(Op::commit);p.tracker=token;deadline_=Clock::now()+std::chrono::milliseconds(options_.frameMs);transport::sendPacket(child_->fd(),p);auto r=response(p,Op::committed,Phase::committed);require(wire::same(r.tracker,token),"Resident commit acknowledgment token differs");phase_=Phase::committed;});}
 void abort(Token token)override{require(phase_==Phase::prepared&&wire::same(token,token_),"Resident abort token/state differs");
 guarded([&]{auto p=request(Op::abort);p.tracker=token;deadline_=Clock::now()+std::chrono::milliseconds(options_.frameMs);transport::sendPacket(child_->fd(),p);response(p,Op::aborted,Phase::rejected);phase_=Phase::rejected;});}
 void published()override{require(phase_==Phase::committed,"Resident publication before commit");
 guarded([&]{auto p=request(Op::published);p.tracker=token_;deadline_=Clock::now()+std::chrono::milliseconds(options_.frameMs);transport::sendPacket(child_->fd(),p);response(p,Op::settled,Phase::settled);phase_=Phase::settled;haveFrame_=true;lastLogical_=active_.frame.logicalId;lastConsumed_=active_.frame.consumedSequence;});}
 std::vector<uint8_t> snapshot()override{inactive();
  require(config_.replay.maxBytes&&phase_==Phase::settled&&haveFrame_&&
   snapshotCount_<config_.replay.maxFrames&&snapshotLogical_!=active_.frame.logicalId,
   "Resident snapshot requires a new settled frame within its bound");
  return guarded([&]{auto p=request(Op::snapshot);p.tracker=token_;
   deadline_=Clock::now()+std::chrono::milliseconds(options_.startupMs);
   transport::sendPacket(child_->fd(),p);response(p,Op::snapshotted,Phase::settled);
   const auto evidence=map_.read(Section::preparedEvidence);const auto proof=preparedProof(evidence);
   resident_replay::Identity expected{config_.epoch,p.frame.logicalId,proof.residentGeneration,
    p.frame.consumedSequence,p.frame.signalTimestampMs,token_.transaction,wire::configHash(config_),{},rawIqHash_};
   std::memcpy(expected.moduleHash.data(),evidence.data+160,32);
   const auto device=map_.read(Section::device);blah2::Sha256 deviceHash;
   deviceHash.update(device.data,device.size);expected.deviceHash=deviceHash.finishBytes();
   const auto source=map_.read(Section::replaySnapshot);
   std::vector<uint8_t> copy(source.data,source.data+source.size);
   resident_replay::decode(copy.data(),copy.size(),config_,expected);
   ++snapshotCount_;snapshotLogical_=p.frame.logicalId;return copy;});
 }
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
 void resetInput()override{inactive();
  guarded([&]{auto p=request(Op::resetInput);p.frame={};p.tracker={};p.diagnosticPresence=0;
   deadline_=Clock::now()+std::chrono::milliseconds(options_.frameMs);
   transport::sendPacket(child_->fd(),p);const auto reply=response(p,Op::inputReset,Phase::idle);
   require(wire::same(reply.tracker,{}),"Resident input reset returned a stale token");
   phase_=Phase::idle;active_={};token_={};rawIqHash_={};
   // Keep haveFrame_/last IDs and snapshot bounds: a gap cannot replay a token.
  });
 }
 void stop()noexcept override{phase_=Phase::poisoned;
 if(child_)child_->stop();
 child_.reset();
 map_.data=nullptr;
 map_.size=0;}
};
}
std::unique_ptr<Session> createProcess(const Config& c,const GpuProcessOptions& o){return std::make_unique<Process>(c,o);}

int runWorker(Factory create){
 if(!create||!gpu_worker_lifetime::protectWorker())return 2;
 void* address=MAP_FAILED;
 size_t bytes=0;
 Mapping map;
 std::unique_ptr<Backend> backend;
 Packet current{};
 bool havePacket=false;
 Phase phase=Phase::idle;
 try{
  gpu_artifact_binding::workerStart();
  replay_gpu_qualification::Worker qualification("resident");
  // No caller-controlled descriptor or count is dereferenced before the sealed
  // size and fixed setup prefix have been checked. The complete table is then
  // compared with the locally derived layout, including all unused bytes.
  struct stat info{};
  require(fstat(4,&info)==0&&info.st_size>=1280+configBytes&&uint64_t(info.st_size)<=replaySharedLimit,"Resident shared descriptor size invalid");
#ifndef __APPLE__
  const int seals=fcntl(4,F_GET_SEALS);
  require(seals>=0&&(seals&(F_SEAL_GROW|F_SEAL_SHRINK|F_SEAL_SEAL))==(F_SEAL_GROW|F_SEAL_SHRINK|F_SEAL_SEAL),"Resident memory descriptor is not size sealed");
#endif
  bytes=size_t(info.st_size);
  address=mmap(nullptr,bytes,PROT_READ|PROT_WRITE,MAP_SHARED,4,0);
  require(address!=MAP_FAILED,"Resident worker mapping failed");
  close(4);
  auto* data=static_cast<uint8_t*>(address);
  wire::Reader prefix{{data,bytes}},setup{{data+1280,configBytes}};
  const uint64_t delays=setup.u64(64),doppler=setup.u64(56);
  require(delays&&delays<=65535&&doppler&&doppler<=65535,"Resident setup axis bounds invalid");
  const uint64_t delayOffset=wire::align(1280+configBytes,64),dopplerOffset=wire::align(wire::add(delayOffset,wire::mul(delays,4)),64);
  require(dopplerOffset<=bytes&&wire::mul(doppler,8)<=bytes-dopplerOffset,"Resident setup axes outside sealed mapping");
  Config config=decodeConfig({data+1280,configBytes},{data+delayOffset,size_t(delays*4)},{data+dopplerOffset,size_t(doppler*8)});
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
  qualification.device(config.device);
  if(qualification.hit("startup"))throw std::runtime_error("Replay qualification resident startup rejection");
  qualification.hit("allocation"); // Exercise the real allocator with its one-byte ceiling.
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
   if(p.op==Op::resetInput){require(phase==Phase::idle||phase==Phase::settled,"Resident input reset while frame owned");
   require(!p.frame.logicalId&&!p.frame.consumedSequence&&!p.frame.signalTimestampMs&&wire::same(p.tracker,{})&&!p.diagnosticPresence,"Resident input reset contains frame token");
   backend->resetInput(p);
   frame={};token={};frameDiagnostics=0;
   // Preserve completed/last IDs across a capture gap, unlike checkpoint restore.
   reply(p,Op::inputReset,Phase::idle);
   continue;}
   if(p.op==Op::begin){require((phase==Phase::idle||phase==Phase::settled)&&p.frame.logicalId&&(!completed||(p.frame.logicalId>lastLogical&&p.frame.consumedSequence>lastConsumed))&&wire::same(p.tracker,{}),"Resident Begin frame/state invalid");
   frame=p.frame;
   frameDiagnostics=p.diagnosticPresence;
   phase=Phase::begun;
    qualification.beforeFrame(frame.logicalId,frame.consumedSequence);
    if(!backend->prepareReference(p))throw StageRejected("Resident GPU reference rejected the frame",backend->rejectionStatus());
    if(qualification.hit("prepare",frame.logicalId,frame.consumedSequence))
     throw std::runtime_error("Replay qualification resident prepare rejection");
    reply(p,Op::referenceReady,Phase::referenceReady);
    // Ready bytes are immutable after its acknowledgment. Radar may now write
    // only Prepared/output storage; the canonical parent input stays owned.
    if(!backend->finishRadar(p))throw StageRejected("Resident GPU radar rejected the frame",backend->rejectionStatus());
    if(qualification.hit("numeric-rejection",frame.logicalId,frame.consumedSequence))
     wire::Writer{map.bytes(Section::metrics)}.f64(24,std::numeric_limits<double>::quiet_NaN());
    token=backend->preparedToken();
    require(config.tracker.enabled?(token.epoch==config.epoch&&token.transaction&&token.generation&&token.consumedSequence==frame.consumedSequence):wire::same(token,{}),"Resident prepared token invalid");
    p.tracker=token;
    qualification.complete("frame-prepared",frame.logicalId,frame.consumedSequence);
    reply(p,Op::framePrepared,Phase::prepared);
    continue;
   }
   require(p.frame.logicalId==frame.logicalId&&p.frame.consumedSequence==frame.consumedSequence&&p.frame.signalTimestampMs==frame.signalTimestampMs&&p.diagnosticPresence==frameDiagnostics&&wire::same(p.tracker,token),"Resident transaction identity differs");
   if(p.op==Op::commit){require(phase==Phase::prepared||phase==Phase::committed,"Resident Commit without prepared transaction");
   backend->commit(p);
   if(qualification.hit("commit",frame.logicalId,frame.consumedSequence))
    throw std::runtime_error("Replay qualification resident commit acknowledgement rejection");
   qualification.complete("frame-committed",frame.logicalId,frame.consumedSequence);
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
   else if(p.op==Op::snapshot){require(phase==Phase::settled&&completed&&config.replay.maxBytes,
    "Resident Snapshot before settlement or without opt-in");
   backend->snapshot(p);
   reply(p,Op::snapshotted,Phase::settled);}
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

// Map-only capability shares the canonical worker lifetime/isolation owner,
// but has its own protocol and never reinterprets a resident IQ mapping.
namespace blah2::temporal_gpu {
namespace {
using Clock=std::chrono::steady_clock;
struct Message {
 uint32_t tag=protocol,op=0;
 uint64_t nonce=0,sequence=0;
 Metrics metrics{};
 char reason[256]{};
};
void sendMessage(int fd,const Message& message){
 const auto* p=reinterpret_cast<const uint8_t*>(&message);size_t left=sizeof(message);
 while(left){const auto n=send(fd,p,left,MSG_NOSIGNAL|MSG_DONTWAIT);
  if(n<0&&errno==EINTR)continue;
  if(n<=0)throw std::runtime_error("Temporal GPU worker send failed");
  p+=n;left-=size_t(n);
 }
}
Message receiveMessage(int fd,Clock::time_point deadline){
 Message message{};auto* p=reinterpret_cast<uint8_t*>(&message);size_t left=sizeof(message);
 while(left){gpu_receive_deadline::waitReadable(fd,deadline,"Temporal GPU worker deadline exceeded");
  const auto n=recv(fd,p,left,MSG_DONTWAIT);
  if(n<0&&(errno==EINTR||errno==EAGAIN||errno==EWOULDBLOCK))continue;
  if(n<=0)throw std::runtime_error("Temporal GPU worker stopped");
  p+=n;left-=size_t(n);
 }
 if(message.tag!=protocol)throw std::runtime_error("Temporal GPU protocol mismatch");
 return message;
}
class Process final:public Session {
 Layout layout_;
 GpuProcessOptions options_;
 std::unique_ptr<gpu_worker_transport::Child> child_;
 uint8_t* data_=nullptr;
 uint64_t nonce_=0,last_=0;
 Output output_;
 Message exchange(uint32_t op,uint64_t sequence,uint32_t timeout){
  try{Message request{};request.op=op;request.nonce=nonce_;request.sequence=sequence;
   sendMessage(child_->fd(),request);
   auto reply=receiveMessage(child_->fd(),Clock::now()+std::chrono::milliseconds(timeout));
   if(reply.nonce!=nonce_||reply.sequence!=sequence)
    throw std::runtime_error("Temporal GPU response identity mismatch");
   if(reply.op!=op){reply.reason[sizeof(reply.reason)-1]=0;
    throw std::runtime_error(std::string("Temporal GPU worker rejected: ")+reply.reason);}
   return reply;
  }catch(...){child_.reset();data_=nullptr;throw;}
 }
public:
 Process(const Config& config,GpuProcessOptions options)
  :layout_(layout(uint32_t(config.dopplerAxis.size()),uint32_t(config.delayAxis.size()))),options_(std::move(options)){
  validate(config);
  if(!options_.argument.empty()&&options_.argument!="--temporal-v1")
   throw std::invalid_argument("Temporal worker route must be explicit");
  options_.argument="--temporal-v1";
  if(getentropy(&nonce_,sizeof(nonce_))||!nonce_)throw std::runtime_error("Temporal GPU nonce unavailable");
  child_=std::make_unique<gpu_worker_transport::Child>(gpu_worker_transport::resident,layout_.bytes,options_,
   [&](uint8_t* p,uint64_t bytes){data_=p;Header header{};
    header.rows=uint32_t(config.dopplerAxis.size());header.columns=uint32_t(config.delayAxis.size());
    header.sampleRate=config.sampleRate;header.detector=config.detector;header.drift=config.delayDriftPerHz;
    header.bytes=bytes;header.nonce=nonce_;std::memcpy(header.device,config.device.c_str(),config.device.size());
    std::memcpy(p,&header,sizeof(header));
    std::memcpy(p+layout_.delay,config.delayAxis.data(),config.delayAxis.size()*4);
    std::memcpy(p+layout_.doppler,config.dopplerAxis.data(),config.dopplerAxis.size()*8);
   },config.device);
  output_=outputs(data_,layout_);exchange(1,0,options_.startupMs);
 }
 ~Process()override{if(child_)try{exchange(4,last_,std::min(500u,options_.startupMs));}catch(...){} }
 std::complex<double>* input()override{
  if(!data_)throw std::runtime_error("Temporal GPU worker unavailable");
  return reinterpret_cast<std::complex<double>*>(data_+layout_.input);
 }
 const Output& process(uint64_t sequence)override{
  if(!child_||!sequence||sequence<=last_)throw std::runtime_error("Temporal GPU sequence is not increasing");
  const auto reply=exchange(2,sequence,options_.frameMs);last_=sequence;output_.metrics=reply.metrics;
  if(output_.metrics.count>layout_.count||!output_.metrics.available||output_.metrics.available>8)
   throw std::runtime_error("Temporal GPU output population invalid");
  return output_;
 }
 void reset()override{if(!child_)throw std::runtime_error("Temporal GPU worker unavailable");
  exchange(3,last_,options_.frameMs);}
};
}
std::unique_ptr<Session> createProcess(const Config& c,const GpuProcessOptions& o){return std::make_unique<Process>(c,o);}
int runWorker(const Factory& factory){
 if(!factory||!gpu_worker_lifetime::protectWorker())return 2;
 void* address=MAP_FAILED;size_t bytes=0;Message current{};bool received=false;
 std::unique_ptr<Backend> backend;
 try{
  gpu_artifact_binding::workerStart();struct stat info{};
  if(fstat(4,&info)||info.st_size<int64_t(sizeof(Header))||uint64_t(info.st_size)>(128ULL<<20))
   throw std::runtime_error("Temporal GPU shared size invalid");
#ifndef __APPLE__
  const int seals=fcntl(4,F_GET_SEALS),required=F_SEAL_GROW|F_SEAL_SHRINK|F_SEAL_SEAL;
  if(seals<0||(seals&required)!=required)throw std::runtime_error("Temporal GPU mapping is not size sealed");
#endif
  bytes=size_t(info.st_size);address=mmap(nullptr,bytes,PROT_READ|PROT_WRITE,MAP_SHARED,4,0);
  if(address==MAP_FAILED)throw std::runtime_error("Temporal GPU mapping failed");
  close(4);auto* data=static_cast<uint8_t*>(address);Header header{};std::memcpy(&header,data,sizeof(header));
  const auto l=layout(header.rows,header.columns);
  if(header.tag!=protocol||header.bytes!=bytes||l.bytes!=bytes||!header.nonce||
     !std::memchr(header.device,0,sizeof(header.device)))throw std::runtime_error("Temporal GPU setup identity invalid");
  Config config;config.detector=header.detector;config.sampleRate=header.sampleRate;
  config.delayDriftPerHz=header.drift;config.device=header.device;
  config.delayAxis.resize(header.columns);config.dopplerAxis.resize(header.rows);
  std::memcpy(config.delayAxis.data(),data+l.delay,header.columns*4);
  std::memcpy(config.dopplerAxis.data(),data+l.doppler,header.rows*8);validate(config);
  auto out=outputs(data,l);uint64_t last=0;bool initialized=false;
  for(;;){current=receiveMessage(3,Clock::now()+std::chrono::hours(24));received=true;
   if(current.nonce!=header.nonce||std::memcmp(data,&header,sizeof(header)))
    throw std::runtime_error("Temporal GPU request identity changed");
   if(!initialized){if(current.op!=1||current.sequence)throw std::runtime_error("Temporal GPU initialize required");
    backend=factory(config);if(!backend)throw std::runtime_error("Temporal GPU factory returned null");initialized=true;
   }else if(current.op==2){if(!current.sequence||current.sequence<=last)
     throw std::runtime_error("Temporal GPU input sequence replayed");
    backend->process(reinterpret_cast<const std::complex<double>*>(data+l.input),out);
    current.metrics=out.metrics;last=current.sequence;
   }else if((current.op==3||current.op==4)&&current.sequence==last){
    if(current.op==3)backend->reset();else backend.reset();
   }else throw std::runtime_error("Temporal GPU operation out of order");
   sendMessage(3,current);if(current.op==4)break;
  }
  munmap(address,bytes);close(3);return 0;
 }catch(const std::exception& error){
  if(received)try{current.op=0;std::strncpy(current.reason,error.what(),sizeof(current.reason)-1);sendMessage(3,current);}catch(...){}
  backend.reset();if(address!=MAP_FAILED)munmap(address,bytes);close(3);close(4);return 1;
 }
}
} // namespace blah2::temporal_gpu
