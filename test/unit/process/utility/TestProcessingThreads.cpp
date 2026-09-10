#include "process/utility/ProcessingThreads.h"
#include <algorithm>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>

void check(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

int main() {
  try {
    for (std::size_t cpus : {1U, 2U, 4U, 8U, 16U, 32U, 64U, 256U})
      for (std::size_t paths = 1; paths <= 8; ++paths)
        for (std::size_t workers : {0U, 1U, 2U, 4U, 8U})
          for (std::size_t fft : {0U, 1U, 2U, 4U, 8U, 256U}) {
            const auto plan = blah2::choose_processing_threads(paths, workers, fft, cpus);
            check(plan.workers >= 1 && plan.workers <= paths, "Worker bounds");
            check(plan.fftThreads >= 1, "FFTW never receives zero threads");
            if (workers) check(plan.workers == std::min(paths, workers), "Preserve manual workers");
            if (fft) check(plan.fftThreads == fft, "Preserve manual FFT count");
            else check(plan.fftThreads <= 4, "Bound automatic FFT overhead");
            if (!workers && !fft)
              check(plan.workers * plan.fftThreads <= std::max<std::size_t>(1, cpus - 1), "Auto oversubscribed available CPU budget");
          }
    const auto roomy = blah2::choose_processing_threads(5, 0, 0, 32);
    check(roomy.workers == 5 && roomy.fftThreads == 4, "Use capacity across workers and FFT teams");
    const auto small = blah2::choose_processing_threads(8, 0, 0, 4);
    check(small.workers == 3 && small.fftThreads == 1, "Small host needs room for acquisition");
    const auto manual = blah2::choose_processing_threads(1, 1, 4, 2);
    check(manual.workers == 1 && manual.fftThreads == 4, "Legacy explicit defaults must not change");
    const auto missing = blah2::choose_processing_threads(1, 0, 0, 0);
    check(missing.workers == 1 && missing.fftThreads == 1, "Unknown CPU count must be safe");
    bool rejected = false;
    try { blah2::choose_processing_threads(2, 0, 257, 32); }
    catch (const std::invalid_argument&) { rejected = true; }
    check(rejected, "Reject unreasonable explicit FFT count");

    std::map<std::string, std::string> files;
    const auto read = [&](const std::string& path) {
      const auto found = files.find(path);
      return found == files.end() ? std::string() : found->second;
    };
    const std::string mount = "1 0 0:1 / /sys/fs/cgroup rw - cgroup2 cgroup rw\n";
    const std::string group = "0::/parent/radar\n";
    files["/sys/fs/cgroup/parent/radar/cpu.max"] = "max 100000";
    files["/sys/fs/cgroup/parent/cpu.max"] = "150000 100000";
    check(blah2::detail::cgroup_cpu_limit(32, group, mount, read) == 1, "Respect parent and fractional CPU quotas");
    files["/sys/fs/cgroup/parent/cpu.max"] = "400000 100000";
    files["/sys/fs/cgroup/parent/radar/cpu.max"] = "200000 100000";
    check(blah2::detail::cgroup_cpu_limit(32, group, mount, read) == 2, "Use tightest visible quota");
    check(blah2::detail::cgroup_cpu_limit(1, group, mount, read) == 1, "Quota cannot expand affinity");
    files["/sys/fs/cgroup/cpu.max"] = "50000 100000";
    check(blah2::detail::cgroup_cpu_limit(32, "0::/\n", mount, read) == 1, "Namespaced half-CPU container");
    files["/sys/fs/cgroup/cpu.max"] = "max 100000";
    check(blah2::detail::cgroup_cpu_limit(32, "0::/\n", mount, read) == 32, "Unlimited quota");
    files["/sys/fs/cgroup/cpu.max"] = "broken";
    check(blah2::detail::cgroup_cpu_limit(32, "0::/\n", mount, read) == 32, "Unavailable telemetry falls back to affinity");
    const std::string v1Mount = "2 0 0:2 /slice /cpu\\040quota rw - cgroup cgroup rw,cpu,cpuacct\n";
    files["/cpu quota/radar/cpu.cfs_quota_us"] = "250000";
    files["/cpu quota/radar/cpu.cfs_period_us"] = "100000";
    files["/cpu quota/cpu.cfs_quota_us"] = "100000";
    files["/cpu quota/cpu.cfs_period_us"] = "100000";
    check(blah2::detail::cgroup_cpu_limit(32, "2:cpu,cpuacct:/slice/radar\n", v1Mount, read) == 1,
      "Support v1 controller lists, mount roots, escaped mount paths and parent quotas");
    check(blah2::detail::cgroup_cpu_limit(4, "", "", read) == 4, "Non-cgroup host fallback");
    std::cout << "Processing thread policy tests passed; detected CPU capacity="
      << blah2::available_cpu_threads() << "\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << "\n";
    return 1;
  }
}
