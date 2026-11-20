#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "GpuProcess.h"
#include "GpuArtifactBinding.h"
#include "ReplayGpuQualification.h"
#include "GpuWorkerLifetime.h"
#include "data/meta/StreamProfile.h"
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
#include "GpuWorkerTransport.private.h"
namespace blah2 {
namespace {
using Clock = std::chrono::steady_clock;
using Complex = std::complex<float>;
constexpr uint32_t protocol = 0x42475004;
enum Operation : uint32_t { initialize = 1, frame = 2, ready = 3, failure = 4,
  quit = 5, clutterFrame = 6, clutterRejected = 7 };
// Operations 8/9 and capability bit 2 are reserved for the retired FIR route.
constexpr uint32_t clutterCapability = 1;
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
  explicit Layout(GpuGeometry g) {
    if (g.kind != GpuWorkKind::radar)
      throw std::runtime_error("GPU work kind is retired or unsupported; using CPU");
    if (g.firSamples || g.firTaps || g.firFft || g.firPercent)
      throw std::runtime_error("GPU reserved geometry fields must be zero; using CPU");
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
    reference = r;
    surveillance = s;
    output = o;
    bytes = total;
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
    const auto left = std::max<int64_t>(0,
      std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count());
    pollfd descriptor{fd, POLLIN, 0};
    const int result = poll(&descriptor, 1, std::min<int64_t>(left, INT_MAX));
    if (result < 0 && errno == EINTR) continue;
    if (result < 0) throw std::runtime_error("GPU worker communication failed; using CPU");
    if (!result) {
      if (Clock::now() >= deadline)
        throw std::runtime_error(std::string("GPU worker ") + phase +
          " timed out after " + std::to_string(timeoutMs) + " ms; using CPU");
      continue;
    }
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
class Process final : public GpuBackend, public GpuFrameBackend,
    public GpuClutterFrameBackend, public GpuClutterHostWorkBackend {
  Layout layout_;
  GpuProcessOptions options_;
  std::unique_ptr<gpu_worker_transport::Child> child_;
  Complex* shared() const { return reinterpret_cast<Complex*>(child_->data()); }
  GpuDevice device_;
  uint64_t sequence_ = 0;
  bool clutterSupported_ = false;
  const bool profileDrain_ = stream_profile::frameLimit()!=0;
  stream_profile::Worker streamProfile_{"portable-parent",2};
  // Snapshot once per worker. Default-off isolates host projection from worker
  // FP64 refinement; only the exact opt-in "1" enables their CPU overlap.
  const bool clutterHostWorkOverlap_ = [] {
    const char* value = std::getenv("BLAH2_GPU_CLUTTER_HOST_WORK_OVERLAP");
    return value && std::strcmp(value, "1") == 0;
  }();
  void stop(bool offlineDrain=false) noexcept {
    if (child_) child_->stop(offlineDrain);
  }
public:
  Process(GpuGeometry geometry, const std::string& device, GpuProcessOptions options)
    : layout_(geometry), options_(std::move(options)) {
    try {
      if (device.size() >= sizeof(Message::id) || !options_.startupMs || !options_.frameMs)
        throw std::invalid_argument("Invalid GPU worker options");
      child_=std::make_unique<gpu_worker_transport::Child>(
        gpu_worker_transport::portable,layout_.bytes,options_,
        [](uint8_t*,uint64_t){},device,profileDrain_);
      Message init;
      init.operation = initialize;
      init.geometry = geometry;
      std::snprintf(init.id, sizeof(init.id), "%s", device.c_str());
      sendMessage(child_->fd(), init);
      const auto response = receiveMessage(child_->fd(), options_.startupMs, "startup");
      if (response.operation == failure) throw std::runtime_error(response.reason);
      if (response.operation != ready || response.sequence)
        throw std::runtime_error("GPU worker initialization response is invalid; using CPU");
      device_ = {response.id, response.name, response.memoryBytes};
      clutterSupported_ = response.capabilities & clutterCapability;
    } catch (...) {
      stop();
      throw;
    }
  }
  ~Process() override { stop(options_.offlineTransportTrace); }
  GpuDevice device() const override { return device_; }
  GpuFrameBuffers frameBuffers() override {
    if (!layout_.reference)
      throw std::runtime_error("GPU radar processing is unavailable; using CPU");
    if (child_->fd() < 0 || !shared())
      throw std::runtime_error("GPU worker is no longer available; using CPU");
    return {shared(), layout_.reference,
      shared() + layout_.reference, layout_.surveillance,
      shared() + layout_.reference + layout_.surveillance, layout_.output};
  }
  bool clutterAvailable() const override { return clutterSupported_; }
  GpuClutterBuffers clutterBuffers() override {
    if (!clutterSupported_ || !layout_.clutterReference)
      throw std::runtime_error("GPU clutter processing is unavailable; using CPU clutter");
    if (child_->fd() < 0 || !shared())
      throw std::runtime_error("GPU worker is no longer available; using CPU");
    auto* start = shared() + layout_.clutterOffset;
    return {start, layout_.clutterReference,
      start + layout_.clutterReference, layout_.clutterSurveillance,
      start + layout_.clutterReference + layout_.clutterSurveillance,
      layout_.clutterOutput};
  }
  void processFrame() override {
    if (!layout_.reference)
      throw std::runtime_error("GPU radar processing is unavailable; using CPU");
    if (child_->fd() < 0 || !shared())
      throw std::runtime_error("GPU worker is no longer available; using CPU");
    try {
      Message request;
      request.operation = frame;
      request.sequence = ++sequence_;
      stream_profile::WorkScope profile(streamProfile_,sequence_,frame,child_->pid());
      sendMessage(child_->fd(), request);
      profile.mark(1); // dispatch completed
      const auto response = receiveMessage(child_->fd(), options_.frameMs, "frame execution");
      profile.mark(2); // response received, before validation
      if (response.operation == failure) throw std::runtime_error(response.reason);
      if (response.operation != ready || response.sequence != sequence_)
        throw std::runtime_error("GPU worker returned the wrong frame; using CPU");
      profile.completed();
    } catch (...) { stop();
    throw; }
  }
  bool processClutterFrame() override {
    return processClutterFrameWithHostWork({});
  }
  bool processClutterFrameWithHostWork(const std::function<void()>& hostWork) override {
    if (!clutterSupported_ || !layout_.clutterReference)
      throw std::runtime_error("GPU clutter processing is unavailable; using CPU clutter");
    if (child_->fd() < 0 || !shared())
      throw std::runtime_error("GPU worker is no longer available; using CPU");
    try {
      // Non-overlapped host work precedes submission and therefore does not
      // consume the worker's frame deadline. Input/result ownership and the
      // caller's exactly-once exception/fallback handling remain unchanged.
      if (hostWork && !clutterHostWorkOverlap_) hostWork();
      Message request;
      request.operation = clutterFrame;
      request.sequence = ++sequence_;
      stream_profile::WorkScope profile(streamProfile_,sequence_,clutterFrame,child_->pid());
      sendMessage(child_->fd(), request);
      profile.mark(1);
      unsigned remainingMs = options_.frameMs;
      if (hostWork && clutterHostWorkOverlap_) {
        const auto start = Clock::now();
        hostWork();
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();
        // Host scheduling may outlast a completed GPU frame. A zero-time poll
        // still drains its ready reply; unfinished work receives no new budget.
        remainingMs = elapsed >= options_.frameMs ? 0 :
          options_.frameMs - unsigned(elapsed);
      }
      const auto response = receiveMessage(child_->fd(), remainingMs, "clutter execution");
      profile.mark(2);
      if (response.operation == failure) throw std::runtime_error(response.reason);
      if ((response.operation != ready && response.operation != clutterRejected) ||
          response.sequence != sequence_)
        throw std::runtime_error("GPU worker returned the wrong clutter frame; using CPU");
      replay_gpu_qualification::acceptClutterProof(response.reason);
      profile.completed();
      return response.operation == ready;
    } catch (...) { stop();
    throw; }
  }
  void process(const std::vector<Complex>& reference, const std::vector<Complex>& surveillance,
      std::vector<Complex>& output) override {
    if (!layout_.reference)
      throw std::runtime_error("GPU radar processing is unavailable; using CPU");
    if (child_->fd() < 0) throw std::runtime_error("GPU worker is no longer available; using CPU");
    if (reference.size() != layout_.reference || surveillance.size() != layout_.surveillance)
      throw std::invalid_argument("GPU input dimensions changed");
    try {
      std::memcpy(shared(), reference.data(), reference.size() * sizeof(Complex));
      std::memcpy(shared() + layout_.reference, surveillance.data(), surveillance.size() * sizeof(Complex));
      processFrame();
      output.assign(shared() + layout_.reference + layout_.surveillance,
        shared() + layout_.reference + layout_.surveillance + layout_.output);
    } catch (...) { if (child_->fd() >= 0) stop();
    throw; }
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
std::string queryGpuDriverStatus(const GpuProcessOptions& options) {
  if (!options.startupMs || options.startupMs > 30000)
    throw std::invalid_argument("GPU driver query deadline must be 1..30000 ms");
  int descriptors[2]{-1, -1}, output = -1, childOutput = -1;
  pid_t child = -1;
  posix_spawn_file_actions_t actions;
  bool actionsReady = false;
#ifdef __APPLE__
  posix_spawnattr_t attributes;
  bool attributesReady = false;
  const bool stderrOpen = fcntl(STDERR_FILENO, F_GETFD) >= 0;
#endif
  const auto cleanup = [&] {
    for (int fd : {descriptors[0], descriptors[1], output, childOutput})
      if (fd >= 0) close(fd);
    if (actionsReady) posix_spawn_file_actions_destroy(&actions);
#ifdef __APPLE__
    if (attributesReady) posix_spawnattr_destroy(&attributes);
#endif
    gpu_worker_transport::reap(child,false);
  };
  try {
    if (pipe(descriptors)) throw std::runtime_error("Cannot open GPU driver query pipe");
    // Standard descriptors may be closed. Move both endpoints above them
    // before preparing dup2/close actions, then retain CLOEXEC on the parent.
    output = fcntl(descriptors[0], F_DUPFD_CLOEXEC, 3);
    childOutput = fcntl(descriptors[1], F_DUPFD_CLOEXEC, 3);
    if (output < 0 || childOutput < 0)
      throw std::runtime_error("Cannot isolate GPU driver query pipe");
    close(descriptors[0]); close(descriptors[1]);
    descriptors[0] = descriptors[1] = -1;
    if (fcntl(output, F_SETFL, O_NONBLOCK) || posix_spawn_file_actions_init(&actions))
      throw std::runtime_error("Cannot prepare GPU driver query");
    actionsReady = true;
    if (posix_spawn_file_actions_adddup2(&actions, childOutput, STDOUT_FILENO) ||
        posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0))
      throw std::runtime_error("Cannot prepare GPU driver query descriptors");
    gpu_artifact_binding::spawnActions(actions,true); // fd 5 is explicitly closed below.
    if(gpu_artifact_binding::active())for(int fd=3;fd<6;++fd)
      if(posix_spawn_file_actions_addclose(&actions,fd))
        throw std::runtime_error("Cannot isolate GPU driver query descriptors");
    const int closeFrom=gpu_artifact_binding::active()?7:3;
#ifdef __APPLE__
    if (posix_spawnattr_init(&attributes))
      throw std::runtime_error("Cannot isolate GPU driver query");
    attributesReady = true;
    if (posix_spawnattr_setflags(&attributes, POSIX_SPAWN_CLOEXEC_DEFAULT) ||
        (stderrOpen && posix_spawn_file_actions_addinherit_np(&actions, STDERR_FILENO)))
      throw std::runtime_error("Cannot isolate GPU driver query descriptors");
#elif defined(__GLIBC__) && defined(__GLIBC_MINOR__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 34))
    if (posix_spawn_file_actions_addclosefrom_np(&actions, closeFrom))
      throw std::runtime_error("Cannot isolate GPU driver query descriptors");
#else
    DIR* inherited = opendir("/proc/self/fd");
    if (!inherited) throw std::runtime_error("Cannot inspect GPU driver query descriptors");
    while (const auto* entry = readdir(inherited)) {
      const int fd = std::atoi(entry->d_name);
      if (fd >= closeFrom) posix_spawn_file_actions_addclose(&actions, fd);
    }
    closedir(inherited);
#endif
    std::string executable = options.executable.empty() ? gpuSiblingPath("blah2-gpu-worker") : options.executable;
    std::string argument = options.argument.empty() ? "--driver-status" : options.argument;
    char* argv[] = {executable.data(), argument.data(), nullptr};
    std::vector<std::string> childEnvironment;
    for(char** item=environ;*item;++item)
      if(!gpu_artifact_binding::inherited(*item)&&!replay_gpu_qualification::inherited(*item))
        childEnvironment.emplace_back(*item);
    gpu_artifact_binding::appendEnvironment(childEnvironment);
    gpu_offline_diagnostics::appendEnvironment(childEnvironment,false);
    gpu_worker_lifetime::bindParent(childEnvironment);
    std::vector<char*> childEnvironmentPointers;
    for(auto& item:childEnvironment)childEnvironmentPointers.push_back(item.data());
    childEnvironmentPointers.push_back(nullptr);
    const auto deadline = Clock::now() + std::chrono::milliseconds(options.startupMs);
    const int error = posix_spawn(&child, executable.c_str(), &actions,
#ifdef __APPLE__
      &attributes,
#else
      nullptr,
#endif
      argv, childEnvironmentPointers.data());
    if (error) { child = -1; throw std::runtime_error("GPU driver query worker is unavailable"); }
    gpu_artifact_binding::spawned(child,executable);
    close(childOutput); childOutput = -1;
    std::string result;
    bool eof = false, exited = false;
    int status = 0;
    while (!eof || !exited) {
      const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
      if (left <= 0) throw std::runtime_error("GPU driver query timed out; resident route stopped");
      pollfd descriptor{output, POLLIN, 0};
      const int ready = poll(&descriptor, 1, int(std::min<int64_t>(left, 10)));
      if (ready < 0 && errno != EINTR) throw std::runtime_error("GPU driver query poll failed");
      if (ready > 0 && !eof) {
        char buffer[4096];
        const auto count = read(output, buffer, sizeof(buffer));
        if (count > 0) {
          result.append(buffer, size_t(count));
          if (result.size() > 32768) throw std::runtime_error("GPU driver query exceeded output limit");
        } else if (!count) eof = true;
        else if (errno != EINTR && errno != EAGAIN)
          throw std::runtime_error("GPU driver query read failed");
      }
      if (!exited) {
        const auto waited = waitpid(child, &status, WNOHANG);
        if (waited == child) { child = -1; exited = true; }
        else if (waited < 0 && errno != EINTR) throw std::runtime_error("GPU driver query wait failed");
      }
      if (eof && !exited) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
      throw std::runtime_error("GPU driver query failed; resident route stopped: " + result);
    cleanup();
    return result;
  } catch (...) { cleanup(); throw; }
}
std::unique_ptr<GpuBackend> createGpuProcess(const GpuGeometry& geometry,
    const std::string& device, const GpuProcessOptions& options) {
  return std::make_unique<Process>(geometry, device, options);
}
uint64_t gpuProcessMappingBytes(const GpuGeometry& geometry) {
  const uint64_t bytes = Layout(geometry).bytes;
  const long page = sysconf(_SC_PAGESIZE);
  if (page <= 0 || bytes > UINT64_MAX - (uint64_t(page) - 1))
    throw std::runtime_error("Cannot reserve GPU shared-memory pages; using CPU");
  // mmap consumes complete pages even where fstat exposes the unrounded wire
  // length. Reserve that physical padding without changing the IPC protocol.
  return ((bytes + uint64_t(page) - 1) / uint64_t(page)) * uint64_t(page);
}
int runGpuWorker(const GpuFactory& factory) {
  if (!gpu_worker_lifetime::protectWorker()) return 1;
  void* mapping = MAP_FAILED;
  size_t bytes = 0;
  std::unique_ptr<GpuBackend> backend;
  try {
    gpu_artifact_binding::workerStart();
    replay_gpu_qualification::Worker qualification("portable");
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
    qualification.device(init.id);
    if(qualification.hit("startup"))throw std::runtime_error("Replay qualification startup rejection");
    qualification.hit("allocation"); // The real allocator receives a one-byte ceiling.
    backend = factory(init.geometry, init.id);
    if (!backend) throw std::runtime_error("GPU module did not create a processor");
    const auto device = backend->device();
    qualification.device(device.id);
    auto* buffers = dynamic_cast<GpuBufferBackend*>(backend.get());
    auto* clutter = dynamic_cast<GpuClutterBufferBackend*>(backend.get());
    auto* replayClutter = dynamic_cast<clutter_replay::Backend*>(backend.get());
    if(qualification.clutter().active()){
      if(!gpu_artifact_binding::active()||!replayClutter||!clutter)
        throw std::runtime_error("Replay clutter checkpoint lacks authenticated portable backend");
      replayClutter->configureClutterReplay(qualification.clutter(),[&qualification]{return qualification.binding();});
    }
    Message response;
    response.operation = ready;
    response.memoryBytes = device.memoryBytes;
    if (clutter && layout.clutterReference) response.capabilities |= clutterCapability;
    std::snprintf(response.id, sizeof(response.id), "%s", device.id.c_str());
    std::snprintf(response.name, sizeof(response.name), "%s", device.name.c_str());
    sendMessage(3, response);
    std::vector<Complex> reference, surveillance, output;
    if (!buffers) {
      reference.resize(layout.reference);
      surveillance.resize(layout.surveillance);
    }
    uint64_t sequence = 0;
    stream_profile::Worker streamProfile("portable-worker",2);
    for (;;) {
      // Idle waits are not GPU work. Parent death closes the socket or kills us.
      const auto request = receiveMessage(3, INT_MAX, "idle request");
      if (request.operation == quit) break;
      auto* writable = static_cast<Complex*>(mapping);
      if ((request.operation != frame && request.operation != clutterFrame) ||
          request.sequence != ++sequence)
        throw std::runtime_error("Invalid GPU worker frame request");
      if(request.operation==frame)qualification.beforeFrame(sequence,sequence);
      stream_profile::WorkScope profile(streamProfile,sequence,request.operation,getppid(),true);
      response.reason[0]='\0';
      if (request.operation == clutterFrame) {
        if (!clutter || !layout.clutterReference)
          throw std::runtime_error("GPU clutter processing is unavailable; using CPU clutter");
        auto* start = writable + layout.clutterOffset;
        const bool accepted = clutter->processClutterBuffers(start, layout.clutterReference,
          start + layout.clutterReference, layout.clutterSurveillance,
          start + layout.clutterReference + layout.clutterSurveillance,
          layout.clutterOutput);
        response.operation = accepted ? ready : clutterRejected;
        if(qualification.clutter().active()){
          const auto proof=replayClutter->clutterReplayProof();const auto text=proof.encode();
          if(!accepted||text.size()>=sizeof(response.reason))throw std::runtime_error("Replay clutter checkpoint execution failed");
          std::memcpy(response.reason,text.c_str(),text.size()+1);qualification.clutterComplete(proof);
        }
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
      if(request.operation==frame&&qualification.hit("numeric-rejection",sequence,sequence))
        writable[layout.reference+layout.surveillance]={std::numeric_limits<float>::quiet_NaN(),0};
      if(request.operation==frame)qualification.complete("frame-computed",sequence,sequence);
      response.sequence = sequence;
      profile.mark(1); // computation complete
      sendMessage(3, response);
      profile.mark(2); // response sent
      profile.completed();
    }
    munmap(mapping, bytes);
    return 0;
  } catch (const std::exception& error) {
    Message response;
    response.operation = failure;
    std::snprintf(response.reason, sizeof(response.reason), "%s", error.what());
    try { sendMessage(3, response); } catch (...) {}
    if (mapping != MAP_FAILED) munmap(mapping, bytes);
    return 1;
  }
}
}
