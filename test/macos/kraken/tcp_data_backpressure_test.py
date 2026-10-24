#!/usr/bin/env python3
"""Exercise Heimdall's actual TCP data-server tail/backpressure loop without USB."""
import argparse
import pathlib
import shutil
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[3]
ap = argparse.ArgumentParser()
ap.add_argument('--source', type=pathlib.Path,
                default=ROOT / 'build/kraken-macos/upstream/krakensdr_suite/heimdall_v2')
source = ap.parse_args().source.resolve()
needed = ('src/net/tcp_data_server.cpp', 'src/net/tcp_data_server.hpp',
          'src/net/tcp_common.cpp', 'src/net/tcp_common.hpp', 'src/core/ingest_timing.hpp')
for name in needed:
    if not (source / name).is_file():
        raise SystemExit(f'missing source input: {source / name}')

stubs = {
'src/core/types.hpp': '''#pragma once
#include "ingest_timing.hpp"
#include <atomic>
#include <complex>
#include <cstdint>
#include <memory>
#include <vector>
using Complex=std::complex<float>; using ComplexBuffer=std::vector<Complex>;
enum class OperatingMode { COHERENT, WIDEBAND_SCAN, INDEPENDENT };
extern std::atomic<int> current_gain,active_num_elements; extern std::atomic<uint64_t> current_frequency;
extern std::atomic<OperatingMode> operating_mode;
struct WidebandConfig { uint32_t get_tuner_frequency(int) const{return 101000000;} int get_tuner_gain(int) const{return 200;} };
extern WidebandConfig wideband_config;
struct Scanner { std::atomic<uint32_t> frequency_change_counter{0},current_group_index{0}; std::atomic<bool> retuning_in_progress{false}; };
extern Scanner discrete_scanner;
struct SDRDevice { std::atomic<int> actual_gain_tenths{200}; };
extern std::vector<std::unique_ptr<SDRDevice>> devices;
''',
'src/core/config.hpp': '''#pragma once
#include <cstdint>
#include <string>
constexpr uint32_t TCP_MAGIC=0x4d434851; extern int tcp_data_port; extern std::string heimdall_bind_host;
''',
'src/core/utils.hpp': '''#pragma once
#include <algorithm>
#include <cstdint>
#include <vector>
template<class T> constexpr T clamp(T v,T lo,T hi){return std::min(std::max(v,lo),hi);}
inline void append_big_endian(std::vector<uint8_t>& v,uint32_t x){for(int s=24;s>=0;s-=8)v.push_back((x>>s)&255);}
''',
'src/dsp/compensation.hpp': '''#pragma once
#include <atomic>
#include <optional>
enum class PhaseCompensatorState : uint32_t { WAITING_FOR_LAG_COMPLETION=0, CONVERGED=5 };
inline std::optional<PhaseCompensatorState> get_phase_compensation_state_nonblocking(){return PhaseCompensatorState::CONVERGED;}
inline bool kerberos_manual_cal_only(){return false;} extern std::atomic<bool> kerberos_cal_stale;
''',
'src/sdr/pipeline_control.hpp': '#pragma once\n',
'src/sdr/sdr_device.hpp': '#pragma once\n#include "../core/types.hpp"\n',
}

