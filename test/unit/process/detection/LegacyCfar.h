#pragma once
#include "data/Detection.h"
#include "data/Map.h"
#include <cmath>
#include <memory>
#include <vector>

// Frozen algorithm from VectorWarp 2a9bfdf, before allocation/formatting work.
// Keep its per-cell construction and floating-point operation order as the
// correctness/performance control; do not share helpers with production CFAR.
inline std::unique_ptr<Detection> legacyCfar(Map<std::complex<double>>& map,
    double pfa, int guard, int train, int minDelay, double minDoppler) {
  const int columns = map.get_nCols(), rows = map.get_nRows();
  std::vector<std::complex<double>> row;
  std::vector<double> square, levels, delays, dopplers, snrs;
  for (int i = 0; i < rows; ++i) {
    if (std::abs(map.doppler[i]) < minDoppler) continue;
    row = map.get_row(i);
    for (int j = 0; j < columns; ++j) {
      square.push_back((double)std::abs(row[j] * row[j]));
      levels.push_back((double)10 * std::log10(std::abs(row[j])) - map.noisePower);
    }
    for (int j = 0; j < columns; ++j) {
      if (map.delay[j] < minDelay) continue;
      std::vector<int> indices;
      for (int k = j - guard - train; k < j - guard; ++k)
        if (k >= 0 && k < columns) indices.push_back(k);
      for (int k = j + guard + 1; k < j + guard + train + 1; ++k)
        if (k >= 0 && k < columns) indices.push_back(k);
      const int count = indices.size();
      if (!count) continue;
      const double alpha = count * (pow(pfa, -1.0 / count) - 1);
      double noise = 0;
      for (int k = 0; k < count; ++k) noise += square[indices[k]];
      noise /= count;
      const double threshold = alpha * noise;
      if (square[j] > threshold) {
        delays.push_back(j + map.delay[0]);
        dopplers.push_back(map.doppler[i]);
        snrs.push_back(levels[j]);
      }
      indices.clear();
    }
    square.clear(); levels.clear();
  }
  return std::make_unique<Detection>(delays, dopplers, snrs);
}
