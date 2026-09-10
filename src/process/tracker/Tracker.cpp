#include "Tracker.h"
#include <iostream>
#include <cmath>
#include <limits>
#include <stdexcept>

// constructor
Tracker::Tracker(uint32_t _m, uint32_t _n, uint32_t _nDelete, 
  double _cpi, double _maxAccInit, double _rangeRes, double _lambda)
{
  if (_m == 0 || _m > _n || _n > 255 || !std::isfinite(_cpi) || _cpi <= 0 ||
      !std::isfinite(_rangeRes) || _rangeRes <= 0 || !std::isfinite(_lambda) ||
      _lambda <= 0 || !std::isfinite(_maxAccInit) || _maxAccInit < 0)
    throw std::invalid_argument("Invalid tracking window, frame timing or radar geometry");
  m = _m;
  n = _n;
  nDelete = _nDelete;
  cpi = _cpi;
  maxAccInit = _maxAccInit;
  timestamp = 0;
  rangeRes = _rangeRes;
  lambda = _lambda;

  double resolutionAcc = 1/(cpi*cpi);
  const double accelerationBins = std::floor(maxAccInit / resolutionAcc);
  if (accelerationBins > 65535)
    throw std::invalid_argument("Too many initial tracking acceleration hypotheses");
  const int nAcc = static_cast<int>(accelerationBins);
  for (int i = 0; i < 2*nAcc+1; i++)
  {
    accInit.push_back(resolutionAcc*(i-nAcc));
  }

}

Tracker::~Tracker()
{
}

std::unique_ptr<Track> Tracker::process(Detection *detection, uint64_t currentTime)
{
  // Duplicate or reversed timestamps cannot define a velocity update. Do not
  // age tracks or create duplicates from a repeated frame.
  if (track.get_n() > 0 && currentTime <= timestamp)
    return std::make_unique<Track>(track);
  doNotInitiate.clear();
  for (size_t i = 0; i < detection->get_nDetections(); i++)
  {
    doNotInitiate.push_back(false);
  }

  if (track.get_n() > 0)
  {
    update(detection, currentTime);
  }
  else
  {
    timestamp = currentTime;
  }
  initiate(detection);

  return std::make_unique<Track>(track);
}

void Tracker::update(Detection *detection, uint64_t current)
{
  std::vector<double> delay = detection->get_delay();
  std::vector<double> doppler = detection->get_doppler();
  std::vector<double> snr = detection->get_snr();

  // get time between detections
  if (current <= timestamp) return;
  const double T = static_cast<double>(current - timestamp) / 1000;
  timestamp = current;

  // loop over each track
  for (uint64_t i = 0; i < track.get_n();)
  {
    // predict next position
    Detection detectionCurrent = track.get_current(i);
    const double acc = track.get_acceleration(i);
    Detection prediction = predict(detectionCurrent, acc, T);
    const double delayPredict = prediction.get_delay().front();
    const double dopplerPredict = prediction.get_doppler().front();
    size_t match = detection->get_nDetections();
    double bestDistance = std::numeric_limits<double>::infinity();
    
    // loop over detections to associate
    for (size_t j = 0; j < detection->get_nDetections(); j++)
    {
      // associate detections
      if (!doNotInitiate[j] && delay[j] > delayPredict-1 &&
        delay[j] < delayPredict+1 &&
        doppler[j] > dopplerPredict-1*(1/cpi) &&
        doppler[j] < dopplerPredict+1*(1/cpi))
      {
        const double deltaDelay = delay[j] - delayPredict;
        const double deltaDoppler = (doppler[j] - dopplerPredict) * cpi;
        const double distance = deltaDelay * deltaDelay + deltaDoppler * deltaDoppler;
        if (distance < bestDistance) { bestDistance = distance; match = j; }
      }
    }

    if (match < detection->get_nDetections())
    {
      const std::string previousState = track.get_state(i);
      track.set_current(i, Detection(delay[match], doppler[match], snr[match]));
      track.set_acceleration(i, (doppler[match] - detectionCurrent.get_doppler().front()) / T);
      track.set_nInactive(i, 0);
      doNotInitiate[match] = true;
      track.set_state(i, previousState == "ACTIVE" || previousState == "COASTING" ?
        "ACTIVE" : "ASSOCIATED");
      track.promote(i, m, n);
      ++i;
      continue;
    }

    // update state if no detections associated
    track.set_current(i, prediction);
    if (track.get_state(i) == "ACTIVE")
    {
      track.set_state(i, "COASTING");
    }
    else if (track.get_state(i) == "ASSOCIATED")
    {
      track.set_state(i, "TENTATIVE");
    }
    else
    {
      track.set_state(i, track.get_state(i));
    }
    track.set_nInactive(i, track.get_nInactive(i)+1);

    // remove if tentative or coasting too long
    if (track.get_nInactive(i) > nDelete)
    {
      track.remove(i);
    }
    else ++i;
  }
}

Detection Tracker::predict(Detection current, double acc, double T)
{
  double delayTrack = current.get_delay().front();
  double dopplerTrack = current.get_doppler().front();
  // Positive Doppler is decreasing bistatic path length. Acceleration is in
  // Hz/s, so both terms need wavelength before conversion to delay bins.
  double delayPredict = delayTrack - lambda * (dopplerTrack*T +
    0.5*acc*T*T) / rangeRes;
  double dopplerPredict = dopplerTrack+(acc*T);
  Detection prediction(delayPredict, dopplerPredict, 0);
  return prediction;
}

void Tracker::initiate(Detection *detection)
{  
  std::vector<double> delay = detection->get_delay();
  std::vector<double> doppler = detection->get_doppler();
  std::vector<double> snr = detection->get_snr();
  uint64_t index;

  // loop over new detections
  for (size_t i = 0; i < detection->get_nDetections(); i++)
  {
    // skip if detection used in update
    if (doNotInitiate.at(i))
    {
      continue;
    }
    // add tentative detection for each acc
    for (size_t j = 0; j < accInit.size(); j++)
    {
      Detection detectionCurrent(delay[i], doppler[i], snr[i]);
      index = track.add(detectionCurrent);
      track.set_acceleration(index, accInit[j]);
    }
  }
}
