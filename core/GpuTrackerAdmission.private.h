#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <stdexcept>

namespace gpu_tracker {
struct InitiationAdmission {
 uint32_t unmatched=0,admittedDetections=0,droppedDetections=0,initiatedTracks=0,totalTracks=0;
};

struct StationaryRetirementPolicy {
 uint32_t samples=0;
 uint32_t minimumAssociated=0;
 double maxDopplerHz=0;
 double maxDelaySpan=0;
};

inline StationaryRetirementPolicy stationaryRetirementPolicy(bool enabled,
 double stationaryMaxDopplerHz){
 if(!enabled)return {};
 if(!std::isfinite(stationaryMaxDopplerHz)||stationaryMaxDopplerHz<0)
  throw std::invalid_argument("GPU tracker stationary-retirement policy invalid");
 // The public trail always retains 100 samples. Requiring 90 associated
 // samples across that entire window rejects startup clutter without treating
 // a brief zero-Doppler crossing as stationary.
 return {100,90,std::min(1.0,stationaryMaxDopplerHz),1.0};
}

inline InitiationAdmission boundInitiations(uint32_t capacity,uint32_t retained,
 uint32_t unmatched,uint32_t hypotheses){
 if(!hypotheses||retained>capacity)throw std::invalid_argument("GPU tracker initiation admission invalid");
 const uint32_t admitted=std::min(unmatched,(capacity-retained)/hypotheses);
 return {unmatched,admitted,unmatched-admitted,admitted*hypotheses,
  retained+admitted*hypotheses};
}
} // namespace gpu_tracker
