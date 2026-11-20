#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include "process/tracker/ConfirmationContract.h"
#include "process/tracker/AssociationContract.h"

namespace gpu_tracker {
inline constexpr uint32_t persistenceHeaderWords=12,persistenceCellWords=3;
inline uint64_t persistenceStateWords(uint32_t cells){return 1u+uint64_t(cells)*persistenceCellWords;}
inline uint64_t persistenceBytes(bool enabled,uint32_t cells){
 // Immutable policy plus two transactional occupied-count/cell banks. The
 // candidate is copied from committed state, then promoted with the track bank.
 return (persistenceHeaderWords+(enabled?2u*persistenceStateWords(cells):0u))*sizeof(uint32_t);
}
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

inline bool validQuarantineCoverage(uint32_t associated,uint32_t quarantined){
 // Quarantine resets publication state after a successful association; it no
 // longer removes the retained tracker record.
 return quarantined<=associated;
}

inline StationaryRetirementPolicy stationaryRetirementPolicy(bool enabled,
 double cpiSeconds,uint32_t persistenceCpis){
 if(!enabled)return {};
 if(!std::isfinite(cpiSeconds)||cpiSeconds<=0||persistenceCpis<2)
  throw std::invalid_argument("GPU tracker stationary-retirement policy invalid");
 // A learned hot cell must not delete tracker state: a weak moving target may
 // temporarily overlap site clutter. Quarantine publication after the same
 // bounded evidence interval used by the birth map, while retaining the track
 // so it can recover after leaving the cell. Require at least 75% associated
 // samples, one delay unit of spread and at most one-and-a-half Doppler cells.
 return {persistenceCpis,tracker_quarantine::quarantineMinimumAssociated(persistenceCpis),
  1.0,1.5/cpiSeconds};
}

// Persistence is spatial: delay is the fast axis and Doppler the slow axis.
inline uint32_t persistentCell(uint32_t delayBin,uint32_t dopplerRow,
 uint32_t delayBins){
 if(!delayBins||delayBin>=delayBins||dopplerRow>=(UINT32_MAX-delayBin)/delayBins)
  throw std::invalid_argument("GPU tracker persistent-cell index invalid");
 return dopplerRow*delayBins+delayBin;
}

inline bool persistentRidgeContains(uint32_t learnedCell,uint32_t candidateCell,
 uint32_t delayBins,uint32_t dopplerRows,uint32_t delayRadius=4,
 uint32_t dopplerRadius=1){
 if(!delayBins||!dopplerRows||learnedCell>=uint64_t(delayBins)*dopplerRows||
    candidateCell>=uint64_t(delayBins)*dopplerRows)
  throw std::invalid_argument("GPU tracker persistent-ridge geometry invalid");
 const uint32_t learnedDelay=learnedCell%delayBins,candidateDelay=candidateCell%delayBins;
 const uint32_t learnedRow=learnedCell/delayBins,candidateRow=candidateCell/delayBins;
 const uint32_t delayDelta=learnedDelay>candidateDelay?
  learnedDelay-candidateDelay:candidateDelay-learnedDelay;
 const uint32_t rowDelta=learnedRow>candidateRow?
  learnedRow-candidateRow:candidateRow-learnedRow;
 return delayDelta<=delayRadius&&rowDelta<=dopplerRadius;
}


inline InitiationAdmission boundInitiations(uint32_t capacity,uint32_t retained,
 uint32_t unmatched,uint32_t hypotheses){
 if(!hypotheses||retained>capacity)throw std::invalid_argument("GPU tracker initiation admission invalid");
 const uint32_t admitted=std::min(unmatched,(capacity-retained)/hypotheses);
 return {unmatched,admitted,unmatched-admitted,admitted*hypotheses,
  retained+admitted*hypotheses};
}
} // namespace gpu_tracker
