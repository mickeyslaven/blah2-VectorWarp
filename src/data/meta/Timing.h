/// @file Timing.h
/// @class Timing
/// @brief A class to store timing statistics.
/// @author 30hours

#ifndef TIMING_H
#define TIMING_H

#include <algorithm>
#include <cstddef>
#include <stdint.h>
#include <vector>
#include <string>
#include "process/ambiguity/AccelerationStatus.h"
#include "process/analysis/MultiIlluminatorHealth.h"

class Timing
{
private:
  /// @brief Start time (POSIX ms).
  uint64_t tStart;

  /// @brief Current time (POSIX ms).
  uint64_t tNow = 0;

  /// @brief Number of CPI's.
  uint64_t n;

  /// @brief Time since first CPI (ms).
  uint64_t uptime = 0;

  /// @brief Time differences (ms).
  std::vector<double> time;

  /// @brief Names of time differences.
  std::vector<std::string> name;
  blah2::AccelerationStatus acceleration;
  bool ambiguityExecutionSet = false;
  bool ambiguityGpuExecuted = false, ambiguityCpuExecuted = false;
  blah2::AccelerationStatus widebandAcceleration;
  bool widebandAccelerationSet = false;
  bool widebandExecutionSet = false;
  bool widebandGpuExecuted = false;
  bool widebandCpuExecuted = false;
  blah2::AccelerationStatus clutterAcceleration;
  bool clutterExecutionSet = false;
  bool clutterGpuExecuted = false;
  bool clutterCpuExecuted = false;
  std::vector<uint64_t> captureBacklogSamples;
  std::vector<uint64_t> captureDroppedSamples;
  std::vector<uint64_t> capturePeakBacklogSamples;
  std::vector<uint64_t> captureBoundaryDiscardedSamples;
  std::vector<uint64_t> captureStopDiscardedSamples;
  uint32_t captureStorageBytesPerSample = 0;
  uint64_t captureConfiguredSamplesPerChannel = 0;
  bool captureSamplePositionAvailable = false;
  uint64_t captureEpoch = 0;
  uint64_t captureFirstEpochSample = 0;
  uint64_t captureEndEpochSample = 0;
  bool inputDiscontinuity = false;
  bool captureTerminal = false;
  std::vector<blah2::multi_illuminator::LaneHealthStatus> multiLaneHealth;

public:
  /// @brief Constructor.
  /// @param tStart Start time (POSIX ms).
  /// @return The object.
  Timing(uint64_t tStart);

  /// @brief Update the time differences and names.
  /// @param tNow Current time (POSIX ms).
  /// @param time Vector of time differences (ms).
  /// @param name Vector of time difference names.
  /// @return Void.
  void update(uint64_t tNow, std::vector<double> time, std::vector<std::string> name);

  /// @brief Generate JSON of the map and metadata.
  /// @return JSON string.
  std::string to_json();
  void set_acceleration(const blah2::AccelerationStatus& value) {
    acceleration = value; ambiguityExecutionSet = false;
  }
  void set_acceleration(const blah2::AccelerationStatus& value,
      bool gpuExecuted, bool cpuExecuted) {
    acceleration = value; ambiguityExecutionSet = true;
    ambiguityGpuExecuted = gpuExecuted; ambiguityCpuExecuted = cpuExecuted;
  }
  void set_wideband_acceleration(const blah2::AccelerationStatus& value) {
    widebandAcceleration = value;
    widebandAccelerationSet = true;
    widebandExecutionSet = false;
  }
  void set_wideband_acceleration(const blah2::AccelerationStatus& value,
      bool gpuExecuted, bool cpuExecuted) {
    widebandAcceleration = value;
    widebandAccelerationSet = true;
    widebandExecutionSet = true;
    widebandGpuExecuted = gpuExecuted;
    widebandCpuExecuted = cpuExecuted;
  }
  void set_clutter_acceleration(const blah2::AccelerationStatus& value,
    bool gpuExecuted, bool cpuExecuted) {
    clutterAcceleration = value;
    clutterExecutionSet = true;
    clutterGpuExecuted = gpuExecuted;
    clutterCpuExecuted = cpuExecuted;
  }
  void set_capture_queues(std::vector<uint64_t> backlog,
    std::vector<uint64_t> dropped) {
    captureBacklogSamples = std::move(backlog);
    captureDroppedSamples = std::move(dropped);
  }
  void set_multi_lane_health(
      std::vector<blah2::multi_illuminator::LaneHealthStatus> health) {
    multiLaneHealth = std::move(health);
  }
  void observe_capture_peak(const std::vector<uint64_t>& backlog) {
    if (capturePeakBacklogSamples.size() < backlog.size())
      capturePeakBacklogSamples.resize(backlog.size());
    for (std::size_t channel = 0; channel < backlog.size(); ++channel)
      capturePeakBacklogSamples[channel] = std::max(
        capturePeakBacklogSamples[channel], backlog[channel]);
  }
  void set_capture_discard_accounting(std::vector<uint64_t> boundary,
    std::vector<uint64_t> stopped) {
    captureBoundaryDiscardedSamples = std::move(boundary);
    captureStopDiscardedSamples = std::move(stopped);
  }
  void set_capture_storage_contract(uint32_t bytesPerSample,
    uint64_t configuredSamplesPerChannel) {
    captureStorageBytesPerSample = bytesPerSample;
    captureConfiguredSamplesPerChannel = configuredSamplesPerChannel;
  }
  void set_capture_sample_position(bool available, uint64_t epoch,
    uint64_t first, uint64_t end) {
    captureSamplePositionAvailable = available;
    captureEpoch = epoch;
    captureFirstEpochSample = first;
    captureEndEpochSample = end;
  }
  void set_input_discontinuity(bool value) { inputDiscontinuity = value; }
  void clear_execution() {
    ambiguityExecutionSet = widebandExecutionSet = clutterExecutionSet = false;
    ambiguityGpuExecuted = ambiguityCpuExecuted = false;
    widebandGpuExecuted = widebandCpuExecuted = false;
    clutterGpuExecuted = clutterCpuExecuted = false;
  }
  void set_capture_terminal(bool value) {
    captureTerminal = value;
    if (value) clear_execution();
  }

  /// @brief Append the map to a save file.
  /// @param json JSON string of map and metadata.
  /// @param path Path of file to save.
  /// @return True is save is successful.
  bool save(std::string json, std::string path);
};

#endif
