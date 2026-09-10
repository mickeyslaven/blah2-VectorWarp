#include "ProcessingThreads.h"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <thread>
#ifdef __linux__
#include <sched.h>
#endif

namespace blah2 {
namespace {
#ifdef __linux__
std::string read_text(const std::string& path) {
  std::ifstream stream(path);
  std::ostringstream result;
  result << stream.rdbuf();
  return result.str();
}
#endif

bool has_controller(const std::string& list, const std::string& name) {
  return ("," + list + ",").find("," + name + ",") != std::string::npos;
}

std::string unescape_path(const std::string& text) {
  std::string result;
  for (std::size_t i = 0; i < text.size(); ++i) {
    if (text[i] == '\\' && i + 3 < text.size() &&
        text[i + 1] >= '0' && text[i + 1] <= '7' &&
        text[i + 2] >= '0' && text[i + 2] <= '7' &&
        text[i + 3] >= '0' && text[i + 3] <= '7') {
      result += static_cast<char>((text[i + 1] - '0') * 64 +
        (text[i + 2] - '0') * 8 + text[i + 3] - '0');
      i += 3;
    } else result += text[i];
  }
  return result;
}

std::size_t quota_limit(std::size_t cpus, const std::string& text) {
  long long quota = 0, period = 0;
  std::istringstream input(text);
  if (!(input >> quota >> period) || quota <= 0 || period <= 0) return cpus;
  return std::min(cpus, static_cast<std::size_t>(std::max(1LL, quota / period)));
}
}

std::size_t detail::cgroup_cpu_limit(std::size_t cpus,
    const std::string& membership, const std::string& mounts, const ReadText& read) {
  std::string v2Group, v1Group, line;
  std::istringstream groups(membership);
  while (std::getline(groups, line)) {
    const auto first = line.find(':');
    const auto second = first == std::string::npos ? first : line.find(':', first + 1);
    if (second == std::string::npos) continue;
    const auto controllers = line.substr(first + 1, second - first - 1);
    if (controllers.empty()) v2Group = line.substr(second + 1);
    else if (has_controller(controllers, "cpu")) v1Group = line.substr(second + 1);
  }
  std::istringstream mountLines(mounts);
  while (std::getline(mountLines, line)) {
    std::istringstream fields(line);
    std::string id, parent, device, root, mount, options, token, type, source, controllers;
    if (!(fields >> id >> parent >> device >> root >> mount >> options)) continue;
    while (fields >> token && token != "-") {}
    if (!(fields >> type >> source >> controllers)) continue;
    const bool v2 = type == "cgroup2";
    if (!v2 && !(type == "cgroup" && has_controller(controllers, "cpu"))) continue;
    const std::string group = v2 ? v2Group : v1Group;
    if (group.empty()) continue;
    root = unescape_path(root);
    mount = unescape_path(mount);
    // Do not follow paths outside the visible cgroup mount (e.g. namespaces).
    std::string relative;
    if (root == "/") relative = group;
    else if (group == root) relative = "/";
    else if (group.compare(0, root.size() + 1, root + "/") == 0)
      relative = group.substr(root.size());
    else relative = "/";
    if (relative.empty() || relative[0] != '/' ||
        (relative + "/").find("/../") != std::string::npos) relative = "/";
    std::string current = mount + (relative == "/" ? "" : relative);
    for (;;) {
      cpus = quota_limit(cpus, v2 ? read(current + "/cpu.max") :
        read(current + "/cpu.cfs_quota_us") + " " + read(current + "/cpu.cfs_period_us"));
      if (current == mount) break;
      const auto slash = current.find_last_of('/');
      if (slash == std::string::npos || slash < mount.size()) break;
      current.resize(slash);
    }
  }
  return std::max<std::size_t>(1, cpus);
}

std::size_t available_cpu_threads() {
  std::size_t cpus = std::max(1U, std::thread::hardware_concurrency());
#ifdef __linux__
  cpu_set_t affinity;
  CPU_ZERO(&affinity);
  if (sched_getaffinity(0, sizeof(affinity), &affinity) == 0 && CPU_COUNT(&affinity) > 0)
    cpus = std::min(cpus, static_cast<std::size_t>(CPU_COUNT(&affinity)));
  cpus = detail::cgroup_cpu_limit(cpus, read_text("/proc/self/cgroup"),
    read_text("/proc/self/mountinfo"), read_text);
#endif
  return cpus;
}

ProcessingThreads choose_processing_threads(std::size_t paths,
    std::size_t configuredWorkers, std::size_t configuredFft, std::size_t availableCpus) {
  if (paths == 0 || configuredWorkers > 8 || configuredFft > 256)
    throw std::invalid_argument("Processing needs at least one path, 0-8 workers and 0-256 FFT threads (0 = auto)");
  availableCpus = std::max<std::size_t>(1, availableCpus);
  // Leave capacity for acquisition and the UI when possible. Favor independent
  // paths; FFT threading is a bounded fallback, not a throughput benchmark.
  const auto budget = std::max<std::size_t>(1, availableCpus - 1);
  const auto workers = configuredWorkers ? std::min(paths, configuredWorkers) :
    std::min(paths, std::max<std::size_t>(1, budget / std::max<std::size_t>(1, configuredFft)));
  const auto fft = configuredFft ? configuredFft :
    std::min<std::size_t>(4, std::max<std::size_t>(1, budget / workers));
  return {availableCpus, workers, fft};
}
}
