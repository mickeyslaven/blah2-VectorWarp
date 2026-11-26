#include "Centroid.h"
#include "IntegratedCfar.h"
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

std::unique_ptr<Detection> Centroid::process(Detection *x, double currentCpiPreferenceSnr)
{ 
  const auto delay = x->get_delay(), doppler = x->get_doppler(), snr = x->get_snr();
  const auto finite = [&](size_t i) {
    return std::isfinite(delay[i]) && std::isfinite(doppler[i]) && std::isfinite(snr[i]);
  };
  const auto rank = [&](size_t i) {
    return blah2::detection::integrated_cfar::temporalCandidateRank(
      !x->get_evidence().empty() && x->get_evidence()[i].depth == 1, snr[i], currentCpiPreferenceSnr);
  };

  std::vector<double> delay2, doppler2, snr2;
  std::vector<Detection::Evidence> evidence;
  std::vector<uint32_t> spatial;

  const size_t nDetections = std::min({delay.size(), doppler.size(), snr.size()});
  for (size_t i = 0; i < nDetections; i++)
  {
    if (!finite(i)) continue;
    // Keep bounds signed/double: uint16 underflow previously made a detection
    // near delay zero immune to nearby stronger detections.
    const double delayMin = delay[i] - nDelay, delayMax = delay[i] + nDelay;
    const double dopplerMin = doppler[i] - (nDoppler * resolutionDoppler);
    const double dopplerMax = doppler[i] + (nDoppler * resolutionDoppler);
    bool isCentroid = true;
    
    for (size_t j = 0; j < nDetections; j++)
    {
      if (j == i || !finite(j)) continue;
      if (delay[j] > delayMin && delay[j] < delayMax &&
          doppler[j] > dopplerMin && doppler[j] < dopplerMax &&
          (rank(i) < rank(j) || (rank(i) == rank(j) && snr[i] < snr[j]))) {
        isCentroid = false;
        break;
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
