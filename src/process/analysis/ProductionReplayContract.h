#pragma once

// Shared, side-effect-free portion of the production detector contract.  The
// raw-IQ exporter uses this exact bounded validation before it is permitted to
// serialize a full-bank grid; resident setup uses it too.  Keeping it here
// prevents an offline tool from quietly accepting a wider temporal or trial
// space than the live ABI.
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace blah2::production_replay {
inline double familywisePfa(double configuredPfa,uint32_t beamTrials,uint32_t motionTrials){
 if(!std::isfinite(configuredPfa)||!(configuredPfa>0&&configuredPfa<1)||!beamTrials||!motionTrials)
  throw std::invalid_argument("Production replay trial accounting is invalid");
 const double trials=double(beamTrials)*motionTrials;
 if(!std::isfinite(trials)||trials>1e9)throw std::invalid_argument("Production replay trial count is invalid");
 return configuredPfa/trials;
}
inline void temporal(uint32_t cpis,uint32_t delayMotion,uint32_t dopplerMotion,bool coherent){
 if(!(cpis==1||cpis==2||cpis==4||cpis==8)||delayMotion>2||
    dopplerMotion>(cpis>2?16u:1u)||
    (cpis==1&&(delayMotion||dopplerMotion))||(cpis>2&&!coherent))
  throw std::invalid_argument("Production replay temporal/TBD contract is invalid");
}
inline void covariance(const std::vector<double>& matrix,uint32_t channels){
 if(!channels||channels>8||matrix.size()!=uint64_t(channels)*channels)
  throw std::invalid_argument("Production replay covariance dimensions are invalid");
 for(uint32_t row=0;row<channels;++row)for(uint32_t column=0;column<channels;++column){
  const double value=matrix[uint64_t(row)*channels+column];
  if(!std::isfinite(value)||value<0||(row==column&&!(value>0)))
   throw std::invalid_argument("Production replay covariance is invalid");
 }
}
}
