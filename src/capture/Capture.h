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
#include "capture/Replay.h"

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

public:
  std::atomic<bool> stopping{false}, inputStopped{false}, processingBusy{false};
  std::atomic<uint64_t> replayGeneration{0};
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
  std::unique_ptr<Source> device;

  /// @brief Constructor.
  /// @param type The capture device type.
  /// @param fs Sampling frequency (Hz).
  /// @param fc Center frequency (Hz).
  /// @param path Absolute path to IQ save location.
  /// @return The object.
  Capture(std::string type, uint32_t fs, uint32_t fc, std::string path);

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
  std::unique_ptr<Source> factory_source(const std::string& type,
    c4::yml::NodeRef config, std::size_t channelCount = 2);

  /// @brief Set parameters to enable file replay.
  /// @param loop True if replay file should loop when complete.
  /// @param file Absolute path of file to replay.
  /// @return Void.
  void set_replay(bool loop, std::string file, std::string format = "auto",
    uint32_t legacyBlockSamples = 0);

};

#endif
