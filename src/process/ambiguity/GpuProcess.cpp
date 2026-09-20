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
#ifdef __APPLE__
#include <mach-o/dyld.h>
#include <sys/event.h>
#else
#include <sys/prctl.h>
#endif
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
constexpr uint32_t protocol = 0x42475004;
enum Operation : uint32_t { initialize = 1, frame = 2, ready = 3, failure = 4,
  quit = 5, clutterFrame = 6, clutterRejected = 7, firReference = 8,
  firFinal = 9 };
constexpr uint32_t clutterCapability = 1;
constexpr uint32_t firCapability = 2;
struct Message {
  uint32_t version = protocol, operation = 0, capabilities = 0;
  uint64_t sequence = 0, memoryBytes = 0;
  GpuGeometry geometry{};
  char id[96]{}, name[256]{}, reason[512]{};
};
struct Layout {
  size_t reference, surveillance, output;
  size_t clutterReference = 0, clutterSurveillance = 0, clutterOutput = 0;
  size_t clutterOffset = 0, bytes;
  size_t firReference = 0, firWeights = 0, firOutput = 0;
  size_t firOffset = 0;
  explicit Layout(GpuGeometry g) {
    if (g.kind == GpuWorkKind::fir) {
      if (g.range || g.doppler || g.delays || g.channels || g.delayMin ||
          g.clutterSamples || g.clutterBins || g.clutterDelayMin ||
          !g.firSamples || g.firSamples > 10000000 || !g.firTaps ||
          g.firTaps > g.firSamples || g.firTaps > 2048 ||
          g.firFft != 2048 || g.firTaps > g.firFft || g.firPercent != 50)
        throw std::runtime_error("GPU FIR dimensions are unsupported; using CPU");
      const uint64_t hop = uint64_t(g.firFft) - g.firTaps + 1;
      const uint64_t totalBlocks = (uint64_t(g.firSamples) + hop - 1) / hop;
      const uint64_t gpuBlocks = std::max<uint64_t>(1, totalBlocks / 2);
      const uint64_t prefix = std::min<uint64_t>(g.firSamples, gpuBlocks * hop);
      const uint64_t totalElements = prefix * 2 + g.firTaps;
      const uint64_t total = totalElements * sizeof(Complex);
      if (total > (512ULL << 20) || prefix > SIZE_MAX)
        throw std::runtime_error("GPU FIR shared-memory budget exceeded; using CPU");
      reference = surveillance = output = 0;
      firReference = firOutput = size_t(prefix); firWeights = g.firTaps;
      firOffset = 0; bytes = size_t(total);
#ifdef __APPLE__
      // Darwin reports POSIX shared-memory lengths rounded to a VM page. Use the
      // same explicit size in both processes so the exact fstat check stays useful.
      const long page = sysconf(_SC_PAGESIZE);
      if (page <= 0) throw std::runtime_error("Cannot size GPU shared memory; using CPU");
      bytes = ((bytes + size_t(page) - 1) / size_t(page)) * size_t(page);
#endif
      return;
    }
    if (g.kind != GpuWorkKind::radar || g.firSamples || g.firTaps ||
        g.firFft || g.firPercent)
      throw std::runtime_error("GPU radar dimensions are unsupported; using CPU");
    if (!g.range || g.range > 65535 || !g.doppler || g.doppler > 65535 ||
        !g.delays || g.delays > 65535 || !g.channels || g.channels > 8)
      throw std::runtime_error("GPU radar dimensions are unsupported; using CPU");
    const uint64_t r = uint64_t(g.range) * g.doppler;
    const uint64_t s = r * g.channels, o = uint64_t(g.doppler) * g.delays * g.channels;
    uint64_t totalElements = r + s + o;
    clutterOffset = totalElements;
    if (g.clutterSamples) {
      if (!g.clutterBins || g.clutterBins > g.clutterSamples ||
          g.clutterDelayMin <= -int64_t(g.clutterSamples) ||
          int64_t(g.clutterDelayMin) + g.clutterBins > g.clutterSamples)
        throw std::runtime_error("GPU clutter dimensions are unsupported; using CPU");
      clutterReference = g.clutterSamples;
      clutterSurveillance = uint64_t(g.clutterSamples) * g.channels;
      clutterOutput = clutterSurveillance;
      totalElements += clutterReference + clutterSurveillance + clutterOutput;
    }
    const uint64_t total = totalElements * sizeof(Complex);
    if (total > (512ULL << 20))
      throw std::runtime_error("GPU shared-memory budget exceeded; using CPU");
    reference = r; surveillance = s; output = o; bytes = total;
#ifdef __APPLE__
    // Darwin reports POSIX shared-memory lengths rounded to a VM page. Use the
    // same explicit size in both processes so the exact fstat check stays useful.
    const long page = sysconf(_SC_PAGESIZE);
    if (page <= 0) throw std::runtime_error("Cannot size GPU shared memory; using CPU");
    bytes = ((bytes + size_t(page) - 1) / size_t(page)) * size_t(page);
#endif
  }
};
void sendMessage(int fd, const Message& message) {
  ssize_t count;
  do { count = send(fd, &message, sizeof(message), MSG_NOSIGNAL | MSG_DONTWAIT); }
  while (count < 0 && errno == EINTR);
  if (count != sizeof(message)) throw std::runtime_error("GPU worker connection failed; using CPU");
}
Message receiveMessage(int fd, unsigned timeoutMs, const char* phase) {
  const auto deadline = Clock::now() + std::chrono::milliseconds(timeoutMs);
  Message message;
#ifdef __APPLE__
  size_t received = 0;
#endif
  for (;;) {
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
    if (left <= 0) throw std::runtime_error(std::string("GPU worker ") + phase +
      " timed out after " + std::to_string(timeoutMs) + " ms; using CPU");
    pollfd descriptor{fd, POLLIN, 0};
    const int result = poll(&descriptor, 1, std::min<int64_t>(left, INT_MAX));
    if (result < 0 && errno == EINTR) continue;
    if (result < 0) throw std::runtime_error("GPU worker communication failed; using CPU");
    if (!result) continue;
#ifdef __APPLE__
    const auto count = recv(fd, reinterpret_cast<char*>(&message) + received,
      sizeof(message) - received, MSG_DONTWAIT);
#else
    const auto count = recv(fd, &message, sizeof(message), MSG_TRUNC | MSG_DONTWAIT);
#endif
    if (count < 0 && (errno == EINTR || errno == EAGAIN)) continue;
    if (count <= 0) throw std::runtime_error("GPU worker stopped unexpectedly; using CPU");
#ifdef __APPLE__
    received += count;
    if (received != sizeof(message)) continue;
    if (message.version != protocol)
#else
    if (count != sizeof(message) || message.version != protocol)
#endif
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
class Process final : public GpuBackend, public GpuFrameBackend,
    public GpuClutterFrameBackend, public GpuFirFrameBackend {
  Layout layout_;
  GpuProcessOptions options_;
  int socket_ = -1, memory_ = -1;
  pid_t process_ = -1;
  Complex* shared_ = nullptr;
  GpuDevice device_;
  uint64_t sequence_ = 0;
  bool clutterSupported_ = false, firSupported_ = false, firActive_ = false,
    firFinalSubmitted_ = false;
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
#ifdef __APPLE__
    posix_spawnattr_t attributes;
    bool attributesReady = false;
    // Capture this before allocating IPC: closed standard descriptors may be
    // reused by socketpair/shm_open and must not then be inherited as logging.
    const bool standardOpen[] = {fcntl(0, F_GETFD) >= 0,
      fcntl(1, F_GETFD) >= 0, fcntl(2, F_GETFD) >= 0};
#endif
    try {
      if (device.size() >= sizeof(Message::id) || !options_.startupMs || !options_.frameMs)
        throw std::invalid_argument("Invalid GPU worker options");
      int sockets[2];
#ifdef __APPLE__
      // Darwin UNIX sockets do not support SOCK_SEQPACKET. The receiver above
      // assembles exactly one fixed-size protocol record under a single deadline.
      if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets))
#else
      if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sockets))
