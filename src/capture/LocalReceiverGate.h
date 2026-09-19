#pragma once
#include <array>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <cerrno>
#include <filesystem>
#include <fstream>
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

#if defined(__APPLE__)
#include <CommonCrypto/CommonDigest.h>
namespace blah2 {
namespace local_receiver {
struct Gate {
  std::shared_ptr<int> lock;
  std::shared_ptr<int> runtime;
  std::string module;
};
inline bool trusted_user_path(const std::filesystem::path& value, const std::filesystem::path& permitted, bool regular = true) {
  try {
    if (!value.is_absolute()) return false;
    const char* homeenv = std::getenv("HOME");
    if (!homeenv || !*homeenv) return false;
    const auto canonical = std::filesystem::canonical(value);
    const auto home = std::filesystem::canonical(std::filesystem::path(homeenv));
    const auto root = std::filesystem::canonical(permitted);
    const auto support = home / "Library/Application Support/VectorWarp";
    if ((root != support && root.string().rfind(support.string() + "/", 0) != 0) ||
        (canonical != root && canonical.string().rfind(root.string() + "/", 0) != 0)) return false;
    for (auto current = canonical; current != home; current = current.parent_path()) {
      struct stat info{};
      if (lstat(current.c_str(), &info) || info.st_uid != getuid() || (info.st_mode & 0022) ||
          (!S_ISDIR(info.st_mode) && current != canonical)) return false;
    }
    struct stat info{};
    return !lstat(canonical.c_str(), &info) && (!regular || (S_ISREG(info.st_mode) && info.st_size > 0 && info.st_size <= 32 * 1024 * 1024));
  } catch (...) { return false; }
}
template <class Consume>
inline void read_user_file(const std::filesystem::path& file, std::size_t limit, Consume consume) {
  const int descriptor = open(file.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
  if (descriptor < 0) throw std::runtime_error("Local SDRplay publication changed while checking it");
  auto closer = [](int* value) { close(*value); delete value; };
  std::unique_ptr<int, decltype(closer)> guard(new int(descriptor), closer);
  struct stat before{}; if (fstat(descriptor, &before) || !S_ISREG(before.st_mode) || before.st_size <= 0 || static_cast<std::uintmax_t>(before.st_size) > limit)
    throw std::runtime_error("Local SDRplay publication is not a bounded regular file");
  std::array<unsigned char, 65536> buffer{};
  std::size_t total = 0;
  while (true) {
    const auto count = read(descriptor, buffer.data(), buffer.size());
    if (!count) break;
    if (count < 0) { if (errno == EINTR) continue; throw std::runtime_error("Cannot read local SDRplay publication"); }
    total += static_cast<std::size_t>(count);
    if (total > static_cast<std::size_t>(before.st_size)) throw std::runtime_error("Local SDRplay publication changed while checking it");
    consume(buffer.data(), static_cast<std::size_t>(count));
  }
  const auto unchanged = [&](const struct stat& after) {
    return S_ISREG(after.st_mode) && before.st_dev == after.st_dev && before.st_ino == after.st_ino &&
      before.st_size == after.st_size && before.st_mtimespec.tv_sec == after.st_mtimespec.tv_sec &&
      before.st_mtimespec.tv_nsec == after.st_mtimespec.tv_nsec &&
      before.st_ctimespec.tv_sec == after.st_ctimespec.tv_sec && before.st_ctimespec.tv_nsec == after.st_ctimespec.tv_nsec;
  };
  struct stat after{}, named{};
  if (total != static_cast<std::size_t>(before.st_size) || fstat(descriptor, &after) ||
      lstat(file.c_str(), &named) || !unchanged(after) || !unchanged(named))
    throw std::runtime_error("Local SDRplay publication changed while checking it");
}
inline std::string sha256_user_file(const std::filesystem::path& file) {
  CC_SHA256_CTX context; CC_SHA256_Init(&context);
  read_user_file(file, 32 * 1024 * 1024, [&](const unsigned char* data, std::size_t size) {
    CC_SHA256_Update(&context, data, static_cast<CC_LONG>(size));
  });
  std::array<unsigned char, CC_SHA256_DIGEST_LENGTH> digest{}; CC_SHA256_Final(digest.data(), &context); const char hex[] = "0123456789abcdef"; std::string result; result.reserve(64);
  for (auto byte : digest) { result.push_back(hex[byte >> 4]); result.push_back(hex[byte & 15]); } return result;
}
inline Gate verify(const std::string& executable_directory, const char* kit_id, const char* cohort) {
  const char* state = std::getenv("VECTORWARP_MACOS_STATE");
  if (!state || !*state) throw std::runtime_error("Local SDRplay support needs the VectorWarp macOS launcher state directory");
  const auto state_root = std::filesystem::path(state);
  const auto base = state_root / "adapters/rspduo";
  const auto current = base / "current";
  if (!trusted_user_path(base, state_root, false))
    throw std::runtime_error("Build or rebuild local SDRplay support in Receiver setup before starting RSPduo");
  const auto generation = std::filesystem::canonical(current);
  if (generation.parent_path() != std::filesystem::canonical(base / "generations") || !trusted_user_path(generation, state_root, false))
    throw std::runtime_error("Local SDRplay publication pointer is invalid; rebuild it in Receiver setup");
  const auto module = generation / "blah2-receiver-rspduo.dylib";
  const auto receipt = generation / "receipt.json";
  if (!trusted_user_path(module, state_root) || !trusted_user_path(receipt, state_root)) throw std::runtime_error("Local SDRplay publication is incomplete");
  std::string raw;
  read_user_file(receipt, 8192, [&](const unsigned char* data, std::size_t size) {
    raw.append(reinterpret_cast<const char*>(data), size);
  });
  rapidjson::Document value; value.Parse(raw.data(), raw.size());
  if (raw.size() > 8192 || value.HasParseError() || !value.IsObject())
    throw std::runtime_error("Local SDRplay publication receipt is invalid");
  const auto string = [&](const char* name) -> const char* { return value.HasMember(name) && value[name].IsString() ? value[name].GetString() : nullptr; };
  const char* receiptKit = string("kit_id"); const char* receiptCohort = string("cohort"); const char* receiptCore = string("core_sha256"); const char* receiptModule = string("module_sha256");
  const char* sdkInclude = value.HasMember("sdk") && value["sdk"].IsObject() && value["sdk"].HasMember("include") && value["sdk"]["include"].IsString() ? value["sdk"]["include"].GetString() : nullptr;
  const char* sdkLibrary = value.HasMember("sdk") && value["sdk"].IsObject() && value["sdk"].HasMember("library") && value["sdk"]["library"].IsString() ? value["sdk"]["library"].GetString() : nullptr;
  const char* include = std::getenv("BLAH2_SDRPLAY_INCLUDE_DIR"); const char* library = std::getenv("BLAH2_SDRPLAY_LIBRARY");
  if (!value.HasMember("schema") || !value["schema"].IsInt() || value["schema"].GetInt() != 1 || !receiptKit || !receiptCohort || !receiptCore || !receiptModule || !sdkInclude || !sdkLibrary || !include || !library || std::strcmp(receiptKit, kit_id) || std::strcmp(receiptCohort, cohort))
    throw std::runtime_error("Local SDRplay adapter belongs to a different VectorWarp build; rebuild it in Receiver setup");
  // Installed dylib SONAMEs and explicitly selected vendor SDK paths commonly
  // use symlinks. Resolve those once, then hash their bounded regular targets;
  // the local publication module and receipt remain no-follow generation files.
  const auto core = std::filesystem::canonical(std::filesystem::path(executable_directory) / "libblah2-capture-core.dylib");
  const auto sdkHeader = std::filesystem::canonical(std::filesystem::path(include) / "sdrplay_api.h");
  const auto sdkRuntime = std::filesystem::canonical(library);
  if (sha256_user_file(module) != receiptModule || sha256_user_file(core) != receiptCore ||
      sha256_user_file(sdkHeader) != sdkInclude || sha256_user_file(sdkRuntime) != sdkLibrary)
    throw std::runtime_error("Local SDRplay adapter belongs to a different VectorWarp build; rebuild it in Receiver setup");
  return {{}, {}, module.string()};
}
}
}
#elif defined(__linux__)
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
#endif
