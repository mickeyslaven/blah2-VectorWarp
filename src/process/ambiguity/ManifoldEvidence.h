#pragma once
#include "data/Detection.h"
#include "data/Map.h"
#include "GpuResultJson.private.h"
#include <algorithm>
#include <complex>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace blah2::manifold_evidence {
inline std::string cpu(uint64_t timestamp,double frequencyHz,double sampleRate,
 Detection& detection,const std::vector<Map<std::complex<double>>*>& maps,
 const std::vector<std::complex<double>>& referenceWeights){
 using namespace gpu_result_json;
 require(std::isfinite(frequencyHz)&&frequencyHz>0&&std::isfinite(sampleRate)&&sampleRate>0&&
  maps.size()>=2&&maps.size()<=8&&referenceWeights.size()==maps.size(),
  "Invalid CPU manifold evidence geometry");
 const auto delays=detection.get_delay(),dopplers=detection.get_doppler(),snr=detection.get_snr();
 require(delays.size()==dopplers.size()&&delays.size()==snr.size(),
  "CPU manifold evidence detections differ");
 auto* first=maps.front();require(first&&first->get_nRows()&&first->get_nCols()&&
  first->doppler.size()==first->get_nRows()&&first->delay.size()==first->get_nCols(),
  "CPU manifold evidence map geometry differs");
 for(auto* map:maps)require(map&&map->get_nRows()==first->get_nRows()&&
  map->get_nCols()==first->get_nCols(),"CPU manifold evidence channel maps differ");
 rapidjson::StringBuffer b;Writer w(b);w.SetMaxDecimalPlaces(8);
 w.StartObject();w.Key("schema");w.String("vectorwarp-manifold-evidence-v1");
 w.Key("timestamp");w.Uint64(timestamp);w.Key("frequency_hz");w.Double(frequencyHz);
 w.Key("sample_rate_hz");w.Double(sampleRate);w.Key("channels");w.Uint(uint32_t(maps.size()));
 w.Key("reference_weights");w.StartArray();for(const auto value:referenceWeights){
  finite(value.real());finite(value.imag());w.StartArray();w.Double(value.real());w.Double(value.imag());w.EndArray();}
 w.EndArray();w.Key("data");w.StartArray();
 for(size_t i=0;i<delays.size();++i){finite(delays[i]);finite(dopplers[i]);finite(snr[i]);
  uint32_t column=0,row=0;double best=std::numeric_limits<double>::infinity();
  for(uint32_t candidate=0;candidate<first->delay.size();++candidate){const double error=std::abs(double(first->delay[candidate])-delays[i]);
   if(error<best){best=error;column=candidate;}}
  best=std::numeric_limits<double>::infinity();for(uint32_t candidate=0;candidate<first->doppler.size();++candidate){
   const double error=std::abs(first->doppler[candidate]-dopplers[i]);if(error<best){best=error;row=candidate;}}
  w.StartObject();w.Key("cell");w.Uint(row*first->get_nCols()+column);
  w.Key("delay_km");w.Double(delays[i]*(299792458.0/sampleRate)/1000.0);
  w.Key("doppler_hz");w.Double(dopplers[i]);w.Key("snr_db");w.Double(snr[i]);
  w.Key("response");w.StartArray();for(auto* map:maps){const auto value=map->data[row][column];
   finite(value.real());finite(value.imag());w.StartArray();w.Double(value.real());w.Double(value.imag());w.EndArray();}
  w.EndArray();w.EndObject();}
 w.EndArray();w.EndObject();return {b.GetString(),b.GetSize()};
}
}
