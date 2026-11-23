#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

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
std::size_t cap_fft_threads_for_lanes(std::size_t configuredMaximum,
  std::size_t activeLanes, std::size_t availableCpus);
std::size_t choose_illuminator_lane_workers(std::size_t activeLanes,
  std::size_t configuredWorkers, std::size_t availableCpus);
// The legacy selector remains per Wiener-Hopf instance. The explicitly named
// total selector is consumed only by RadarBank when the legacy selector is
// absent, so existing service overrides cannot change scope silently.
std::size_t configured_clutter_worker_slots();
std::size_t configured_clutter_total_worker_budget();
// Explicit totals below active lanes are refused; totals above the safe
// hardware/cache bound clamp to that bound.
std::vector<std::size_t> distribute_clutter_worker_budget(std::size_t lanes,
  std::size_t configuredWorkers, std::size_t availableCpus);
std::vector<std::size_t> resolve_clutter_worker_slots(std::size_t lanes,
  std::size_t legacyWorkers, std::size_t totalWorkers,
  std::size_t availableCpus);

namespace detail {
// Exposed for tests with synthetic cgroup files, not changes to the host.
using ReadText = std::function<std::string(const std::string&)>;
std::size_t cgroup_cpu_limit(std::size_t cpus, const std::string& membership,
  const std::string& mounts, const ReadText& read);
}
}
