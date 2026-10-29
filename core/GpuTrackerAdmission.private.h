#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <stdexcept>

namespace gpu_tracker {
// Fine-bearing evidence requires a primary lobe. Secondary and tertiary lobes
// are optional (zero) but, when present, must be inside the configured bank.
inline constexpr const char* fineSpatialInvalidGlsl=
 "a==0u||a>post[83]||b>post[83]||c>post[83]";
inline bool validFineSpatialEvidence(uint32_t spatial,uint32_t fineBeams){
 const uint32_t primary=spatial&127u,secondary=(spatial>>7u)&127u,tertiary=(spatial>>14u)&127u;
 return primary>0&&primary<=fineBeams&&secondary<=fineBeams&&tertiary<=fineBeams;
}
struct InitiationAdmission {
 uint32_t unmatched=0,admittedDetections=0,droppedDetections=0,initiatedTracks=0,totalTracks=0;
};

struct StationaryRetirementPolicy {
 uint32_t samples=0;
 uint32_t minimumAssociated=0;
 double maxDelaySpan=0;
 double maxDopplerSpanHz=0;
};

inline StationaryRetirementPolicy stationaryRetirementPolicy(bool enabled,
 double cpiSeconds){
 if(!enabled)return {};
 if(!std::isfinite(cpiSeconds)||cpiSeconds<=0)
  throw std::invalid_argument("GPU tracker stationary-retirement policy invalid");
 // The public trail always retains 100 samples. Requiring 90 associated
 // samples across that entire window, one delay unit of spread and at most
 // one-and-a-half Doppler cells rejects stable site clutter without treating
 // an aircraft that merely crosses a learned cell as stationary.
 return {100,90,1.0,1.5/cpiSeconds};
}

// Persistence is spatial: delay is the fast axis and Doppler the slow axis.
inline uint32_t persistentCell(uint32_t delayBin,uint32_t dopplerRow,
 uint32_t delayBins){
 if(!delayBins||delayBin>=delayBins||dopplerRow>=(UINT32_MAX-delayBin)/delayBins)
  throw std::invalid_argument("GPU tracker persistent-cell index invalid");
 return dopplerRow*delayBins+delayBin;
}

inline bool persistentRidgeContains(uint32_t learnedCell,uint32_t candidateCell,
 uint32_t delayBins,uint32_t dopplerRows,uint32_t radius=1){
 if(!delayBins||!dopplerRows||learnedCell>=uint64_t(delayBins)*dopplerRows||
    candidateCell>=uint64_t(delayBins)*dopplerRows)
  throw std::invalid_argument("GPU tracker persistent-ridge geometry invalid");
 const uint32_t learnedDelay=learnedCell%delayBins,candidateDelay=candidateCell%delayBins;
 const uint32_t learnedRow=learnedCell/delayBins,candidateRow=candidateCell/delayBins;
 const uint32_t delayDelta=learnedDelay>candidateDelay?
  learnedDelay-candidateDelay:candidateDelay-learnedDelay;
 const uint32_t rowDelta=learnedRow>candidateRow?
  learnedRow-candidateRow:candidateRow-learnedRow;
 return delayDelta<=radius&&rowDelta<=radius;
}

// One credit leaks per four missed CPIs. Recurrence with short CFAR/quantizer
// gaps therefore accumulates, while one-off occupancy is gone before it can
// affect a later independent arrival.
constexpr uint32_t persistentLeakCpis=4;

inline uint32_t persistentEvidence(uint32_t packed,uint16_t frame){
 const uint32_t streak=packed&65535u,last=packed>>16u;
 const uint32_t age=(uint32_t(frame)-last)&65535u;
 const uint32_t missed=age?age-1u:0u;
 const uint32_t leaked=missed/persistentLeakCpis;
 return !packed||leaked>=streak?0u:streak-leaked;
}

inline uint32_t advancePersistentStreak(uint32_t packed,uint16_t frame){
 if(packed&&(packed>>16u)==frame)return packed;
 const uint32_t decayed=persistentEvidence(packed,frame);
 return (uint32_t(frame)<<16u)|std::min(65535u,decayed+1u);
}

inline InitiationAdmission boundInitiations(uint32_t capacity,uint32_t retained,
 uint32_t unmatched,uint32_t hypotheses){
 if(!hypotheses||retained>capacity)throw std::invalid_argument("GPU tracker initiation admission invalid");
 const uint32_t admitted=std::min(unmatched,(capacity-retained)/hypotheses);
 return {unmatched,admitted,unmatched-admitted,admitted*hypotheses,
  retained+admitted*hypotheses};
}
} // namespace gpu_tracker
