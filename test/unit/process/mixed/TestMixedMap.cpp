#include "process/mixed/MixedMap.h"
#include <cassert>
#include <complex>
#include <limits>
#include <vector>
int main(){
  using namespace blah2::mixed;
  Map<Complex> cpu(rows,delays);
  std::vector<Complex> candidate(mapSamples);
  for(uint32_t row=0;row<rows;++row){
    std::vector<Complex> values(delays);
    for(uint32_t col=0;col<delays;++col){
      const auto value=Complex(double(row+1),double(col+1));
      values[col]=value;candidate[uint64_t(row)*delays+col]=value;
    }
    cpu.set_row(row,values);
  }
  auto error=compareMap(candidate,cpu);
  assert(error.rms==0&&error.peak==0);
  candidate[mapSamples-1]+=Complex(.01,0);
  error=compareMap(candidate,cpu);
  assert(error.rms>0&&error.peak>0);
  commitMap(candidate,cpu);
  assert(cpu.data.back().back()==candidate.back());
  candidate[123]={std::numeric_limits<double>::quiet_NaN(),0};
  bool rejected=false;
  try{(void)compareMap(candidate,cpu);}catch(...){rejected=true;}
  assert(rejected);
}
