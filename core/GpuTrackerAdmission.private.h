#pragma once
#include <algorithm>
#include <cstdint>
#include <stdexcept>

namespace gpu_tracker {
struct InitiationAdmission {
 uint32_t unmatched=0,admittedDetections=0,droppedDetections=0,initiatedTracks=0,totalTracks=0;
};

inline InitiationAdmission boundInitiations(uint32_t capacity,uint32_t retained,
 uint32_t unmatched,uint32_t hypotheses){
 if(!hypotheses||retained>capacity)throw std::invalid_argument("GPU tracker initiation admission invalid");
 const uint32_t admitted=std::min(unmatched,(capacity-retained)/hypotheses);
 return {unmatched,admitted,unmatched-admitted,admitted*hypotheses,
  retained+admitted*hypotheses};
}
} // namespace gpu_tracker
