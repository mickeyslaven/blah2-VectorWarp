#include "process/ambiguity/GpuWorkerLifetime.h"
#include <array>
#include <cstring>
#include <iostream>
#include <sys/socket.h>
#include <unistd.h>
using blah2::gpu_receive_deadline::Clock;
using blah2::gpu_receive_deadline::waitReadable;
static void require(bool yes,const char* message){if(!yes)throw std::runtime_error(message);}
struct Pair {
 int descriptors[2];
 explicit Pair(int type){require(socketpair(AF_UNIX,type,0,descriptors)==0,"socketpair failed");}
 ~Pair(){close(descriptors[0]);close(descriptors[1]);}
};
static void expired(int fd){
 const auto started=Clock::now();bool rejected=false;
 try{waitReadable(fd,started-std::chrono::seconds(1),"deadline");}
 catch(const std::runtime_error&){rejected=true;}
 require(rejected&&Clock::now()-started<std::chrono::milliseconds(100),"Expired work received a renewed wait");
}
static void queued(int type){
 Pair pair(type);const char expected[]="already complete";std::array<char,sizeof(expected)> actual{};
 require(send(pair.descriptors[1],expected,sizeof(expected),0)==ssize_t(sizeof(expected)),"queue failed");
 const auto deadline=Clock::now()-std::chrono::seconds(1);
 waitReadable(pair.descriptors[0],deadline,"ready reply rejected");
 require(recv(pair.descriptors[0],actual.data(),actual.size(),MSG_DONTWAIT)==ssize_t(sizeof(expected))&&
   std::memcmp(actual.data(),expected,sizeof(expected))==0,"Queued reply changed");
 expired(pair.descriptors[0]);
}
int main()try{
 queued(SOCK_STREAM);
#ifndef __APPLE__
 queued(SOCK_SEQPACKET);
#endif
 Pair partial(SOCK_STREAM);require(send(partial.descriptors[1],"a",1,0)==1,"partial queue failed");
 waitReadable(partial.descriptors[0],Clock::now()-std::chrono::seconds(1),"partial prefix missing");
 char byte=0;require(recv(partial.descriptors[0],&byte,1,MSG_DONTWAIT)==1,"partial read failed");
 expired(partial.descriptors[0]); // A ready prefix never renews the suffix wait.
 const auto start=Clock::now();bool rejected=false;
 try{waitReadable(partial.descriptors[0],start+std::chrono::milliseconds(40),"timeout");}
 catch(const std::runtime_error&){rejected=true;}
 const auto elapsed=Clock::now()-start;
 require(rejected&&elapsed>=std::chrono::milliseconds(30)&&elapsed<std::chrono::seconds(1),"True timeout lost its bound");
 std::cout<<"GPU_RECEIVE_DEADLINE_PASS queued_expired=1 partial_no_renewal=1 true_timeout=1\n";
 return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
