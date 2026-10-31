#pragma once

#include "IqClip.h"
#include "Recording.h"

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>

namespace blah2 {

// Receiver-independent bounded pre/post-roll recorder. Sample ingress never
// waits for filesystem I/O and deliberately drops its recorder copy when the
// short state mutex is contended.
class PortableClipRecorder {
public:
  struct Options {
    double ringSeconds = 2.0;
    double maximumClipSeconds = 5.0;
    std::size_t maximumWriterJobs = 2;
    std::function<int64_t()> clockNanoseconds;
    std::function<void()> beforeWrite;
  };

  PortableClipRecorder(std::string directory, std::string receiver, uint32_t channels,
    uint32_t sampleRate, uint32_t frequency, Options options);
  ~PortableClipRecorder();
  PortableClipRecorder(const PortableClipRecorder&) = delete;
  PortableClipRecorder& operator=(const PortableClipRecorder&) = delete;

  bool ingest(IqBlock samples, uint64_t epoch = 0,
    uint64_t firstSample = 0, bool positionAvailable = false,
    int64_t receiptNanoseconds = 0);
  void request(const IqClipRequest& request);
  IqClipStatus status() const;
  void discontinuity(const std::string& reason);

private:
  struct Block;
  struct Active;
  struct Job;

  std::string directory, receiver;
  uint32_t channels, sampleRate, frequency;
  Options options;
  uint64_t ringSamples;
  mutable std::mutex mutex;
  std::condition_variable condition;
  std::deque<std::shared_ptr<Block>> ring;
  uint64_t retainedSamples = 0;
  std::unique_ptr<Active> active;
  std::deque<std::unique_ptr<Job>> jobs;
  std::unordered_set<uint64_t> writerRevisions;
  IqClipStatus terminal;
  std::thread writer;
  bool stopping = false, haveEpoch = false;
  uint64_t streamEpoch = 0, nextSample = 0, fileSequence = 0;
  int64_t epochNanoseconds = 0;

  int64_t now_nanoseconds() const;
  int64_t sample_for_time(int64_t nanoseconds) const;
  void publish_locked(IqClipStatus value);
  void fail_active_locked(const std::string& error);
  void finish_active_locked();
  void writer_loop();
  IqClipStatus write_job(const Job& job);
};
}
