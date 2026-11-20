#include "process/ambiguity/GpuProcess.h"
#include "process/ambiguity/GpuWorkerLifetime.h"
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <poll.h>
#include <stdexcept>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {
void check(bool value, const char* reason) {
  if (!value) throw std::runtime_error(reason);
}

// Retain an ABI-4 peer fixture independently of the current worker internals.
// These offsets catch accidental removal/reordering of the retired fields.
static_assert(blah2::GPU_ABI == 4);
static_assert(sizeof(blah2::GpuGeometry) == 52);
static_assert(offsetof(blah2::GpuGeometry, kind) == 32);
static_assert(offsetof(blah2::GpuGeometry, firSamples) == 36);
static_assert(offsetof(blah2::GpuGeometry, firTaps) == 40);
static_assert(offsetof(blah2::GpuGeometry, firFft) == 44);
static_assert(offsetof(blah2::GpuGeometry, firPercent) == 48);
static_assert(uint32_t(blah2::GpuWorkKind::retiredFir) == 1);
struct LegacyMessage {
  uint32_t version = 0x42475004, operation = 0, capabilities = 0;
  uint64_t sequence = 0, memoryBytes = 0;
  blah2::GpuGeometry geometry{};
  char id[96]{}, name[256]{}, reason[512]{};
};
class Backend final : public blah2::GpuBackend {
public:
  blah2::GpuDevice device() const override { return {"cpu-double", "CPU double", 0}; }
  void process(const std::vector<std::complex<float>>&,
      const std::vector<std::complex<float>>&,
      std::vector<std::complex<float>>&) override {
    throw std::runtime_error("Retired operation reached a processing backend");
  }
};
class Peer {
  int socket_ = -1;
  pid_t child_ = -1;
public:
  Peer() {
    int sockets[2];
#ifdef __APPLE__
    check(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0, "socketpair failed");
#else
    check(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sockets) == 0, "socketpair failed");
#endif
    socket_ = sockets[0];
    const int workerSocket = fcntl(sockets[1], F_DUPFD, 10);
    close(sockets[1]);
    check(workerSocket >= 0, "Cannot duplicate worker socket");
    char path[] = "/tmp/vectorwarp-retired-wire-XXXXXX";
    const int temporary = mkstemp(path);
    check(temporary >= 0, "Cannot create worker mapping");
    check(unlink(path) == 0, "Cannot unlink private worker mapping");
    const int memory = fcntl(temporary, F_DUPFD, 10);
    close(temporary);
    check(memory >= 0, "Cannot duplicate worker mapping");
    // Radar fixture: range=4, doppler=1, delay=1, channel=1.
    size_t bytes = 9 * sizeof(std::complex<float>);
#ifdef __APPLE__
    bytes = size_t(sysconf(_SC_PAGESIZE));
#endif
    check(ftruncate(memory, bytes) == 0, "Cannot size worker mapping");
    const std::string expectedParent = std::to_string(getpid());
    child_ = fork();
    check(child_ >= 0, "Cannot fork CPU worker");
    if (!child_) {
      if (setenv(blah2::gpu_worker_lifetime::parentVariable, expectedParent.c_str(), 1)) _exit(2);
      close(socket_);
      if (dup2(workerSocket, 3) < 0 || dup2(memory, 4) < 0) _exit(2);
      close(workerSocket);
      close(memory);
      _exit(blah2::runGpuWorker([](const blah2::GpuGeometry&, const std::string&) {
        return std::make_unique<Backend>();
      }));
    }
    close(workerSocket);
    close(memory);
  }
  ~Peer() {
    if (socket_ >= 0) close(socket_);
    if (child_ > 0) {
      kill(child_, SIGKILL);
      while (waitpid(child_, nullptr, 0) < 0 && errno == EINTR) {}
    }
  }
  LegacyMessage exchange(const LegacyMessage& request) {
    check(send(socket_, &request, sizeof(request), MSG_NOSIGNAL) == sizeof(request),
      "Cannot send legacy message");
    LegacyMessage response;
    size_t received = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (received < sizeof(response)) {
      const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - std::chrono::steady_clock::now()).count();
      check(left > 0, "Worker did not reject retired work within one second");
      pollfd descriptor{socket_, POLLIN, 0};
      check(poll(&descriptor, 1, int(left)) > 0, "Worker response timed out");
      const auto count = recv(socket_, reinterpret_cast<char*>(&response) + received,
        sizeof(response) - received, 0);
      check(count > 0, "Worker closed without a rejection response");
      received += size_t(count);
    }
    response.reason[sizeof(response.reason) - 1] = '\0';
    check(response.version == request.version, "Worker changed ABI-4 framing");
    return response;
  }
};

void rejectBeforeSpawn(const blah2::GpuGeometry& geometry, const char* reason) {
  blah2::GpuProcessOptions options;
  options.executable = "/nonexistent-retired-gpu-worker";
  for (bool mappingOnly : {true, false}) {
    bool rejected = false;
    try {
      if (mappingOnly) (void)blah2::gpuProcessMappingBytes(geometry);
      else (void)blah2::createGpuProcess(geometry, "auto", options);
    } catch (const std::runtime_error& error) {
      rejected = std::string(error.what()).find(reason) != std::string::npos;
    }
    check(rejected, "Retired geometry was not rejected before allocation/spawn");
  }
  Peer peer;
  LegacyMessage request;
  request.operation = 1;
  request.geometry = geometry;
  const auto response = peer.exchange(request);
  check(response.operation == 4 && std::strstr(response.reason, reason),
    "Legacy worker initialization did not reject retired geometry");
}
}

int main() {
  try {
    for (uint32_t kind : {1u, 2u, UINT32_MAX}) {
      blah2::GpuGeometry geometry{};
      geometry.kind = static_cast<blah2::GpuWorkKind>(kind);
      geometry.firSamples = 10000;
      geometry.firTaps = 410;
      geometry.firFft = 2048;
      geometry.firPercent = 50;
      rejectBeforeSpawn(geometry, "work kind is retired or unsupported");
    }
    for (auto field : {&blah2::GpuGeometry::firSamples, &blah2::GpuGeometry::firTaps,
        &blah2::GpuGeometry::firFft, &blah2::GpuGeometry::firPercent}) {
      blah2::GpuGeometry geometry{4, 1, 1, 1, 0};
      geometry.*field = 1;
      rejectBeforeSpawn(geometry, "reserved geometry fields must be zero");
    }
    for (uint32_t operation : {8u, 9u}) {
      Peer peer;
      LegacyMessage request;
      request.operation = 1;
      request.geometry = {4, 1, 1, 1, 0};
      const auto ready = peer.exchange(request);
      check(ready.operation == 3 && !(ready.capabilities & 2),
        "Radar worker advertised the retired FIR capability");
      request.operation = operation;
      request.sequence = 1;
      const auto rejected = peer.exchange(request);
      check(rejected.operation == 4 &&
        std::strstr(rejected.reason, "Invalid GPU worker frame request"),
        "Worker accepted a retired FIR operation");
    }
    std::cout << "PASS retired ABI-4 kinds, fields, capabilities, and operations\n";
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
