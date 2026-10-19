#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace track_guided_shadow {
struct Result {
 bool valid=false,aboveThreshold=false;
 double cut=0,noise=0,threshold=0;
};

inline uint32_t nearestRow(const std::vector<double>& axis,double value){
 if(axis.empty()||!std::isfinite(value))throw std::invalid_argument("Track-guided Doppler axis invalid");
 const auto right=std::lower_bound(axis.begin(),axis.end(),value);
 if(right==axis.begin())return 0;
 if(right==axis.end())return uint32_t(axis.size()-1);
 const auto r=uint32_t(right-axis.begin()),l=r-1;
 return std::abs(value-axis[l])<=std::abs(axis[r]-value)?l:r;
}

inline Result evaluate(const std::vector<double>& current,const std::vector<double>& prior,
 uint32_t rows,uint32_t columns,int32_t firstDelay,const std::vector<double>& doppler,
 double currentDelay,double currentDoppler,double priorDelay,double priorDoppler,
 uint32_t guard,uint32_t train,double alpha){
 if(!rows||!columns||current.size()!=uint64_t(rows)*columns||prior.size()!=current.size()||
    doppler.size()!=rows||!train||guard>127||train>127||!std::isfinite(alpha)||alpha<0)
  throw std::invalid_argument("Track-guided shadow dimensions invalid");
 const int cc=int(std::floor(currentDelay-double(firstDelay)+.5));
 const int pc=int(std::floor(priorDelay-double(firstDelay)+.5));
 if(cc<0||pc<0||cc>=int(columns)||pc>=int(columns))return {};
 const uint32_t cr=nearestRow(doppler,currentDoppler),pr=nearestRow(doppler,priorDoppler);
 const int leftBegin=std::max(0,cc-int(guard)-int(train));
 const int leftEnd=std::max(0,cc-int(guard));
 const int rightBegin=std::min(int(columns),cc+int(guard)+1);
 const int rightEnd=std::min(int(columns),cc+int(guard)+int(train)+1);
 const uint32_t expected=uint32_t(leftEnd-leftBegin+rightEnd-rightBegin);
 const int shift=pc-cc;double sum=0;uint32_t seen=0;
 auto add=[&](int begin,int end){for(int j=begin;j<end;++j){const int old=j+shift;
   if(old<0||old>=int(columns))continue;
   const double a=current[uint64_t(cr)*columns+uint32_t(j)],b=prior[uint64_t(pr)*columns+uint32_t(old)];
   if(std::isfinite(a)&&std::isfinite(b)&&a>=0&&b>=0){sum+=.5*(a+b);++seen;}}};
 add(leftBegin,leftEnd);add(rightBegin,rightEnd);
 const double a=current[uint64_t(cr)*columns+uint32_t(cc)],b=prior[uint64_t(pr)*columns+uint32_t(pc)];
 if(seen!=expected||!expected||!std::isfinite(a)||!std::isfinite(b)||a<0||b<0)return {};
 Result out;out.valid=true;out.cut=.5*(a+b);out.noise=sum/double(expected);out.threshold=alpha*out.noise;
 if(!std::isfinite(out.cut)||!std::isfinite(out.noise)||!std::isfinite(out.threshold))return {};
 out.aboveThreshold=out.cut>out.threshold;return out;
}
} // namespace track_guided_shadow
