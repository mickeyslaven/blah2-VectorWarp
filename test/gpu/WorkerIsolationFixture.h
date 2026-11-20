#pragma once
// Exercise the real parent transport against CPU doubles, never GPU hardware.
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <fcntl.h>
#include <stdexcept>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

namespace worker_isolation_fixture {
inline unsigned openDescriptors() {
  unsigned count = 0;
  for (int descriptor = 0; descriptor < 256; ++descriptor)
    count += fcntl(descriptor, F_GETFD) >= 0;
  return count;
}
template<class Create> void verify(Create create) {
  for (unsigned closed : {0u, 1u, 2u, 4u, 7u}) {
    for (bool absent : {false, true}) {
      const pid_t child = fork();
      if (child < 0) throw std::runtime_error("Cannot fork worker isolation fixture");
      if (!child) {
        alarm(8);
        try {
          const int input = open("/dev/null", O_RDONLY);
          if (input < 0) _exit(20);
          const int inherited = fcntl(input, F_DUPFD, 100);
          close(input);
          if (inherited < 100 || setenv("BLAH2_TEST_INHERITED_FD",
              std::to_string(inherited).c_str(), 1)) _exit(21);
          for (unsigned fd = 0; fd < 3; ++fd) if (closed & (1u << fd)) close(fd);
          const unsigned descriptors = openDescriptors();
          for (unsigned attempt = 0; attempt < 3; ++attempt) {
            bool failed = false;
            try { create(absent); } catch (const std::exception&) { failed = true; }
            if (failed != absent || openDescriptors() != descriptors) _exit(22);
            // The transport owns every spawned worker; the test must not reap it.
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(300);
            for (;;) {
              int status = 0;
              const auto pending = waitpid(-1, &status, WNOHANG);
              if (pending == -1 && errno == ECHILD) break;
              if (pending > 0 || std::chrono::steady_clock::now() >= deadline) _exit(23);
              std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
          }
          _exit(0);
        } catch (...) { _exit(24); }
      }
      int status = 0;
      pid_t waited;
      do { waited = waitpid(child, &status, 0); } while (waited < 0 && errno == EINTR);
      if (waited != child || !WIFEXITED(status) || WEXITSTATUS(status))
        throw std::runtime_error("Worker isolation/closed-stdio cleanup failed: mask=" +
          std::to_string(closed) + " absent=" + std::to_string(absent) +
          " status=" + std::to_string(status));
    }
  }
}
}
