#include "GpuResidentWire.h"
#include "GpuResidentBackend.h"
#include "GpuResidentQualification.h"
#include "CpuEvidenceDouble.h"
#include "ResidentReplayFixture.h"
#include "WorkerIsolationFixture.h"
#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

// This target never loads a module. An explicit executable always names this
// CPU double; an accidental production-worker lookup is a test failure.
namespace blah2 {std::string gpuSiblingPath(const char*){throw std::runtime_error("CPU fixture must name its own executable");}}
// Exercise the exact private transport, without exporting a public API or
// introducing clock hooks into the numerical/module contracts.
namespace blah2::resident::transport {
 Packet receive(int,std::chrono::steady_clock::time_point);
 void sendPacket(int,const Packet&);
}
using namespace blah2::resident;
namespace {
unsigned checks=0,cases=0;
void check(bool yes,const char* why){++checks;if(!yes)throw std::runtime_error(why);}
template<class F>void reject(F f,const char* why){bool failed=false;try{f();}catch(const std::exception&){failed=true;}check(failed,why);}
Config configuration(){
 Config c;c.inputChannels=2;auto&g=c.geometry;g.range=12;g.doppler=7;g.delays=3;g.channels=2;g.delayMin=-1;g.clutterSamples=64;g.clutterBins=4;
 c.correlation=9;c.sampleRate=128000;c.epoch=9;c.surveillanceChannels={0,1};c.delayAxis={-1,0,1};
 const double r=1.0/(63.0/128000);c.dopplerAxis={-3*r,-2*r,-r,0,r,2*r,3*r};c.spectrumBins=64;c.reference.channels={0,1};c.reference.analysisSamples=64;
 c.detector.enabled=true;c.detector.guard=1;c.detector.train=2;c.detector.pfa=.001;c.detector.centroidDopplerResolution=2;
 c.tracker.enabled=true;c.tracker.capacity=4;c.tracker.m=2;c.tracker.n=3;c.tracker.nDelete=2;c.tracker.cpi=63.0/128000;c.tracker.rangeRes=1;c.tracker.lambda=1;
 c.device="cpu-test-double";c.presence=wire::mandatoryPresence(c)|bit(Section::filtered);return c;
}
class Double final:public Backend {
 Config c_;Mapping m_;std::string mode_;Token token_{};CpuEvidenceDouble proof_;
public:
 Double(const Config&c,const Mapping&m):c_(c),m_(m),mode_(std::getenv("VECTORWARP_TEST_RESIDENT_MODE")?std::getenv("VECTORWARP_TEST_RESIDENT_MODE"):"normal"),proof_{c_,m_}{}
 void initializeEvidence(const Packet&p)override{
  if (const char* inherited = std::getenv("BLAH2_TEST_INHERITED_FD")) {
   check(fcntl(std::atoi(inherited), F_GETFD) < 0 && errno == EBADF,
     "Resident worker inherited a parent descriptor");
   check(fcntl(4, F_GETFD) < 0 && errno == EBADF,
     "Resident worker retained the shared-memory descriptor");
  }
  if(mode_=="init-error")throw std::runtime_error("injected init error");
  if(mode_=="init-timeout")std::this_thread::sleep_for(std::chrono::seconds(5));
  proof_.device();proof_.control(p,Phase::idle);
  if(mode_=="image-change")wire::Writer{m_.bytes(Section::controlEvidence)}.u32(112,2);
  if(mode_=="image-absent"){
   std::fill_n(m_.bytes(Section::controlEvidence).data+112,16,0);
   std::fill_n(m_.bytes(Section::controlEvidence).data+488,16,0);
  }
  if(mode_=="driver-change")wire::Writer{m_.bytes(Section::device)}.u32(16,2);
  if(mode_=="budget-change"){
   wire::Writer{m_.bytes(Section::device)}.u64(56,(1<<20)+4096);
   wire::Writer{m_.bytes(Section::controlEvidence)}.u64(432,(1<<20)+4096);
  }
 }
 bool prepareReference(const Packet&p)override{
  if(mode_=="reference-error")return false;
  if(mode_=="mask-substitution"){
   auto wrong=p;wrong.op=Op::referenceReady;wrong.phase=Phase::referenceReady;wrong.diagnosticPresence=bit(Section::filtered);const auto raw=encode(wrong);size_t sent=0;
   while(sent<raw.size()){const auto n=send(3,raw.data()+sent,raw.size()-sent,MSG_NOSIGNAL);if(n<=0)_exit(9);sent+=size_t(n);}
   std::this_thread::sleep_for(std::chrono::seconds(5));
  }
  proof_.ready(p);return true;
 }
 bool finishRadar(const Packet&p)override{
  if(mode_=="radar-exit")_exit(7);
  if(mode_=="frame-timeout")std::this_thread::sleep_for(std::chrono::seconds(5));
  token_={c_.epoch,17,p.frame.logicalId,p.frame.consumedSequence};auto bytes=m_.bytes(Section::metrics);wire::Writer w{bytes};w.zero();
  w.u32(0,1);w.u32(4,128);w.u64(8,p.frame.logicalId);w.u64(16,p.frame.consumedSequence);w.u64(64,32);w.u32(72,1);w.u32(80,c_.stages());w.u32(84,c_.stages());
  w.u32(104,1);
  auto bank=m_.bytes(Section::tracker1);wire::Writer t{bank};t.zero();t.u32(0,5);t.u32(108,uint32_t(c_.detector.integrationCpis>1));t.u32(4,trackerRecordWords(c_.tracker)*4);t.u32(8,c_.tracker.capacity);t.u32(12,trackerHistoryCapacity(c_.tracker));t.u64(40,c_.epoch);t.u64(48,p.frame.logicalId);t.u64(56,p.frame.consumedSequence);proof_.prepared(p);
  // An unused numerical output identifies the exact worker without a new API.
  wire::Writer{m_.bytes(Section::spectrum)}.f64(0,double(getpid()));return true;
 }
 Token preparedToken()const override{return token_;}
 void commit(const Packet&p)override{if(mode_=="commit-exit")_exit(8);if(mode_=="commit-timeout")std::this_thread::sleep_for(std::chrono::seconds(5));proof_.control(p,Phase::committed);}
 void abort(const Packet&p)override{proof_.control(p,Phase::rejected);}
 void restore(const Packet&p)override{check(p.restoreTrackerSlot==0,"Restore did not use input slot");proof_.control(p,Phase::idle);}
 void resetInput(const Packet&p)override{
  if(mode_=="reset-error")throw std::runtime_error("injected input reset error");
  if(mode_=="reset-timeout")std::this_thread::sleep_for(std::chrono::seconds(5));
  proof_.readyCalls=0;token_={};proof_.control(p,Phase::idle);
  if(mode_=="reset-coverage")wire::Writer{m_.bytes(Section::controlEvidence)}.u32(480,1);
  if(mode_=="reset-token"){auto wrong=p;wrong.op=Op::inputReset;wrong.phase=Phase::idle;
   wrong.tracker={c_.epoch,17,1,101};const auto raw=encode(wrong);size_t sent=0;
   while(sent<raw.size()){const auto n=send(3,raw.data()+sent,raw.size()-sent,MSG_NOSIGNAL);if(n<=0)_exit(9);sent+=size_t(n);}}
 }
 void settle(const Packet&p)override{if(mode_=="published-timeout")std::this_thread::sleep_for(std::chrono::seconds(5));proof_.control(p,Phase::settled);}
 void snapshot(const Packet&p)override{namespace rr=blah2::resident_replay;
  auto s=rr::test::fixture(c_,1,p.frame,p.tracker.transaction);
  const auto raw=m_.read(Section::canonical);s.blocks[13].bytes.assign(raw.data,raw.data+raw.size);
  blah2::Sha256 hash;hash.update(raw.data,raw.size);s.identity.rawIqHash=hash.finishBytes();
  const auto device=m_.read(Section::device);blah2::Sha256 deviceHash;deviceHash.update(device.data,device.size);s.identity.deviceHash=deviceHash.finishBytes();
  auto encoded=rr::encode(s,c_);if(mode_=="snapshot-corrupt")encoded.back()^=1;
  const auto out=m_.bytes(Section::replaySnapshot);std::memcpy(out.data,encoded.data(),encoded.size());proof_.control(p,Phase::settled);}
 void shutdown(const Packet&p)override{proof_.control(p,Phase::settled);}
};
void reaped(){
 const auto deadline=std::chrono::steady_clock::now()+std::chrono::milliseconds(300);
 for(;;){int status=0;errno=0;const pid_t p=waitpid(-1,&status,WNOHANG);check(p<=0,"Fixture had to reap a leaked worker");if(p==-1&&errno==ECHILD)return;check(std::chrono::steady_clock::now()<deadline,"Worker remained after bounded cleanup");std::this_thread::sleep_for(std::chrono::milliseconds(1));}
}
std::unique_ptr<Session> session(const std::string&exe,const char*mode){setenv("VECTORWARP_TEST_RESIDENT_MODE",mode,1);blah2::GpuProcessOptions o;o.executable=exe;o.startupMs=std::strcmp(mode,"init-timeout")==0?200:2000;o.frameMs=std::strstr(mode,"timeout")?200:2000;return createProcess(configuration(),o);}
OutputViews prepared(Session&s,uint64_t id,uint64_t diagnostics=0){auto input=s.input();std::fill_n(input.data,input.size,std::complex<double>{2,3});s.begin({id,id+100,1000+id},diagnostics);const auto ready=s.receiveReferenceReady();std::array<uint8_t,256> saved{};std::copy_n(ready.referenceCheckpoint.data,saved.size(),saved.data());auto v=s.receivePrepared();check(std::memcmp(saved.data(),v.referenceCheckpoint.data,256)==0,"Prepared mutated acknowledged reference bytes");return v;}
void deadlineCases(const std::string& exe){
 ++cases;
 int pair[2];
#ifdef __APPLE__
 const int type=SOCK_STREAM;
#else
 const int type=SOCK_SEQPACKET;
#endif
 check(socketpair(AF_UNIX,type,0,pair)==0,"Resident deadline fixture socket failed");
 Packet queued;queued.session={7,8};queued.requestId=1;queued.op=Op::framePrepared;queued.phase=Phase::prepared;
 transport::sendPacket(pair[1],queued);
 const auto expired=std::chrono::steady_clock::now()-std::chrono::seconds(1);
 const auto actual=transport::receive(pair[0],expired);
 check(actual.requestId==queued.requestId&&actual.op==queued.op,"Queued resident reply rejected after deadline");
 const auto begin=std::chrono::steady_clock::now();
 reject([&]{transport::receive(pair[0],expired);},"Expired empty resident queue accepted");
 check(std::chrono::steady_clock::now()-begin<std::chrono::milliseconds(100),"Expired resident work received a new wait");
 close(pair[0]);close(pair[1]);

 setenv("VECTORWARP_TEST_RESIDENT_MODE","normal",1);
 blah2::GpuProcessOptions options;options.executable=exe;options.startupMs=2000;options.frameMs=100;
 auto s=createProcess(configuration(),options);auto v=prepared(*s,1);
 // GPU completion is already proven. Delays now represent only host oracle
 // and publication work, not more time granted to an outstanding GPU frame.
 std::this_thread::sleep_for(std::chrono::milliseconds(150));s->commit(v.trackerToken);
 std::this_thread::sleep_for(std::chrono::milliseconds(150));s->published();
 std::array<uint8_t,256> reference{};std::copy_n(v.referenceCheckpoint.data,reference.size(),reference.data());
 std::array<uint32_t,32> tracker{};std::copy_n(v.trackerCheckpoint.data,tracker.size(),tracker.data());
 std::this_thread::sleep_for(std::chrono::milliseconds(150));
 s->restore({reference.data(),reference.size()},{tracker.data(),tracker.size()});
 std::this_thread::sleep_for(std::chrono::milliseconds(150));s->resetInput();
 check(s->phase()==Phase::idle,"Delayed control acknowledgments poisoned the session");
 s.reset();reaped();
 s=createProcess(configuration(),options);v=prepared(*s,1);
 std::this_thread::sleep_for(std::chrono::milliseconds(150));s->abort(v.trackerToken);
 check(s->phase()==Phase::rejected,"Delayed abort inherited the frame deadline");s.reset();reaped();
}
void normal(const std::string&exe){
 ++cases;auto s=session(exe,"normal");auto v=prepared(*s,UINT64_C(0x100000001),bit(Section::filtered));
 {const auto cp=referenceState(v.referenceCheckpoint);check(cp.frames==0&&cp.updates==1,"First Ready source counters changed");}
 reject([&]{s->input();},"Input writable while frame owned");reject([&]{s->published();},"Publication allowed before commit");
 auto wrong=v.trackerToken;++wrong.transaction;reject([&]{s->commit(wrong);},"Wrong transaction accepted");check(s->phase()==Phase::prepared,"Rejected local token changed transaction");
 s->commit(v.trackerToken);check(wire::Reader{v.controlEvidence}.u32(8)==uint32_t(Phase::committed),"Commit not acknowledged");s->published();check(wire::Reader{v.controlEvidence}.u32(8)==uint32_t(Phase::settled),"Publication not settled");
 const std::vector<uint8_t> settledEvidence(v.controlEvidence.data,
  v.controlEvidence.data+v.controlEvidence.size);
 reject([&]{s->begin({UINT64_C(0x100000001),UINT64_C(0x100000001)+100,2000},0);},
  "Settled session admitted reused frame identity");
 reject([&]{s->begin({UINT64_C(0x100000002),UINT64_C(0x100000001)+100,2000},0);},
  "Settled session admitted reused consumed identity");
 reject([&]{s->begin({UINT64_C(0x100000002),UINT64_C(0x100000001)+99,2000},0);},
  "Settled session admitted reversed consumed identity");
 check(s->phase()==Phase::settled&&
  std::equal(settledEvidence.begin(),settledEvidence.end(),v.controlEvidence.data),
  "Rejected settled identity changed published/committed state");
 std::array<uint8_t,256> reference{};std::copy_n(v.referenceCheckpoint.data,reference.size(),reference.data());std::array<uint32_t,32> tracker{};std::copy_n(v.trackerCheckpoint.data,tracker.size(),tracker.data());
 const auto before=tracker;
 reject([&]{s->restore(v.referenceCheckpoint,{tracker.data(),tracker.size()});},"Aliased reference restore accepted");
 reject([&]{s->restore({reference.data(),reference.size()},v.trackerCheckpoint);},"Aliased tracker restore accepted");
 check(std::memcmp(reference.data(),v.referenceCheckpoint.data,256)==0&&std::equal(before.begin(),before.end(),v.trackerCheckpoint.data),"Rejected alias mutated checkpoint destinations");
 check(s->phase()==Phase::settled,"Rejected alias destroyed usable session");s->restore({reference.data(),reference.size()},{tracker.data(),tracker.size()});check(s->phase()==Phase::idle,"Independent restore failed");
 v=prepared(*s,2);{const auto cp=referenceState(v.referenceCheckpoint);check(cp.frames==1&&cp.updates==2,"Second Ready source counters changed");}s->commit(v.trackerToken);s->published();s.reset();reaped();
}
void dedicated(const std::string&exe){
 for(uint32_t reference:{0u,1u}){
  ++cases;auto c=configuration();c.geometry.channels=1;
  c.reference.mode=ReferenceMode::capturedDedicated;c.reference.channels={reference};
  c.surveillanceChannels={1-reference};
  setenv("VECTORWARP_TEST_RESIDENT_MODE","normal",1);
  blah2::GpuProcessOptions options;options.executable=exe;options.startupMs=2000;options.frameMs=2000;
  auto s=createProcess(c,options);
  check(s->input().size==128,"Dedicated session dropped canonical reference input");
  const auto v=prepared(*s,1,bit(Section::filtered));
  const auto cp=referenceState(v.referenceCheckpoint);
  check(cp.channels==2&&!cp.weightCount&&!cp.frames&&!cp.updates&&
   v.maps.size==21&&v.filtered.size==64,"Dedicated process evidence/output topology differs");
  s->commit(v.trackerToken);s->published();s->resetInput();s->stop();reaped();
 }
}
void snapshots(const std::string& exe){for(const char* mode:{"snapshot","snapshot-corrupt"}){
 ++cases;setenv("VECTORWARP_TEST_RESIDENT_MODE",mode,1);auto config=configuration();blah2::resident_replay::test::enable(config);
 blah2::GpuProcessOptions options;options.executable=exe;options.startupMs=2000;options.frameMs=2000;
 auto s=createProcess(config,options);reject([&]{s->snapshot();},"Idle snapshot accepted");
 auto v=prepared(*s,1);reject([&]{s->snapshot();},"Prepared snapshot accepted");
 s->commit(v.trackerToken);reject([&]{s->snapshot();},"Unsettled snapshot accepted");s->published();
 if(std::string(mode)=="snapshot-corrupt"){reject([&]{s->snapshot();},"Malformed worker snapshot accepted");check(s->phase()==Phase::poisoned,"Corrupt snapshot did not poison worker");}
 else{check(!s->snapshot().empty(),"Snapshot empty");reject([&]{s->snapshot();},"Duplicate snapshot accepted");
  check(s->phase()==Phase::settled,"Duplicate snapshot changed settled state");
  for(uint32_t frame=2;frame<=4;++frame){s->resetInput();
   reject([&]{s->snapshot();},"Input reset exposed previous replay snapshot");
   v=prepared(*s,frame);s->commit(v.trackerToken);s->published();
   if(frame<=config.replay.maxFrames)check(!s->snapshot().empty(),"Post-gap snapshot empty");
   else reject([&]{s->snapshot();},"Input reset bypassed replay export frame limit");}}
 s.reset();reaped();}}
void inputResets(const std::string&exe){
 ++cases;auto s=session(exe,"normal");Qualification qualification(8);
 const auto* input=s->input().data;double worker=0;uint64_t event=0;
 s->resetInput();check(qualification.completed()==0,"Idle reset credited qualification");
 for(uint32_t i=1;i<=9;++i){
  qualification.begin();auto v=prepared(*s,i,bit(Section::filtered));
  if(!worker)worker=v.spectrum.data[0].real();
  check(v.spectrum.data[0].real()==worker,"Input reset reconstructed worker");
  const auto cp=referenceState(v.referenceCheckpoint);
  check(cp.frames==0&&cp.updates==1,"Input reset retained learned reference history");
  reject([&]{s->resetInput();},"Input reset accepted while frame owned");
  check(s->phase()==Phase::prepared,"Rejected reset changed frame ownership");
  if(qualification.pending()){
   reject([&]{qualification.requireComplete();},"Missing all oracles accepted");
   qualification.checked(Qualification::reference);qualification.checked(Qualification::filtered);
   reject([&]{qualification.requireNumeric();},"Missing map oracle accepted");
   qualification.checked(Qualification::maps);qualification.requireNumeric();
   reject([&]{qualification.requireComplete();},"Missing product oracle accepted");
   qualification.checked(Qualification::products);
  }
  qualification.requireComplete();s->commit(v.trackerToken);
  reject([&]{s->resetInput();},"Input reset accepted before publication settled");
  check(s->phase()==Phase::committed,"Rejected reset changed committed ownership");
  s->published();qualification.published();
  check(qualification.completed()==std::min(i,8u),"Qualification count changed");
  check(qualification.pending()==(i<8),"Eight-check qualification gate changed");
  s->resetInput();qualification.resetInput();
  check(s->phase()==Phase::idle&&s->input().data==input,"Input reset replaced mapping or failed");
  const auto control=wire::Reader{v.controlEvidence};
  check(control.u64(104)>event&&control.u32(480)==7,"Reset proof missing or rewound");event=control.u64(104);
  check(qualification.completed()==std::min(i,8u),"Input reset erased qualification progress");
  reject([&]{s->begin({i,i+100,0});},"Input reset admitted old frame identity");
  reject([&]{s->begin({i+1,i+100,0});},"Input reset admitted old consumed identity");
  reject([&]{s->commit(v.trackerToken);},"Input reset admitted old transaction");
  reject([&]{s->snapshot();},"Input reset exposed stale snapshot");
 }
 s.reset();reaped();
 // A partially verified CPI is not credit, including after a discontinuity.
 Qualification partial(8);partial.begin();partial.checked(Qualification::reference);
 partial.resetInput();reject([&]{partial.published();},"Gap credited a partial qualification frame");
 check(partial.completed()==0,"Partial oracle increased qualification count");
 for(const auto missing:{Qualification::reference,Qualification::filtered,Qualification::maps,Qualification::products}){
  Qualification gate(8);gate.begin();
  for(const auto stage:{Qualification::reference,Qualification::filtered,Qualification::maps,Qualification::products})
   if(stage!=missing)gate.checked(stage);
  reject([&]{gate.published();},"One missing oracle stage was accepted");
  check(gate.completed()==0,"Missing oracle credited qualification");
 }
 for(const char* mode:{"reset-error","reset-coverage","reset-timeout","reset-token"}){
  ++cases;setenv("VECTORWARP_TEST_RESIDENT_MODE",mode,1);blah2::GpuProcessOptions o;
  o.executable=exe;o.startupMs=2000;o.frameMs=std::strcmp(mode,"reset-timeout")==0?200:2000;
  auto failed=createProcess(configuration(),o);
  reject([&]{failed->resetInput();},"Failed or incomplete input reset accepted");
  check(failed->phase()==Phase::poisoned,"Unknown reset result did not poison worker");
  failed.reset();reaped();
 }
 ++cases;setenv("VECTORWARP_TEST_RESIDENT_MODE","normal",1);
 auto noTracker=configuration();noTracker.tracker={};noTracker.presence=wire::mandatoryPresence(noTracker);
 blah2::GpuProcessOptions options;options.executable=exe;options.startupMs=2000;options.frameMs=2000;
 auto disabled=createProcess(noTracker,options);disabled->resetInput();disabled->resetInput();
 check(disabled->phase()==Phase::idle,"Input reset requires a tracker");disabled.reset();reaped();
}
void startupQualification(const std::string&exe){
 for(const uint32_t depth:{1u,2u,4u,8u}){
  Qualification controls(std::max(8u,depth+1u),depth);
  reject([&]{controls.requireControls();},"Empty private controls admitted capture");
  controls.control(false,true,0,0,0,true,false);
  reject([&]{controls.requireControls();},"Negative-only qualification admitted capture");
  controls.control(true,false,0,0,0,true,false);
  reject([&]{controls.requireControls();},"Immature positive qualified temporal products");
  reject([&]{controls.control(true,true,0,0,0,true,false);},"Vacuous positive products admitted capture");
  controls.control(true,true,1,1,0,true,false);
  reject([&]{controls.requireControls();},"Tentative/associated track qualified before confirmation");
  reject([&]{controls.control(true,true,1,1,0,true,true);},"Unconfirmed track admitted at its deadline");
  reject([&]{controls.control(true,true,1,0,1,true,true);},"Impossible confirmed count was accepted");
  reject([&]{controls.control(false,true,1,0,0,false,false);},"Negative false detection was accepted");
  reject([&]{controls.control(false,true,0,1,0,true,false);},"Negative tentative track was accepted");
  controls.control(true,true,1,1,1,true,true);controls.requireControls();
  for(const uint32_t minimum:{1u,2u,3u,1u+254u/depth}){
   const uint32_t window=tracker_confirmation::confirmationMinimumWindow(minimum,depth);
   const uint32_t due=Qualification::confirmationFrame(depth,minimum);
   const uint32_t count=Qualification::startupCount(8,depth,minimum,window,depth-1);
   check(due==depth*(minimum+1)&&count+depth-1==std::max(8+depth-1,due),
    "Private confirmation budget differs from disjoint source-CPI windows");
   Qualification bounded(count,depth);
   check(bounded.pending(),"Private confirmation skipped numerical qualification");
   check(count+depth-1<=Qualification::maximumStartupFrames,"Private confirmation budget unbounded");
   reject([&]{Qualification::startupCount(8,depth,minimum,window-1,depth-1);},
    "Infeasible M/N became a longer private qualification");
   if(depth>1)reject([&]{Qualification::startupCount(8,depth,minimum,window,depth-2);},
    "Infeasible deletion misses became a private qualification");
  }
 }
 reject([]{Qualification gate(Qualification::maximumStartupFrames-6,8);},
  "Private qualification exceeded its hard frame cap");
 reject([]{Qualification::confirmationFrame(8,33);},"Confirmation deadline exceeded its hard cap");
 reject([]{Qualification::confirmationFrame(3,1);},"Unsupported temporal depth admitted private controls");
 for(const uint32_t depth:{1u,2u,4u,8u}){
  ++cases;Qualification gate(8,depth);uint32_t frames=0,settled=0,resets=0;
  gate.prepareStartup([&](uint32_t frame){
   check(frame==frames++,"Temporal qualification reused a warmup frame");
   gate.checked(Qualification::reference);gate.checked(Qualification::filtered);
   gate.checked(Qualification::maps);
   if(depth>1)reject([&]{gate.checked(Qualification::products);},
    "Shape-only products qualified temporal processing");
   gate.checkedProducts(std::min(frame+1,depth));
  },[&]{++settled;},[&]{++resets;});
  check(frames==8+depth-1&&settled==frames&&resets==1&&gate.completed()==8,
   "Temporal qualification did not require eight mature numerical frames");
  gate.resetInput();check(!gate.pending(),"Capture reset erased numerical cohort qualification");
  Qualification interrupted(8,depth);interrupted.begin();
  interrupted.checked(Qualification::reference);interrupted.checked(Qualification::filtered);
  interrupted.checked(Qualification::maps);interrupted.checkedProducts(1);
  interrupted.resetInput();reject([&]{interrupted.published();},
   "Reset admitted an unchecked temporal product");
 }
 // Real transport/control ownership, synthetic evidence only: no GPU numerical
 // qualification is claimed by this fixture. Main's source contract separately
 // binds each callback to its unchanged full numerical oracle.
 for(const char* mode:{"normal","reset-error"}){
  ++cases;auto s=session(exe,mode);Qualification qualification(8);
  const auto* input=s->input().data;double worker=0;
  uint32_t preparedCount=0,settledCount=0;bool resetAcknowledged=false,admitted=false;
  OutputViews last;
  const auto run=[&]{
   qualification.prepareStartup([&](uint32_t frame){
    check(!admitted&&!resetAcknowledged,"Capture admitted during startup oracle");
    check(frame==preparedCount&&settledCount==frame,"Startup qualification skipped a frame");
    last=prepared(*s,uint64_t(frame)+1,bit(Section::filtered));++preparedCount;
    if(!worker)worker=last.spectrum.data[0].real();
    check(last.spectrum.data[0].real()==worker,"Startup replaced worker");
    const auto cp=referenceState(last.referenceCheckpoint);
    check(cp.frames==frame&&cp.updates==frame+1,"Startup reset history before complete qualification");
    for(const auto stage:{Qualification::reference,Qualification::filtered,Qualification::maps,Qualification::products})
     qualification.checked(stage);
    qualification.requireComplete();s->commit(last.trackerToken);
   },[&]{
    check(!admitted&&s->phase()==Phase::committed,"Startup settled before private commit");
    s->published();++settledCount;
   },[&]{
    check(!admitted&&settledCount==8&&!qualification.pending(),"Startup reset before complete qualification");
    s->resetInput();resetAcknowledged=true;
   });
   admitted=true;
  };
  if(std::strcmp(mode,"normal")==0){
   run();check(admitted&&resetAcknowledged,"Capture admission preceded reset acknowledgment");
   check(qualification.completed()==8&&!qualification.pending(),"Startup reset discarded qualification");
   check(s->phase()==Phase::idle&&s->input().data==input,"Startup reset replaced mapping");
   check(wire::Reader{last.controlEvidence}.u32(480)==7,"Startup reset omitted adaptive stages");
   reject([&]{s->begin({8,108,0});},"Startup reset rewound identity fence");
   reject([&]{s->commit(last.trackerToken);},"Synthetic tracker transaction survived reset");
   const auto identity=s->qualificationIdentity();
   s.reset();reaped(); // A reset cannot rewind IDs; production owns a fresh instance.
   s=session(exe,"normal");
   check(s->qualificationIdentity()==identity,"Fresh matching instance lost qualification identity");
   std::fill_n(s->input().data,s->input().size,std::complex<double>{2,3});
   s->begin({1,1,1000});s->receiveReferenceReady();auto live=s->receivePrepared();
   const auto cp=referenceState(live.referenceCheckpoint);
   check(cp.frames==0&&cp.updates==1,"First admitted CPI inherited synthetic reference history");
   check(cp.logicalFrame==1&&live.trackerToken.generation==1&&live.trackerToken.consumedSequence==1,
    "Private qualification changed production frame-one semantics");
   check(live.spectrum.data[0].real()!=worker,"Production reused private worker state");
   qualification.requireComplete();s->commit(live.trackerToken);s->published();
   check(qualification.completed()==8,"First admitted CPI repeated CPU qualification");
   reject([&]{qualification.prepareStartup([](uint32_t){},[]{},[]{});},
    "Already qualified cohort admitted a second synthetic startup");
  }else{
   reject(run,"Failed startup reset admitted capture");
   check(!admitted&&!resetAcknowledged,"Failed reset returned capture-ready");
   check(s->phase()==Phase::poisoned,"Failed startup reset kept worker usable");
  }
  s.reset();reaped();
 }
 {
  auto baseline=session(exe,"normal");const auto identity=baseline->qualificationIdentity();baseline.reset();reaped();
  auto budget=session(exe,"budget-change");check(budget->qualificationIdentity()==identity,
   "Volatile device budget changed private qualification identity");budget.reset();reaped();
  for(const char* mutation:{"image-change","driver-change"}){
   auto changed=session(exe,mutation);check(changed->qualificationIdentity()!=identity,
    "Changed module/driver retained private qualification identity");changed.reset();reaped();
  }
  reject([&]{session(exe,"image-absent");},"Legacy control without image hash admitted ABI18");reaped();
  setenv("VECTORWARP_TEST_RESIDENT_MODE","normal",1);
  auto config=configuration();++config.reference.analysisInterval;
  blah2::GpuProcessOptions options;options.executable=exe;options.startupMs=2000;options.frameMs=2000;
  auto changed=createProcess(config,options);check(changed->qualificationIdentity()!=identity,
   "Changed config retained private qualification identity");changed.reset();reaped();
 }
 for(const auto missing:{Qualification::reference,Qualification::filtered,Qualification::maps,Qualification::products}){
  ++cases;Qualification gate(8);uint32_t settled=0,resets=0;
  reject([&]{gate.prepareStartup([&](uint32_t){
   for(const auto stage:{Qualification::reference,Qualification::filtered,Qualification::maps,Qualification::products})
    if(stage!=missing)gate.checked(stage);
  },[&]{++settled;},[&]{++resets;});},"Incomplete startup oracle admitted capture");
  check(!settled&&!resets&&!gate.completed(),"Incomplete startup oracle received credit or reset");
 }
 for(bool failSettle:{false,true}){
  ++cases;Qualification gate(8);uint32_t settled=0,resets=0;
  reject([&]{gate.prepareStartup([&](uint32_t frame){
   if(!failSettle&&frame==3)throw std::runtime_error("injected oracle rejection");
   for(const auto stage:{Qualification::reference,Qualification::filtered,Qualification::maps,Qualification::products})
    gate.checked(stage);
  },[&]{if(failSettle&&settled==3)throw std::runtime_error("injected settle rejection");++settled;},
    [&]{++resets;});},"Interrupted startup qualification admitted capture");
  check(gate.completed()==3&&settled==3&&!resets,"Rejected startup transaction received credit");
 }
}
void failure(const std::string&exe,const char*mode){
 ++cases;const auto start=std::chrono::steady_clock::now();bool failed=false;std::unique_ptr<Session>s;
 try{s=session(exe,mode);auto v=prepared(*s,1);s->commit(v.trackerToken);s->published();}catch(const std::exception&){failed=true;}
 check(failed,"Injected worker failure was accepted");if(s){check(s->phase()==Phase::poisoned,"Unknown worker result did not poison session");reject([&]{s->input();},"Input allowed after unknown worker result");}s.reset();reaped();
 check(std::chrono::steady_clock::now()-start<std::chrono::seconds(3),"Injected failure exceeded bound");
}
}
int main(int argc,char**argv)try{
 if(argc==2&&std::strcmp(argv[1],"--temporal-v1")==0){
  class TemporalDouble final:public blah2::temporal_gpu::Backend {
   uint32_t count_,depth_,available_=0;
  public:
   explicit TemporalDouble(const blah2::temporal_gpu::Config& c):count_(uint32_t(c.delayAxis.size()*c.dopplerAxis.size())),depth_(c.detector.integrationCpis){
    check(c.detector.currentCpiPreferenceSnr==9.1,"Temporal map transport lost exact merge threshold");}
   void reset()override{available_=0;}
   void process(const std::complex<double>* input,blah2::temporal_gpu::Output& output)override{
    if(input[0].real()<0)throw std::runtime_error("injected temporal rejection");
    output.metrics.available=available_=std::min(available_+1,depth_);output.metrics.count=0;
    for(uint32_t i=0;i<count_;++i)output.display[i]=input[i].real();
   }
  };
  return blah2::temporal_gpu::runWorker([](const auto& c){return std::make_unique<TemporalDouble>(c);});
 }
 if(argc==2&&std::strcmp(argv[1],"--resident-v5")==0)return runWorker([](const Config&c,const Mapping&m){return std::make_unique<Double>(c,m);});
 setenv("BLAH2_REPLAY_GPU_QUALIFICATION","untrusted-inherited-recipe",1);
 check(argc==1,"Unexpected CPU process fixture argument");const std::string exe=argv[0];
 {
  blah2::temporal_gpu::Config config;config.sampleRate=6144000;config.device="cpu-test-double";
  config.detector.enabled=true;config.detector.guard=2;config.detector.train=6;config.detector.pfa=1e-6;
  config.detector.centroidDopplerResolution=2;config.detector.integrationCpis=8;
  config.detector.currentCpiPreferenceSnr=9.1;
  static_assert(blah2::temporal_gpu::moduleAbi==2&&blah2::temporal_gpu::protocol==0x42545002);
  for(int i=0;i<1261;++i)config.delayAxis.push_back(i-31);
  config.dopplerAxis={-2,0,2};
  blah2::GpuProcessOptions options;options.executable=exe;options.startupMs=2000;options.frameMs=150;
  auto temporal=blah2::temporal_gpu::createProcess(config,options);
  std::fill_n(temporal->input(),config.delayAxis.size()*config.dopplerAxis.size(),std::complex<double>{7,0});
  for(uint64_t i=1;i<=9;++i){const auto& out=temporal->process(i);
   check(out.metrics.available==std::min(uint64_t(8),i)&&out.display[1260]==7,"Temporal map transport/warmup differs");}
  temporal->reset();check(temporal->process(10).metrics.available==1,"Temporal reset retained history");
  reject([&]{temporal->process(10);},"Temporal duplicate sequence admitted");
  temporal->input()[0]={-1,0};reject([&]{temporal->process(11);},"Temporal numeric rejection accepted");
  reject([&]{temporal->input();},"Failed temporal worker retained input");temporal.reset();reaped();
 }
 worker_isolation_fixture::verify([&](bool absent){
  blah2::GpuProcessOptions options;options.executable=absent?exe+".absent":exe;
  options.startupMs=2000;options.frameMs=150;
  auto worker=createProcess(configuration(),options);
 });
 normal(exe);deadlineCases(exe);dedicated(exe);snapshots(exe);inputResets(exe);startupQualification(exe);
 for(const char*mode:{"init-error","init-timeout","reference-error","radar-exit","frame-timeout","commit-exit","commit-timeout","published-timeout","mask-substitution"})failure(exe,mode);
 unsetenv("VECTORWARP_TEST_RESIDENT_MODE");std::cout<<"COMPLETE resident-process CPU-only cases="<<cases<<" checks="<<checks<<" gpu_execution=0\n";return 0;
}catch(const std::exception&e){std::cerr<<"FAIL resident-process: "<<e.what()<<'\n';return 1;}
