// Offline-only bounded adapter around the exact portable numerical contract.
// No detector, tracking, truth, configuration tuning, or service connection.
#include "process/detection/PostClutterBootstrap.h"
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <iostream>
#include <string>
int main()try{
 using Bootstrap=blah2::detection::PostClutterBootstrap;
 Bootstrap diagonal;Bootstrap::Config cfg;cfg.covariance=Bootstrap::Covariance::shrinkage;
 const Bootstrap shrinkage(cfg);constexpr size_t lineCapacity=8*1024*1024+2;
 const auto line=std::make_unique<char[]>(lineCapacity);unsigned frames=0;
 while(std::cin.getline(line.get(),lineCapacity)){
  if(++frames>32)throw std::invalid_argument("cutout bounds");
  rapidjson::Document d;d.Parse(line.get());
  if(d.HasParseError()||!d.IsObject()||!d.HasMember("channels")||!d["channels"].IsArray()||
     !d.HasMember("doppler_hz")||!d["doppler_hz"].IsArray()||!d.HasMember("delay_km")||!d["delay_km"].IsArray()||
     !d.HasMember("frame")||!d["frame"].IsUint())throw std::invalid_argument("cutout schema");
  const unsigned channels=d["channels"].Size(),rows=d["doppler_hz"].Size(),columns=d["delay_km"].Size();
  if(!channels||channels>8||!rows||rows>129||!columns||columns>129)throw std::invalid_argument("cutout geometry");
  std::vector<std::unique_ptr<Map<std::complex<double>>>> owned;
  std::vector<Map<std::complex<double>>*> maps;
  for(unsigned c=0;c<channels;++c){auto map=std::make_unique<Map<std::complex<double>>>(rows,columns);
   if(!d["channels"][c].IsArray()||d["channels"][c].Size()!=rows)throw std::invalid_argument("cutout rows");
   for(unsigned r=0;r<rows;++r){
    if(!d["doppler_hz"][r].IsNumber())throw std::invalid_argument("cutout Doppler");
    map->doppler.push_back(d["doppler_hz"][r].GetDouble());
    if(!d["channels"][c][r].IsArray()||d["channels"][c][r].Size()!=columns)throw std::invalid_argument("cutout columns");
    for(unsigned col=0;col<columns;++col){const auto& z=d["channels"][c][r][col];
     if(!z.IsArray()||z.Size()!=2||!z[0].IsNumber()||!z[1].IsNumber())throw std::invalid_argument("cutout complex");
     const std::complex<double> value{z[0].GetDouble(),z[1].GetDouble()};
     if(!std::isfinite(value.real())||!std::isfinite(value.imag()))throw std::invalid_argument("cutout finite");
     map->data[r][col]=value;}}
   for(unsigned col=0;col<columns;++col)map->delay.push_back(int(col));
   maps.push_back(map.get());owned.push_back(std::move(map));}
  rapidjson::StringBuffer buffer;rapidjson::Writer<rapidjson::StringBuffer>w(buffer);
  w.StartObject();w.Key("frame");w.Uint(d["frame"].GetUint());
  for(bool full:{false,true}){const auto result=(full?shrinkage:diagonal).process(maps);
   w.Key(full?"shrinkage075":"diagonal");w.StartObject();
   w.Key("trained");w.Uint64(result.trainedCells);w.Key("fallback");w.Uint64(result.fallbackCells);
   w.Key("invalid");w.Uint64(result.invalidCells);w.Key("power");w.StartArray();
   for(const auto& row:result.map->data){w.StartArray();for(const auto z:row)w.Double(std::norm(z));w.EndArray();}
   w.EndArray();w.EndObject();}
  w.EndObject();std::cout<<buffer.GetString()<<'\n';
 }
 if(!std::cin.eof())throw std::invalid_argument("cutout byte bound");
 if(!frames)throw std::invalid_argument("empty cutout");
 return 0;
}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}
