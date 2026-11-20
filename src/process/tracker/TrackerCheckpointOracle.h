#pragma once
#include "Tracker.h"
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace tracker_qualification {
struct ClockBinding {
  uint64_t cpuEvidenceEpoch=0,residentCohortEpoch=0;
  uint64_t cpuNewestSequence=0,residentNewestSequence=0;
};
// Call only during bounded startup qualification/replay. This compares actual
// shared Tracker state, including unpublished tracks; it is not another tracker.
inline void compareCheckpoint(Tracker& tracker,const uint32_t* words,size_t count,
  const ClockBinding* binding=nullptr) {
  const auto require=[](bool okay,const char* what){if(!okay)throw std::runtime_error(
    std::string("Resident tracker startup state mismatch: ")+what);};
  require(words&&count>=32,"checkpoint header");
  const auto pair=[&](size_t at){require(at+1<count,"checkpoint bounds");
    return uint64_t(words[at])|(uint64_t(words[at+1])<<32);};
  const auto real=[&](size_t at){const auto bits=pair(at);double value;
    std::memcpy(&value,&bits,8);return value;};
  const auto equal=[](double a,double b){return std::isfinite(a)&&std::isfinite(b)&&
    std::abs(a-b)<=1e-8+1e-7*std::max(std::abs(a),std::abs(b));};
  require(words[0]==5&&words[3]>=100&&words[3]<=255&&words[27]<=1,"schema5 lineage");
  if(binding)require(pair(10)==binding->residentCohortEpoch&&
    pair(14)==binding->residentNewestSequence,"resident clock binding");
  const size_t history=words[3],stride=20+8*history;
  const auto expected=tracker.qualificationState();
  require(words[1]==stride*4&&pair(4)==expected.size()&&count==32+stride*expected.size(),"record count/stride");
  for(size_t i=0;i<expected.size();++i){const auto& t=expected[i];const size_t r=32+i*stride;
    require((words[r+12]&65535u)==std::stoul(t.id,nullptr,16),"track identity");
    require(words[r+14]==t.state,"publication state");
    require(equal(real(r),t.delay)&&equal(real(r+2),t.doppler)&&equal(real(r+6),t.acceleration),"kinematics");
    require(pair(r+10)==t.inactive&&pair(r+18+8*history)==t.measurements,"coast/measurement count");
    require(equal(real(r+16+8*history),t.missedSeconds),"elapsed measurement interval");
    require(std::min<uint64_t>(pair(r+8),255)==t.updates,"update count");
    require(bool(words[r+12]&0x80000000u)==t.accelerationEstimated&&
      bool(words[r+12]&0x40000000u)==t.everConfirmed,"estimated/confirmed latch");
    const uint32_t packed=words[r+13],head=packed&255u,length=(packed>>8)&255u;
    require(head<history&&length&&length<=history,"history ring");
    const size_t newest=r+16+8*((head+length-1)%history);
    require(words[newest+7]==t.spatial,"spatial history");
    require(((words[newest+6]&4u)?1u:(words[newest+6]&8u)?2u:0u)==t.source,"measurement provenance");
    require(t.confirmationVotes.size()<=length,"confirmation window length");
    for(size_t j=0;j<t.confirmationVotes.size();++j){const size_t at=r+16+8*((head+length-t.confirmationVotes.size()+j)%history)+6;
      require(bool(words[at]&(words[27]?16u:4u))==bool(t.confirmationVotes[j]),"independent confirmation credit");}
    if(words[27]&&t.lineageKnown){
      require(t.epoch==(binding?binding->cpuEvidenceEpoch:pair(10)),"evidence epoch");
      const uint64_t newest=binding?binding->cpuNewestSequence:pair(14);
      require(newest>=t.highWaterSequence,"evidence sequence clock");
      const uint64_t cpuAge=newest-t.highWaterSequence;
      uint32_t age=0;for(;age<length;++age){const size_t at=r+16+8*((head+length-1-age)%history)+6;
        if(words[at]&16u)break;}
      if(age<length)require(pair(14)>=age&&cpuAge==age,"evidence high-water age");
      else require(cpuAge>=length,"expired evidence high-water age");
    }
  }
}
}
