#include "Centroid.h"
#include <iostream>
#include <vector>
#include <cmath>
#include <algorithm>

// constructor
Centroid::Centroid(uint16_t _nDelay, uint16_t _nDoppler, double _resolutionDoppler)
{
  // input
  nDelay = _nDelay;
  nDoppler = _nDoppler;
  resolutionDoppler = _resolutionDoppler;
}

Centroid::~Centroid()
{
}

std::unique_ptr<Detection> Centroid::process(Detection *x)
{ 
  // store detections temporarily
  std::vector<double> delay, doppler, snr;
  delay = x->get_delay();
  doppler = x->get_doppler();
  snr = x->get_snr();

  // centroid data
  double delayMin, delayMax;
  double dopplerMin, dopplerMax;
  bool isCentroid;
  std::vector<double> delay2, doppler2, snr2;
  std::vector<Detection::Evidence> evidence;
  std::vector<uint32_t> spatial;

  // loop over every detection
  const size_t nDetections = std::min(delay.size(), std::min(doppler.size(), snr.size()));
  for (size_t i = 0; i < nDetections; i++)
  {
    if (!std::isfinite(delay[i]) || !std::isfinite(doppler[i]) || !std::isfinite(snr[i]))
      continue;
    // Keep bounds signed/double: uint16 underflow previously made a detection
    // near delay zero immune to nearby stronger detections.
    delayMin = delay[i] - static_cast<double>(nDelay);
    delayMax = delay[i] + static_cast<double>(nDelay);
    dopplerMin = doppler[i] - (nDoppler * resolutionDoppler);
    dopplerMax = doppler[i] + (nDoppler * resolutionDoppler);
    isCentroid = true;
    
    // find detections to keep
    for (size_t j = 0; j < nDetections; j++)
    {
      // skip same detection
      if (j == i)
      {
        continue;
      }
      // search detections close by
      if (!std::isfinite(delay[j]) || !std::isfinite(doppler[j]) || !std::isfinite(snr[j]))
        continue;
      if (delay[j] > delayMin && delay[j] < delayMax &&
        doppler[j] > dopplerMin && doppler[j] < dopplerMax)
        {
          // remove if SNR is lower
          if (snr[i] < snr[j])
          {
            isCentroid = false;
            break;
          }
        }
    }
    // store centroided detections
    if (isCentroid)
    {
      delay2.push_back(delay[i]);
      doppler2.push_back(doppler[i]);
      snr2.push_back(snr[i]);  
      if (!x->get_evidence().empty()) evidence.push_back(x->get_evidence()[i]);
      if (!x->get_spatial().empty()) spatial.push_back(x->get_spatial()[i]);
    }
  }

  // create detection
  auto result = std::make_unique<Detection>(delay2, doppler2, snr2);
  result->set_evidence(std::move(evidence));
  result->set_spatial(std::move(spatial));
  return result;
}
