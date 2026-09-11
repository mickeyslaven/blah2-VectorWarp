#include "CfarDetector1D.h"
#include "data/Map.h"

#include <iostream>
#include <vector>
#include <cmath>
#include <stdexcept>
#include <algorithm>

// constructor
CfarDetector1D::CfarDetector1D(double _pfa, int8_t _nGuard, int8_t _nTrain, int8_t _minDelay, double _minDoppler)
{
  if (!std::isfinite(_pfa) || _pfa <= 0 || _pfa >= 1 || _nGuard < 0 ||
      _nTrain <= 0 || !std::isfinite(_minDoppler) || _minDoppler < 0)
    throw std::invalid_argument("Invalid CFAR probability, training window or minimum Doppler");
  // input
  pfa = _pfa;
  nGuard = _nGuard;
  nTrain = _nTrain;
  minDelay = _minDelay;
  minDoppler = _minDoppler;
}

CfarDetector1D::~CfarDetector1D()
{
}

std::unique_ptr<Detection> CfarDetector1D::process(Map<std::complex<double>> *x)
{ 
  int32_t nDelayBins = x->get_nCols();
  int32_t nDopplerBins = x->get_nRows();

  std::vector<double> mapRowSquare(nDelayBins);
  struct Window {
    int leftBegin, leftEnd, rightBegin, rightEnd, count;
    double alpha;
  };
  // The training geometry and false-alarm factor depend on delay, not Doppler.
  // Preserve the original left-then-right summation order: rolling/prefix sums
  // can round differently at the detection threshold.
  std::vector<Window> windows;
  windows.reserve(nDelayBins);
  for (int j = 0; j < nDelayBins; ++j) {
    const int leftBegin = std::max(0, j - nGuard - nTrain);
    const int leftEnd = std::max(0, j - nGuard);
    const int rightBegin = std::min(nDelayBins, j + nGuard + 1);
    const int rightEnd = std::min(nDelayBins, j + nGuard + nTrain + 1);
    const int count = leftEnd - leftBegin + rightEnd - rightBegin;
    windows.push_back({leftBegin, leftEnd, rightBegin, rightEnd, count,
      count ? count * (pow(pfa, -1.0 / count) - 1) : 0});
  }

  // store detections temporarily
  std::vector<double> delay;
  std::vector<double> doppler;
  std::vector<double> snr;

  // loop over every cell
  for (int i = 0; i < nDopplerBins; i++)
  { 
    // skip if less than min Doppler
    if (std::abs(x->doppler[i]) < minDoppler)
    {
      continue;
    } 
    const auto& mapRow = x->data[i];
    for (int j = 0; j < nDelayBins; j++)
    {
      mapRowSquare[j] = (double) std::abs(mapRow[j]*mapRow[j]);
    }
    for (int j = 0; j < nDelayBins; j++)
    {
      // skip if less than min delay
      if (x->delay[j] < minDelay)
      {
        continue;
      } 
      // compute threshold
      const auto& window = windows[j];
      int nCells = window.count;
      if (nCells == 0) continue;
      double trainNoise = 0.0;
      for (int k = window.leftBegin; k < window.leftEnd; ++k)
        trainNoise += mapRowSquare[k];
      for (int k = window.rightBegin; k < window.rightEnd; ++k)
        trainNoise += mapRowSquare[k];
      trainNoise /= nCells;
      double threshold = window.alpha * trainNoise;

      // detection if over threshold
      if (mapRowSquare[j] > threshold)
      {
        delay.push_back(j + x->delay[0]);
        doppler.push_back(x->doppler[i]);
        snr.push_back((double)10 * std::log10(std::abs(mapRow[j])) - x->noisePower);
      }
    }
  }

  // create detection
  return std::make_unique<Detection>(delay, doppler, snr);
}
