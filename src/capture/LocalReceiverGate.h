#pragma once
#include <array>
#include <chrono>
#include <cerrno>
#include <filesystem>
#include <deque>
#include <fcntl.h>
#include <memory>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <stdexcept>
#include <string>
#include <utility>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <rapidjson/document.h>

namespace blah2 {
namespace local_receiver {

inline bool trusted(const std::filesystem::path& path, bool regular = true,
                    uid_t owner = 0) {
  try {
    if (!path.is_absolute()) return false;
    std::deque<std::filesystem::path> pending;
    for (const auto& part : path.relative_path()) pending.push_back(part);
    auto current = path.root_path();
    unsigned links = 0;
    struct stat root{};
    if (lstat(current.c_str(), &root) != 0 || root.st_uid != owner ||
        !S_ISDIR(root.st_mode) || (root.st_mode & 0022)) return false;
    while (!pending.empty()) {
      const auto part = pending.front(); pending.pop_front();
      if (part == "." || part.empty()) continue;
      if (part == "..") { current = current.parent_path(); continue; }
      const auto candidate = current / part;
      struct stat info{};
      if (lstat(candidate.c_str(), &info) != 0 || info.st_uid != owner ||
          (!S_ISLNK(info.st_mode) && (info.st_mode & 0022))) return false;
      if (S_ISLNK(info.st_mode)) {
        if (++links > 40) return false;
        const auto target = std::filesystem::read_symlink(candidate);
        if (target.is_absolute()) current = target.root_path();
        std::deque<std::filesystem::path> expanded;
        for (const auto& item : target.relative_path()) expanded.push_back(item);
        expanded.insert(expanded.end(), pending.begin(), pending.end());
        pending.swap(expanded);
      } else {
        current = candidate;
        if (!pending.empty() && !S_ISDIR(info.st_mode)) return false;
      }
    }
    struct stat info{};
    return stat(current.c_str(), &info) == 0 && (!regular ||
      (S_ISREG(info.st_mode) && info.st_size <= 32 * 1024 * 1024));
  } catch (const std::filesystem::filesystem_error&) { return false; }
}

inline std::string status_process(const std::string& helper) {
  int pipefd[2];
  if (pipe2(pipefd, O_CLOEXEC | O_NONBLOCK) != 0)
    throw std::runtime_error("Cannot open the local receiver status pipe");
  auto close_fd = [](int* fd) { close(*fd); delete fd; };
  std::unique_ptr<int, decltype(close_fd)> input(new int(pipefd[0]), close_fd);
  std::unique_ptr<int, decltype(close_fd)> output(new int(pipefd[1]), close_fd);
  posix_spawn_file_actions_t actions;
  posix_spawnattr_t attributes;
  if (posix_spawn_file_actions_init(&actions) != 0)
    throw std::runtime_error("Cannot prepare local receiver status process");
  if (posix_spawnattr_init(&attributes) != 0) {
    posix_spawn_file_actions_destroy(&actions);
    throw std::runtime_error("Cannot prepare local receiver status limits");
  }
  int result = posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
  if (!result) result = posix_spawn_file_actions_adddup2(&actions, *output, STDOUT_FILENO);
  if (!result) result = posix_spawn_file_actions_adddup2(&actions, *output, STDERR_FILENO);
  if (!result) result = posix_spawn_file_actions_addclosefrom_np(&actions, 3);
  if (!result) result = posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP);
  if (!result) result = posix_spawnattr_setpgroup(&attributes, 0);
  std::array<char*, 5> argv{{const_cast<char*>("/usr/bin/python3"), const_cast<char*>("-I"),
    const_cast<char*>(helper.c_str()), const_cast<char*>("status"), nullptr}};
  std::array<char*, 4> env{{const_cast<char*>("PATH=/usr/bin:/bin"), const_cast<char*>("LANG=C"),
    const_cast<char*>("LC_ALL=C"), nullptr}};
  pid_t child = -1;
  if (!result) result = posix_spawn(&child, argv[0], &actions, &attributes, argv.data(), env.data());
  posix_spawn_file_actions_destroy(&actions);
  posix_spawnattr_destroy(&attributes);
  if (result) throw std::runtime_error("Cannot start the installed local receiver status helper");
  output.reset();
  std::string text;
  bool eof = false;
  int wait_status = 0;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  try {
    while (!eof) {
      if (std::chrono::steady_clock::now() >= deadline)
        throw std::runtime_error("Checking local SDRplay support timed out; use Receiver setup");
      pollfd descriptor{*input, POLLIN | POLLHUP, 0};
      const auto ready = poll(&descriptor, 1, 50);
      if (ready < 0 && errno != EINTR) throw std::runtime_error("Local receiver status pipe failed");
      if (ready <= 0) continue;
      std::array<char, 1024> buffer{};
      const auto count = read(*input, buffer.data(), buffer.size());
      if (count == 0) eof = true;
      else if (count > 0) {
        text.append(buffer.data(), static_cast<std::size_t>(count));
        if (text.size() > 4096) throw std::runtime_error("Local receiver status exceeds its size limit");
      } else if (errno != EAGAIN && errno != EINTR)
        throw std::runtime_error("Cannot read local receiver status");
    }
    // EOF alone does not establish that the helper has exited.
    while (true) {
      const auto waited = waitpid(child, &wait_status, WNOHANG);
      if (waited == child) break;
      if (waited < 0 && errno != EINTR) throw std::runtime_error("Cannot reap local receiver status helper");
      if (std::chrono::steady_clock::now() >= deadline)
        throw std::runtime_error("Local receiver status helper did not exit");
      poll(nullptr, 0, 10);
    }
  } catch (...) {
    kill(-child, SIGKILL);
    while (waitpid(child, &wait_status, 0) < 0 && errno == EINTR) {}
    throw;
  }
  if (!WIFEXITED(wait_status) || WEXITSTATUS(wait_status) != 0)
    throw std::runtime_error("Local SDRplay support check failed; use Receiver setup for details");
  return text;
}

struct Gate {
  std::shared_ptr<int> lock;
  std::shared_ptr<int> runtime;
  std::string module;
};

inline Gate verify(const std::string& executable_directory, const char* kit_id, const char* cohort) {
  if (geteuid() == 0)
    throw std::runtime_error("Locally built receiver adapters must run under the unprivileged VectorWarp service account");
  const std::filesystem::path bin(executable_directory);
  if (bin.filename() != "bin" || bin.parent_path().parent_path().filename() != "releases")
    throw std::runtime_error("Install VectorWarp before building local SDRplay support");
  const auto prefix = bin.parent_path().parent_path().parent_path();
  const auto helper = prefix / "libexec/vectorwarp-build-sdrplay";
  const std::filesystem::path lock_path("/run/vectorwarp-rspduo-build.lock");
  if (!trusted(helper) || !trusted("/usr/bin/python3") || !trusted(lock_path))
    throw std::runtime_error("Local SDRplay build support is missing or has unsafe permissions; reinstall VectorWarp");
  const int descriptor = open(lock_path.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
  if (descriptor < 0) throw std::runtime_error("Cannot read the local SDRplay build lock");
  Gate gate{std::shared_ptr<int>(new int(descriptor), [](int* fd) { close(*fd); delete fd; }), {}, {}};
  struct stat info{};
  if (fstat(descriptor, &info) != 0 || !S_ISREG(info.st_mode) || info.st_uid != 0 ||
      (info.st_mode & 0022) || flock(descriptor, LOCK_SH | LOCK_NB) != 0)
    throw std::runtime_error("Local SDRplay support is being built or its lock is unsafe; try again when the build finishes");
  const char* vendor = "/usr/local/lib/libsdrplay_api.so.3.15";
  if (!trusted(vendor)) throw std::runtime_error("Install the SDRplay Hardware API before building RSPduo support");
  const int runtime_fd = open(vendor, O_PATH | O_CLOEXEC | O_NOFOLLOW);
  if (runtime_fd < 0) throw std::runtime_error("Cannot pin the installed SDRplay runtime");
  gate.runtime = std::shared_ptr<int>(new int(runtime_fd), [](int* fd) { close(*fd); delete fd; });
  struct stat runtime_before{};
  if (fstat(runtime_fd, &runtime_before) || !S_ISREG(runtime_before.st_mode))
    throw std::runtime_error("The SDRplay runtime is not a regular library");
  const auto raw = status_process(helper.string());
  struct stat runtime_after{};
  if (stat(vendor, &runtime_after) || runtime_before.st_dev != runtime_after.st_dev ||
      runtime_before.st_ino != runtime_after.st_ino || runtime_before.st_size != runtime_after.st_size ||
      runtime_before.st_ctim.tv_sec != runtime_after.st_ctim.tv_sec ||
      runtime_before.st_ctim.tv_nsec != runtime_after.st_ctim.tv_nsec)
    throw std::runtime_error("The SDRplay runtime changed during verification; retry after installation finishes");
  rapidjson::Document status;
  status.Parse(raw.data(), raw.size());
  if (status.HasParseError() || !status.IsObject() || !status.HasMember("ok") ||
      !status["ok"].IsBool() || !status["ok"].GetBool() || !status.HasMember("state") ||
      !status["state"].IsString() || std::string(status["state"].GetString()) != "current")
    throw std::runtime_error("Build or rebuild SDRplay support in Receiver setup before starting RSPduo");
  for (const auto& identity : {std::pair<const char*, const char*>{"kit_id", kit_id}, {"cohort", cohort}}) {
    if (!status.HasMember(identity.first) || !status[identity.first].IsString() ||
        std::string(status[identity.first].GetString()) != identity.second)
      throw std::runtime_error("Local SDRplay adapter belongs to a different VectorWarp build; rebuild it in Receiver setup");
  }
  gate.module = "/var/lib/vectorwarp-adapters/rspduo/" + std::string(kit_id) +
    "/current/blah2-receiver-rspduo.so";
  if (!trusted(gate.module)) throw std::runtime_error("Local SDRplay adapter has unsafe ownership or permissions");
  // Keep the publication lock until the caller completes dlopen and ABI checks.
  return gate;
}
}
}
