#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "GpuProcess.h"
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <spawn.h>
#include <stdexcept>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

extern char** environ;
namespace blah2 {
namespace {
using Clock = std::chrono::steady_clock;
using Complex = std::complex<float>;
constexpr uint32_t protocol = 0x42475001;
enum Operation : uint32_t { initialize = 1, frame = 2, ready = 3, failure = 4, quit = 5 };
struct Message {
  uint32_t version = protocol, operation = 0;
  uint64_t sequence = 0, memoryBytes = 0;
  GpuGeometry geometry{};
  char id[96]{}, name[256]{}, reason[512]{};
};
struct Layout {
  size_t reference, surveillance, output, bytes;
  explicit Layout(GpuGeometry g) {
    if (!g.range || g.range > 65535 || !g.doppler || g.doppler > 65535 ||
        !g.delays || g.delays > 65535 || !g.channels || g.channels > 8)
      throw std::runtime_error("GPU radar dimensions are unsupported; using CPU");
    const uint64_t r = uint64_t(g.range) * g.doppler;
    const uint64_t s = r * g.channels, o = uint64_t(g.doppler) * g.delays * g.channels;
    const uint64_t total = (r + s + o) * sizeof(Complex);
    if (total > (512ULL << 20))
      throw std::runtime_error("GPU shared-memory budget exceeded; using CPU");
    reference = r; surveillance = s; output = o; bytes = total;
  }
};
void sendMessage(int fd, const Message& message) {
  ssize_t count;
  do { count = send(fd, &message, sizeof(message), MSG_NOSIGNAL | MSG_DONTWAIT); }
  while (count < 0 && errno == EINTR);
  if (count != sizeof(message)) throw std::runtime_error("GPU worker connection failed; using CPU");
}
Message receiveMessage(int fd, unsigned timeoutMs) {
  const auto deadline = Clock::now() + std::chrono::milliseconds(timeoutMs);
  for (;;) {
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
    if (left <= 0) throw std::runtime_error("GPU worker timed out; using CPU");
    pollfd descriptor{fd, POLLIN, 0};
    const int result = poll(&descriptor, 1, std::min<int64_t>(left, INT_MAX));
    if (result < 0 && errno == EINTR) continue;
    if (result < 0) throw std::runtime_error("GPU worker communication failed; using CPU");
    if (!result) continue;
    Message message;
    const auto count = recv(fd, &message, sizeof(message), MSG_TRUNC | MSG_DONTWAIT);
    if (count < 0 && (errno == EINTR || errno == EAGAIN)) continue;
    if (count <= 0) throw std::runtime_error("GPU worker stopped unexpectedly; using CPU");
    if (count != sizeof(message) || message.version != protocol)
      throw std::runtime_error("GPU worker returned an invalid response; using CPU");
    message.id[sizeof(message.id)-1] = '\0';
    message.name[sizeof(message.name)-1] = '\0';
    message.reason[sizeof(message.reason)-1] = '\0';
    return message;
  }
}
void reap(pid_t process) noexcept {
  if (process <= 0) return;
  int status;
  pid_t result;
  do { result = waitpid(process, &status, WNOHANG); } while (result < 0 && errno == EINTR);
  if (result != 0) return;
  kill(process, SIGKILL);
  // Never wait indefinitely for a broken kernel driver. The shared mapping stays
  // alive in the child/kernel until it exits; parent input is independent.
  const auto deadline = Clock::now() + std::chrono::milliseconds(100);
  do {
    result = waitpid(process, &status, WNOHANG);
    if (result == process || (result < 0 && errno != EINTR)) return;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  } while (Clock::now() < deadline);
  try {
    std::thread([process] {
      int code;
      while (waitpid(process, &code, 0) < 0 && errno == EINTR) {}
    }).detach();
  } catch (...) {} // SIGKILL was already sent; parent must still recover.
}
class Process final : public GpuBackend {
  Layout layout_;
  GpuProcessOptions options_;
  int socket_ = -1, memory_ = -1;
  pid_t process_ = -1;
  Complex* shared_ = nullptr;
  GpuDevice device_;
  uint64_t sequence_ = 0;
  void stop() noexcept {
    if (socket_ >= 0) { close(socket_); socket_ = -1; }
    reap(process_); process_ = -1;
    if (shared_) { munmap(shared_, layout_.bytes); shared_ = nullptr; }
    if (memory_ >= 0) { close(memory_); memory_ = -1; }
  }
public:
  Process(GpuGeometry geometry, const std::string& device, GpuProcessOptions options)
    : layout_(geometry), options_(std::move(options)) {
    int childSocket = -1, childMemory = -1;
    posix_spawn_file_actions_t actions;
    bool actionsReady = false;
    try {
      if (device.size() >= sizeof(Message::id) || !options_.startupMs || !options_.frameMs)
        throw std::invalid_argument("Invalid GPU worker options");
      int sockets[2];
      if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sockets))
        throw std::runtime_error("Cannot create GPU worker connection; using CPU");
      socket_ = sockets[0];
      childSocket = fcntl(sockets[1], F_DUPFD_CLOEXEC, 10);
      close(sockets[1]);
      memory_ = memfd_create("blah2-gpu-frame", MFD_CLOEXEC | MFD_ALLOW_SEALING);
      if (childSocket < 0 || memory_ < 0 || ftruncate(memory_, layout_.bytes) ||
          fcntl(memory_, F_ADD_SEALS, F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL) < 0)
        throw std::runtime_error("Cannot allocate GPU shared memory; using CPU");
      void* mapping = mmap(nullptr, layout_.bytes, PROT_READ | PROT_WRITE, MAP_SHARED, memory_, 0);
      if (mapping == MAP_FAILED) throw std::runtime_error("Cannot map GPU shared memory; using CPU");
      shared_ = static_cast<Complex*>(mapping);
      childMemory = fcntl(memory_, F_DUPFD_CLOEXEC, 10);
      if (childMemory < 0 || posix_spawn_file_actions_init(&actions))
        throw std::runtime_error("Cannot prepare GPU worker; using CPU");
      actionsReady = true;
      if (posix_spawn_file_actions_adddup2(&actions, childSocket, 3) ||
          posix_spawn_file_actions_adddup2(&actions, childMemory, 4))
        throw std::runtime_error("Cannot prepare GPU worker descriptors; using CPU");
      // Close inherited descriptors after installing our two endpoints. This also
      // prevents a GPU worker from accidentally keeping capture/listening sockets alive.
#if defined(__GLIBC__) && defined(__GLIBC_MINOR__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 34))
      if (posix_spawn_file_actions_addclosefrom_np(&actions, 5))
        throw std::runtime_error("Cannot isolate GPU worker descriptors; using CPU");
#else
      DIR* descriptors = opendir("/proc/self/fd");
      if (!descriptors) throw std::runtime_error("Cannot inspect GPU worker descriptors; using CPU");
      while (const auto* entry = readdir(descriptors)) {
        const int fd = std::atoi(entry->d_name);
        if (fd > 4) posix_spawn_file_actions_addclose(&actions, fd);
      }
      closedir(descriptors);
#endif
      std::string executable = options_.executable.empty() ? gpuSiblingPath("blah2-gpu-worker") : options_.executable;
      char* argv[] = {executable.data(), options_.argument.empty() ? nullptr : options_.argument.data(), nullptr};
      const int error = posix_spawn(&process_, executable.c_str(), &actions, nullptr, argv, environ);
      if (error) { process_ = -1; throw std::runtime_error("GPU worker is unavailable; using CPU"); }
      close(childSocket); childSocket = -1;
      close(childMemory); childMemory = -1;
      posix_spawn_file_actions_destroy(&actions); actionsReady = false;
      Message init; init.operation = initialize; init.geometry = geometry;
      std::snprintf(init.id, sizeof(init.id), "%s", device.c_str());
      sendMessage(socket_, init);
      const auto response = receiveMessage(socket_, options_.startupMs);
      if (response.operation == failure) throw std::runtime_error(response.reason);
      if (response.operation != ready || response.sequence)
        throw std::runtime_error("GPU worker initialization response is invalid; using CPU");
      device_ = {response.id, response.name, response.memoryBytes};
    } catch (...) {
      if (actionsReady) posix_spawn_file_actions_destroy(&actions);
      if (childSocket >= 0) close(childSocket);
      if (childMemory >= 0) close(childMemory);
      stop(); throw;
    }
  }
  ~Process() override { stop(); }
  GpuDevice device() const override { return device_; }
  void process(const std::vector<Complex>& reference, const std::vector<Complex>& surveillance,
      std::vector<Complex>& output) override {
    if (socket_ < 0) throw std::runtime_error("GPU worker is no longer available; using CPU");
    if (reference.size() != layout_.reference || surveillance.size() != layout_.surveillance)
      throw std::invalid_argument("GPU input dimensions changed");
    try {
      std::memcpy(shared_, reference.data(), reference.size() * sizeof(Complex));
      std::memcpy(shared_ + layout_.reference, surveillance.data(), surveillance.size() * sizeof(Complex));
      Message request; request.operation = frame; request.sequence = ++sequence_;
      sendMessage(socket_, request);
      const auto response = receiveMessage(socket_, options_.frameMs);
      if (response.operation == failure) throw std::runtime_error(response.reason);
      if (response.operation != ready || response.sequence != sequence_)
        throw std::runtime_error("GPU worker returned the wrong frame; using CPU");
      output.assign(shared_ + layout_.reference + layout_.surveillance,
        shared_ + layout_.reference + layout_.surveillance + layout_.output);
    } catch (...) { stop(); throw; }
  }
};
}
std::string gpuSiblingPath(const char* filename) {
  char executable[PATH_MAX];
  const auto length = readlink("/proc/self/exe", executable, sizeof(executable)-1);
  if (length < 0 || length == sizeof(executable)-1) throw std::runtime_error("Cannot locate GPU worker files");
  executable[length] = '\0';
  std::string path(executable);
  return path.substr(0, path.find_last_of('/')) + "/" + filename;
}
std::unique_ptr<GpuBackend> createGpuProcess(const GpuGeometry& geometry,
    const std::string& device, const GpuProcessOptions& options) {
  return std::make_unique<Process>(geometry, device, options);
}
int runGpuWorker(const GpuFactory& factory) {
  const pid_t parent = getppid();
  if (parent == 1 || prctl(PR_SET_PDEATHSIG, SIGKILL) || getppid() != parent) return 1;
  prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0);
  const rlimit noCore{0, 0}; setrlimit(RLIMIT_CORE, &noCore);
  void* mapping = MAP_FAILED;
  size_t bytes = 0;
  try {
    const auto init = receiveMessage(3, 30000);
    if (init.operation != initialize || init.sequence) throw std::runtime_error("Invalid GPU worker initialization");
    const Layout layout(init.geometry);
    struct stat metadata{};
    if (fstat(4, &metadata) || metadata.st_size != static_cast<off_t>(layout.bytes))
      throw std::runtime_error("Invalid GPU worker shared memory");
    bytes = layout.bytes;
    mapping = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, 4, 0);
    if (mapping == MAP_FAILED) throw std::runtime_error("Cannot map GPU worker input");
    const auto* shared = static_cast<const Complex*>(mapping);
    auto backend = factory(init.geometry, init.id);
    if (!backend) throw std::runtime_error("GPU module did not create a processor");
    const auto device = backend->device();
    Message response; response.operation = ready; response.memoryBytes = device.memoryBytes;
    std::snprintf(response.id, sizeof(response.id), "%s", device.id.c_str());
    std::snprintf(response.name, sizeof(response.name), "%s", device.name.c_str());
    sendMessage(3, response);
    std::vector<Complex> reference(layout.reference), surveillance(layout.surveillance), output;
    uint64_t sequence = 0;
    for (;;) {
      // Idle waits are not GPU work. Parent death closes the socket or kills us.
      const auto request = receiveMessage(3, INT_MAX);
      if (request.operation == quit) break;
      if (request.operation != frame || request.sequence != ++sequence)
        throw std::runtime_error("Invalid GPU worker frame request");
      std::memcpy(reference.data(), shared, layout.reference * sizeof(Complex));
      std::memcpy(surveillance.data(), shared + layout.reference, layout.surveillance * sizeof(Complex));
      backend->process(reference, surveillance, output);
      if (output.size() != layout.output) throw std::runtime_error("GPU returned invalid radar dimensions; using CPU");
      std::memcpy(static_cast<Complex*>(mapping) + layout.reference + layout.surveillance,
        output.data(), layout.output * sizeof(Complex));
      response.sequence = sequence; sendMessage(3, response);
    }
    munmap(mapping, bytes); return 0;
  } catch (const std::exception& error) {
    Message response; response.operation = failure;
    std::snprintf(response.reason, sizeof(response.reason), "%s", error.what());
    try { sendMessage(3, response); } catch (...) {}
    if (mapping != MAP_FAILED) munmap(mapping, bytes);
    return 1;
  }
}
}
