#pragma once
#include "MixedProtocol.h"
#include "data/Map.h"
#include <algorithm>
#include <cmath>
#include <complex>
#include <stdexcept>
#include <utility>
#include <vector>
namespace blah2::mixed {
using Complex=std::complex<double>;
struct Error { double rms=0,peak=0; };
inline Error compareMap(const std::vector<Complex>& candidate,const Map<Complex>& cpu){
  if(candidate.size()!=mapSamples||cpu.data.size()!=rows)
    throw std::invalid_argument("Mixed map shape changed");
  double error=0,signal=0,maxError=0,maxSignal=0;
  for(uint32_t row=0;row<rows;++row){
    if(cpu.data[row].size()!=delays)throw std::invalid_argument("CPU map columns changed");
    for(uint32_t col=0;col<delays;++col){
      const auto& actual=candidate[uint64_t(row)*delays+col];
      const auto& expected=cpu.data[row][col];
      if(!std::isfinite(actual.real())||!std::isfinite(actual.imag())||
         !std::isfinite(expected.real())||!std::isfinite(expected.imag()))
        throw std::runtime_error("Non-finite mixed map comparison");
      const double delta=std::norm(actual-expected);
      error+=delta;signal+=std::norm(expected);
      maxError=std::max(maxError,std::abs(actual-expected));
      maxSignal=std::max(maxSignal,std::abs(expected));
    }
  }
  return {std::sqrt(error/std::max(signal,1e-30)),maxError/std::max(maxSignal,1e-30)};
}
inline void commitMap(std::vector<Complex> candidate,Map<Complex>& destination){
  if(candidate.size()!=mapSamples||destination.data.size()!=rows)
    throw std::invalid_argument("Mixed map shape changed");
  std::vector<std::vector<Complex>> complete(rows);
  for(uint32_t row=0;row<rows;++row){
    if(destination.data[row].size()!=delays)throw std::invalid_argument("Mixed map columns changed");
    auto first=candidate.begin()+uint64_t(row)*delays;
    complete[row].assign(first,first+delays);
  }
  destination.data.swap(complete);
}
}
