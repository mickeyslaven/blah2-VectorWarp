#pragma once
// Canonical confirmation arithmetic, compiled by Tracker and resident shaders.
// N counts source-CPI endpoints (including the seed), never overlapping hits.
#include <cstdint>
#include <stdexcept>
#include <string>

#define BLAH2_CONFIRMATION_FUNCTIONS(Q) \
 Q bool confirmationDepthValid(uint depth) { \
   return depth==1u || depth==2u || depth==4u || depth==8u; } \
 Q bool confirmationDisjoint(uint depth,uint recentCredits) { \
   return confirmationDepthValid(depth) && \
     (recentCredits & ((1u << (depth-1u))-1u))==0u; } \
 Q uint confirmationMinimumWindow(uint minimum,uint depth) { \
   return 1u+depth*(minimum-1u); } \
 Q bool confirmationFeasible(uint minimum,uint window,uint depth) { \
   return confirmationDepthValid(depth) && minimum>0u && minimum<=window && \
     window<=255u && window>=confirmationMinimumWindow(minimum,depth); } \
 Q bool confirmationCoastFeasible(uint misses,uint depth) { \
   return confirmationDepthValid(depth) && misses>=depth-1u; }

namespace tracker_confirmation {
using uint=uint32_t;
BLAH2_CONFIRMATION_FUNCTIONS(inline constexpr)
#define BLAH2_CONFIRMATION_STRINGIFY_IMPL(...) #__VA_ARGS__
#define BLAH2_CONFIRMATION_STRINGIFY(...) BLAH2_CONFIRMATION_STRINGIFY_IMPL(__VA_ARGS__)
inline constexpr const char* shaderSource =
  BLAH2_CONFIRMATION_STRINGIFY(BLAH2_CONFIRMATION_FUNCTIONS());
#undef BLAH2_CONFIRMATION_STRINGIFY
#undef BLAH2_CONFIRMATION_STRINGIFY_IMPL
inline void validate(uint minimum,uint window,uint misses,uint depth) {
  if (!confirmationFeasible(minimum,window,depth) ||
      !confirmationCoastFeasible(misses,depth))
    throw std::invalid_argument("Disjoint temporal confirmation requires N >= 1 + depth*(M-1) ("+
      std::to_string(confirmationDepthValid(depth)&&minimum?confirmationMinimumWindow(minimum,depth):0)+
      ") and deletion misses >= depth-1 ("+std::to_string(depth?depth-1:0)+
      "); explicit replay qualification is required before changing these limits");
}
}
#undef BLAH2_CONFIRMATION_FUNCTIONS
