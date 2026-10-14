#pragma once
// Resident-only copy of the established isolated-worker lifecycle. Legacy source unchanged.
// Included after GpuProcess platform headers, inside blah2::resident::transport.
using Clock=std::chrono::steady_clock;
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

class Child {
 GpuProcessOptions options_;
 uint64_t bytes_=0;
 int socket_=-1,memory_=-1;
 pid_t process_=-1;
 uint8_t* shared_=nullptr;
public:
 void stop()noexcept{if(socket_>=0){close(socket_);
 socket_=-1;}reap(process_);
 process_=-1;
 if(shared_){munmap(shared_,size_t(bytes_));
 shared_=nullptr;}if(memory_>=0){close(memory_);
 memory_=-1;}}
 Child(const Child&)=delete;
 Child&operator=(const Child&)=delete;
 template<class Prepare>Child(uint64_t bytes,GpuProcessOptions options,Prepare prepare):options_(std::move(options)),bytes_(bytes){
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
      if (!options_.startupMs || !options_.frameMs || options_.argument != "--resident-v5")
        throw std::invalid_argument("Invalid GPU worker options");
      int sockets[2];
#ifdef __APPLE__
      // Darwin UNIX sockets do not support SOCK_SEQPACKET. The receiver above
      // assembles exactly one fixed-size protocol record under a single deadline.
      if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets))
#else
      if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sockets))
#endif
        throw std::runtime_error("Cannot create GPU worker connection");
      socket_ = sockets[0];
#ifdef __APPLE__
      if (fcntl(sockets[0], F_SETFD, FD_CLOEXEC) || fcntl(sockets[1], F_SETFD, FD_CLOEXEC)) {
        close(sockets[1]);
        throw std::runtime_error("Cannot isolate GPU connection");
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
          throw std::runtime_error("Cannot unlink GPU shared memory");
        }
        if (memory_ < 0 && errno != EEXIST) break;
      }
      if (childSocket < 0 || memory_ < 0 || fcntl(memory_, F_SETFD, FD_CLOEXEC) ||
          ftruncate(memory_, bytes_))
#else
      memory_ = memfd_create("blah2-gpu-resident-frame", MFD_CLOEXEC | MFD_ALLOW_SEALING);
      if (childSocket < 0 || memory_ < 0 || ftruncate(memory_, bytes_) ||
          fcntl(memory_, F_ADD_SEALS, F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL) < 0)
#endif
        throw std::runtime_error("Cannot allocate GPU shared memory");
      void* mapping = mmap(nullptr, bytes_, PROT_READ | PROT_WRITE, MAP_SHARED, memory_, 0);
      if (mapping == MAP_FAILED) throw std::runtime_error("Cannot map GPU shared memory");
      shared_ = static_cast<uint8_t*>(mapping);
      prepare(shared_,bytes_);
      childMemory = fcntl(memory_, F_DUPFD_CLOEXEC, 10);
      if (childMemory < 0 || posix_spawn_file_actions_init(&actions))
        throw std::runtime_error("Cannot prepare GPU worker");
      actionsReady = true;
      if (posix_spawn_file_actions_adddup2(&actions, childSocket, 3) ||
          posix_spawn_file_actions_adddup2(&actions, childMemory, 4))
        throw std::runtime_error("Cannot prepare GPU worker descriptors");
      // Close inherited descriptors after installing our two endpoints. This also
      // prevents a GPU worker from accidentally keeping capture/listening sockets alive.
#if defined(__APPLE__)
      // Kernel-enforced allowlisting also closes descriptors opened by another
      // thread during spawn, unlike a user-space /dev/fd snapshot.
      if (posix_spawnattr_init(&attributes))
        throw std::runtime_error("Cannot isolate GPU worker");
      attributesReady = true;
      if (posix_spawnattr_setflags(&attributes, POSIX_SPAWN_CLOEXEC_DEFAULT))
        throw std::runtime_error("Cannot isolate GPU worker descriptors");
      for (int fd = 0; fd < 3; ++fd)
        if (standardOpen[fd] && posix_spawn_file_actions_addinherit_np(&actions, fd))
          throw std::runtime_error("Cannot inherit GPU worker logging");
#elif defined(__GLIBC__) && defined(__GLIBC_MINOR__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 34))
      if (posix_spawn_file_actions_addclosefrom_np(&actions, 5))
        throw std::runtime_error("Cannot isolate GPU worker descriptors");
#else
      DIR* descriptors = opendir("/proc/self/fd");
      if (!descriptors) throw std::runtime_error("Cannot inspect GPU worker descriptors");
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
      if (error) { process_ = -1;
      throw std::runtime_error("GPU worker is unavailable"); }
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
};
inline bool protectWorker(){
  const pid_t parent = getppid();
#ifdef __APPLE__
  // EOF cannot stop a driver stuck inside an initialization/frame call. Watch
  // the parent's exact process lifetime independently and exit even then.
  if (parent == 1) return false;
  const int watcher = kqueue();
  if (watcher < 0) return false;
  struct kevent change;
  EV_SET(&change, parent, EVFILT_PROC, EV_ADD | EV_ENABLE | EV_ONESHOT, NOTE_EXIT, 0, nullptr);
  if (fcntl(watcher, F_SETFD, FD_CLOEXEC) ||
      kevent(watcher, &change, 1, nullptr, 0, nullptr) < 0 || getppid() != parent) {
    close(watcher);
    return false;
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
  } catch (...) { close(watcher);
  return false; }
#else
  if (parent == 1 || prctl(PR_SET_PDEATHSIG, SIGKILL) || getppid() != parent) return false;
  prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0);
#endif
  const rlimit noCore{0, 0};
  setrlimit(RLIMIT_CORE, &noCore);
  return true;
}
