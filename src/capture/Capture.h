/// @file Capture.h
/// @class Capture
/// @brief A class for a generic IQ capture device.
/// @author 30hours

#ifndef CAPTURE_H
#define CAPTURE_H

#include <string>
#include <vector>
#include <memory>
#include "data/Yaml.h"
#include <c4/format.hpp> // needed for the examples below

#include "data/IqData.h"
#include "capture/Source.h"
#include "capture/ReceiverLoader.h"
#include "capture/Replay.h"
#include "capture/PairedCpiQueue.h"

class Capture
{
private:
  /// @brief The valid capture devices.
  static const std::string VALID_TYPE[4];

  /// @brief The capture device type.
  std::string type;

  /// @brief True if IQ data to be saved.
  bool saveIq;

  /// @brief True if file replay is enabled.
  bool replay;

  /// @brief True if replay file should loop when complete.
  bool loop;

  /// @brief Absolute path of file to replay.
  std::string file;
  blah2::ReplayOptions replayOptions;
  mutable std::mutex statusMutex;
  blah2::ReplayProgress replayProgress;
  std::string captureError;
  uint64_t recordingRequestId = 0;
  uint64_t unsupportedClipRevision = 0;
  std::string unsupportedClipError;
  bool automaticClips = false;
  double clipPreSeconds = 0, clipMaximumSeconds = 0;

public:
  struct SamplePosition
  {
    bool available = false;
    uint64_t epoch = 0;
    uint64_t nextSample = 0;
    int64_t epochEstimateNanoseconds = 0;
    int64_t hostReceiptResidualNanoseconds = 0;
    int64_t hostReceiptMaximumResidualNanoseconds = 0;
    int64_t hostReceiptDriftPpm = 0;
    uint32_t hostReceiptAnchors = 0;
    int64_t deviceTimeEpochTick = 0;
    uint32_t sampleRate = 0;
    std::string clockSource, timeSource, motherboard, subdevices;
  };
  std::atomic<bool> stopping{false}, inputStopped{false}, processingBusy{false};
  std::atomic<bool> pairedCpiQueueActive{false};
  std::atomic<uint64_t> replayGeneration{0};
  std::atomic<uint64_t> replayControlRevision{0}, replaySeekSample{0};
  std::atomic<bool> replayPaused{false};
  void request_stop() { inputStopped.store(true); stopping.store(true); }
  void request_input_stop() { inputStopped.store(true); }
  std::string status_json() const;
  void processing_error(const std::string& error);

  /// @brief Sampling frequency (Hz).
  uint32_t fs;

  /// @brief Center frequency (Hz).
  uint32_t fc;

  /// @brief Absolute path to IQ save location.
  std::string path;

  /// @brief Pointer to capture device.
  blah2::ReceiverSource device;

  /// @brief Constructor.
  /// @param type The capture device type.
  /// @param fs Sampling frequency (Hz).
  /// @param fc Center frequency (Hz).
  /// @param path Absolute path to IQ save location.
  /// @return The object.
  Capture(std::string type, uint32_t fs, uint32_t fc, std::string path);
  void configure_aircraft_clips(bool enabled, double preSeconds,
    double postSeconds);

  /// @brief Implement the capture process.
  /// @param buffer1 Buffer for reference samples.
  /// @param buffer2 Buffer for surveillance samples.
  /// @param config Yaml config for device.
  /// @param ip_capture IP address of capture API.
  /// @param port_capture Port of capture API.
  /// @return Void.
  void process(IqData *buffer1, IqData *buffer2, c4::yml::NodeRef config,
    std::string ip_capture, uint16_t port_capture);

  void process(const std::vector<IqData *>& buffers,
    c4::yml::NodeRef config, std::string ip_capture, uint16_t port_capture,
    uint32_t frameSamples = 0);

  /// @brief Construct a capture source for the configured input channels.
  blah2::ReceiverSource factory_source(const std::string& type,
    c4::yml::NodeRef config, std::size_t channelCount = 2);

  /// @brief Set parameters to enable file replay.
  /// @param loop True if replay file should loop when complete.
  /// @param file Absolute path of file to replay.
  /// @return Void.
  void set_replay(bool loop, std::string file, std::string format = "auto",
    uint32_t legacyBlockSamples = 0,
    std::string provenanceMode = "legacy-unqualified");
  // Optional receiver extension, installed before start when the loaded
  // module supports it. Older modules retain the normal FIFO contract.
  void set_paired_cpi_queue(PairedCpiQueue* queue) { pairedCpiQueue = queue; }
  SamplePosition live_sample_position() const;
  std::vector<double> input_gain_db() const;
  uint64_t input_generation() const;
  bool requires_kraken_time_provenance() const {
    // Replay retains the configured receiver identity, but the recording
    // format has no stored RF-time epoch. Mark it inexact too instead of
    // letting wall-clock playback time masquerade as sample time.
    return type == "Kraken";
  }
  bool requires_usrp_time_provenance() const { return type == "Usrp"; }
  bool requires_hackrf_time_provenance() const { return type == "HackRF"; }
  bool requires_rspduo_time_provenance() const { return type == "RspDuo"; }

private:
  PairedCpiQueue* pairedCpiQueue = nullptr;

};

#endif
