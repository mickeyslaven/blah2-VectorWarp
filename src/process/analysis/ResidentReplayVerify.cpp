// Bounded offline contract verifier. Does not load a GPU module or run DSP.
#include "ResidentReplaySnapshot.h"
#include "process/ambiguity/GpuResidentHash.h"
#include <fstream>
#include <iomanip>
#include <iostream>
#include <stdexcept>

int main(int argc,char** argv){try{
 if(argc<2||argc>129)throw std::invalid_argument("Expected 1..128 native snapshot paths");
 for(int i=1;i<argc;++i){std::ifstream input(argv[i],std::ios::binary|std::ios::ate);
  const auto size=input.tellg();
  if(!input||size<2048||uint64_t(size)>blah2::resident_replay::byteLimit)
   throw std::invalid_argument("Snapshot missing or outside byte bound");
  std::vector<uint8_t> data(static_cast<size_t>(size));input.seekg(0);
  if(!input.read(reinterpret_cast<char*>(data.data()),size))throw std::runtime_error("Snapshot read failed");
  blah2::resident_replay::decodeSelfContained(data.data(),data.size());
  blah2::resident::Sha256 hash;hash.add(data.data(),data.size());
  for(auto byte:hash.finish())std::cout<<std::hex<<std::setw(2)<<std::setfill('0')<<unsigned(byte);
  std::cout<<'\n';
 }
 return 0;
 }catch(const std::exception& e){std::cerr<<"Native replay verification failed: "<<e.what()<<'\n';return 1;}}
