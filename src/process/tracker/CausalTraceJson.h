#pragma once
#include "CausalTraceLayout.h"
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>

namespace blah2::causal_trace {
struct Capture {bool available=false;uint64_t epoch=0,first=0,end=0;};
inline uint64_t pair(const uint32_t* p){return uint64_t(p[0])|(uint64_t(p[1])<<32);}
inline double value(const uint32_t* p){double d;std::memcpy(&d,p,8);return d;}
inline void require(bool ok){if(!ok)throw std::invalid_argument("Invalid causal trace");}
inline std::string hex(const uint8_t* bytes,size_t count){std::string out;out.reserve(count*2);
 for(size_t i=0;i<count;++i){out.push_back("0123456789abcdef"[bytes[i]>>4]);out.push_back("0123456789abcdef"[bytes[i]&15]);}return out;}
inline std::string json(const uint32_t* data,size_t size,double rangeKm,
 uint64_t sequence,uint32_t maximum,Capture capture={},bool reset=false,
 uint64_t dropped=0,const std::string& configHash={},const std::string& moduleHash={}){
 require(data&&size==words&&data[0]==schema&&data[3]<=1&&maximum&&maximum<=64&&
  std::isfinite(rangeKm)&&rangeKm>0&&pair(data+8)<=UINT64_MAX/1000000&&
  (!capture.available||capture.end>=capture.first));
 const auto generation=pair(data+4);
 const uint32_t nc=reset?0:std::min({data[1],prefix,(maximum+1)/2});
 const uint32_t nt=reset?0:std::min({data[2],prefix,maximum-nc});
 rapidjson::StringBuffer b;rapidjson::Writer<rapidjson::StringBuffer>w(b);
 auto exact=[&](const char* key,uint64_t n){w.Key(key);const auto s=std::to_string(n);w.String(s.c_str());};
 auto number=[&](const char* key,double n){require(std::isfinite(n));w.Key(key);w.Double(n);};
 w.StartObject();w.Key("schema");w.String("vectorwarp-causal-funnel-frame-v2");
 w.Key("source");w.String("native");exact("event_time_ns",pair(data+8)*1000000);
 exact("sequence",sequence);exact("transport_dropped",dropped);
 if(!configHash.empty()||!moduleHash.empty()){
  const auto valid=[](const std::string& s){return s.empty()||(s.size()==64&&s.find_first_not_of("0123456789abcdef")==std::string::npos);};
  require(valid(configHash)&&valid(moduleHash));w.Key("producer");w.StartObject();
  if(!configHash.empty()){w.Key("config_fingerprint");w.String(configHash.c_str());}
  if(!moduleHash.empty()){w.Key("build_id");w.String(moduleHash.c_str());}w.EndObject();}
 w.Key("stage");w.String(reset?"input":"tracker_frame");w.Key("reason");
 w.String(reset?"reset_input":(data[3]?"duplicate":(nc+nt?"interval":"no_candidates")));
 w.Key("lineage");w.StartObject();exact("epoch",pair(data+10));exact("generation",generation);
 exact("consumed_sequence",pair(data+6));exact("clock_precision_ns",1000000);
 if(capture.available){exact("input_epoch",capture.epoch);exact("first_sample",capture.first);exact("end_sample",capture.end);}
 w.EndObject();w.Key("coverage");w.StartObject();
 w.Key("candidates_total");w.Uint(reset?0:data[1]);w.Key("tracks_total");w.Uint(reset?0:data[2]);
 w.Key("candidates_retained");w.Uint(nc);w.Key("tracks_retained");w.Uint(nt);
 w.Key("truncated");w.Bool(!reset&&(nc<data[1]||nt<data[2]));w.EndObject();
 const char* reasons[]={"invalid","associated","minimum_snr","doppler_comb","range_comb",
  "persistent_occupancy","boundary","capacity","seeded","miss","tentative_expired",
  "inactivity_deleted","confirmed","retained"};
 auto candidateId=[&](uint32_t i){return "c"+std::to_string(generation)+"-"+std::to_string(i);};
 w.Key("candidates");w.StartArray();
 for(uint32_t i=0;i<nc+nt;++i){const bool track=i>=nc;
  const auto* r=data+headerWords+recordWords*(track?prefix+i-nc:i);
  require(r[1]>0&&r[1]<14&&r[2]<4&&r[3]<4);
  const uint32_t depth=r[5]>>8;require(depth==0||depth==1||depth==2||depth==4||depth==8);
  const std::string id=track?"t"+std::to_string(r[0]):candidateId(r[0]);
  w.StartObject();w.Key("id");w.String(id.c_str());w.Key("kind");w.String(track?"track":"candidate");
  w.Key("reason");w.String(reasons[r[1]]);w.Key("state");w.Uint(r[2]);w.Key("previous_state");w.Uint(r[3]);
  number("delay_km",value(r+6)*rangeKm);number("doppler_hz",value(r+8));
  w.Key("snr_db");if((r[5]&1u)!=0u){const double snr=value(r+10);require(std::isfinite(snr));w.Double(snr);}else w.Null();
  w.Key("bearing_candidates_deg");w.StartArray();w.EndArray(); // not fabricated from a beam index
  w.Key("measured");w.Bool((r[5]&1u)!=0u);w.Key("confirmation_credit");
  // Admission alone does not determine an associated track's lineage credit.
  // Seed credit is known; other candidate rows defer to their track record.
  if(track||r[1]==8u)w.Bool((r[5]&2u)!=0u);else w.Null();
  w.Key("support_depth");w.Uint(depth);
  if(!track&&r[1]==8u){require(r[26]<65535u&&r[27]>0u);const auto first="t"+std::to_string(r[26]);
   w.Key("spawned_first_track_id");w.String(first.c_str());
   w.Key("spawned_track_count");w.Uint(r[27]);} // IDs advance modulo65535.
  if(track){const bool gate=(r[5]&4u)!=0u,near=(r[5]&8u)!=0u;
   w.Key("candidate_id");if(near){const auto id=candidateId(r[25]);w.String(id.c_str());}else w.Null();
   w.Key("gate_rejection_mask");w.Uint(r[24]);
   w.Key("residual_search_truncated");w.Bool(r[4]==UINT32_MAX&&data[1]>prefix);
   const char* keys[]={"predicted_delay_km","predicted_doppler_hz","delay_residual_km",
    "doppler_residual_hz","delay_gate_km","doppler_gate_hz"};
   for(uint32_t k=0;k<6;++k){w.Key(keys[k]);if(gate&&((k!=2&&k!=3)||near)){
     const double n=value(r+12+2*k)*(k%2==0?rangeKm:1);require(std::isfinite(n));w.Double(n);
    }else w.Null();}}
  w.EndObject();}
 w.EndArray();w.EndObject();require(b.GetSize()<65535);return {b.GetString(),b.GetSize()};
}
}