#endif
        throw std::runtime_error("Cannot create GPU worker connection; using CPU");
      socket_ = sockets[0];
#ifdef __APPLE__
      if (fcntl(sockets[0], F_SETFD, FD_CLOEXEC) || fcntl(sockets[1], F_SETFD, FD_CLOEXEC)) {
        close(sockets[1]);
        throw std::runtime_error("Cannot isolate GPU connection; using CPU");
      }
#endif
      childSocket = fcntl(sockets[1], F_DUPFD_CLOEXEC, 10);
      close(sockets[1]);
#ifdef __APPLE__
      // A private POSIX shared-memory object is unlinked before it can escape
      // this constructor. Only the two explicit descriptors name the mapping.
      for (unsigned attempt = 0; attempt < 16 && memory_ < 0; ++attempt) {
        char name[32];
        std::snprintf(name, sizeof(name), "/vw-gpu-%08x%08x", arc4random(), arc4random());
        memory_ = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
        if (memory_ >= 0 && shm_unlink(name)) {
          close(memory_); memory_ = -1;
          throw std::runtime_error("Cannot unlink GPU shared memory; using CPU");
        }
        if (memory_ < 0 && errno != EEXIST) break;
      }
      if (childSocket < 0 || memory_ < 0 || fcntl(memory_, F_SETFD, FD_CLOEXEC) ||
          ftruncate(memory_, layout_.bytes))
