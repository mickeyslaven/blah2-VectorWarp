#!/usr/bin/env python3
"""Focused source-independent test for Heimdall's bounded complete-frame writer."""
import argparse, pathlib, subprocess, tempfile
ROOT=pathlib.Path(__file__).resolve().parents[3]
p=argparse.ArgumentParser(); p.add_argument('--source',type=pathlib.Path,default=ROOT/'build/kraken-macos/upstream/krakensdr_suite/heimdall_v2'); a=p.parse_args()
src=a.source.resolve()
code=r'''
#include "net/tcp_common.hpp"
#include <sys/socket.h>
#include <thread>
#include <vector>
#include <cassert>
#include <unistd.h>
#include <chrono>
int main(){
 int a[2]; assert(socketpair(AF_UNIX,SOCK_STREAM,0,a)==0); set_socket_nonblocking(a[0]); int small=1024; setsockopt(a[0],SOL_SOCKET,SO_SNDBUF,&small,sizeof(small));
 std::vector<uint8_t> p(1<<20,7), got; got.reserve(p.size());
 std::thread r([&]{ uint8_t b[8192]; while(got.size()<p.size()){ auto n=read(a[1],b,sizeof b); if(n>0) got.insert(got.end(),b,b+n); else if(n==0) return; }});
 auto good=send_packet_with_deadline(a[0],p.data(),p.size(),std::chrono::milliseconds(1500)); assert(good.complete && good.bytes_written==p.size() && good.failure==PacketWriteFailure::none); r.join(); assert(got==p);
 std::vector<uint8_t> fill(1<<20,1); (void)send(a[0],fill.data(),fill.size(),MSG_NOSIGNAL);
 auto t=std::chrono::steady_clock::now(); auto timed=send_packet_with_deadline(a[0],p.data(),p.size(),std::chrono::milliseconds(20)); assert(!timed.complete && timed.failure==PacketWriteFailure::deadline && timed.bytes_written < p.size() && timed.error_number==0); auto ms=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-t).count(); assert(ms>=15 && ms<200);
 close(a[1]); auto closed=send_packet_with_deadline(a[0],p.data(),p.size(),std::chrono::milliseconds(20)); assert(!closed.complete && closed.failure==PacketWriteFailure::peer_closed); close(a[0]);
}'''
with tempfile.TemporaryDirectory() as d:
 q=pathlib.Path(d); f=q/'t.cpp'; f.write_text(code); out=q/'t'
 subprocess.run(['c++','-std=c++20','-I',str(src/'src'),str(f),str(src/'src/net/tcp_common.cpp'),'-o',str(out)],check=True,timeout=30)
 subprocess.run([str(out)],check=True,timeout=30)
print('tcp full-frame recovery, bounded EAGAIN deadline, and peer-close: PASS')
