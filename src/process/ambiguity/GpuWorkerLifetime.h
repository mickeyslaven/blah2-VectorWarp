#pragma once
// Private spawn contract, independent of the numerical/module and frame ABIs.
// PID 1 may be the live radar parent inside a container. Bind the spawning
// process explicitly instead of inferring orphanhood from a numeric PID.
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <csignal>
#include <chrono>
#include <climits>
#include <cstdint>
#include <poll.h>
#include <sstream>
#include <stdexcept>
#include <cstdlib>
#include <fcntl.h>
#include <limits>
#include <string>
#include <sys/resource.h>
#include <unistd.h>
#include <vector>
#ifdef __APPLE__
#include <sys/event.h>
#include <thread>
#else
#include <sys/prctl.h>
#endif

namespace blah2::gpu_worker_lifetime {
inline constexpr char parentVariable[] = "VECTORWARP_GPU_PARENT_PID";
// A killed worker stuck in a kernel driver may still own its reservation.
// Replacement backends must not allocate a second allowance until it exits.
inline std::atomic<unsigned>& pendingReaps() {
  static std::atomic<unsigned> count{0};
  return count;
}
inline bool replacementAvailable() noexcept { return pendingReaps().load() == 0; }

inline void bindParent(std::vector<std::string>& environment) {
  const std::string prefix = std::string(parentVariable) + '=';
  environment.erase(std::remove_if(environment.begin(), environment.end(),
    [&](const std::string& entry) { return entry.rfind(prefix, 0) == 0; }),
    environment.end());
  environment.push_back(prefix + std::to_string(getpid()));
}

inline pid_t expectedParent() noexcept {
  const char* value = std::getenv(parentVariable);
  if (!value || !*value) return 0;
  unsigned long long result = 0;
  const auto maximum = static_cast<unsigned long long>(std::numeric_limits<pid_t>::max());
  for (; *value; ++value) {
    if (*value < '0' || *value > '9') return 0;
    const unsigned digit = unsigned(*value - '0');
    if (result > (maximum - digit) / 10) return 0;
    result = result * 10 + digit;
  }
  return static_cast<pid_t>(result);
}

inline bool protectWorker() {
  const pid_t parent = expectedParent();
  // This also rejects a worker whose original parent died before it could
  // install the guard, even if a namespace init/subreaper has adopted it.
  if (parent <= 0 || getppid() != parent) return false;
#ifdef __APPLE__
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
      // Parent exit and failure of the lifetime guard both fail closed,
      // independently of a driver blocked in initialization or execution.
      _exit(1);
    }).detach();
  } catch (...) { close(watcher); return false; }
#else
  if (prctl(PR_SET_PDEATHSIG, SIGKILL) || getppid() != parent) return false;
  prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0);
#endif
  const rlimit noCore{0, 0};
  setrlimit(RLIMIT_CORE, &noCore);
  return true;
}
}

// Per-spawn diagnostics require an explicitly authorized offline caller.
namespace blah2::gpu_offline_diagnostics {
inline constexpr char profileVariable[]="BLAH2_GPU_CLUTTER_PROFILE_FRAMES";
inline constexpr char recipeVariable[]="VECTORWARP_GPU_OFFLINE_DIAGNOSTICS";
inline unsigned parseLimit(const char* value) noexcept {
  if(!value||!*value)return 0;
  unsigned count=0;
  for(;*value;++value){
    if(*value<'0'||*value>'9'||count>6)return 0;
    count=count*10+unsigned(*value-'0');
  }
  return count<=64?count:0;
}
inline void appendEnvironment(std::vector<std::string>& environment,bool offline) {
  const std::string profile=std::string(profileVariable)+'=',recipe=std::string(recipeVariable)+'=';
  environment.erase(std::remove_if(environment.begin(),environment.end(),[&](const auto& item){
    return item.rfind(profile,0)==0||item.rfind(recipe,0)==0;
  }),environment.end());
  const unsigned count=offline?parseLimit(std::getenv(profileVariable)):0;
  if(count)environment.push_back(recipe+std::to_string(getpid())+' '+std::to_string(count));
}
inline unsigned frameLimit() noexcept {
  try {
    const char* recipe=std::getenv(recipeVariable);
    if(!recipe)return 0;
    uint64_t parent=0;std::string count,extra;std::istringstream input(recipe);
    if(!(input>>parent>>count)||(input>>extra)||parent!=uint64_t(getppid())||
        gpu_worker_lifetime::expectedParent()!=getppid())return 0;
    return parseLimit(count.c_str());
  }catch(...){return 0;}
}
}

namespace blah2::gpu_receive_deadline {
using Clock=std::chrono::steady_clock;
// A host thread may resume after the worker has completed. Always inspect the
// queue, even at/after expiry; an empty queue receives no renewed allowance.
inline void waitReadable(int descriptor,Clock::time_point deadline,const char* failure){
  for(;;){
    const auto remaining=deadline-Clock::now();
    const auto milliseconds=std::chrono::duration_cast<std::chrono::milliseconds>(remaining);
    const auto rounded=milliseconds.count()+(remaining>milliseconds?1:0);
    pollfd socket{descriptor,POLLIN,0};
    const int ready=poll(&socket,1,int(std::max<int64_t>(0,std::min<int64_t>(rounded,INT_MAX))));
    if(ready<0&&errno==EINTR)continue; // Recompute the same absolute deadline.
    if(ready<0)throw std::runtime_error(failure);
    if(ready>0){
      if(socket.revents&(POLLIN|POLLHUP))return;
      throw std::runtime_error(failure);
    }
    if(Clock::now()>=deadline)throw std::runtime_error(failure);
  }
}
}
