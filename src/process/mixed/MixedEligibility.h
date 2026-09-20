#pragma once
#include "MixedProtocol.h"
#include <cstdint>
#include <fstream>
#include <string_view>
namespace blah2::mixed {
struct Shape {
  uint32_t sampleRate=0,samplesPerCpi=0,paths=0,dopplers=0,delayBins=0;
  uint32_t correlation=0,rangeFft=0,clutterBins=0;
  int32_t clutterMin=0,ambiguityMin=0,ambiguityMax=0;
  int32_t dopplerMin=0,dopplerMax=0;
  double dopplerMiddle=0;
  bool arrayReference=false,clutterEnabled=false;
};
inline bool qualified(const Shape& g){
  return !g.arrayReference&&g.clutterEnabled&&g.sampleRate==2000000&&
    g.samplesPerCpi==samples&&g.paths==1&&g.dopplers==rows&&
    g.delayBins==delays&&g.correlation==3322&&g.rangeFft==4096&&
    g.clutterBins==410&&g.clutterMin==-10&&g.ambiguityMin==-10&&
    g.ambiguityMax==400&&g.dopplerMin==-300&&g.dopplerMax==300&&
    g.dopplerMiddle==0;
}
inline bool pi4Host(){
#if defined(__linux__)
  // Device-tree identity needs no GPU initialization. Pi 5 and non-Pi hosts
  // retain the existing generic AUTO policy until independently qualified.
  std::ifstream identity("/proc/device-tree/compatible",std::ios::binary);
  char bytes[4096]{};
  identity.read(bytes,sizeof(bytes));
  const std::string_view compatible(bytes,static_cast<size_t>(identity.gcount()));
  return compatible.find("raspberrypi,4-model-b")!=std::string_view::npos &&
    compatible.find("brcm,bcm2711")!=std::string_view::npos;
#else
  return false;
#endif
}
inline bool autoCandidate(std::string_view mode,const Shape& g,bool pi4,
    std::string_view device="auto"){
  return mode=="auto"&&pi4&&(device.empty()||device=="auto")&&qualified(g);
}
}
