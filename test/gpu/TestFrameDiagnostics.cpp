// CPU-only contracts for the exact shared portable/native diagnostic helper.
#include "core/GpuFrameDiagnostics.private.h"
#include <iomanip>
#include <sstream>
#include <string>
#include <iostream>
#include <poll.h>
#include <sys/wait.h>
class Sink final:public std::streambuf {
public:
 std::string bytes;size_t writes=0;bool shortWrite=false;
 std::streamsize xsputn(const char* p,std::streamsize n)override{
  ++writes;if(shortWrite)return n? n-1:0;
  bytes.append(p,static_cast<size_t>(n));return n;
 }
};
static void require(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
int main()try{
 using namespace blah2::gpu_offline_diagnostics;
 setenv(profileVariable,"64",1);setenv(recipeVariable,"999999999 64",1);
 std::vector<std::string> environment={"KEEP=1",std::string(profileVariable)+"=64",std::string(recipeVariable)+"=1 64"};
 appendEnvironment(environment,false);
 require(environment==std::vector<std::string>{"KEEP=1"},"Live child retained inherited diagnostic opt-in");
 require(frameLimit()==0,"Stale worker recipe enabled diagnostics");
 Sink sink;std::ostream out(&sink);
 {gpu_diagnostics::FrameLog live;live.begin();live.stream()<<"never live\n";live.flush();live.drain(out);}
 require(sink.writes==0,"Live diagnostic emitted bytes");
 appendEnvironment(environment,true);
 require(environment.size()==2&&environment.back()==std::string(recipeVariable)+'='+std::to_string(getpid())+" 64","Offline recipe was not rebuilt by the parent");
 const std::string parent=std::to_string(getppid());
 setenv(blah2::gpu_worker_lifetime::parentVariable,parent.c_str(),1);
 setenv(recipeVariable,(parent+" 2").c_str(),1);
 require(frameLimit()==2,"Bounded parent-bound offline recipe rejected");
 for(const char* value:{"65","-1","1x","999999999999999999999"})require(parseLimit(value)==0,"Invalid profiler limit accepted");
 setenv(profileVariable,"invalid",1);appendEnvironment(environment,true);
 require(environment==std::vector<std::string>{"KEEP=1"},"Invalid inherited setting affected the worker");
 unsetenv(profileVariable);unsetenv(recipeVariable);unsetenv(blah2::gpu_worker_lifetime::parentVariable);
 gpu_diagnostics::FrameLog frame(2);std::ostringstream expected;
 frame.begin();frame.stream()<<std::setprecision(12)<<"frame="<<1<<" value="<<1.2345678901234<<'\n';
 expected<<std::setprecision(12)<<"frame="<<1<<" value="<<1.2345678901234<<'\n';frame.flush();
 frame.begin();frame.stream()<<"second="<<9.8765432101234<<'\n';expected<<"second="<<9.8765432101234<<'\n';frame.flush();
 frame.begin();frame.stream()<<"over frame limit\n";frame.flush();
 require(sink.writes==0&&!frame.active(),"Frame boundary touched a sink");
 frame.drain(out);require(sink.writes==1&&sink.bytes==expected.str(),"Offline exact bytes/frame bound differ");
 frame.drain(out);require(sink.writes==1,"Drain duplicated output");
 const std::string exact(gpu_diagnostics::FrameLog::capacity,'x');
 gpu_diagnostics::FrameLog bounded(1);bounded.begin();bounded.stream()<<exact<<'y';bounded.flush();
 require(bounded.retainedBytes()==exact.size(),"Diagnostic storage exceeded its bound");
 Sink fullSink;std::ostream fullOut(&fullSink);bounded.drain(fullOut);
 require(fullSink.bytes==exact+"GPU_DIAGNOSTIC_TRUNCATED bounded_offline_storage=1\n","Overflow marker missing");
 gpu_diagnostics::FrameLog failedSink(1);failedSink.begin();failedSink.stream()<<"optional\n";failedSink.flush();
 Sink failing;failing.shortWrite=true;std::ostream bad(&failing);failedSink.drain(bad);
 require(failing.writes==1&&failedSink.retainedBytes()==0,"Sink failure changed frame acceptance");
 // Fill a real blocking stderr pipe. Flush AND object destruction must still
 // permit the child to reply before its explicitly offline process teardown.
 int logs[2],reply[2];require(pipe(logs)==0&&pipe(reply)==0,"pipe failed");
 const int flags=fcntl(logs[1],F_GETFL);require(flags>=0&&fcntl(logs[1],F_SETFL,flags|O_NONBLOCK)==0,"pipe flags failed");
 char padding[4096]{};while(write(logs[1],padding,sizeof(padding))>0){}
 require(errno==EAGAIN&&fcntl(logs[1],F_SETFL,flags)==0,"pipe did not fill");
 const pid_t child=fork();require(child>=0,"fork failed");
 if(!child){
   close(reply[0]);close(logs[0]);if(dup2(logs[1],2)<0)_exit(2);close(logs[1]);
   {gpu_diagnostics::FrameLog offline(1);offline.begin();offline.stream()<<exact;offline.flush();}
   if(write(reply[1],"R",1)!=1)_exit(3);
   std::exit(0); // Deferred drain may block; the parent bounds this teardown.
 }
 close(reply[1]);close(logs[1]);pollfd ready{reply[0],POLLIN,0};
 const int polled=poll(&ready,1,1000);char response=0;
 const bool replied=polled==1&&(ready.revents&POLLIN)&&read(reply[0],&response,1)==1&&response=='R';
 kill(child,SIGKILL);int status=0;while(waitpid(child,&status,0)<0&&errno==EINTR){}
 close(reply[0]);close(logs[0]);require(replied,"Blocked stderr delayed reply/object destruction");
 std::cout<<"FRAME_DIAGNOSTICS_PASS live_inert=1 offline_authorized=1 bytes_exact=1 frame_bound=1 deferred_write=1 blocked_sink_reply=1\n";
 return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
