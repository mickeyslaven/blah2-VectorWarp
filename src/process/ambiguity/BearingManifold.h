#pragma once
#include <rapidjson/document.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace blah2::bearing_manifold {
struct Calibration {
 double frequencyHz=0;
 uint32_t beams=0,channels=0;
 std::vector<double> actualGainDb;
 std::vector<std::complex<float>> responses;
};
struct Identity {
 std::vector<uint32_t> channelMapping;
 std::vector<double> requestedGainDb;
 std::vector<std::array<double,3>> positionsM;
 std::array<double,3> receiverLocation{},transmitterLocation{};
 double frequencyHz=0,sampleRateHz=0,xAxisBearingDeg=0,directPathAzimuthDeg=0;
};
inline std::string compatibilityFingerprint(const Identity& identity){
 uint64_t hash=1469598103934665603ULL;
 const auto addByte=[&](uint8_t byte){hash^=byte;hash*=1099511628211ULL;};
 const auto add32=[&](uint32_t value){for(unsigned i=0;i<4;++i)addByte(uint8_t(value>>(8*i)));};
 const auto add64=[&](double value){uint64_t bits=0;std::memcpy(&bits,&value,8);
  for(unsigned i=0;i<8;++i)addByte(uint8_t(bits>>(8*i)));};
 constexpr char tag[]="vectorwarp-manifold-identity-v2";
 for(size_t i=0;i<sizeof(tag)-1;++i)addByte(uint8_t(tag[i]));
 add64(identity.frequencyHz);add64(identity.sampleRateHz);
 add32(uint32_t(identity.channelMapping.size()));
 for(auto value:identity.channelMapping)add32(value);
 for(auto value:identity.requestedGainDb)add64(value);
 for(const auto& position:identity.positionsM)for(auto value:position)add64(value);
 for(auto value:identity.receiverLocation)add64(value);
 for(auto value:identity.transmitterLocation)add64(value);
 add64(identity.xAxisBearingDeg);
 char text[25]{};std::snprintf(text,sizeof(text),"fnv1a64:%016llx",
  static_cast<unsigned long long>(hash));return text;
}
inline Calibration load(const std::string& path,double expectedFrequencyHz,
 uint32_t expectedBeams,uint32_t expectedChannels,const Identity& expected){
 if(!std::isfinite(expectedFrequencyHz)||!(expectedFrequencyHz>0)||
    expected.frequencyHz!=expectedFrequencyHz||!std::isfinite(expected.sampleRateHz)||
    !(expected.sampleRateHz>0)||
    !expectedBeams||!expectedChannels||expected.channelMapping.size()!=expectedChannels||
    expected.requestedGainDb.size()!=expectedChannels||expected.positionsM.size()!=expectedChannels||
    !std::isfinite(expected.xAxisBearingDeg)||!std::isfinite(expected.directPathAzimuthDeg)||
    expected.directPathAzimuthDeg<0||expected.directPathAzimuthDeg>=360)
  throw std::invalid_argument("Invalid expected bearing manifold dimensions");
 for(uint32_t channel=0;channel<expectedChannels;++channel){
  if(std::find(expected.channelMapping.begin(),expected.channelMapping.begin()+channel,
       expected.channelMapping[channel])!=expected.channelMapping.begin()+channel||
     !std::isfinite(expected.requestedGainDb[channel])||expected.requestedGainDb[channel]<0||
     expected.requestedGainDb[channel]>50)
   throw std::invalid_argument("Invalid expected manifold gain identity");
  for(double coordinate:expected.positionsM[channel])if(!std::isfinite(coordinate))
   throw std::invalid_argument("Invalid expected manifold geometry identity");
 }
 for(double value:expected.receiverLocation)if(!std::isfinite(value))
  throw std::invalid_argument("Invalid expected manifold receiver identity");
 for(double value:expected.transmitterLocation)if(!std::isfinite(value))
  throw std::invalid_argument("Invalid expected manifold transmitter identity");
 std::ifstream input(path);if(!input)throw std::invalid_argument("Cannot open bearing manifold_file");
 const std::string json((std::istreambuf_iterator<char>(input)),{});rapidjson::Document doc;doc.Parse(json.c_str(),json.size());
 if(doc.HasParseError()||!doc.IsObject()||!doc.HasMember("schema")||!doc["schema"].IsString()||
    std::string(doc["schema"].GetString())!="vectorwarp-bearing-manifold-v2"||
    !doc.HasMember("frequency_hz")||!doc["frequency_hz"].IsNumber()||
    !doc.HasMember("identity")||!doc["identity"].IsObject()||
    !doc.HasMember("responses")||!doc["responses"].IsArray())
  throw std::invalid_argument("Invalid bearing manifold schema");
 Calibration result;result.frequencyHz=doc["frequency_hz"].GetDouble();result.beams=expectedBeams;result.channels=expectedChannels;
 if(!std::isfinite(result.frequencyHz)||result.frequencyHz!=expectedFrequencyHz||
    doc["responses"].Size()!=expectedBeams)throw std::invalid_argument("Bearing manifold frequency or beam count differs");
 const auto& identity=doc["identity"];
 if(!identity.HasMember("frequency_hz")||!identity["frequency_hz"].IsNumber()||
    identity["frequency_hz"].GetDouble()!=expectedFrequencyHz||
    !identity.HasMember("sample_rate_hz")||!identity["sample_rate_hz"].IsNumber()||
    identity["sample_rate_hz"].GetDouble()!=expected.sampleRateHz||
    !identity.HasMember("compatibility_fingerprint")||
    !identity["compatibility_fingerprint"].IsString()||
    std::string(identity["compatibility_fingerprint"].GetString())!=
      compatibilityFingerprint(expected)||
    !identity.HasMember("direct_path_local_azimuth_deg")||
    !identity["direct_path_local_azimuth_deg"].IsNumber()||
    std::abs(identity["direct_path_local_azimuth_deg"].GetDouble()-
      expected.directPathAzimuthDeg)>1e-6||
    !identity.HasMember("direct_path_beam")||!identity["direct_path_beam"].IsUint()||
    identity["direct_path_beam"].GetUint()!=uint32_t(std::llround(
      expected.directPathAzimuthDeg*expectedBeams/360.0))%expectedBeams||
    !identity.HasMember("channel_mapping")||!identity["channel_mapping"].IsArray()||
    !identity.HasMember("requested_gain_db")||!identity["requested_gain_db"].IsArray()||
    !identity.HasMember("actual_gain_db")||!identity["actual_gain_db"].IsArray()||
    !identity.HasMember("positions_m")||!identity["positions_m"].IsArray()||
    !identity.HasMember("receiver_location")||!identity["receiver_location"].IsArray()||
    !identity.HasMember("transmitter_location")||!identity["transmitter_location"].IsArray()||
    identity["receiver_location"].Size()!=3||identity["transmitter_location"].Size()!=3||
    !identity.HasMember("x_axis_bearing_deg_true")||
    !identity["x_axis_bearing_deg_true"].IsNumber()||
    identity["x_axis_bearing_deg_true"].GetDouble()!=expected.xAxisBearingDeg||
    identity["channel_mapping"].Size()!=expectedChannels||
    identity["requested_gain_db"].Size()!=expectedChannels||
    identity["actual_gain_db"].Size()!=expectedChannels||
    identity["positions_m"].Size()!=expectedChannels)
  throw std::invalid_argument("Bearing manifold identity dimensions differ");
 for(uint32_t channel=0;channel<expectedChannels;++channel){
  const auto& mapping=identity["channel_mapping"][channel];
  const auto& requested=identity["requested_gain_db"][channel];
  const auto& actual=identity["actual_gain_db"][channel];
  const auto& position=identity["positions_m"][channel];
  if(!mapping.IsUint()||mapping.GetUint()!=expected.channelMapping[channel]||
     !requested.IsNumber()||requested.GetDouble()!=expected.requestedGainDb[channel]||
     !actual.IsNumber()||!std::isfinite(actual.GetDouble())||
     !position.IsArray()||position.Size()!=3)
   throw std::invalid_argument("Bearing manifold channel or gain identity differs");
  for(uint32_t coordinate=0;coordinate<3;++coordinate)
   if(!position[coordinate].IsNumber()||
      position[coordinate].GetDouble()!=expected.positionsM[channel][coordinate])
    throw std::invalid_argument("Bearing manifold geometry identity differs");
  result.actualGainDb.push_back(actual.GetDouble());
 }
 for(uint32_t coordinate=0;coordinate<3;++coordinate)
  if(!identity["receiver_location"][coordinate].IsNumber()||
     identity["receiver_location"][coordinate].GetDouble()!=expected.receiverLocation[coordinate]||
     !identity["transmitter_location"][coordinate].IsNumber()||
     identity["transmitter_location"][coordinate].GetDouble()!=expected.transmitterLocation[coordinate])
   throw std::invalid_argument("Bearing manifold illuminator anchor identity differs");
 result.responses.reserve(uint64_t(expectedBeams)*expectedChannels);
 for(const auto& row:doc["responses"].GetArray()){
  if(!row.IsArray()||row.Size()!=expectedChannels)throw std::invalid_argument("Bearing manifold channel count differs");
  for(const auto& value:row.GetArray()){
   if(!value.IsArray()||value.Size()!=2||!value[0].IsNumber()||!value[1].IsNumber())
    throw std::invalid_argument("Bearing manifold response must be [real, imaginary]");
   const double re=value[0].GetDouble(),im=value[1].GetDouble(),power=re*re+im*im;
   if(!std::isfinite(re)||!std::isfinite(im)||!(power>1e-12))throw std::invalid_argument("Bearing manifold response is nonfinite or zero");
   const std::complex<float> response{float(re),float(im)};
   if(!std::isfinite(response.real())||!std::isfinite(response.imag())||
      !(std::norm(response)>1e-12f))
    throw std::invalid_argument("Bearing manifold response is outside the resident numeric range");
   result.responses.push_back(response);
  }
 }
 return result;
}
inline void validateActualGains(const Calibration& calibration,
 const std::vector<double>& actualGainDb){
 if(actualGainDb.size()!=calibration.channels||
    calibration.actualGainDb.size()!=calibration.channels)
  throw std::invalid_argument("Bearing manifold actual gain count differs");
 for(uint32_t channel=0;channel<calibration.channels;++channel)
  if(!std::isfinite(actualGainDb[channel])||
     std::abs(actualGainDb[channel]-calibration.actualGainDb[channel])>1e-4)
   throw std::invalid_argument("Bearing manifold actual gain identity differs");
}
}