#else
      memory_ = memfd_create("blah2-gpu-frame", MFD_CLOEXEC | MFD_ALLOW_SEALING);
      if (childSocket < 0 || memory_ < 0 || ftruncate(memory_, layout_.bytes) ||
          fcntl(memory_, F_ADD_SEALS, F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL) < 0)
#endif
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
#if defined(__APPLE__)
      // Kernel-enforced allowlisting also closes descriptors opened by another
      // thread during spawn, unlike a user-space /dev/fd snapshot.
      if (posix_spawnattr_init(&attributes))
        throw std::runtime_error("Cannot isolate GPU worker; using CPU");
      attributesReady = true;
      if (posix_spawnattr_setflags(&attributes, POSIX_SPAWN_CLOEXEC_DEFAULT))
        throw std::runtime_error("Cannot isolate GPU worker descriptors; using CPU");
      for (int fd = 0; fd < 3; ++fd)
        if (standardOpen[fd] && posix_spawn_file_actions_addinherit_np(&actions, fd))
          throw std::runtime_error("Cannot inherit GPU worker logging; using CPU");
#elif defined(__GLIBC__) && defined(__GLIBC_MINOR__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 34))
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
      const int error = posix_spawn(&process_, executable.c_str(), &actions,
#ifdef __APPLE__
        &attributes,
#else
        nullptr,
#endif
        argv, environ);
      if (error) { process_ = -1; throw std::runtime_error("GPU worker is unavailable; using CPU"); }
      close(childSocket); childSocket = -1;
      close(childMemory); childMemory = -1;
      posix_spawn_file_actions_destroy(&actions); actionsReady = false;
#ifdef __APPLE__
      posix_spawnattr_destroy(&attributes); attributesReady = false;
