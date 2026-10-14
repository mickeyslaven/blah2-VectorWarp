#include "GpuResidentWire.h"
#include "CpuEvidenceDouble.h"
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
 Config c;auto&g=c.geometry;g.range=12;g.doppler=7;g.delays=3;g.channels=2;g.delayMin=-1;g.clutterSamples=64;g.clutterBins=4;
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
  auto bank=m_.bytes(Section::tracker1);wire::Writer t{bank};t.zero();t.u32(0,1);t.u32(4,8224);t.u32(8,c_.tracker.capacity);t.u32(12,255);t.u64(40,c_.epoch);t.u64(48,p.frame.logicalId);t.u64(56,p.frame.consumedSequence);proof_.prepared(p);return true;
 }
 Token preparedToken()const override{return token_;}
 void commit(const Packet&p)override{if(mode_=="commit-exit")_exit(8);proof_.control(p,Phase::committed);}
 void abort(const Packet&p)override{proof_.control(p,Phase::rejected);}
 void restore(const Packet&p)override{check(p.restoreTrackerSlot==0,"Restore did not use input slot");proof_.control(p,Phase::idle);}
 void settle(const Packet&p)override{proof_.control(p,Phase::settled);}
 void shutdown(const Packet&p)override{proof_.control(p,Phase::settled);}
};
void reaped(){
 const auto deadline=std::chrono::steady_clock::now()+std::chrono::milliseconds(300);
 for(;;){int status=0;errno=0;const pid_t p=waitpid(-1,&status,WNOHANG);check(p<=0,"Fixture had to reap a leaked worker");if(p==-1&&errno==ECHILD)return;check(std::chrono::steady_clock::now()<deadline,"Worker remained after bounded cleanup");std::this_thread::sleep_for(std::chrono::milliseconds(1));}
}
std::unique_ptr<Session> session(const std::string&exe,const char*mode){setenv("VECTORWARP_TEST_RESIDENT_MODE",mode,1);blah2::GpuProcessOptions o;o.executable=exe;o.startupMs=std::strcmp(mode,"init-timeout")==0?200:2000;o.frameMs=std::strcmp(mode,"frame-timeout")==0?200:2000;return createProcess(configuration(),o);}
OutputViews prepared(Session&s,uint64_t id){auto input=s.input();std::fill_n(input.data,input.size,std::complex<double>{2,3});s.begin({id,id+100,1000+id});const auto ready=s.receiveReferenceReady();std::array<uint8_t,256> saved{};std::copy_n(ready.referenceCheckpoint.data,saved.size(),saved.data());auto v=s.receivePrepared();check(std::memcmp(saved.data(),v.referenceCheckpoint.data,256)==0,"Prepared mutated acknowledged reference bytes");return v;}
void normal(const std::string&exe){
 ++cases;auto s=session(exe,"normal");auto v=prepared(*s,UINT64_C(0x100000001));
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
void failure(const std::string&exe,const char*mode){
 ++cases;const auto start=std::chrono::steady_clock::now();bool failed=false;std::unique_ptr<Session>s;
 try{s=session(exe,mode);auto v=prepared(*s,1);s->commit(v.trackerToken);s->published();}catch(const std::exception&){failed=true;}
 check(failed,"Injected worker failure was accepted");if(s){check(s->phase()==Phase::poisoned,"Unknown worker result did not poison session");reject([&]{s->input();},"Input allowed after unknown worker result");}s.reset();reaped();
 check(std::chrono::steady_clock::now()-start<std::chrono::seconds(3),"Injected failure exceeded bound");
}
}
int main(int argc,char**argv)try{
 if(argc==2&&std::strcmp(argv[1],"--resident-v5")==0)return runWorker([](const Config&c,const Mapping&m){return std::make_unique<Double>(c,m);});
 check(argc==1,"Unexpected CPU process fixture argument");const std::string exe=argv[0];normal(exe);
 for(const char*mode:{"init-error","init-timeout","reference-error","radar-exit","frame-timeout","commit-exit","mask-substitution"})failure(exe,mode);
 unsetenv("VECTORWARP_TEST_RESIDENT_MODE");std::cout<<"COMPLETE resident-process CPU-only cases="<<cases<<" checks="<<checks<<" gpu_execution=0\n";return 0;
}catch(const std::exception&e){std::cerr<<"FAIL resident-process: "<<e.what()<<'\n';return 1;}
