#ifndef KRAKEN_CLIP_RECORDER_H
#define KRAKEN_CLIP_RECORDER_H

#include "HeimdallFrame.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <condition_variable>

namespace blah2
{
struct IqClipRequest
{
  uint64_t revision = 0;
  int64_t requestedEventTimeMs = 0;
  int64_t windowStartMs = 0;
  int64_t windowEndMs = 0;
  std::vector<std::string> eventIds;
  std::vector<std::string> passIds;
};

struct IqClipStatus
{
  uint64_t revision = 0;
  bool terminal = false;
  std::string file;
  std::string error;
  uint64_t samples = 0;
};

// Keeps compact, raw Heimdall frames on the receive side and performs all
// filesystem work on a bounded background queue. Ingress never waits for the
// queue mutex: contention is an explicit clip failure and starts a new epoch.
class KrakenClipRecorder
{
public:
  struct Options
  {
    double ringSeconds = 4.0;
    double maximumClipSeconds = 10.0;
    std::size_t maximumWriterJobs = 2;
    std::function<int64_t()> clockNanoseconds;
    // Test/diagnostic hook runs only on the background writer thread.
    std::function<void()> beforeWrite;
  };

  KrakenClipRecorder(std::string directory, uint32_t channels,
    uint32_t sampleRate, uint32_t frequency);
  KrakenClipRecorder(std::string directory, uint32_t channels,
    uint32_t sampleRate, uint32_t frequency, Options options);
  ~KrakenClipRecorder();
  KrakenClipRecorder(const KrakenClipRecorder&) = delete;
  KrakenClipRecorder& operator=(const KrakenClipRecorder&) = delete;

  void request(const IqClipRequest& request);
  IqClipStatus status() const;
  void discontinuity(const std::string& reason);

  // Ownership of metadata/payload moves into the bounded ring. False means
  // the receive thread deliberately dropped the recorder copy rather than
  // waiting; radar ingress remains unaffected.
  bool ingest(const std::array<uint8_t, HeimdallFrame::FIXED_HEADER_SIZE>& fixed,
    std::vector<uint8_t> metadata, std::vector<uint8_t> payload,
    const HeimdallFrame::Header& header);

private:
  struct Frame;
  struct Active;
  struct Job;

  std::string directory;
  uint32_t channels;
  uint32_t sampleRate;
  uint32_t frequency;
  Options options;
  uint64_t ringSamples;

  mutable std::mutex mutex;
  std::condition_variable condition;
  std::deque<std::shared_ptr<Frame>> ring;
  std::unique_ptr<Active> active;
  std::deque<std::unique_ptr<Job>> jobs;
  IqClipStatus terminal;
  std::thread writer;
  bool stopping = false;
  bool haveEpoch = false;
  int64_t epochNanoseconds = 0;
  uint64_t nextSample = 0;
  uint64_t epoch = 0;
  uint64_t fileSequence = 0;
  std::atomic<bool> ingressContention{false};

  int64_t now_nanoseconds() const;
  int64_t sample_for_time(int64_t timeNanoseconds) const;
  void start_request_locked(const IqClipRequest& request);
  void finish_active_locked();
  void fail_active_locked(const std::string& error);
  void reset_epoch_locked(const std::string& reason);
  void publish_locked(IqClipStatus status);
  void writer_loop();
  IqClipStatus write_job(const Job& job);
};
}

#endif