#endif
      Message init; init.operation = initialize; init.geometry = geometry;
      std::snprintf(init.id, sizeof(init.id), "%s", device.c_str());
      sendMessage(socket_, init);
      const auto response = receiveMessage(socket_, options_.startupMs, "startup");
      if (response.operation == failure) throw std::runtime_error(response.reason);
      if (response.operation != ready || response.sequence)
        throw std::runtime_error("GPU worker initialization response is invalid; using CPU");
      device_ = {response.id, response.name, response.memoryBytes};
      clutterSupported_ = response.capabilities & clutterCapability;
      firSupported_ = response.capabilities & firCapability;
    } catch (...) {
#ifdef __APPLE__
      if (attributesReady) posix_spawnattr_destroy(&attributes);
#endif
      if (actionsReady) posix_spawn_file_actions_destroy(&actions);
      if (childSocket >= 0) close(childSocket);
      if (childMemory >= 0) close(childMemory);
      stop(); throw;
    }
  }
  ~Process() override { stop(); }
  GpuDevice device() const override { return device_; }
  GpuFrameBuffers frameBuffers() override {
    if (!layout_.reference)
      throw std::runtime_error("GPU radar processing is unavailable; using CPU");
    if (socket_ < 0 || !shared_)
      throw std::runtime_error("GPU worker is no longer available; using CPU");
    return {shared_, layout_.reference,
      shared_ + layout_.reference, layout_.surveillance,
      shared_ + layout_.reference + layout_.surveillance, layout_.output};
  }
  bool clutterAvailable() const override { return clutterSupported_; }
  bool firAvailable() const override { return firSupported_; }
  GpuFirBuffers firBuffers() override {
    if (!firSupported_ || !layout_.firReference)
      throw std::runtime_error("GPU FIR processing is unavailable; using CPU");
    if (socket_ < 0 || !shared_)
      throw std::runtime_error("GPU worker is no longer available; using CPU");
    auto* start = shared_ + layout_.firOffset;
    return {start, layout_.firReference,
      start + layout_.firReference, layout_.firWeights,
      start + layout_.firReference + layout_.firWeights, layout_.firOutput};
  }
  void submitFirReference() override {
    if (!firSupported_ || !layout_.firReference)
      throw std::runtime_error("GPU FIR processing is unavailable; using CPU");
    if (socket_ < 0 || !shared_)
      throw std::runtime_error("GPU worker is no longer available; using CPU");
    if (firActive_) throw std::logic_error("GPU FIR frame is already active");
    try {
      Message request; request.operation = firReference;
      request.sequence = ++sequence_; sendMessage(socket_, request);
      firActive_ = true; firFinalSubmitted_ = false;
    } catch (...) { stop(); throw; }
  }
  void submitFirWeights() override {
    if (!firActive_ || firFinalSubmitted_)
      throw std::logic_error("GPU FIR reference was not submitted exactly once");
    try {
      Message request; request.operation = firFinal; request.sequence = sequence_;
      sendMessage(socket_, request); firFinalSubmitted_ = true;
    } catch (...) { stop(); throw; }
  }
  void finishFir() override {
    if (!firActive_ || !firFinalSubmitted_)
      throw std::logic_error("GPU FIR final phase was not submitted");
    try {
      const auto response = receiveMessage(socket_, options_.frameMs, "FIR execution");
      if (response.operation == failure) throw std::runtime_error(response.reason);
      if (response.operation != ready || response.sequence != sequence_)
        throw std::runtime_error("GPU worker returned the wrong FIR frame; using CPU");
      firActive_ = false; firFinalSubmitted_ = false;
    } catch (...) { stop(); throw; }
  }
  GpuClutterBuffers clutterBuffers() override {
    if (!clutterSupported_ || !layout_.clutterReference)
      throw std::runtime_error("GPU clutter processing is unavailable; using CPU clutter");
    if (socket_ < 0 || !shared_)
      throw std::runtime_error("GPU worker is no longer available; using CPU");
    auto* start = shared_ + layout_.clutterOffset;
    return {start, layout_.clutterReference,
      start + layout_.clutterReference, layout_.clutterSurveillance,
      start + layout_.clutterReference + layout_.clutterSurveillance,
      layout_.clutterOutput};
  }
  void processFrame() override {
    if (!layout_.reference)
      throw std::runtime_error("GPU radar processing is unavailable; using CPU");
    if (socket_ < 0 || !shared_)
      throw std::runtime_error("GPU worker is no longer available; using CPU");
    try {
      Message request; request.operation = frame; request.sequence = ++sequence_;
      sendMessage(socket_, request);
      const auto response = receiveMessage(socket_, options_.frameMs, "frame execution");
      if (response.operation == failure) throw std::runtime_error(response.reason);
      if (response.operation != ready || response.sequence != sequence_)
        throw std::runtime_error("GPU worker returned the wrong frame; using CPU");
    } catch (...) { stop(); throw; }
  }
  bool processClutterFrame() override {
    if (!clutterSupported_ || !layout_.clutterReference)
      throw std::runtime_error("GPU clutter processing is unavailable; using CPU clutter");
    if (socket_ < 0 || !shared_)
      throw std::runtime_error("GPU worker is no longer available; using CPU");
    try {
      Message request; request.operation = clutterFrame; request.sequence = ++sequence_;
      sendMessage(socket_, request);
      const auto response = receiveMessage(socket_, options_.frameMs, "clutter execution");
      if (response.operation == failure) throw std::runtime_error(response.reason);
      if ((response.operation != ready && response.operation != clutterRejected) ||
          response.sequence != sequence_)
        throw std::runtime_error("GPU worker returned the wrong clutter frame; using CPU");
      return response.operation == ready;
    } catch (...) { stop(); throw; }
  }
  void process(const std::vector<Complex>& reference, const std::vector<Complex>& surveillance,
      std::vector<Complex>& output) override {
    if (!layout_.reference)
      throw std::runtime_error("GPU radar processing is unavailable; using CPU");
    if (socket_ < 0) throw std::runtime_error("GPU worker is no longer available; using CPU");
    if (reference.size() != layout_.reference || surveillance.size() != layout_.surveillance)
      throw std::invalid_argument("GPU input dimensions changed");
    try {
      std::memcpy(shared_, reference.data(), reference.size() * sizeof(Complex));
      std::memcpy(shared_ + layout_.reference, surveillance.data(), surveillance.size() * sizeof(Complex));
      processFrame();
      output.assign(shared_ + layout_.reference + layout_.surveillance,
        shared_ + layout_.reference + layout_.surveillance + layout_.output);
    } catch (...) { if (socket_ >= 0) stop(); throw; }
  }
};
}
std::string gpuSiblingPath(const char* filename) {
  char executable[PATH_MAX];
#ifdef __APPLE__
  uint32_t length = sizeof(executable);
  if (_NSGetExecutablePath(executable, &length))
    throw std::runtime_error("Cannot locate GPU worker files");
  char resolved[PATH_MAX];
  if (!realpath(executable, resolved)) throw std::runtime_error("Cannot locate GPU worker files");
  std::string path(resolved);
#else
  const auto length = readlink("/proc/self/exe", executable, sizeof(executable)-1);
  if (length < 0 || length == sizeof(executable)-1) throw std::runtime_error("Cannot locate GPU worker files");
  executable[length] = '\0';
  std::string path(executable);
#endif
  return path.substr(0, path.find_last_of('/')) + "/" + filename;
}
std::unique_ptr<GpuBackend> createGpuProcess(const GpuGeometry& geometry,
    const std::string& device, const GpuProcessOptions& options) {
  return std::make_unique<Process>(geometry, device, options);
}
std::unique_ptr<GpuBackend> createGpuFirProcess(uint32_t samples, uint32_t taps,
    const std::string& device, const GpuProcessOptions& options) {
  GpuGeometry geometry{};
  geometry.kind = GpuWorkKind::fir;
  geometry.firSamples = samples; geometry.firTaps = taps;
  geometry.firFft = 2048; geometry.firPercent = 50;
  return std::make_unique<Process>(geometry, device, options);
}
int runGpuWorker(const GpuFactory& factory) {
  const pid_t parent = getppid();
#ifdef __APPLE__
  // EOF cannot stop a driver stuck inside an initialization/frame call. Watch
  // the parent's exact process lifetime independently and exit even then.
  if (parent == 1) return 1;
  const int watcher = kqueue();
  if (watcher < 0) return 1;
  struct kevent change;
  EV_SET(&change, parent, EVFILT_PROC, EV_ADD | EV_ENABLE | EV_ONESHOT, NOTE_EXIT, 0, nullptr);
  if (fcntl(watcher, F_SETFD, FD_CLOEXEC) ||
      kevent(watcher, &change, 1, nullptr, 0, nullptr) < 0 || getppid() != parent) {
    close(watcher); return 1;
  }
  try {
    std::thread([watcher] {
      struct kevent event;
      int result;
      do { result = kevent(watcher, nullptr, 0, &event, 1, nullptr); }
      while (result < 0 && errno == EINTR);
      // Failure of the lifetime guard is fail-closed as well.
      _exit(1);
    }).detach();
  } catch (...) { close(watcher); return 1; }
#else
  if (parent == 1 || prctl(PR_SET_PDEATHSIG, SIGKILL) || getppid() != parent) return 1;
  prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0);
