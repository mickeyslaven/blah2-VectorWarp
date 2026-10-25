#!/usr/bin/env python3
"""Exercise actual server start/stop/listener bodies with deterministic fake FDs.

No sockets or receivers are opened. select is paused until the test observes
stop entering join, so the old close-before-join ordering fails deterministically.
RTL sample production is stubbed; the actual worker and server own write/close.
"""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile


def function(text, signature):
    start = text.index(signature)
    opening = text.index('{', start)
    depth = 1
    end = opening + 1
    # These selected methods contain no braces in string literals. Strip comments
    # for counting, preserving the original method text for compilation.
    tokens = re.finditer(r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\])*"|[{}]', text[opening + 1:], re.S)
    for token in tokens:
        if token.group() == '{': depth += 1
        elif token.group() == '}': depth -= 1
        if depth == 0:
            end = opening + 1 + token.end()
            return text[start:end]
    raise ValueError('Unterminated actual method: ' + signature)


PRELUDE = r'''
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#if defined(__APPLE__)
#define TCP_KEEPIDLE TCP_KEEPALIVE
#endif
using namespace std::chrono_literals;
std::mutex observation_mutex;
std::condition_variable observation;
std::set<int> open_fds, nonblocking_fds;
std::map<int,int> closes;
std::map<int,std::thread::id> close_owner;
int listener_number=40, queued_accept=-1, fail_stage=0, fail_nonblocking_fd=-1;
bool select_entered=false, release_select=false, join_entered=false;
bool worker_entered=false, release_worker=false;
int accept_calls=0, send_calls=0;
std::thread::id listener_thread;

void reset() {
    std::lock_guard<std::mutex> lock(observation_mutex);
    assert(open_fds.empty());
    nonblocking_fds.clear(); closes.clear(); close_owner.clear();
    listener_number=40; queued_accept=-1; fail_stage=0; fail_nonblocking_fd=-1;
    select_entered=release_select=join_entered=worker_entered=release_worker=false;
    accept_calls=send_calls=0;
}
void claim(int fd) { std::lock_guard<std::mutex> lock(observation_mutex); assert(open_fds.insert(fd).second); }
bool is_open(int fd) { std::lock_guard<std::mutex> lock(observation_mutex); return open_fds.count(fd); }
int fake_socket(int,int,int) { if(fail_stage==1) return -1; claim(listener_number); return listener_number; }
int fake_close(int fd) {
    std::lock_guard<std::mutex> lock(observation_mutex);
    assert(open_fds.erase(fd)==1); ++closes[fd]; close_owner[fd]=std::this_thread::get_id();
    observation.notify_all(); return 0;
}
int fake_bind(int,const sockaddr*,socklen_t) { return fail_stage==5?-1:0; }
int fake_listen(int,int) { return fail_stage==6?-1:0; }
int fake_pton(int,const char*,void*) { return fail_stage==4?0:1; }
int fake_setsockopt(int,int,int,const void*,socklen_t) { return 0; }
bool set_socket_reuse(int) { return fail_stage!=2; }
bool set_socket_nonblocking(int fd) {
    if(fail_stage==3 || fd==fail_nonblocking_fd) return false;
    std::lock_guard<std::mutex> lock(observation_mutex); nonblocking_fds.insert(fd); return true;
}
void checked_fd(int fd) { assert(fd>=0 && fd<FD_SETSIZE && is_open(fd)); }
void checked_set(int fd, fd_set* set) { checked_fd(fd); FD_SET(fd,set); }
int checked_isset(int fd, fd_set* set) { checked_fd(fd); return FD_ISSET(fd,set); }
int fake_select(int,fd_set* reads,fd_set*,fd_set*,timeval*) {
    std::unique_lock<std::mutex> lock(observation_mutex);
    if (!reads) {
        assert(open_fds.count(41) && nonblocking_fds.count(41));
        worker_entered=true; observation.notify_all();
        assert(observation.wait_for(lock,2s,[]{return release_worker;}));
        return 1;
    }
    listener_thread=std::this_thread::get_id();
    assert(open_fds.count(listener_number) && nonblocking_fds.count(listener_number));
    select_entered=true; observation.notify_all();
    assert(observation.wait_for(lock,2s,[]{return release_select;}));
    return 1;
}
int fake_accept(int fd,sockaddr*,socklen_t*) {
    checked_fd(fd);
    std::lock_guard<std::mutex> lock(observation_mutex);
    ++accept_calls;
    const int value=queued_accept; queued_accept=-1;
    if(value>=0) assert(open_fds.insert(value).second);
    else errno=EAGAIN;
    observation.notify_all(); return value;
}
ssize_t fake_send(int fd,const void*,size_t bytes,int) {
    checked_fd(fd); ++send_calls; return static_cast<ssize_t>(bytes);
}
struct JoiningThread {
    std::thread thread;
    JoiningThread& operator=(std::thread&& value) { thread=std::move(value); return *this; }
    bool joinable() const { return thread.joinable(); }
    void join() {
        {
            std::lock_guard<std::mutex> lock(observation_mutex);
            // Ignore the RTL server joining its worker; observe only stop's
            // external join of the listener thread.
            if(std::this_thread::get_id()!=listener_thread) join_entered=true;
            observation.notify_all();
        }
        thread.join();
    }
};
struct TcpClient {
    int socket; bool active=true; std::string tx_buffer;
    explicit TcpClient(int fd):socket(fd) {}
    ~TcpClient() { fake_close(socket); }
};
int tcp_data_port=0, tcp_control_port=0, rtl_tcp_port=0;
std::string heimdall_bind_host="127.0.0.1";
std::atomic<int> active_num_elements{5}, rtl_tcp_channel{0};
constexpr size_t MAX_DATA_CLIENTS=8, MAX_CONTROL_CLIENTS=8;
#define socket(...) fake_socket(__VA_ARGS__)
#define close(...) fake_close(__VA_ARGS__)
#define bind(...) fake_bind(__VA_ARGS__)
#define listen(...) fake_listen(__VA_ARGS__)
#define inet_pton(...) fake_pton(__VA_ARGS__)
#define setsockopt(...) fake_setsockopt(__VA_ARGS__)
#define select(...) fake_select(__VA_ARGS__)
#define accept(...) fake_accept(__VA_ARGS__)
#define send(...) fake_send(__VA_ARGS__)
#define sock_socket(...) fake_socket(__VA_ARGS__)
#define sock_close(...) fake_close(__VA_ARGS__)
#define sock_bind(...) fake_bind(__VA_ARGS__)
#define sock_listen(...) fake_listen(__VA_ARGS__)
#define sock_select(...) fake_select(__VA_ARGS__)
#define sock_accept(...) fake_accept(__VA_ARGS__)
#define sock_send(...) fake_send(__VA_ARGS__)
#define sock_setsockopt(...) fake_setsockopt(__VA_ARGS__)
#define sock_fits_fd_set(fd,count) ((fd) >= 0 && (fd) < FD_SETSIZE)
#define sock_set_keepalive(fd,idle,interval,count) ((void)0)
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif
#undef FD_SET
#undef FD_ISSET
#define FD_SET checked_set
#define FD_ISSET checked_isset
struct TcpDataServer {
    int server_socket=-1; std::atomic<bool> running{true}; JoiningThread server_thread;
    std::mutex clients_mutex; std::vector<std::unique_ptr<TcpClient>> clients;
    bool start(); void stop(); void accept_loop();
};
struct TcpControlServer {
    int server_socket=-1; std::atomic<bool> running{true}; JoiningThread server_thread;
    std::mutex clients_mutex; std::vector<std::unique_ptr<TcpClient>> clients;
    bool start(); void stop(); void control_loop();
    void flush_output(TcpClient*) {} void handle_client_data(TcpClient*) {}
    void broadcast_status();
    std::string build_status_json(bool) {return "{}";}
    void queue_output(TcpClient* client,const std::string&) {client->active=false;}
};
struct RtlTcpServer {
    int server_socket=-1; std::atomic<bool> running{true}; JoiningThread server_thread,worker_thread;
    std::atomic<int> client_socket{-1}; std::condition_variable buffer_cv;
    int ignored_cmds_logged=0,cmd_carry_len=0;
    bool start(); void stop(); void server_loop();
    struct BufferNode {std::vector<uint8_t> data{1,2}; BufferNode* next=nullptr;};
    std::mutex buffer_mutex; BufferNode *buffer_head=nullptr,*buffer_tail=nullptr; int buffer_count=0;
    void clear_buffers() {
        while(buffer_head) {auto* next=buffer_head->next;delete buffer_head;buffer_head=next;}
        buffer_tail=nullptr; buffer_count=0;
    }
    bool send_dongle_info(int) {return true;}
    bool read_client_commands(int) {return true;}
    void worker_loop(int client_fd);

};
'''

