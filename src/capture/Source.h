/// @file Source.h
/// @class Source
/// @brief An abstract class for capture sources.
/// @author 30hours

#ifndef SOURCE_H
#define SOURCE_H

#include <string>
#include <stdint.h>
#include <fstream>
#include <atomic>
#include <vector>
#include <memory>
#include <mutex>
#include <deque>
#include "Recording.h"
#include "data/IqData.h"

class Source
{
public:
  struct SampleTimePosition {
    bool available = false;
    uint64_t epoch = 0;
    uint64_t nextSample = 0;
    int64_t deviceTimeEpochTick = 0;
    uint32_t sampleRate = 0;
    std::string clockSource, timeSource, motherboard, subdevices;
  };

protected:

  /// @brief The capture device type.
  std::string type;

  /// @brief Center frequency (Hz).
  uint32_t fc;

  /// @brief Sampling frequency (Hz).
  uint32_t fs;

  /// @brief Absolute path to IQ save location.
  std::string path;

  /// @brief True if IQ data to be saved.
  bool *saveIq;

  std::atomic<bool> stopRequested{false};
  uint32_t recordingChannels = 2;

private:
  // Monotonic signal-processing epoch. Receiver discontinuities advance this
  // even when recording is disabled so adaptive DSP never spans incompatible
  // hardware/sample epochs.
  std::atomic<uint64_t> processingEpoch{0};
  mutable std::mutex recordingMutex;
  std::atomic<bool> recordingActive{false};
  std::atomic<bool> recordingResumePending{false};
  std::unique_ptr<blah2::RecordingWriter> recordingWriter;
  std::string recordingFile, recordingError;
  uint64_t recordedSamples = 0;
  std::vector<std::deque<std::complex<float>>> pendingChannels;
  std::vector<uint64_t> pendingStarts;
  std::vector<bool> channelStarted;
  SampleTimePosition recordingStartPosition;
  std::vector<std::string> recordingDiscontinuities;
  std::string open_file_locked(const SampleTimePosition* startPosition = nullptr);
  void record_block_locked(const blah2::IqBlock& samples,
    const SampleTimePosition* startPosition);
  void recording_failed(const std::string& error);
  void flush_recording_locked();
  void write_recording_provenance_locked(bool complete);

public:

  Source();

  /// @brief Constructor.
  /// @param type The capture device type.
  /// @param fs Sampling frequency (Hz).
  /// @param fc Center frequency (Hz).
  /// @param path Absolute path to IQ save location.
  /// @return The object.
  Source(std::string type, uint32_t fc, uint32_t fs, 
    std::string path, bool *saveIq, uint32_t channels = 2);

  virtual ~Source() = default;

  /// @brief Implement the capture process.
  /// @param buffer1 Buffer for reference samples.
  /// @param buffer2 Buffer for surveillance samples.
  /// @return Void.
  virtual void process(IqData *buffer1, IqData *buffer2) = 0;

  /// @brief Multi-channel capture; two-channel sources use this adapter.
  virtual void process(const std::vector<IqData *>& buffers);

  /// @brief Call methods to start capture.
  /// @return Void.
  virtual void start() = 0;

  /// @brief Call methods to gracefully stop capture.
  /// @return Void.
  virtual void stop() = 0;

  virtual SampleTimePosition sample_time_position() const { return {}; }
  // Backend-specific applied settings/readback embedded in the portable v2
  // recording contract. Empty is represented explicitly, never as exact.
  virtual std::string recording_provenance_json() const { return "{}"; }

  // File replay is receiver-independent and is owned by Capture/ReplayPlayer.

  struct RecordingStatus {
    bool active = false;
    std::string file, error;
    uint64_t samples = 0;
    bool resumePending = false;
  };
  bool is_recording() const { return recordingActive.load(); }
  bool recording_wants_samples() const {
    return recordingActive.load() || recordingResumePending.load();
  }
  RecordingStatus recording_status() const;
  uint64_t processing_epoch() const { return processingEpoch.load(std::memory_order_acquire); }
  // Preserve a partial file for diagnosis, but never mark it complete after a
  // receiver reports a gap, reset, overrun, or malformed callback.
  void recording_discontinuity(const std::string& error);
  // Close only the ambiguous epoch, then lazily start a new file on the first
  // qualified post-boundary block. This preserves receiver operation without
  // ever joining samples from different hardware-setting epochs.
  void recording_epoch_discontinuity(const std::string& error,
    bool advanceProcessingEpoch=true);
  void advance_processing_epoch() noexcept {
    processingEpoch.fetch_add(1, std::memory_order_acq_rel);
  }
  void record_block(const blah2::IqBlock& samples);
  void record_block(const blah2::IqBlock& samples,
    const SampleTimePosition& startPosition);
  void record_channel(unsigned channel, uint64_t firstSample,
    const std::vector<std::complex<float>>& samples);

  /// @brief Open a new file to record IQ.
  /// @details First creates a new file from current timestamp.
  /// Files are of format <path>.<type>.iq.
  /// @return String of full path to file.
  std::string open_file();

  /// @brief Close IQ file gracefully.
  /// @return Void.
  void close_file();

  /// @brief Graceful handler for SIGTERM.
  /// @return Void.
  void kill();

};

#endif
