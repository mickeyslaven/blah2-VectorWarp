#include "process/ambiguity/ReplayGpuQualification.h"
#include "process/ambiguity/GpuArtifactBinding.h"
#include <iostream>
#include <cstdlib>
#include <unistd.h>
#include <chrono>
#include <sys/wait.h>
namespace q=blah2::replay_gpu_qualification;
namespace a=blah2::gpu_artifact_binding;
void check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
template<class F>void rejects(F f){try{f();}catch(const std::exception&){return;}throw std::runtime_error("Invalid qualification accepted");}
template<class F>void childCheck(F f){
 const auto child=fork();check(child>=0,"Artifact test fork failed");
 if(!child){try{f();_exit(0);}catch(const std::exception& e){std::cerr<<e.what()<<'\n';_exit(1);}}
 int status=0;check(waitpid(child,&status,0)==child&&WIFEXITED(status)&&!WEXITSTATUS(status),"Artifact channel regression failed");
}
int main()try{
 char artifactPath[]="/tmp/vectorwarp-artifact-binding-XXXXXX";
 const int artifact=mkstemp(artifactPath);
 check(artifact>=0,"Artifact fixture create failed");
 struct RemoveArtifact {const char* path;int fd;~RemoveArtifact(){close(fd);unlink(path);}} cleanup{artifactPath,artifact};
 check(write(artifact,"abc",3)==3,"Artifact fixture write failed");
 const auto pinned=a::inspect(artifactPath);
 check(pinned.sha256=="ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad","Artifact SHA-256 differs");
 setenv("VECTORWARP_GPU_ARTIFACT_TEST_PATH",pinned.path.c_str(),1);
 setenv("VECTORWARP_GPU_ARTIFACT_TEST_SHA256",pinned.sha256.c_str(),1);
 setenv("VECTORWARP_GPU_ARTIFACT_TEST_STAT",pinned.stat.c_str(),1);
 a::state().pins={{"TEST_PATH",pinned.path},{"TEST_SHA256",pinned.sha256},{"TEST_STAT",pinned.stat}};
 a::verify(pinned,"TEST");
 check(pwrite(artifact,"abd",3,0)==3,"Artifact replacement write failed");
 rejects([&]{a::verify(a::inspect(artifactPath),"TEST");});
 unsetenv("VECTORWARP_GPU_ARTIFACT_TEST_PATH");unsetenv("VECTORWARP_GPU_ARTIFACT_TEST_SHA256");
 unsetenv("VECTORWARP_GPU_ARTIFACT_TEST_STAT");
 a::state().pins.clear();
 childCheck([&]{
  setenv("VECTORWARP_GPU_ARTIFACT_NONCE","stale-hostile-nonce",1);
  setenv("VECTORWARP_GPU_ARTIFACT_WORKER_PATH","/never/hash/live/worker",1);
  check(!a::active(),"Inherited nonce enabled live artifacts");
  a::emit("ignored-live-event"); // No hashing, descriptor access or stderr I/O.
  std::vector<std::string> rebuilt;
  for(const auto* entry:{"VECTORWARP_GPU_ARTIFACT_NONCE=stale","VECTORWARP_GPU_ARTIFACT_CHANNEL=hostile","NORMAL=retained"})
   if(!a::inherited(entry))rebuilt.emplace_back(entry);
  a::appendEnvironment(rebuilt);
  check(rebuilt==std::vector<std::string>{"NORMAL=retained"},"Live worker inherited artifact controls");
 });
 childCheck([&]{
  const auto image=a::inspect(artifactPath);
  setenv("VECTORWARP_GPU_ARTIFACT_NONCE","0123456789abcdef0123456789abcdef",1);
  for(const auto& pin:std::map<std::string,std::string>{{"PATH",image.path},{"SHA256",image.sha256},{"STAT",image.stat}})
   setenv(("VECTORWARP_GPU_ARTIFACT_PROCESSOR_"+pin.first).c_str(),pin.second.c_str(),1);
  int pair[2];check(!socketpair(AF_UNIX,SOCK_DGRAM,0,pair),"Artifact socketpair failed");
  a::Config config;config.enabled=true;config.confirmation=a::confirmation;
  config.nonce="0123456789abcdef0123456789abcdef";config.descriptor=pair[0];
  rejects([&]{a::configure(config,false,"blah2");});
  rejects([&]{a::configure(config,true,"raw");});
  auto bad=config;bad.confirmation="wrong";rejects([&]{a::configure(bad,true,"blah2");});
  bad=config;bad.nonce="aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";rejects([&]{a::configure(bad,true,"blah2");});
  bad=config;bad.descriptor=artifact;rejects([&]{a::configure(bad,true,"blah2");});
  a::configure(config,true,"blah2");check(a::active()&&a::closeFrom()==7,"Artifact descriptor layout lost");
  setenv("VECTORWARP_GPU_ARTIFACT_PROCESSOR_PATH","/stale-after-config",1);
  check(a::env("PROCESSOR_PATH")==image.path,"Launch pins were not snapshotted");
  a::emit("bounded-datagram");char received[4097];
  check(recv(pair[1],received,sizeof(received),MSG_DONTWAIT)>0,"Dedicated channel did not receive receipt");
  int capacity=4096;check(!setsockopt(a::state().descriptor,SOL_SOCKET,SO_SNDBUF,&capacity,sizeof(capacity)),"Socket bound failed");
  std::array<char,1> payload{};unsigned fills=0;
  while(fills<10000&&send(a::state().descriptor,payload.data(),payload.size(),MSG_DONTWAIT)==ssize_t(payload.size()))++fills;
  check(fills<10000&&(errno==EAGAIN||errno==EWOULDBLOCK),"Receipt socket did not fill");
  const auto start=std::chrono::steady_clock::now();rejects([&]{a::emit("full-channel");});
  check(std::chrono::steady_clock::now()-start<std::chrono::seconds(1),"Receipt send blocked on stalled runner");
  close(pair[1]);
 });
 check(!q::enabled(),"Qualification is not default off");
 check(!q::clutterActive()&&q::injectEcho(),"Live/default replay acquired a frozen-clutter control");
 childCheck([&]{
  blah2::clutter_replay::Plan plan;plan.mode="export";plan.path="/tmp/not-created-checkpoint-fixture";
  plan.identity=std::string(64,'a');plan.sourceSha=std::string(40,'b');plan.configSha=std::string(64,'c');
  plan.warmFrame=1;plan.epoch=7;plan.samples=4;plan.channels=2;plan.taps=1;
  plan.warmEnd=4;plan.injectionFrame=2;plan.injectionFirst=4;
  rejects([&]{q::configureClutter(plan);});
  q::Config config;config.enabled=true;config.confirmation=q::confirmation;q::configure(config,true,"blah2");
  q::configureClutter(plan);rejects([&]{q::configureClutter(plan);});
  std::vector<std::string> recipe;q::appendEnvironment(recipe,"portable",0,{});
  rejects([&]{std::vector<std::string> ignored;q::appendEnvironment(ignored,"resident",0,{});});
  rejects([&]{std::vector<std::string> ignored;q::appendEnvironment(ignored,"portable",1,{});});
  childCheck([&]{
   check(dup2(q::state().reader,5)==5,"Checkpoint descriptor duplication failed");
   const auto value=recipe.front().substr(std::strlen(q::environmentKey));
   setenv("BLAH2_REPLAY_GPU_QUALIFICATION",value.c_str(),1);
   q::Worker worker("portable");check(worker.clutter().encode()==plan.encode(),"Authenticated checkpoint trailer lost");
   auto bad=value;bad.back()=bad.back()=='0'?'1':'0';setenv("BLAH2_REPLAY_GPU_QUALIFICATION",bad.c_str(),1);
   rejects([&]{q::Worker wrong("portable");});
   setenv("BLAH2_REPLAY_GPU_QUALIFICATION",value.c_str(),1);
   rejects([&]{q::Worker wrong("resident");});close(5);rejects([&]{q::Worker missing("portable");});
  });
  q::bind(1,7,0,4);check(!q::injectEcho(),"Export injected warm samples");
  rejects([&]{q::acceptClutterProof("");});
  blah2::clutter_replay::Proof proof{{1,7,0,4},false,std::string(64,'d'),std::string(64,'e'),1,1,2};
  q::acceptClutterProof(proof.encode());proof.frame.epoch++;rejects([&]{q::acceptClutterProof(proof.encode());});
  rejects([&]{q::bind(2,7,5,9);});
 });
 q::Config c;c.enabled=true;c.confirmation=q::confirmation;c.kind="numeric-rejection";c.route="resident";c.frame=7;c.lane=1;
 q::validate(c,true,"blah2");
 q::validate(c,true,"mchq");
 rejects([&]{q::validate(c,false,"mchq");});
 rejects([&]{q::validate(c,false,"blah2");});rejects([&]{q::validate(c,true,"auto");});
 for(const auto& kind:{"startup","allocation","prepare","commit","worker-exit","timeout","numeric-rejection","input-gap","cpu-oracle-reset"}){
  auto next=c;next.kind=kind;next.frame=(next.kind=="startup"||next.kind=="allocation")?0:7;
  if(next.kind=="input-gap"){next.route="capture";next.lane=0;}
  if(next.kind=="cpu-oracle-reset")next.route="host";
  q::validate(next,true,"blah2");
 }
 auto reset=c;reset.kind="cpu-oracle-reset";reset.route="host";
 q::validate(reset,true,"blah2");q::validateRoute(reset,true,2,"cpu");
 rejects([&]{q::validate(reset,false,"blah2");});rejects([&]{q::validate(reset,true,"raw");});
 rejects([&]{q::validateRoute(reset,false,2,"cpu");});
 for(const auto* mode:{"auto","gpu","gpu-resident"})rejects([&]{q::validateRoute(reset,true,2,mode);});
 rejects([&]{q::validateRoute(reset,true,1,"cpu");});
 auto badReset=reset;badReset.route="resident";rejects([&]{q::validate(badReset,true,"blah2");});
 q::validateRoute(c,true,2,"auto");rejects([&]{q::validateRoute(c,true,2,"cpu");});
 auto bad=c;bad.enabled=false;rejects([&]{q::validate(bad,true,"blah2");});
 bad=c;bad.confirmation+="wrong";rejects([&]{q::validate(bad,true,"blah2");});
 bad=c;bad.kind="commti";rejects([&]{q::validate(bad,true,"blah2");});
 bad=c;bad.kind="commit";bad.route="portable";rejects([&]{q::validate(bad,true,"blah2");});
 bad=c;bad.frame=257;rejects([&]{q::validate(bad,true,"blah2");});
 bad=c;bad.lane=8;rejects([&]{q::validate(bad,true,"blah2");});
 q::configure(c,true,"blah2");q::bind(7,19,100,200);
 rejects([&]{q::configure(c,true,"blah2");});rejects([&]{q::bind(257,19,100,200);});
 rejects([&]{std::vector<std::string> env;q::appendEnvironment(env,"resident",1,"double");});
 check(q::inherited("BLAH2_REPLAY_GPU_QUALIFICATION=bad"),"Inherited recipe was not recognized");
 check(!q::inherited("OTHER=value"),"Unrelated environment filtered");
 std::vector<std::string> env;q::appendEnvironment(env,"resident",1,{});
 const auto child=fork();check(child>=0,"Fork failed");
 if(!child){try{
  check(dup2(q::state().reader,5)==5,"Read-only descriptor duplication failed");
  setenv("BLAH2_REPLAY_GPU_QUALIFICATION",env.front().substr(std::strlen(q::environmentKey)).c_str(),1);
  q::Worker worker("resident");check(worker.binding().frame==7&&worker.binding().first==100,"Exact capture binding lost");
  check(!worker.hit("startup"),"Wrong stage fired");
  check(worker.hit("numeric-rejection",22,33),"Requested boundary did not fire");
  check(!worker.hit("numeric-rejection",22,33),"Fault fired twice");
  close(5);rejects([&]{q::Worker invalid("resident");});_exit(0);
 }catch(...){_exit(1);}}
 int status=0;check(waitpid(child,&status,0)==child&&WIFEXITED(status)&&WEXITSTATUS(status)==0,"Worker control regression failed");
 q::state().config=reset;q::bind(6,19,0,100);
 check(!q::takeCpuOracleReset(1),"CPU reset fired on wrong capture interval");q::bind(7,19,100,200);
 check(!q::takeCpuOracleReset(0),"CPU reset fired on wrong lane");
 check(q::takeCpuOracleReset(1),"CPU reset did not fire at exact boundary");
 check(!q::takeCpuOracleReset(1),"CPU reset fired twice");
 std::cout<<"PASS replay qualification host contract; physical_gpu_execution=0\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
