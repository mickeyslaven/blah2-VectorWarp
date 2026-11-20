#pragma once
// Canonical portable/resident process ownership, independent of either wire ABI.
// Included after the caller's POSIX platform headers. Wideband keeps its stream
// protocol/output-lease transport; all routes share GpuWorkerLifetime.
namespace blah2::gpu_worker_transport {
struct Policy {
 const char* role;
 const char* memoryName;
 const char* failureSuffix;
 bool retainReservation;
};
inline constexpr Policy portable{"portable","blah2-gpu-frame","; using CPU",false};
inline constexpr Policy resident{"resident","blah2-gpu-resident-frame","",true};
using Clock=std::chrono::steady_clock;
inline void reap(pid_t process,bool retainReservation,bool offlineDrain=false,bool profileDrain=false) noexcept {
  if (process <= 0) return;
  int status;
  pid_t result;
  do { result = waitpid(process, &status, WNOHANG); } while (result < 0 && errno == EINTR);
  if (result != 0) {if(result==process)replay_gpu_qualification::reaped(process);return;}
  if(offlineDrain||profileDrain){
    const auto drainDeadline=Clock::now()+std::chrono::milliseconds(profileDrain?500:100);
    do{
      result=waitpid(process,&status,WNOHANG);
      if(result==process||(result<0&&errno!=EINTR)){if(result==process)replay_gpu_qualification::reaped(process);return;}
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }while(Clock::now()<drainDeadline);
  }
  kill(process, SIGKILL);
  // Never wait indefinitely for a broken kernel driver. The shared mapping stays
  // alive in the child/kernel until it exits; parent input is independent.
  const auto deadline = Clock::now() + std::chrono::milliseconds(100);
  do {
    result = waitpid(process, &status, WNOHANG);
    if (result == process || (result < 0 && errno != EINTR)) {if(result==process)replay_gpu_qualification::reaped(process);return;}
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  } while (Clock::now() < deadline);
  if(retainReservation)++gpu_worker_lifetime::pendingReaps();
  try {
    const auto qualificationBinding=replay_gpu_qualification::current();
    const bool qualificationEnabled=replay_gpu_qualification::enabled();
    std::thread([process,qualificationBinding,qualificationEnabled,retainReservation] {
      int code;
      pid_t result;do{result=waitpid(process,&code,0);}while(result<0&&errno==EINTR);
      if(retainReservation&&(result==process||(result<0&&errno==ECHILD)))--gpu_worker_lifetime::pendingReaps();
      if(result==process)replay_gpu_qualification::reaped(process,qualificationBinding,qualificationEnabled);
    }).detach();
  } catch (...) {} // Keep the reservation unavailable if its reaper cannot start.
}

class Child {
 Policy policy_;
 bool profileDrain_=false;
 GpuProcessOptions options_;
 uint64_t bytes_=0;
 int socket_=-1,memory_=-1;
 pid_t process_=-1;
 uint8_t* shared_=nullptr;
public:
 void stop(bool offlineDrain=false)noexcept{if(socket_>=0){close(socket_);
 socket_=-1;}reap(process_,policy_.retainReservation,offlineDrain,profileDrain_);
 process_=-1;
 if(shared_){munmap(shared_,size_t(bytes_));
 shared_=nullptr;}if(memory_>=0){close(memory_);
 memory_=-1;}}
 Child(const Child&)=delete;
 Child&operator=(const Child&)=delete;
 template<class Prepare>Child(Policy policy,uint64_t bytes,GpuProcessOptions options,Prepare prepare,
     const std::string& device={},bool profileDrain=false)
     :policy_(policy),profileDrain_(profileDrain),options_(std::move(options)),bytes_(bytes){
    const auto failure=[&](const char* message){
      return std::runtime_error(std::string(message)+policy_.failureSuffix);
    };
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
      if (!options_.startupMs || !options_.frameMs)
        throw std::invalid_argument("Invalid GPU worker options");
      int sockets[2];
#ifdef __APPLE__
      // Darwin UNIX sockets do not support SOCK_SEQPACKET. The receiver above
      // assembles exactly one fixed-size protocol record under a single deadline.
      if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets))
#else
      if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sockets))
#endif
        throw failure("Cannot create GPU worker connection");
      socket_ = sockets[0];
#ifdef __APPLE__
      if (fcntl(sockets[0], F_SETFD, FD_CLOEXEC) || fcntl(sockets[1], F_SETFD, FD_CLOEXEC)) {
        close(sockets[1]);
        throw failure("Cannot isolate GPU connection");
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
          close(memory_);
          memory_ = -1;
          throw failure("Cannot unlink GPU shared memory");
        }
        if (memory_ < 0 && errno != EEXIST) break;
      }
      if (childSocket < 0 || memory_ < 0 || fcntl(memory_, F_SETFD, FD_CLOEXEC) ||
          ftruncate(memory_, bytes_))
#else
      memory_ = memfd_create(policy_.memoryName, MFD_CLOEXEC | MFD_ALLOW_SEALING);
      if (childSocket < 0 || memory_ < 0 || ftruncate(memory_, bytes_) ||
          fcntl(memory_, F_ADD_SEALS, F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL) < 0)
