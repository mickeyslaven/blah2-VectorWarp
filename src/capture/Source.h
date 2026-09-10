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
  mutable std::mutex recordingMutex;
  std::atomic<bool> recordingActive{false};
  std::unique_ptr<blah2::RecordingWriter> recordingWriter;
  std::string recordingFile, recordingError;
  uint64_t recordedSamples = 0;
  std::vector<std::deque<std::complex<float>>> pendingChannels;
  std::vector<uint64_t> pendingStarts;
  std::vector<bool> channelStarted;
  void recording_failed(const std::string& error);
  void flush_recording_locked();

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

  // File replay is receiver-independent and is owned by Capture/ReplayPlayer.

  struct RecordingStatus {
    bool active = false;
    std::string file, error;
    uint64_t samples = 0;
  };
  bool is_recording() const { return recordingActive.load(); }
  RecordingStatus recording_status() const;
  // Preserve a partial file for diagnosis, but never mark it complete after a
  // receiver reports a gap, reset, overrun, or malformed callback.
  void recording_discontinuity(const std::string& error);
  void record_block(const blah2::IqBlock& samples);
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