program = r'''
#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cerrno>
#include <string>
#include <thread>
#include <vector>
#include <sys/socket.h>
#include <poll.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#define private public
#include "net/tcp_data_server.hpp"
#undef private

std::atomic<int> current_gain{200}, active_num_elements{1};
std::atomic<uint64_t> current_frequency{101000000};
std::atomic<OperatingMode> operating_mode{OperatingMode::COHERENT};
WidebandConfig wideband_config;
Scanner discrete_scanner;
std::vector<std::unique_ptr<SDRDevice>> devices = [] {
    std::vector<std::unique_ptr<SDRDevice>> value;
    value.emplace_back(std::make_unique<SDRDevice>());
    return value;
}();
std::atomic<bool> kerberos_cal_stale{false}, bias_tee_enabled{false};
int tcp_data_port = 0;
std::string heimdall_bind_host = "127.0.0.1";
constexpr uint32_t SOURCE_EPOCH = 9;
constexpr size_t LARGE_SAMPLES = 32768;
constexpr size_t LARGE_PACKET = 40 + LARGE_SAMPLES * 2;

void nonblocking(int fd) { assert(set_socket_nonblocking(fd)); }
void send_buffer(int fd, int bytes) {
    assert(setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &bytes, sizeof bytes) == 0);
}
std::vector<ComplexBuffer> frame(float value, size_t samples = LARGE_SAMPLES) {
    return {ComplexBuffer(samples, Complex(value, value))};
}
uint32_t big_endian(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
           (uint32_t(p[2]) << 8) | p[3];
}
void drain_available(int fd, std::vector<uint8_t>& bytes) {
    uint8_t buffer[8192];
    while (true) {
        ssize_t count = read(fd, buffer, sizeof buffer);
        if (count > 0) {
            bytes.insert(bytes.end(), buffer, buffer + count);
        } else {
            assert(count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK));
            return;
        }
    }
}
struct ReceivedFrame { uint8_t marker; uint32_t epoch; };
std::vector<ReceivedFrame> parse_complete(const std::vector<uint8_t>& bytes) {
    std::vector<ReceivedFrame> result;
    size_t offset = 0;
    while (offset < bytes.size()) {
        assert(bytes.size() - offset >= 40); // No ignored trailing partial header.
        const uint8_t* header = bytes.data() + offset;
        assert(big_endian(header) == 0x4d434851);
        assert(big_endian(header + 4) == 1);
        const uint32_t samples = big_endian(header + 8);
        assert(samples == LARGE_SAMPLES || samples == 16);
        const size_t size = 40 + size_t(samples) * 2;
        assert(bytes.size() - offset >= size); // No ignored trailing partial payload.
        const uint8_t marker = header[40];
        assert(std::all_of(header + 40, header + size,
                           [marker](uint8_t value) { return value == marker; }));
        result.push_back({marker, big_endian(header + 20)});
        offset += size;
    }
    assert(offset == bytes.size());
    return result;
}

void test_accept_loop_cap() {
    TcpDataServer limited;
    tcp_data_port = 0; // Owned loopback listener with an OS-selected unused port.
    assert(limited.start());
    sockaddr_in address{};
    socklen_t length = sizeof address;
    assert(getsockname(limited.server_socket, reinterpret_cast<sockaddr*>(&address), &length) == 0);
    assert(ntohs(address.sin_port) != 0);
    std::vector<int> peers;
    for (size_t expected = 1; expected <= 8; ++expected) {
        int peer = socket(AF_INET, SOCK_STREAM, 0);
        assert(peer >= 0);
        assert(connect(peer, reinterpret_cast<sockaddr*>(&address), sizeof address) == 0);
        nonblocking(peer);
        peers.push_back(peer);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (limited.client_count() != expected && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        assert(limited.client_count() == expected);
    }
    int ninth = socket(AF_INET, SOCK_STREAM, 0);
    assert(ninth >= 0);
    assert(connect(ninth, reinterpret_cast<sockaddr*>(&address), sizeof address) == 0);
    pollfd rejected{ninth, POLLIN | POLLHUP, 0};
    assert(poll(&rejected, 1, 2000) == 1);
    uint8_t byte;
    const ssize_t received = recv(ninth, &byte, 1, 0);
    assert(received == 0 || (received < 0 && errno == ECONNRESET));
    assert(limited.client_count() == 8);
    // Existing eight still receive complete frames after the ninth is refused.
    limited.broadcast_data(frame(1, 16), 5, 0);
    for (int peer : peers) {
        std::vector<uint8_t> bytes;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (bytes.size() < 72 && std::chrono::steady_clock::now() < deadline) {
            drain_available(peer, bytes);
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        const auto packets = parse_complete(bytes);
        assert(packets.size() == 1 && packets[0].marker == 255);
        close(peer);
    }
    close(ninth);
    limited.stop();
}

int main() {
    int healthy[2], stalled[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, healthy) == 0);
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, stalled) == 0);
    for (int fd : {healthy[0], healthy[1], stalled[0], stalled[1]}) nonblocking(fd);
    send_buffer(healthy[0], 1 << 20);
    send_buffer(stalled[0], 1024);
    TcpDataServer server;
    server.clients.emplace_back(std::make_unique<TcpClient>(healthy[0]));
    server.clients.emplace_back(std::make_unique<TcpClient>(stalled[0]));
    discrete_scanner.frequency_change_counter = SOURCE_EPOCH;
    std::vector<uint8_t> fast_bytes, slow_bytes;

    // A begins on both peers. The stalled socket retains exactly one bounded tail.
    server.broadcast_data(frame(-1), 5, 0);
    drain_available(healthy[1], fast_bytes);
    const std::string original_tail = server.clients[1]->tx_buffer;
    assert(!original_tail.empty() && original_tail.size() < LARGE_PACKET);
    assert(server.clients[0]->tx_buffer.empty());

    // B is wholly dropped for the stalled peer; repeated blocking cannot grow
    // its tail or mutate A. The healthy peer receives each B independently.
    for (int attempt = 0; attempt < 6; ++attempt) {
        server.broadcast_data(frame(0), 5, 0);
        drain_available(healthy[1], fast_bytes);
        assert(server.clients[1]->tx_buffer == original_tail);
        assert(server.clients[1]->tx_buffer.size() <= LARGE_PACKET);
        assert(server.clients[0]->tx_buffer.empty());
        assert(discrete_scanner.frequency_change_counter == SOURCE_EPOCH);
    }

    // Read A to completion. Small C fits once the old tail drains, allowing a
    // deterministic complete final packet without closing on a partial frame.
    bool flushed = false;
    for (int attempt = 0; attempt < 300; ++attempt) {
        drain_available(stalled[1], slow_bytes);
        server.broadcast_data(frame(1, 16), 5, 0);
        drain_available(healthy[1], fast_bytes);
        assert(server.clients[1]->tx_buffer.size() <= LARGE_PACKET);
        drain_available(stalled[1], slow_bytes);
        // An empty tail alone can mean C was wholly dropped on EAGAIN.
        // Finish only after the peer actually received a complete fresh C.
        if (server.clients[1]->tx_buffer.empty() && slow_bytes.size() >= LARGE_PACKET + 72) {
            flushed = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    assert(flushed);
    drain_available(stalled[1], slow_bytes);
    drain_available(healthy[1], fast_bytes);
    const auto slow = parse_complete(slow_bytes);
    const auto fast = parse_complete(fast_bytes);
    assert(slow.size() >= 2 && slow.front().marker == 0 && slow.back().marker == 255);
    assert(slow.front().epoch == SOURCE_EPOCH);
    assert(slow.back().epoch > SOURCE_EPOCH); // Gap is visible only to slow peer.
    assert(std::none_of(slow.begin(), slow.end(),
                        [](const auto& packet) { return packet.marker == 128; }));
    assert(std::count_if(fast.begin(), fast.end(),
                        [](const auto& packet) { return packet.marker == 128; }) == 6);
    assert(std::all_of(fast.begin(), fast.end(),
                       [](const auto& packet) { return packet.epoch == SOURCE_EPOCH; }));
    assert(discrete_scanner.frequency_change_counter == SOURCE_EPOCH);

    // Prove this exact disconnected peer is removed while the two live peers stay.
    int dead[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, dead) == 0);
    nonblocking(dead[0]);
    const int dead_fd = dead[0];
    close(dead[1]);
    server.clients.emplace_back(std::make_unique<TcpClient>(dead_fd));
    assert(server.clients.size() == 3);
    server.broadcast_data(frame(.5f, 16), 5, 0);
    assert(server.clients.size() == 2);
    assert(std::none_of(server.clients.begin(), server.clients.end(),
                       [dead_fd](const auto& client) { return client->socket == dead_fd; }));
    close(healthy[1]);
    close(stalled[1]);
    test_accept_loop_cap();
}
'''

with tempfile.TemporaryDirectory(prefix='vectorwarp-tcp-data-') as temporary:
    work = pathlib.Path(temporary)
    for name in needed:
        target = work / name
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source / name, target)
    for name, content in stubs.items():
        target = work / name
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(content)
    test = work / 'test.cpp'; test.write_text(program)
    exe = work / 'test'
    subprocess.run(['c++', '-std=c++20', '-pthread', '-I', str(work / 'src'), str(test),
                    str(work / 'src/net/tcp_common.cpp'), str(work / 'src/net/tcp_data_server.cpp'),
                    '-o', str(exe)], check=True, timeout=30)
    subprocess.run([str(exe)], check=True, timeout=30)
print('actual TCP data bounded tail, whole-frame drops, complete framing, per-peer epochs, healthy progress, exact dead-peer cleanup, real accept-loop eight-client cap: PASS')
