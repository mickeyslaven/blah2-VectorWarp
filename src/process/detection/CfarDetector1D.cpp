#include "CfarDetector1D.h"
#include "data/Map.h"
#include "process/detection/IntegratedCfar.h"
#include "process/integration/EvidenceWorkBudget.h"

#include <iostream>
#include <vector>
#include <cmath>
#include <stdexcept>
#include <algorithm>
#include <limits>

// constructor
CfarDetector1D::CfarDetector1D(double _pfa, int8_t _nGuard, int8_t _nTrain,
  int32_t _minDelay, double _minDoppler, uint32_t _integrationCpis)
{
  if (!std::isfinite(_pfa) || _pfa <= 0 || _pfa >= 1 || _nGuard < 0 ||
      _nTrain <= 0 || !std::isfinite(_minDoppler) || _minDoppler < 0 ||
      (_integrationCpis != 1 && _integrationCpis != 2 &&
       _integrationCpis != 4 && _integrationCpis != 8))
    throw std::invalid_argument("Invalid CFAR probability, training window or minimum Doppler");
  // input
  pfa = _pfa;
  nGuard = _nGuard;
  nTrain = _nTrain;
  minDelay = _minDelay;
  minDoppler = _minDoppler;
  integrationCpis = _integrationCpis;
}

CfarDetector1D::~CfarDetector1D()
{
}

std::unique_ptr<Detection> CfarDetector1D::process(Map<std::complex<double>> *x,
  const std::vector<std::vector<double>>* cellBackgroundPower,
  const blah2::integration::EvidenceWorkBudget* budget,
  std::size_t maximumDetections, uint64_t* truncatedDetections, Trials* trials,
  const std::vector<double>* detectorPower)
{
  return process(x, std::vector<uint8_t>{}, cellBackgroundPower, budget,
    maximumDetections, truncatedDetections, trials, detectorPower);
}

