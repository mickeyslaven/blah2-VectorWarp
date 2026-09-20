#pragma once
#include "MixedProtocol.h"
#include <complex>
#include <deque>
#include <memory>
#include <string>
#include <vector>

class IqData;
namespace blah2::mixed {
struct ProcessOptions {
  // Test injection only; production uses the executable beside the app.
  std::string executable;
  std::string argument;
  unsigned startupMs=30000;
  unsigned frameMs=450;
};
// One child owns all Vulkan and mixed-DSP state. The parent retains its CPI
// until a complete, finite map and filtered tail have been returned.
class Process {
  struct Impl;
  std::unique_ptr<Impl> impl_;
public:
  explicit Process(ProcessOptions options = {});
  ~Process();
  Process(const Process&)=delete;
  Process& operator=(const Process&)=delete;
  void run(const std::deque<std::complex<double>>& reference,
    const std::deque<std::complex<double>>& surveillance,
    std::vector<std::complex<double>>& map,
    std::vector<std::complex<double>>& tail);
  // Keep packed capture in shared memory and decode authoritative parent
  // inputs separately. A prepared frame can be consumed exactly once.
  void prepare_paired_i16(const int16_t* input, uint32_t count,
    IqData& reference, IqData& surveillance, uint32_t referenceChannel);
  void run_prepared(std::vector<std::complex<double>>& map,
    std::vector<std::complex<double>>& tail);
};
}