TESTS = r'''
template<class Predicate> void observe(Predicate predicate) {
    std::unique_lock<std::mutex> lock(observation_mutex);
    assert(observation.wait_for(lock,2s,predicate));
}
void release() { std::lock_guard<std::mutex> lock(observation_mutex); release_select=true; observation.notify_all(); }
template<class Server> void startup_failures() {
    for(int failure=1; failure<=6; ++failure) {
        reset(); fail_stage=failure; Server server;
        assert(!server.start()); assert(server.server_socket==-1);
        if(failure!=1) { assert(closes[40]==1); claim(40); }
        server.stop(); server.stop(); // A reused descriptor must remain untouched.
        if(failure!=1) { assert(is_open(40) && closes[40]==1); fake_close(40); }
    }
    reset(); listener_number=FD_SETSIZE; Server server;
    assert(!server.start() && server.server_socket==-1);
    assert(closes[FD_SETSIZE]==1); server.stop(); server.stop();
}
template<class Server> void deterministic_stop() {
    reset(); Server server; assert(server.start());
    observe([]{return select_entered;});
    std::thread stopping([&]{server.stop();});
    observe([]{return join_entered;});
    // select is still paused. stop must neither close nor reset this listener.
    assert(server.server_socket==40 && is_open(40));
    release(); stopping.join();
    assert(server.server_socket==-1 && closes[40]==1 && accept_calls==0);
    server.stop(); assert(closes[40]==1);
}
template<class Server> void rejected_client(bool high_fd) {
    reset(); queued_accept=high_fd?FD_SETSIZE:41;
    if(!high_fd) fail_nonblocking_fd=41;
    Server server; assert(server.start());
    observe([]{return select_entered;}); release();
    const int refused=high_fd?FD_SETSIZE:41;
    observe([&]{return closes[refused]==1;});
    server.stop(); assert(closes[refused]==1 && !is_open(refused));
}
void rtl_client_owner() {
    reset(); queued_accept=41; RtlTcpServer server;
    server.buffer_head=new RtlTcpServer::BufferNode;
    server.buffer_tail=server.buffer_head; server.buffer_count=1;
    assert(server.start());
    observe([]{return select_entered;}); release();
    observe([]{return worker_entered;});
    std::thread stopping([&]{server.stop();});
    observe([]{return join_entered;});
    assert(server.client_socket.load()==41 && is_open(41) && is_open(40));
    {std::lock_guard<std::mutex> lock(observation_mutex);release_worker=true;observation.notify_all();}
    stopping.join();
    assert(server.client_socket.load()==-1 && closes[41]==1 && closes[40]==1);
    assert(close_owner[41]==listener_thread && send_calls==0);
    server.stop(); assert(closes[41]==1 && closes[40]==1);
}
void control_reaper_owner() {
    reset(); TcpControlServer server; claim(41);
    server.clients.emplace_back(std::make_unique<TcpClient>(41));
    assert(server.start()); observe([]{return select_entered;});
    // This actual broadcaster marks a failed send inactive while select still
    // references the descriptor. It must leave closing to the control thread.
    server.broadcast_status(); server.broadcast_status();
    assert(is_open(41) && closes[41]==0);
    release(); observe([]{return closes[41]==1;});
    server.stop();
    assert(closes[41]==1 && close_owner[41]==listener_thread);
}
int main() {
    startup_failures<TcpDataServer>(); startup_failures<TcpControlServer>(); startup_failures<RtlTcpServer>();
    deterministic_stop<TcpDataServer>(); deterministic_stop<TcpControlServer>(); deterministic_stop<RtlTcpServer>();
    // Data clients are never in fd_set; only their nonblocking failure rejects.
    rejected_client<TcpDataServer>(false);
    rejected_client<TcpControlServer>(true); rejected_client<TcpControlServer>(false);
    rejected_client<RtlTcpServer>(true); rejected_client<RtlTcpServer>(false);
    rtl_client_owner(); control_reaper_owner();
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', required=True, type=Path)
    args = parser.parse_args()
    methods = []
    for filename, klass, loop in [('tcp_data_server.cpp','TcpDataServer','accept_loop'),
                                  ('tcp_control_server.cpp','TcpControlServer','control_loop'),
                                  ('rtl_tcp_server.cpp','RtlTcpServer','server_loop')]:
        text = (args.source / 'src/net' / filename).read_text()
        for kind, method in [('bool','start'),('void','stop'),('void',loop)]:
            methods.append(function(text, f'{kind} {klass}::{method}()'))
    methods.append(function((args.source / 'src/net/rtl_tcp_server.cpp').read_text(),
                            'void RtlTcpServer::worker_loop(int client_fd)'))
    methods.append(function((args.source / 'src/net/tcp_control_server.cpp').read_text(),
                            'void TcpControlServer::broadcast_status()'))
    source = PRELUDE + '\n'.join(methods) + TESTS
    # Restore the faulty native ordering as a negative control, using the actual
    # current close/reset block rather than weakening the test's mock FD checks.
    old = function(source, 'void TcpDataServer::stop()')
    close_block = function(old, 'if (server_socket >= 0)')
    bad = old.replace(close_block, '').replace('running = false;', 'running = false;\n' + close_block)
    assert bad != old
    with tempfile.TemporaryDirectory(prefix='vectorwarp-socket-lifecycle-') as directory:
        work = Path(directory)
        for label, text in [('fixed',source),('close-before-join',source.replace(old,bad))]:
            cpp, binary = work / (label+'.cpp'), work / label
            cpp.write_text(text)
            subprocess.run(['c++','-std=c++20','-O1','-pthread',str(cpp),'-o',str(binary)],check=True,timeout=30)
            result = subprocess.run([str(binary)],capture_output=True,text=True,timeout=15)
            if label=='fixed':
                if result.returncode: raise AssertionError(result.stdout+result.stderr)
            else:
                assert result.returncode != 0, 'Old native close-before-join ordering escaped regression'
                assert 'server.server_socket==40' in result.stderr, result.stderr
    print('Actual native socket lifecycle: paused-select stop, startup failure/reuse, listener/client FD bounds, nonblocking refusal, actual RTL write-worker stop and sole-close ownership, control reaper ownership PASS; old close-before-join rejected')


if __name__ == '__main__':
    main()