std::unique_ptr<Detection> CfarDetector1D::process(Map<std::complex<double>> *x,
  const std::vector<uint8_t>& validCells,
  const std::vector<std::vector<double>>* cellBackgroundPower,
  const blah2::integration::EvidenceWorkBudget* budget,
  std::size_t maximumDetections, uint64_t* truncatedDetections, Trials* trials,
  const std::vector<double>* detectorPower)
{
  if (!maximumDetections)
    throw std::invalid_argument("CFAR maximum detections must be positive");
  if (truncatedDetections) *truncatedDetections = 0;
  int32_t nDelayBins = x->get_nCols();
  int32_t nDopplerBins = x->get_nRows();
  if (!validCells.empty() && validCells.size() != uint64_t(nDopplerBins) * nDelayBins)
    throw std::invalid_argument("CFAR validity dimensions differ");
  if (detectorPower && detectorPower->size() != uint64_t(nDopplerBins) * nDelayBins)
    throw std::invalid_argument("CFAR power dimensions differ");
  if (cellBackgroundPower && cellBackgroundPower->size() !=
      static_cast<std::size_t>(nDopplerBins))
    throw std::invalid_argument("CFAR background has the wrong row count");
  if (cellBackgroundPower)
    for (const auto& row : *cellBackgroundPower)
      if (row.size() != static_cast<std::size_t>(nDelayBins))
        throw std::invalid_argument("CFAR background has the wrong column count");

  std::vector<double> mapRowSquare(nDelayBins);
  std::vector<double> mapRowRawPower(nDelayBins);
  std::vector<double> trainingSums;
  struct Window {
    int leftBegin, leftEnd, rightBegin, rightEnd, count;
    double alpha;
  };
  // The training geometry and false-alarm factor depend on delay, not Doppler.
  // Preserve the original left-then-right summation order: rolling/prefix sums
  // can round differently at the detection threshold.
  std::vector<Window> windows;
  windows.reserve(nDelayBins);
  std::vector<double> integratedAlpha(std::size_t(2 * nTrain + 1),
    std::numeric_limits<double>::quiet_NaN());
  for (int j = 0; j < nDelayBins; ++j) {
    const int leftBegin = std::max(0, j - nGuard - nTrain);
    const int leftEnd = std::max(0, j - nGuard);
    const int rightBegin = std::min(nDelayBins, j + nGuard + 1);
    const int rightEnd = std::min(nDelayBins, j + nGuard + nTrain + 1);
    const int count = leftEnd - leftBegin + rightEnd - rightBegin;
    double alpha = 0;
    if (count) {
      if (integrationCpis == 1)
        alpha = count * (pow(pfa, -1.0 / count) - 1);
      else {
        auto& cached = integratedAlpha.at(std::size_t(count));
        if (!std::isfinite(cached))
          cached = blah2::detection::integrated_cfar::alpha(
            pfa, uint32_t(count), integrationCpis);
        alpha = cached;
      }
    }
    windows.push_back({leftBegin, leftEnd, rightBegin, rightEnd, count, alpha});
  }

  // store detections temporarily
  std::vector<double> delay;
  std::vector<double> doppler;
  std::vector<double> snr;
  struct Peak { double delay, doppler, snr; };
  std::vector<Peak> bounded;
  const bool boundedOutput = maximumDetections !=
    std::numeric_limits<std::size_t>::max();
  if (boundedOutput) bounded.reserve(maximumDetections);
  const auto weakerFirst = [](const Peak& left, const Peak& right) {
    return left.snr > right.snr;
  };
  uint64_t totalDetections = 0;

  // loop over every cell
  for (int i = 0; i < nDopplerBins; i++)
  { 
    if (budget) budget->check();
    // skip if less than min Doppler
    if (std::abs(x->doppler[i]) < minDoppler)
    {
      continue;
    } 
    const auto& mapRow = x->data[i];
    const auto* validRow = validCells.empty() ? nullptr :
      validCells.data() + uint64_t(i) * nDelayBins;
    const auto valid = [validRow](int column) {
      return !validRow || validRow[column] != 0;
    };
    for (int j = 0; j < nDelayBins; j++)
    {
      if (budget && (j & 31) == 0) budget->check();
      if (detectorPower) {
        const double rawPower = (*detectorPower)[uint64_t(i) * nDelayBins + j];
        if (valid(j) && (!std::isfinite(rawPower) || rawPower < 0))
          throw std::invalid_argument("CFAR temporal power is not finite and nonnegative");
        mapRowRawPower[j] = rawPower;
        mapRowSquare[j] = cellBackgroundPower ?
          std::max(0.0, rawPower - (*cellBackgroundPower)[i][j]) : rawPower;
        continue;
      }
      if (!cellBackgroundPower) {
        // The default detector is a compatibility boundary: retain the exact
        // legacy operation order, including its Inf/NaN behavior and rounding
        // at a strict threshold.  The optional background statistic must not
        // perturb callers that did not request it.
        const auto value = mapRow[j];
        // Bootstrap/temporal maps are real amplitudes.  For finite real input,
        // abs(z*z) is exactly real*real (also at underflow/overflow); avoid the
        // complex-multiply recovery path without changing nonfinite behavior.
        if (value.imag() == 0 && std::isfinite(value.real()))
          mapRowSquare[j] = value.real() * value.real();
        else {
          const auto square = value * value;
          mapRowSquare[j] = square.imag() == 0 ?
            std::abs(square.real()) : std::abs(square);
        }
        continue;
      }
      const double rawPower = std::norm(mapRow[j]);
      mapRowRawPower[j] = rawPower;
      const double background = (*cellBackgroundPower)[i][j];
      mapRowSquare[j] = std::max(0.0, rawPower - background);
    }
    if (!validRow)
      blah2::detection::integrated_cfar::orderedTrainingSums(
        cellBackgroundPower ? mapRowRawPower : mapRowSquare,
        nGuard, nTrain, trainingSums, budget);
    for (int j = 0; j < nDelayBins; j++)
    {
      if (!valid(j)) continue;
      // skip if less than min delay
      if (x->delay[j] < minDelay)
      {
        continue;
      } 
      // compute threshold
      const auto& window = windows[j];
      int nCells = window.count;
      if (nCells == 0) continue;
      double trainNoise = validRow ? 0 : trainingSums[j];
      if (validRow) {
        for (int k = window.leftBegin; k < window.leftEnd; ++k) {
          if (!valid(k)) { --nCells; continue; }
          trainNoise += cellBackgroundPower ? mapRowRawPower[k] : mapRowSquare[k];
        }
        for (int k = window.rightBegin; k < window.rightEnd; ++k) {
          if (!valid(k)) { --nCells; continue; }
          trainNoise += cellBackgroundPower ? mapRowRawPower[k] : mapRowSquare[k];
        }
      }
      if (nCells == 0) continue;
      trainNoise /= nCells;
      double alpha = window.alpha;
      if (nCells != window.count) {
        if (integrationCpis == 1)
          alpha = nCells * (pow(pfa, -1.0 / nCells) - 1);
        else {
          auto& cached = integratedAlpha.at(std::size_t(nCells));
          if (!std::isfinite(cached))
            cached = blah2::detection::integrated_cfar::alpha(
              pfa, uint32_t(nCells), integrationCpis);
          alpha = cached;
        }
      }
      double threshold = alpha * trainNoise;

      if (trials) {
        ++trials->evaluated;
        if (!std::isfinite(mapRowSquare[j]) || !std::isfinite(threshold))
          ++trials->nonfinite;
        if (mapRowSquare[j] > threshold) ++trials->accepted;
      }

      // detection if over threshold
      if (mapRowSquare[j] > threshold)
      {
        const Peak peak{double(j + x->delay[0]), x->doppler[i],
          (double)10 * std::log10(std::abs(mapRow[j])) - x->noisePower};
        ++totalDetections;
        if (!boundedOutput) {
          delay.push_back(peak.delay); doppler.push_back(peak.doppler);
          snr.push_back(peak.snr);
        } else if (bounded.size() < maximumDetections) {
          bounded.push_back(peak);
          std::push_heap(bounded.begin(), bounded.end(), weakerFirst);
        } else if (peak.snr > bounded.front().snr) {
          std::pop_heap(bounded.begin(), bounded.end(), weakerFirst);
          bounded.back() = peak;
          std::push_heap(bounded.begin(), bounded.end(), weakerFirst);
        }
      }
    }
  }

  if (boundedOutput) {
    std::sort(bounded.begin(), bounded.end(), [](const Peak& left,
      const Peak& right) {
      if (left.snr != right.snr) return left.snr > right.snr;
      if (left.delay != right.delay) return left.delay < right.delay;
      return left.doppler < right.doppler;
    });
    delay.reserve(bounded.size()); doppler.reserve(bounded.size());
    snr.reserve(bounded.size());
    for (const auto& peak : bounded) {
      delay.push_back(peak.delay); doppler.push_back(peak.doppler);
      snr.push_back(peak.snr);
    }
    if (truncatedDetections)
      *truncatedDetections = totalDetections - bounded.size();
  }
  // create detection
  return std::make_unique<Detection>(delay, doppler, snr);
}
