#include "GpuResidentWire.h"
#include "GpuResidentQualification.h"
#include "CpuEvidenceDouble.h"
#include "ResidentReplayFixture.h"
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
  if(mode_=="init-error")throw std::runtime_error("injected init error");
  if(mode_=="init-timeout")std::this_thread::sleep_for(std::chrono::seconds(5));
  proof_.device();proof_.control(p,Phase::idle);
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
  auto bank=m_.bytes(Section::tracker1);wire::Writer t{bank};t.zero();t.u32(0,1);t.u32(4,trackerRecordWords(c_.tracker)*4);t.u32(8,c_.tracker.capacity);t.u32(12,trackerHistoryCapacity(c_.tracker));t.u64(40,c_.epoch);t.u64(48,p.frame.logicalId);t.u64(56,p.frame.consumedSequence);proof_.prepared(p);
  // An unused numerical output identifies the exact worker without a new API.
  wire::Writer{m_.bytes(Section::spectrum)}.f64(0,double(getpid()));return true;
 }
 Token preparedToken()const override{return token_;}
 void commit(const Packet&p)override{if(mode_=="commit-exit")_exit(8);proof_.control(p,Phase::committed);}
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
 void settle(const Packet&p)override{proof_.control(p,Phase::settled);}
 void snapshot(const Packet&p)override{namespace rr=blah2::resident_replay;
  auto s=rr::test::fixture(c_,1,p.frame,p.tracker.transaction);
  const auto raw=m_.read(Section::canonical);s.blocks[13].bytes.assign(raw.data,raw.data+raw.size);
  Sha256 hash;hash.add(raw.data,raw.size);s.identity.rawIqHash=hash.finish();
  const auto device=m_.read(Section::device);Sha256 deviceHash;deviceHash.add(device.data,device.size);s.identity.deviceHash=deviceHash.finish();
  auto encoded=rr::encode(s,c_);if(mode_=="snapshot-corrupt")encoded.back()^=1;
  const auto out=m_.bytes(Section::replaySnapshot);std::memcpy(out.data,encoded.data(),encoded.size());proof_.control(p,Phase::settled);}
 void shutdown(const Packet&p)override{proof_.control(p,Phase::settled);}
};
void reaped(){
 const auto deadline=std::chrono::steady_clock::now()+std::chrono::milliseconds(300);
 for(;;){int status=0;errno=0;const pid_t p=waitpid(-1,&status,WNOHANG);check(p<=0,"Fixture had to reap a leaked worker");if(p==-1&&errno==ECHILD)return;check(std::chrono::steady_clock::now()<deadline,"Worker remained after bounded cleanup");std::this_thread::sleep_for(std::chrono::milliseconds(1));}
}
std::unique_ptr<Session> session(const std::string&exe,const char*mode){setenv("VECTORWARP_TEST_RESIDENT_MODE",mode,1);blah2::GpuProcessOptions o;o.executable=exe;o.startupMs=std::strcmp(mode,"init-timeout")==0?200:2000;o.frameMs=std::strcmp(mode,"frame-timeout")==0?200:2000;return createProcess(configuration(),o);}
OutputViews prepared(Session&s,uint64_t id,uint64_t diagnostics=0){auto input=s.input();std::fill_n(input.data,input.size,std::complex<double>{2,3});s.begin({id,id+100,1000+id},diagnostics);const auto ready=s.receiveReferenceReady();std::array<uint8_t,256> saved{};std::copy_n(ready.referenceCheckpoint.data,saved.size(),saved.data());auto v=s.receivePrepared();check(std::memcmp(saved.data(),v.referenceCheckpoint.data,256)==0,"Prepared mutated acknowledged reference bytes");return v;}
void normal(const std::string&exe){
 ++cases;auto s=session(exe,"normal");auto v=prepared(*s,UINT64_C(0x100000001),bit(Section::filtered));
 {const auto cp=referenceState(v.referenceCheckpoint);check(cp.frames==0&&cp.updates==1,"First Ready source counters changed");}
 reject([&]{s->input();},"Input writable while frame owned");reject([&]{s->published();},"Publication allowed before commit");
 auto wrong=v.trackerToken;++wrong.transaction;reject([&]{s->commit(wrong);},"Wrong transaction accepted");check(s->phase()==Phase::prepared,"Rejected local token changed transaction");
 s->commit(v.trackerToken);check(wire::Reader{v.controlEvidence}.u32(8)==uint32_t(Phase::committed),"Commit not acknowledged");s->published();check(wire::Reader{v.controlEvidence}.u32(8)==uint32_t(Phase::settled),"Publication not settled");
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
   auto live=prepared(*s,9);const auto cp=referenceState(live.referenceCheckpoint);
   check(cp.frames==0&&cp.updates==1,"First admitted CPI inherited synthetic reference history");
   check(live.spectrum.data[0].real()==worker,"First admitted CPI reconstructed qualified worker");
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
 if(argc==2&&std::strcmp(argv[1],"--resident-v5")==0)return runWorker([](const Config&c,const Mapping&m){return std::make_unique<Double>(c,m);});
 check(argc==1,"Unexpected CPU process fixture argument");const std::string exe=argv[0];normal(exe);dedicated(exe);snapshots(exe);inputResets(exe);startupQualification(exe);
 for(const char*mode:{"init-error","init-timeout","reference-error","radar-exit","frame-timeout","commit-exit","mask-substitution"})failure(exe,mode);
 unsetenv("VECTORWARP_TEST_RESIDENT_MODE");std::cout<<"COMPLETE resident-process CPU-only cases="<<cases<<" checks="<<checks<<" gpu_execution=0\n";return 0;
}catch(const std::exception&e){std::cerr<<"FAIL resident-process: "<<e.what()<<'\n';return 1;}
