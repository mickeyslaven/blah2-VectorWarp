#include "process/ambiguity/GpuWorkerLifetime.h"
#include <iostream>
#include <stdexcept>
#include <sys/wait.h>

namespace lifetime = blah2::gpu_worker_lifetime;
namespace {
void check(bool value, const char* message) {
  if (!value) throw std::runtime_error(message);
}
void childCheck(const char* binding, bool accepted) {
  const pid_t child = fork();
  check(child >= 0, "Cannot fork lifetime fixture");
  if (!child) {
    if (binding) {
      if (setenv(lifetime::parentVariable, binding, 1)) _exit(10);
    } else if (unsetenv(lifetime::parentVariable)) _exit(10);
    _exit(lifetime::protectWorker() == accepted ? 0 : 11);
  }
  int status = 0;
  pid_t waited;
  do { waited = waitpid(child, &status, 0); } while (waited < 0 && errno == EINTR);
  check(waited == child && WIFEXITED(status) && WEXITSTATUS(status) == 0,
    "Worker parent-lifetime acceptance differs");
}
int orphanParent(bool armed) {
  const std::string binding = std::to_string(getpid());
  check(setenv(lifetime::parentVariable, binding.c_str(), 1) == 0,
    "Cannot bind orphan fixture parent");
  const pid_t worker = fork();
  check(worker >= 0, "Cannot fork orphan fixture");
  if (!worker) {
    if (armed && !lifetime::protectWorker()) _exit(12);
    std::cout << getpid() << std::endl;
    if (!armed) {
      // The external test kills the bound parent before releasing this read.
      char byte = 0;
      ssize_t received;
      do { received = read(STDIN_FILENO, &byte, 1); } while (received < 0 && errno == EINTR);
      if (received != 1 || lifetime::protectWorker()) _exit(13);
      std::cout << "rejected-orphan" << std::endl;
      _exit(0);
    }
    for (;;) pause();
  }
  for (;;) pause();
}
}

int main(int argc, char** argv) try {
  if (argc == 2 && std::string(argv[1]) == "--orphan-before-guard") return orphanParent(false);
  if (argc == 2 && std::string(argv[1]) == "--orphan-after-guard") return orphanParent(true);
  check(argc == 1 || (argc == 2 && std::string(argv[1]) == "--require-pid-one"),
    "Invalid lifetime fixture argument");
  if (argc == 2) check(getpid() == 1, "PID namespace fixture is not PID 1");
  alarm(10);
  check(lifetime::replacementAvailable(), "Fresh worker replacement was blocked");
  ++lifetime::pendingReaps();
  check(!lifetime::replacementAvailable(), "Unreaped worker released its GPU reservation");
  --lifetime::pendingReaps();
  check(lifetime::replacementAvailable(), "Reaped worker did not release replacement admission");
  const std::string binding = std::to_string(getpid());
  std::vector<std::string> environment{"VECTORWARP_GPU_PARENT_PID=999999",
    "UNRELATED=preserved", "VECTORWARP_GPU_PARENT_PID=1"};
  lifetime::bindParent(environment);
  check(environment.size() == 2 && environment[0] == "UNRELATED=preserved" &&
    environment[1] == std::string(lifetime::parentVariable) + '=' + binding,
    "Private parent binding did not replace every inherited value");
  childCheck(binding.c_str(), true);
  childCheck(std::to_string(getpid() + 1).c_str(), false);
  for (const char* invalid : std::vector<const char*>{nullptr, "", "0", "-1", "1oops", "999999999999999999999999999999"})
    childCheck(invalid, false);
  std::cout << "PASS worker-lifetime parent=" << getpid() << '\n';
  return 0;
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