#endif
        throw failure("Cannot allocate GPU shared memory");
      void* mapping = mmap(nullptr, bytes_, PROT_READ | PROT_WRITE, MAP_SHARED, memory_, 0);
      if (mapping == MAP_FAILED) throw failure("Cannot map GPU shared memory");
      shared_ = static_cast<uint8_t*>(mapping);
      prepare(shared_,bytes_);
      childMemory = fcntl(memory_, F_DUPFD_CLOEXEC, 10);
      if (childMemory < 0 || posix_spawn_file_actions_init(&actions))
        throw failure("Cannot prepare GPU worker");
      actionsReady = true;
      if (posix_spawn_file_actions_adddup2(&actions, childSocket, 3) ||
          posix_spawn_file_actions_adddup2(&actions, childMemory, 4))
        throw failure("Cannot prepare GPU worker descriptors");
      replay_gpu_qualification::spawnActions(actions);
      gpu_artifact_binding::spawnActions(actions);
      // Close inherited descriptors after installing our two endpoints. This also
      // prevents a GPU worker from accidentally keeping capture/listening sockets alive.
#if defined(__APPLE__)
      // Kernel-enforced allowlisting also closes descriptors opened by another
      // thread during spawn, unlike a user-space /dev/fd snapshot.
      if (posix_spawnattr_init(&attributes))
        throw failure("Cannot isolate GPU worker");
      attributesReady = true;
      if (posix_spawnattr_setflags(&attributes, POSIX_SPAWN_CLOEXEC_DEFAULT))
        throw failure("Cannot isolate GPU worker descriptors");
      for (int fd = 0; fd < 3; ++fd)
        if (standardOpen[fd] && posix_spawn_file_actions_addinherit_np(&actions, fd))
          throw failure("Cannot inherit GPU worker logging");
#elif defined(__GLIBC__) && defined(__GLIBC_MINOR__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 34))
      if (posix_spawn_file_actions_addclosefrom_np(&actions, gpu_artifact_binding::closeFrom()))
        throw failure("Cannot isolate GPU worker descriptors");
#else
      DIR* descriptors = opendir("/proc/self/fd");
      if (!descriptors) throw failure("Cannot inspect GPU worker descriptors");
      while (const auto* entry = readdir(descriptors)) {
        const int fd = std::atoi(entry->d_name);
        if (fd >= gpu_artifact_binding::closeFrom()) posix_spawn_file_actions_addclose(&actions, fd);
      }
      closedir(descriptors);
#endif
      std::string executable = options_.executable.empty() ? gpuSiblingPath("blah2-gpu-worker") : options_.executable;
      char* argv[] = {executable.data(), options_.argument.empty() ? nullptr : options_.argument.data(), nullptr};
      // Per-child environment, never a process-wide setenv race between lanes.
      std::vector<std::string> childEnvironment;
      for (char** item = environ; *item; ++item)
        if (std::string(*item).rfind("BLAH2_GPU_ALLOCATION_LIMIT_BYTES=", 0) != 0 &&
            !replay_gpu_qualification::inherited(*item) && !gpu_artifact_binding::inherited(*item))
          childEnvironment.emplace_back(*item);
      if (replay_gpu_qualification::allocationFault(policy_.role,options_.qualificationLane))
        childEnvironment.push_back("BLAH2_GPU_ALLOCATION_LIMIT_BYTES=1");
      else if (options_.allocationLimitBytes)
        childEnvironment.push_back("BLAH2_GPU_ALLOCATION_LIMIT_BYTES=" +
          std::to_string(options_.allocationLimitBytes));
      replay_gpu_qualification::appendEnvironment(childEnvironment,policy_.role,options_.qualificationLane,options_.executable);
      gpu_artifact_binding::appendEnvironment(childEnvironment);
      gpu_offline_diagnostics::appendEnvironment(childEnvironment,options_.offlineTransportTrace);
      gpu_worker_lifetime::bindParent(childEnvironment);
      std::vector<char*> childEnvironmentPointers;
      for (auto& item : childEnvironment) childEnvironmentPointers.push_back(item.data());
      childEnvironmentPointers.push_back(nullptr);
      const int error = posix_spawn(&process_, executable.c_str(), &actions,
#ifdef __APPLE__
        &attributes,
#else
        nullptr,
#endif
        argv, childEnvironmentPointers.data());
      if (error) { process_ = -1;
      throw failure("GPU worker is unavailable"); }
      gpu_artifact_binding::spawned(process_,executable);
      if(replay_gpu_qualification::enabled())replay_gpu_qualification::emit("worker-spawned",policy_.role,options_.qualificationLane,
        replay_gpu_qualification::current(),{},device,0,0,process_);
      close(childSocket);
      childSocket = -1;
      close(childMemory);
      childMemory = -1;
      posix_spawn_file_actions_destroy(&actions);
      actionsReady = false;
#ifdef __APPLE__
      posix_spawnattr_destroy(&attributes);
      attributesReady = false;
#endif
    } catch (...) {
#ifdef __APPLE__
      if (attributesReady) posix_spawnattr_destroy(&attributes);
#endif
      if (actionsReady) posix_spawn_file_actions_destroy(&actions);
      if (childSocket >= 0) close(childSocket);
      if (childMemory >= 0) close(childMemory);
      stop();
      throw;
    }
  }
 ~Child(){stop();}
 int fd()const{return socket_;}uint8_t* data()const{return shared_;}
 pid_t pid()const{return process_;}
};
} // namespace blah2::gpu_worker_transport
