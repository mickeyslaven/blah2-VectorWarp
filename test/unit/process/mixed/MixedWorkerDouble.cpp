#include "process/mixed/MixedProtocol.h"
#include <cmath>
#include <complex>
#include <cstring>
#include <limits>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
using namespace blah2::mixed;
static Message receive(){Message value;const auto count=recv(3,&value,sizeof(value),MSG_TRUNC);if(count!=sizeof(value))_exit(2);return value;}
static void reply(Message value){if(send(3,&value,sizeof(value),0)!=sizeof(value))_exit(2);}
int main(int argc,char** argv){
  if(argc!=2)_exit(2);
  const char* mode=argv[1];
  const auto init=receive();
  if(init.protocol!=version||init.operation!=Operation::initialize||init.bytes!=sharedBytes)_exit(2);
  if(std::strcmp(mode,"startup-hang")==0){sleep(2);_exit(3);}
  if(std::strcmp(mode,"startup-error")==0)_exit(3);
  struct stat st{};if(fstat(4,&st)||uint64_t(st.st_size)!=sharedBytes)_exit(2);
  auto* data=static_cast<std::complex<double>*>(mmap(nullptr,sharedBytes,
    PROT_READ|PROT_WRITE,MAP_SHARED,4,0));
  if(data==MAP_FAILED)_exit(2);
  Message ready;ready.operation=Operation::ready;reply(ready);
  const auto request=receive();
  if((request.operation!=Operation::frame&&
      request.operation!=Operation::pairedFrameReference0&&
      request.operation!=Operation::pairedFrameReference1)||request.sequence!=1)_exit(2);
  if(std::strcmp(mode,"prepared-channel-0")==0||std::strcmp(mode,"prepared-channel-1")==0){
    const bool reversed=std::strcmp(mode,"prepared-channel-1")==0;
    const auto expected=reversed?Operation::pairedFrameReference1:Operation::pairedFrameReference0;
    const auto* raw=reinterpret_cast<const int16_t*>(data);
    const auto last=uint64_t(samples-1)*4;
    if(request.operation!=expected||raw[0]!=101||raw[1]!=-102||
       raw[2]!=201||raw[3]!=-202||raw[last]!=103||raw[last+1]!=-104||
       raw[last+2]!=203||raw[last+3]!=-204)_exit(3);
  }
  // A broken worker may corrupt its input mapping before failing. Recovery
  // must still use the parent's independent authoritative input samples.
  data[0]=data[samples-1]=data[samples]=data[inputSamples-1]={-30000,30000};
  if(std::strcmp(mode,"crash")==0)_exit(3);
  if(std::strcmp(mode,"hang")==0){sleep(2);_exit(3);}
  for(uint64_t i=0;i<mapSamples+tailSamples;++i)data[inputSamples+i]={double(i%17),double(i%7)};
  if(std::strcmp(mode,"nan")==0)data[inputSamples+10]={std::numeric_limits<double>::quiet_NaN(),0};
  if(std::strcmp(mode,"nan-tail")==0)data[inputSamples+mapSamples]={std::numeric_limits<double>::quiet_NaN(),0};
  ready.sequence=std::strcmp(mode,"wrong-sequence")==0?2:1;
  if(std::strcmp(mode,"wrong-shape")==0)ready.rowCount=rows-1;
  if(std::strcmp(mode,"bad-protocol")==0)ready.protocol=0;
  if(std::strcmp(mode,"short-message")==0){(void)send(3,&ready,sizeof(ready)-1,0);return 0;}
  reply(ready);
  return 0;
}