#endif
  const rlimit noCore{0, 0}; setrlimit(RLIMIT_CORE, &noCore);
  void* mapping = MAP_FAILED;
  size_t bytes = 0;
  try {
    const auto init = receiveMessage(3, 30000, "initialization request");
    if (init.operation != initialize || init.sequence) throw std::runtime_error("Invalid GPU worker initialization");
    const Layout layout(init.geometry);
    struct stat metadata{};
    if (fstat(4, &metadata) || metadata.st_size != static_cast<off_t>(layout.bytes))
      throw std::runtime_error("Invalid GPU worker shared memory");
    bytes = layout.bytes;
    mapping = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, 4, 0);
    if (mapping == MAP_FAILED) throw std::runtime_error("Cannot map GPU worker input");
#ifdef __APPLE__
    // Darwin has no Linux memfd seals. Drop the only child descriptor before
    // loading a driver, so it cannot accidentally resize the shared object.
    // The unlinked object remains alive through the mappings and parent fd.
    close(4);
#endif
    const auto* shared = static_cast<const Complex*>(mapping);
    auto backend = factory(init.geometry, init.id);
    if (!backend) throw std::runtime_error("GPU module did not create a processor");
    const auto device = backend->device();
    auto* buffers = dynamic_cast<GpuBufferBackend*>(backend.get());
    auto* clutter = dynamic_cast<GpuClutterBufferBackend*>(backend.get());
    auto* fir = dynamic_cast<GpuFirBufferBackend*>(backend.get());
    Message response; response.operation = ready; response.memoryBytes = device.memoryBytes;
    if (clutter && layout.clutterReference) response.capabilities |= clutterCapability;
    if (fir && layout.firReference) response.capabilities |= firCapability;
    std::snprintf(response.id, sizeof(response.id), "%s", device.id.c_str());
    std::snprintf(response.name, sizeof(response.name), "%s", device.name.c_str());
    sendMessage(3, response);
    std::vector<Complex> reference, surveillance, output;
    if (!buffers) {
      reference.resize(layout.reference);
      surveillance.resize(layout.surveillance);
    }
    uint64_t sequence = 0, firSequence = 0;
    bool firActive = false;
    for (;;) {
      // Idle waits are not GPU work. Parent death closes the socket or kills us.
      const auto request = receiveMessage(3, INT_MAX, "idle request");
      if (request.operation == quit) break;
      auto* writable = static_cast<Complex*>(mapping);
      if (request.operation == firReference) {
        if (!fir || !layout.firReference || firActive ||
            request.sequence != sequence + 1)
          throw std::runtime_error("Invalid GPU worker FIR reference request");
        const auto* start = shared + layout.firOffset;
        fir->submitFirReferenceBuffers(start, layout.firReference);
        firSequence = request.sequence; firActive = true;
        continue; // Deliberately no acknowledgement: CPU correlation overlaps.
      }
      if (request.operation == firFinal) {
        if (!fir || !layout.firReference || !firActive ||
            request.sequence != firSequence)
          throw std::runtime_error("Invalid GPU worker FIR final request");
        auto* start = writable + layout.firOffset;
        fir->processFirWeightsBuffers(start + layout.firReference,
          layout.firWeights, start + layout.firReference + layout.firWeights,
          layout.firOutput);
        sequence = firSequence; firActive = false;
        response.operation = ready; response.sequence = sequence;
        sendMessage(3, response); continue;
      }
      if (firActive || (request.operation != frame && request.operation != clutterFrame) ||
          request.sequence != ++sequence)
        throw std::runtime_error("Invalid GPU worker frame request");
      if (request.operation == clutterFrame) {
        if (!clutter || !layout.clutterReference)
          throw std::runtime_error("GPU clutter processing is unavailable; using CPU clutter");
        auto* start = writable + layout.clutterOffset;
        const bool accepted = clutter->processClutterBuffers(start, layout.clutterReference,
          start + layout.clutterReference, layout.clutterSurveillance,
          start + layout.clutterReference + layout.clutterSurveillance,
          layout.clutterOutput);
        response.operation = accepted ? ready : clutterRejected;
      } else if (buffers) {
        response.operation = ready;
        buffers->processBuffers(shared, layout.reference,
          shared + layout.reference, layout.surveillance,
          writable + layout.reference + layout.surveillance, layout.output);
      } else {
        response.operation = ready;
        std::memcpy(reference.data(), shared, layout.reference * sizeof(Complex));
        std::memcpy(surveillance.data(), shared + layout.reference, layout.surveillance * sizeof(Complex));
        backend->process(reference, surveillance, output);
        if (output.size() != layout.output)
          throw std::runtime_error("GPU returned invalid radar dimensions; using CPU");
        std::memcpy(writable + layout.reference + layout.surveillance,
          output.data(), layout.output * sizeof(Complex));
      }
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
