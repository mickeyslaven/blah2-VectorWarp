#pragma once
#include "Recording.h"
#include "data/IqData.h"
#include <atomic>
#include <functional>
#include <mutex>

namespace blah2 {
struct ReplayProgress {
  std::string state = "loading";
  uint64_t samples = 0, total = 0, loops = 0, trailingSamples = 0, generation = 0;
  uint32_t sampleRate = 0;
  RecordingProvenance provenance;
  bool controlsAvailable = false;
};

struct ReplayCommand {
  bool paused = false;
  uint64_t seekSample = 0, revision = 0;
};

// A revision and its payload are one publication. Separate atomics can expose
// a new revision with an old seek position even with sequential consistency.
class ReplayControlMailbox {
  mutable std::mutex mutex_;
  ReplayCommand command_;
public:
  void publish(ReplayCommand command) {
    std::lock_guard<std::mutex> lock(mutex_);
    command_ = command;
  }
  ReplayCommand snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return command_;
  }
};

class ReplayPlayer {
public:
  using Progress = std::function<void(const ReplayProgress&)>;
  using Control = std::function<ReplayCommand()>;
  using QueuePosition = std::function<void(uint64_t)>;
  // consumerBusy is checked at EOF so the final processing frame can finish
  // before reporting completion or starting a new loop.
  // Production sets awaitControlsAtEof: complete then means idle/seekable,
  // not an exited player. Bounded standalone calls may return with controls off.
  void run(const std::string& file, const ReplayOptions& options,
    const std::vector<IqData*>& buffers, uint32_t frameSamples, bool loop,
    const std::atomic<bool>& stopped, const Progress& progress,
    const std::function<bool()>& consumerBusy = [] { return false; },
    const Control& control = [] { return ReplayCommand{}; },
    bool awaitControlsAtEof = false,
    const QueuePosition& queuePosition = [](uint64_t) {});
};
}
