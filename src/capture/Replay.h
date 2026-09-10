#pragma once
#include "Recording.h"
#include "data/IqData.h"
#include <atomic>
#include <functional>

namespace blah2 {
struct ReplayProgress {
  std::string state = "loading";
  uint64_t samples = 0, total = 0, loops = 0, trailingSamples = 0;
  uint32_t sampleRate = 0;
};

class ReplayPlayer {
public:
  using Progress = std::function<void(const ReplayProgress&)>;
  // consumerBusy is checked at EOF so the final processing frame can finish
  // before reporting completion or starting a new loop.
  void run(const std::string& file, const ReplayOptions& options,
    const std::vector<IqData*>& buffers, uint32_t frameSamples, bool loop,
    const std::atomic<bool>& stopped, const Progress& progress,
    const std::function<bool()>& consumerBusy = [] { return false; });
};
}
