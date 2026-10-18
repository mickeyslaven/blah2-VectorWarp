#pragma once
#include <rapidjson/document.h>
#include <cmath>
#include <complex>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace blah2::bearing_manifold {
struct Calibration {
 double frequencyHz=0;
 uint32_t beams=0,channels=0;
 std::vector<std::complex<float>> responses;
};
inline Calibration load(const std::string& path,double expectedFrequencyHz,
 uint32_t expectedBeams,uint32_t expectedChannels){
 std::ifstream input(path);if(!input)throw std::invalid_argument("Cannot open bearing manifold_file");
 const std::string json((std::istreambuf_iterator<char>(input)),{});rapidjson::Document doc;doc.Parse(json.c_str(),json.size());
 if(doc.HasParseError()||!doc.IsObject()||!doc.HasMember("schema")||!doc["schema"].IsString()||
    std::string(doc["schema"].GetString())!="vectorwarp-bearing-manifold-v1"||
    !doc.HasMember("frequency_hz")||!doc["frequency_hz"].IsNumber()||
    !doc.HasMember("responses")||!doc["responses"].IsArray())
  throw std::invalid_argument("Invalid bearing manifold schema");
 Calibration result;result.frequencyHz=doc["frequency_hz"].GetDouble();result.beams=expectedBeams;result.channels=expectedChannels;
 if(!std::isfinite(result.frequencyHz)||std::abs(result.frequencyHz-expectedFrequencyHz)>std::max(1.0,expectedFrequencyHz*1e-6)||
    doc["responses"].Size()!=expectedBeams)throw std::invalid_argument("Bearing manifold frequency or beam count differs");
 result.responses.reserve(uint64_t(expectedBeams)*expectedChannels);
 for(const auto& row:doc["responses"].GetArray()){
  if(!row.IsArray()||row.Size()!=expectedChannels)throw std::invalid_argument("Bearing manifold channel count differs");
  for(const auto& value:row.GetArray()){
   if(!value.IsArray()||value.Size()!=2||!value[0].IsNumber()||!value[1].IsNumber())
    throw std::invalid_argument("Bearing manifold response must be [real, imaginary]");
   const double re=value[0].GetDouble(),im=value[1].GetDouble(),power=re*re+im*im;
   if(!std::isfinite(re)||!std::isfinite(im)||!(power>1e-12))throw std::invalid_argument("Bearing manifold response is nonfinite or zero");
   result.responses.emplace_back(float(re),float(im));
  }
 }
 return result;
}
}
