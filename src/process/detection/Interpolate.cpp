#include "Interpolate.h"
#include <iostream>
#include <vector>
#include <cmath>
#include <stdint.h>
#include <algorithm>

// constructor
Interpolate::Interpolate(bool _doDelay, bool _doDoppler)
{
  // input
  doDelay = _doDelay;
  doDoppler = _doDoppler;
}

Interpolate::~Interpolate()
{
}

std::unique_ptr<Detection> Interpolate::process(Detection *x, Map<std::complex<double>> *y)
{ 
  // store detections temporarily
  std::vector<double> delay, doppler, snr;
  delay = x->get_delay();
  doppler = x->get_doppler();
  snr = x->get_snr();

  if (!y)
    return std::make_unique<Detection>(delay, doppler, snr);
  // interpolate data
  double intDelay, intDoppler, intSnrDelay, intSnrDoppler, intSnr[3];
  std::vector<double> delay2, doppler2, snr2;
  std::deque<int> indexDelay = y->delay;
  std::deque<double> indexDoppler = y->doppler;

  if (indexDelay.empty() || indexDoppler.empty() || y->data.empty())
    return std::make_unique<Detection>(delay, doppler, snr);
  const auto snrAt = [&](size_t row, size_t column, double& value) {
    if (row >= y->data.size() || column >= y->data[row].size()) return false;
    const double magnitude = std::abs(y->data[row][column]);
    if (!(magnitude > 0) || !std::isfinite(magnitude) || !std::isfinite(y->noisePower)) return false;
    value = 10 * std::log10(magnitude) - y->noisePower;
    return std::isfinite(value);
  };
  const auto peakOffset = [](const double values[3], double& offset, double& peak) {
    if (!std::isfinite(values[0]) || !std::isfinite(values[1]) || !std::isfinite(values[2]) ||
        values[1] < values[0] || values[1] < values[2]) return false;
    const double denominator = 2 * (values[0] - 2 * values[1] + values[2]);
    if (!std::isfinite(denominator) || std::abs(denominator) < 1e-12) return false;
    offset = (values[0] - values[2]) / denominator;
    if (!std::isfinite(offset) || std::abs(offset) > 1) return false;
    peak = values[1] - ((values[0] - values[2]) * offset / 4);
    return std::isfinite(peak);
  };
  // loop over every detection
  const size_t nDetections = std::min(delay.size(), std::min(doppler.size(), snr.size()));
  for (size_t i = 0; i < nDetections; i++)
  {
    if (!std::isfinite(delay[i]) || !std::isfinite(doppler[i]) || !std::isfinite(snr[i])) continue;
    bool rejectNonPeak = false;
    // initialise interpolated values for bool flags
    intDelay = delay[i];
    intDoppler = doppler[i];
    intSnrDelay = snr[i];
    intSnrDoppler = snr[i];
    // interpolate in delay
    if (doDelay)
    {
      const auto delayIt = std::find_if(indexDelay.begin(), indexDelay.end(),
        [&](int bin) { return delay[i] == static_cast<double>(bin); });
      const auto dopplerIt = std::find(indexDoppler.begin(), indexDoppler.end(), doppler[i]);
      if (delayIt != indexDelay.end() && dopplerIt != indexDoppler.end()) {
        const size_t column = std::distance(indexDelay.begin(), delayIt), row = std::distance(indexDoppler.begin(), dopplerIt);
        double offset, peak;
        if (column > 0 && column + 1 < indexDelay.size() &&
            snrAt(row, column - 1, intSnr[0]) && snrAt(row, column, intSnr[1]) && snrAt(row, column + 1, intSnr[2])) {
          if (intSnr[1] < intSnr[0] || intSnr[1] < intSnr[2]) rejectNonPeak = true;
          else if (peakOffset(intSnr, offset, peak)) { intDelay = delay[i] + offset; intSnrDelay = peak; }
        }
      }
    }
    if (rejectNonPeak) continue;
    // interpolate in Doppler
    if (doDoppler)
    {
      const auto delayIt = std::find_if(indexDelay.begin(), indexDelay.end(),
        [&](int bin) { return delay[i] == static_cast<double>(bin); });
      const auto dopplerIt = std::find(indexDoppler.begin(), indexDoppler.end(), doppler[i]);
      if (delayIt != indexDelay.end() && dopplerIt != indexDoppler.end()) {
        const size_t column = std::distance(indexDelay.begin(), delayIt), row = std::distance(indexDoppler.begin(), dopplerIt);
        double offset, peak;
        if (row > 0 && row + 1 < indexDoppler.size() && column < indexDelay.size() &&
            snrAt(row - 1, column, intSnr[0]) && snrAt(row, column, intSnr[1]) && snrAt(row + 1, column, intSnr[2])) {
          if (intSnr[1] < intSnr[0] || intSnr[1] < intSnr[2]) rejectNonPeak = true;
          else if (peakOffset(intSnr, offset, peak)) {
            intDoppler = doppler[i] + (indexDoppler[row + 1] - indexDoppler[row]) * offset;
            intSnrDoppler = peak;
          }
        }
      }
    }
    if (rejectNonPeak) continue;
    // store interpolated detections
    delay2.push_back(intDelay);
    doppler2.push_back(intDoppler);
    snr2.push_back(std::max(std::max(intSnrDelay, intSnrDoppler), snr[i]));
  }

  // create detection
  return std::make_unique<Detection>(delay2, doppler2, snr2);
}
