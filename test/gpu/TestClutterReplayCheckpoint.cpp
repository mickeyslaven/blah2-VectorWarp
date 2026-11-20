#include "process/ambiguity/ClutterReplayCheckpoint.private.h"
#include <iostream>
#include <limits>
namespace c=blah2::clutter_replay;
template<class F>void rejects(F f){try{f();}catch(const std::exception&){return;}
 throw std::runtime_error("Invalid checkpoint accepted");}
int main()try{
 char directory[]="/tmp/vectorwarp-clutter-checkpoint-XXXXXX";
 c::require(mkdtemp(directory),"Temporary checkpoint directory failed");
 const std::string path=std::string(directory)+"/weights";
 struct Cleanup{std::string path,directory;~Cleanup(){unlink(path.c_str());rmdir(directory.c_str());}} cleanup{path,directory};
 c::Plan p;p.mode="export";p.path=path;p.identity=std::string(64,'a');p.configSha=std::string(64,'b');
 p.sourceSha=std::string(40,'c');p.warmFrame=2;p.epoch=9;p.firstSample=10;p.samples=4;
 p.channels=3;p.taps=2;p.delayMin=-1;p.warmFirst=14;p.warmEnd=18;p.injectionFrame=3;p.injectionFirst=18;
 p.validate();c::require(c::Plan::decode(p.encode()).encode()==p.encode(),"Plan roundtrip differs");
	 for(auto change:{0,1,2,3,4,5}){auto bad=p;
	  if(change==0){bad.injectionFirst--;}
	  if(change==1){bad.injectionFrame--;}
	  if(change==2){bad.samples=4097;}
	  if(change==3){bad.sourceSha.clear();}
	  if(change==4){bad.mode="live";}
	  if(change==5){bad.injected=true;}
  rejects([&]{bad.validate();});}
 c::Frame frame{};std::vector<std::complex<float>> reference(4,{1,2}),surveillance(12,{3,4});
 const std::vector<std::complex<double>> q0={{1,.1},{2,.2},{3,.3},{4,.4},{5,.5},{6,.6}};
 const std::vector<std::complex<float>> q1={{.1f,.01f},{.2f,.02f},{.3f,.03f},{.4f,.04f},{.5f,.05f},{.6f,.06f}};
 const auto execute=[&](c::State& state,uint64_t number){
  frame={number,9,10+(number-1)*4,14+(number-1)*4};
  state.begin(reference.data(),reference.size(),surveillance.data(),surveillance.size());
  if(!state.fixed())state.work(1,1,4);
  state.batch(0,2,q0.data(),q1.data());state.batch(2,1,q0.data()+4,q1.data()+4);state.complete(true);
 };
 c::State exportState(p,[&]{return frame;});execute(exportState,1);execute(exportState,2);
 auto bytes=c::read(path);p.expectedSha=c::hash(bytes.data(),bytes.size());p.mode="fixed";
 const auto checkpoint=c::Checkpoint::decode(bytes,p);
 c::require(checkpoint.initial==q0&&checkpoint.correction==q1,"Partial final batch coefficients lost");
 rejects([&]{c::write(path,bytes);});
	 for(auto change:{0,1,2,3,4}){auto bad=p;
	  if(change==0){bad.expectedSha=std::string(64,'0');}
	  if(change==1){bad.channels=2;}
	  if(change==2){bad.identity=std::string(64,'0');}
	  if(change==3){bad.epoch++;}
	  if(change==4){bad.delayMin++;}
  rejects([&]{c::Checkpoint::decode(bytes,bad);});}
 auto corrupt=bytes;corrupt.back()^=1;rejects([&]{c::Checkpoint::decode(corrupt,p);});
 frame={};c::State fixed(p,[&]{return frame;});execute(fixed,1);execute(fixed,2);
 c::require(fixed.proof().checkpoint==p.expectedSha&&!fixed.proof().fixed,"Warm checkpoint differs");
 p.injected=true;c::require(!p.inject({2,9,14,18})&&p.inject({3,9,18,22}),"Injection boundary differs");
 reference.assign(4,{});surveillance.assign(12,{9,10});execute(fixed,3);
 const auto proof=c::Proof::decode(fixed.proof().encode());
 c::require(proof.fixed&&!proof.covariance&&!proof.factor&&!proof.solve&&proof.checkpoint==p.expectedSha,
  "Frozen proof contains fitting work");
 frame={4,9,22,26};fixed.begin(reference.data(),4,surveillance.data(),12);
 rejects([&]{fixed.work(1,0,0);});fixed.poison();rejects([&]{execute(fixed,4);});
 frame={};c::State drift(p,[&]{return frame;});rejects([&]{execute(drift,2);});
 frame={};c::State changed(p,[&]{return frame;});execute(changed,1);rejects([&]{execute(changed,2);});
 frame={};c::State incomplete(p,[&]{return frame;});frame={1,9,10,14};
 incomplete.begin(reference.data(),4,surveillance.data(),12);
 rejects([&]{incomplete.batch(1,2,q0.data(),q1.data());});rejects([&]{incomplete.complete(true);});
 auto badProof=proof;badProof.solve=1;rejects([&]{c::Proof::decode(badProof.encode());});
 std::vector<uint8_t> invalid;rejects([&]{c::scalar(invalid,std::numeric_limits<double>::infinity());});
 std::cout<<"PASS checkpoint byte/lineage/batch/freeze contract; physical_gpu_execution=0\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
