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
  std::atomic<uint64_t> replayQueuedSamples{0};
  blah2::ReplayControlMailbox replayControl;
  std::string captureError;
  uint64_t recordingRequestId = 0;
  uint64_t unsupportedClipRevision = 0;
  std::string unsupportedClipError;
  bool automaticClips = false;
  double clipPreSeconds = 0, clipMaximumSeconds = 0;
  blah2::RecordingContext recordingContext;

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
  // A fatal live-input failure must terminate the process so a service
  // manager can restart acquisition. Replay errors intentionally remain
  // inspectable until the operator stops the replay process.
  std::atomic<bool> fatalLiveInputFailure{false};
  // Publishes the immutable receiver pointer after construction and all
  // optional extensions are installed. Readers must acquire this before
  // touching device; empty startup polling otherwise races factory_source().
  std::atomic<bool> sourceReady{false};
  std::atomic<bool> pairedCpiQueueActive{false};
  std::atomic<uint64_t> replayGeneration{0};
  void request_stop() { inputStopped.store(true); stopping.store(true); }
  void request_input_stop() { inputStopped.store(true); }
  void request_input_failure() {
    inputStopped.store(true);
    if (!replay) {
      fatalLiveInputFailure.store(true);
      stopping.store(true);
    }
  }
  bool fatal_live_input_failure() const {
    return fatalLiveInputFailure.load();
  }
  std::string status_json() const;
  // Publish the same bounded processor contract during both normal capture
  // and fail-fast replay admission. This keeps pre-capture input errors
  // visible without inventing a second service-status path.
  bool publish_status(const std::string& ip, uint16_t port) const;
  void processing_error(const std::string& error);
  // Classify a processing exception at the shared live/replay boundary. A
  // signal-requested stop is normal; an unsignalled live failure remains fatal.
  void processing_failure_or_stop(const std::string& error);

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
  void set_recording_context(blah2::RecordingContext context) {
    if (sourceReady.load()) throw std::logic_error("Recording context must be set before acquisition");
    recordingContext = std::move(context);
  }
  void require_exact_replay_acquisition(std::string trustedSha256,
      std::string trustedAcquisitionSha256 = {}) {
    replayOptions.requireExactAcquisition = true;
    replayOptions.expectedRecordingSha256 = std::move(trustedSha256);
    replayOptions.expectedAcquisitionSha256 = std::move(trustedAcquisitionSha256);
    replayOptions.validateAcquisitionContext = true;
    replayOptions.expectedAcquisitionContext = recordingContext;
  }
  void trust_replay_acquisition(std::string trustedSha256, std::string trustedAcquisitionSha256 = {}) {
    replayOptions.expectedRecordingSha256 = std::move(trustedSha256);
    replayOptions.expectedAcquisitionSha256 = std::move(trustedAcquisitionSha256);
  }

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
    std::string provenanceMode = "legacy-unqualified",
    uint64_t startSample = 0);
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
