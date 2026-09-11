#pragma once

#include <cstddef>
#include <functional>
#include <string>

namespace blah2 {

struct ProcessingThreads {
  std::size_t availableCpus;
  std::size_t workers;
  std::size_t fftThreads;
};

std::size_t available_cpu_threads();
ProcessingThreads choose_processing_threads(std::size_t paths,
  std::size_t configuredWorkers, std::size_t configuredFft,
  std::size_t availableCpus);

namespace detail {
// Exposed for tests with synthetic cgroup files, not changes to the host.
using ReadText = std::function<std::string(const std::string&)>;
std::size_t cgroup_cpu_limit(std::size_t cpus, const std::string& membership,
  const std::string& mounts, const ReadText& read);
}
}
