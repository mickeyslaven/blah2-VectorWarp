/// @file Map.h
/// @class Map
/// @brief A class to store an ambiguity map.
/// @details References:
/// <a href="https://stackoverflow.com/questions/39110263/append-to-an-existing-array-of-json-objects-on-file-using-rapidjson">
/// Append to an existing array using rapidjson.</a>
/// @author 30hours

#ifndef MAP_H
#define MAP_H

#include <stdint.h>
#include <vector>
#include <deque>
#include <complex>
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

template <typename T>

class Map
{
private:
  /// @brief Number of rows.
  uint32_t nRows;

  /// @brief Number of columns.
  uint32_t nCols;

public:
  // Shared row-major metric accumulator. Producers may reuse an identical
  // cell level across their output maps without changing reduction order.
  struct MetricAccumulator {
    double sum = 0, peak = -std::numeric_limits<double>::infinity();
    uint64_t count = 0;
    static double magnitude(double value) { return std::abs(value); }
    static double magnitude(std::complex<double> value) {
      // hypot(x, +/-0) is exactly abs(x); other complex values retain hypot.
      return value.imag() == 0 ? std::abs(value.real()) : std::abs(value);
    }
    static double level(T value) {
      const double absolute = magnitude(value);
      if (!std::isfinite(absolute))
        throw std::runtime_error("Radar map contains a non-finite sample");
      return 10 * std::log10(std::max(1e-30, absolute));
    }
    void addLevel(double value) {
      sum = sum + value; peak = (peak < value) ? value : peak; ++count;
    }
    double add(T value) { const double result = level(value); addLevel(result); return result; }
    void apply(Map<T>& map) const {
      map.noisePower = count ? sum / static_cast<double>(count) : 0;
      map.maxPower = count ? peak - map.noisePower : 0;
    }
  };

  /// @brief Map data to store.
  std::vector<std::vector<T>> data;

  /// @brief Delay units of map data (bins).
  std::deque<int> delay;

  /// @brief Doppler units of map data (Hz).
  std::deque<double> doppler;

  /// @brief Noise power of map (dB).
  double noisePower;

  /// @brief Dynamic range of map (dB).
  double maxPower;

  /// @brief Constructor.
  /// @param nRows Number of rows.
  /// @param nCols Number of columns.
  /// @return The object.
  Map(uint32_t nRows, uint32_t nCols);

  /// @brief Update a row in the 2D map.
  /// @param i Index of row to update.
  /// @param row Data to update.
  /// @return Void.
  void set_row(uint32_t i, const std::vector<T>& row);

  /// @brief Update a column in the 2D map.
  /// @param i Index of column to update.
  /// @param col Data to update.
  /// @return Void.
  void set_col(uint32_t i, const std::vector<T>& col);

  /// @brief Create map metrics (noise power, dynamic range).
  /// @return Void.
  void set_metrics();

  /// @brief Exclude invalid temporal cells; an empty mask preserves all cells.
  /// @param validCells Row-major mask, either empty or one byte per map cell.
  void set_metrics(const std::vector<uint8_t>& validCells);

  /// @brief Get the number of rows in the map.
  /// @return Number of rows.
  uint32_t get_nRows();

  /// @brief Get the number of columns in the map.
  /// @return Number of columns.
  uint32_t get_nCols();

  /// @brief Get a row from the 2D map.
  /// @param row Index of row to get.
  /// @return Vector of data.
  std::vector<T> get_row(uint32_t row);

  /// @brief Get a column from the 2D map.
  /// @param col Index of column to get.
  /// @return Vector of data.
  std::vector<T> get_col(uint32_t col);

  /// @brief Get a copy of the map in dB units.
  /// @return Pointer to dB map.
  Map<double> *get_map_db();

  /// @brief Print the map to stdout (for debugging).
  /// @return Void.
  void print();

  /// @brief Convert a Doppler value from Hz to bins.
  /// @param dopplerHz Doppler value (Hz).
  /// @return dopplerBin Doppler value (bins).
  uint32_t doppler_hz_to_bin(double dopplerHz);

  /// @brief Generate JSON of the map and metadata.
  /// @return JSON string.
  std::string to_json(uint64_t timestamp);

  /// @brief Generate the same display JSON directly with delay in kilometres.
  /// @param fs Sampling frequency (Hz), nonzero.
  std::string to_json_km(uint64_t timestamp, uint32_t fs);

  /// @brief Generate map metadata without the large cell matrix. Downstream
  /// display code retains the most recent full matrix while consuming current
  /// detections and tracks.
  std::string to_json_metadata_km(uint64_t timestamp, uint32_t fs);

  /// @brief Generate a bounded display-only map. Radar processing and saved
  /// experiment products retain the complete native matrix; this method only
  /// reduces the monitor transport raster and quantizes it to quarter dB.
  std::string to_json_presentation_km(uint64_t timestamp, uint32_t fs,
    bool includeData, uint32_t maximumCells = 100000);

  /// @brief Update JSON to convert delay bins to km.
  /// @param json Input JSON string with delay field.
  /// @param fs Sampling frequency (Hz).
  /// @return JSON string.
  std::string delay_bin_to_km(std::string json, uint32_t fs);

  /// @brief Append the map to a save file.
  /// @param json JSON string of map and metadata.
  /// @param path Path of file to save.
  /// @return True is save is successful.
  bool save(std::string json, std::string path);
};

#endif
