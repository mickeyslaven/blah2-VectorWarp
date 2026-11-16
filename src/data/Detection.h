/// @file Detection.h
/// @class Detection
/// @brief A class to store detection data.
/// @author 30hours

#ifndef DETECTION_H
#define DETECTION_H

#include <stdint.h>
#include <vector>
#include <complex>
#include <array>
#include <stdexcept>
#include <utility>

class Detection
{
public:
  // Radar-only, exact CPI support. Empty metadata denotes an ordinary
  // single-CPI measurement, never an inferred temporal history.
  struct Evidence {
    uint64_t epoch = 0, newestSequence = 0;
    uint8_t depth = 0, bank = 0;
    int8_t delayMotion = 0, dopplerMotion = 0;
    std::array<uint64_t, 8> sequences{};
  };
private:
  std::vector<Evidence> evidence;
  /// @brief Detections in delay (bins).
  std::vector<double> delay;

  /// @brief Detections in Doppler (Hz).
  std::vector<double> doppler;

  /// @brief Detections in SNR.
  std::vector<double> snr;

public:
  const std::vector<Evidence>& get_evidence() const { return evidence; }
  void set_evidence(std::vector<Evidence> value) {
    if (!value.empty() && value.size() != delay.size())
      throw std::invalid_argument("Detection evidence dimensions differ");
    evidence = std::move(value);
  }
  /// @brief Constructor.
  /// @param delay Detections in delay (bins).
  /// @param doppler Detections in Doppler (Hz).
  /// @return The object.
  Detection(std::vector<double> delay, std::vector<double> doppler, std::vector<double> snr);

  /// @brief Constructor for single detection.
  /// @param delay Detection in delay (bins).
  /// @param doppler Detection in Doppler (Hz).
  /// @return The object.
  Detection(double delay, double doppler, double snr);

  /// @brief Get detections in delay.
  /// @return Detections in delay (bins).
  std::vector<double> get_delay();

  /// @brief Get detections in Doppler.
  /// @return Detections in Doppler (Hz).
  std::vector<double> get_doppler();

  /// @brief Detections in SNR.
  /// @return Detections in SNR.
  std::vector<double> get_snr();

  /// @brief Get number of detections.
  /// @return Number of detections
  size_t get_nDetections();

  /// @brief Generate JSON of the detections and metadata.
  /// @param timestamp Current time (POSIX ms).
  /// @return JSON string.
  std::string to_json(uint64_t timestamp);

  /// @brief Generate display JSON directly with delay in kilometres.
  /// @param fs Sampling frequency (Hz), nonzero.
  std::string to_json_km(uint64_t timestamp, uint32_t fs);

  /// @brief Update JSON to convert delay bins to km.
  /// @param json Input JSON string with delay field.
  /// @param fs Sampling frequency (Hz).
  /// @return JSON string.
  std::string delay_bin_to_km(std::string json, uint32_t fs);

  /// @brief Append the detections to a save file.
  /// @param json JSON string of detections and metadata.
  /// @param path Path of file to save.
  /// @return True is save is successful.
  bool save(std::string json, std::string path);
};

#endif
