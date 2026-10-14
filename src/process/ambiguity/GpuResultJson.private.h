#pragma once
// GPU-result views only: validation and legacy JSON formatting; no DSP math.
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <array>
#include <vector>
namespace blah2::gpu_result_json {
template<class T> struct View {const T* data=nullptr;
size_t count=0;};
struct SpectrumView {uint64_t timestamp=0;
View<double> frequency,db;}; // Legacy min/max/mean are immutable0.
struct MapView {uint64_t timestamp=0;
uint32_t rows=0,columns=0;
double noisePower=0,maxPower=0;
View<double> delayKm,doppler,display;};
struct DetectionView {uint64_t timestamp=0;
View<double> delayKm,doppler,snr;};
struct HistoryView {View<double> delay,doppler;
View<uint32_t> state;};
struct PublishedTrack {uint32_t id=0,state=0;
double delay=0,doppler=0,acceleration=0;
uint64_t associated=0;
HistoryView history;};
struct TrackView {uint64_t timestamp=0,total=0,tentative=0,associated=0,active=0,coasting=0;
View<PublishedTrack> published;};
inline void require(bool yes,const char*why){if(!yes)throw std::invalid_argument(why);}
inline void finite(double value){require(std::isfinite(value),"Nonfinite GPU JSON field");}
inline bool ordinary(double value){return value>=-1e9&&value<=1e9;}
template<class T> inline void shape(View<T>v){require(v.count==0||v.data,"Null GPU JSON view");
require(v.count<=std::numeric_limits<rapidjson::SizeType>::max()&&v.count<=SIZE_MAX/sizeof(T),"GPU JSON view length overflow");}
inline void values(View<double>v){shape(v);
for(size_t i=0;i<v.count;++i)finite(v.data[i]);}
inline const char* stateName(uint32_t s){switch(s){case 0:return "TENTATIVE";
case 1:return "ASSOCIATED";
case 2:return "ACTIVE";
case 3:return "COASTING";
default:throw std::invalid_argument("Invalid GPU track state");}}
using Writer=rapidjson::Writer<rapidjson::StringBuffer>;
inline void array(Writer&w,View<double>v){w.StartArray();
for(size_t i=0;i<v.count;++i)w.Double(v.data[i]);
w.EndArray();}
inline std::string spectrum(const SpectrumView&v){values(v.frequency);
values(v.db);
require(v.frequency.count==v.db.count,"GPU spectrum axes shape differs");
rapidjson::StringBuffer b;
Writer w(b);
w.SetMaxDecimalPlaces(2);
w.StartObject();
w.Key("timestamp");
w.Uint64(v.timestamp);
w.Key("min");
w.Double(0);
w.Key("max");
w.Double(0);
w.Key("mean");
w.Double(0);
w.Key("frequency");
array(w,v.frequency);
w.Key("spectrum");
array(w,v.db);
w.EndObject();
return {b.GetString(),b.GetSize()};}
// Preserve the legacy scientific-notation format/parse/format branch. GPU km
// values enter ONLY the second write, exactly where old conversion inserted them.
inline std::string replaceDelay(const std::string&first,View<double>km){rapidjson::Document d;
d.Parse(first.c_str());
require(!d.HasParseError()&&d.IsObject()&&d.HasMember("delay")&&d["delay"].IsArray(),"Internal GPU JSON formatting failed");
d["delay"].Clear();
for(size_t i=0;i<km.count;++i)d["delay"].PushBack(km.data[i],d.GetAllocator());
rapidjson::StringBuffer b;
Writer w(b);
w.SetMaxDecimalPlaces(2);
require(d.Accept(w),"Internal GPU JSON rewrite failed");
return {b.GetString(),b.GetSize()};}
inline std::string map(const MapView&v){require(v.rows&&v.columns,"Empty GPU radar map geometry");
require(size_t(v.rows)<=SIZE_MAX/v.columns,"GPU map geometry overflow");
const size_t n=size_t(v.rows)*v.columns;
values(v.delayKm);
values(v.doppler);
values(v.display);
finite(v.noisePower);
finite(v.maxPower);
require(v.delayKm.count==v.columns&&v.doppler.count==v.rows&&v.display.count==n,"GPU map axes/display shape differs");
bool onePass=ordinary(v.noisePower)&&ordinary(v.maxPower);
for(size_t i=0;i<v.doppler.count;++i)onePass=onePass&&ordinary(v.doppler.data[i]);
require(n<=(SIZE_MAX-1024)/12,"GPU map reserve overflow");
rapidjson::StringBuffer b(nullptr,n*12+1024);
Writer w(b);
w.SetMaxDecimalPlaces(2);
w.StartObject();
w.Key("timestamp");
w.Uint64(v.timestamp);
w.Key("nRows");
w.Uint(v.rows);
w.Key("nCols");
w.Uint(v.columns);
w.Key("noisePower");
w.Double(v.noisePower);
w.Key("maxPower");
w.Double(v.maxPower);
w.Key("delay");
if(onePass)array(w,v.delayKm);
else{w.StartArray();
for(size_t i=0;i<v.delayKm.count;++i)w.Int(0);
w.EndArray();}w.Key("doppler");
array(w,v.doppler);
w.Key("data");
w.StartArray();
for(uint32_t r=0;r<v.rows;++r){w.StartArray();
for(uint32_t c=0;c<v.columns;++c)w.Double(v.display.data[size_t(r)*v.columns+c]);
w.EndArray();}w.EndArray();
w.EndObject();
if(onePass)return {b.GetString(),b.GetSize()};
const std::string first(b.GetString(),b.GetSize());
return replaceDelay(first,v.delayKm);}
inline std::string detections(const DetectionView&v){values(v.delayKm);
values(v.doppler);
values(v.snr);
require(v.delayKm.count==v.doppler.count&&v.delayKm.count==v.snr.count,"GPU detection triple shape differs");
bool onePass=true;
for(size_t i=0;i<v.doppler.count;++i)onePass=onePass&&ordinary(v.doppler.data[i])&&ordinary(v.snr.data[i]);
rapidjson::StringBuffer b;
Writer w(b);
w.SetMaxDecimalPlaces(2);
w.StartObject();
w.Key("timestamp");
w.Uint64(v.timestamp);
w.Key("delay");
if(onePass)array(w,v.delayKm);
else{w.StartArray();
for(size_t i=0;i<v.delayKm.count;++i)w.Double(0);
w.EndArray();}w.Key("doppler");
array(w,v.doppler);
w.Key("snr");
array(w,v.snr);
w.EndObject();
const std::string first(b.GetString(),b.GetSize());
return onePass?first:replaceDelay(first,v.delayKm);}
inline std::string tracks(const TrackView&v){shape(v.published);
require(v.tentative<=v.total&&v.associated<=v.total-v.tentative&&v.active<=v.total-v.tentative-v.associated&&v.coasting==v.total-v.tentative-v.associated-v.active,"GPU track totals differ");
require(v.published.count==v.total-v.tentative,"GPU publication count differs");
 for(size_t i=0;i<v.published.count;++i){const auto&t=v.published.data[i];
 require(t.id<65535&&t.state>=1&&t.state<=3,"GPU published track identity/state invalid");
 finite(t.delay);
 finite(t.doppler);
 finite(t.acceleration);
 values(t.history.delay);
 values(t.history.doppler);
 shape(t.history.state);
 require(t.history.delay.count==t.history.doppler.count&&t.history.delay.count==t.history.state.count&&t.history.delay.count>0&&t.history.delay.count<=100,"GPU selected display history shape differs");
 for(size_t j=0;j<t.history.state.count;++j)stateName(t.history.state.data[j]);}
 rapidjson::StringBuffer b;
 Writer w(b);
 w.SetMaxDecimalPlaces(2);
 w.StartObject();
 w.Key("timestamp");
 w.Uint64(v.timestamp);
 w.Key("n");
 w.Uint64(v.total);
 w.Key("nTentative");
 w.Uint64(v.tentative);
 w.Key("nAssociated");
 w.Uint64(v.associated);
 w.Key("nActive");
 w.Uint64(v.active);
 w.Key("nCoasting");
 w.Uint64(v.coasting);
 w.Key("data");
 w.StartArray();
 constexpr char hex[]="0123456789ABCDEF";
 for(size_t i=0;i<v.published.count;++i){const auto&t=v.published.data[i];
 char id[5]={hex[(t.id>>12)&15],hex[(t.id>>8)&15],hex[(t.id>>4)&15],hex[t.id&15],0};
 w.StartObject();
 w.Key("id");
 w.String(id);
 w.Key("state");
 w.String(stateName(t.state));
 w.Key("delay");
 w.Double(t.delay);
 w.Key("doppler");
 w.Double(t.doppler);
 w.Key("acceleration");
 w.Double(t.acceleration);
 w.Key("n");
 w.Uint64(t.associated);
 w.Key("associated_delay");
 array(w,t.history.delay);
 w.Key("associated_doppler");
 array(w,t.history.doppler);
 w.Key("associated_state");
 w.StartArray();
 for(size_t j=0;j<t.history.state.count;++j)w.String(stateName(t.history.state.data[j]));
 w.EndArray();
 w.EndObject();}w.EndArray();
 w.EndObject();
 return {b.GetString(),b.GetSize()};}

// Accept the actual ABI DetectionRecord AoS directly; copy supplied GPU km
// metadata, never convert the delay-bin field.
template<class Record> inline std::string detectionRecords(uint64_t timestamp,View<Record> v){
 shape(v);
 std::vector<double>km,doppler,snr;
 km.reserve(v.count);
 doppler.reserve(v.count);
 snr.reserve(v.count);
 for(size_t i=0;i<v.count;++i){finite(v.data[i].delay);
 finite(v.data[i].delayKm);
 finite(v.data[i].doppler);
 finite(v.data[i].snr);
 km.push_back(v.data[i].delayKm);
 doppler.push_back(v.data[i].doppler);
 snr.push_back(v.data[i].snr);}
 return detections({timestamp,{km.data(),km.size()},{doppler.data(),doppler.size()},{snr.data(),snr.size()}});
}
struct CheckpointBinding {uint32_t capacity=0;
uint64_t epoch=0,generation=0,consumedSequence=0;};
inline uint64_t wordPair(const uint32_t*w,size_t at){return uint64_t(w[at])|(uint64_t(w[at+1])<<32);}
inline double realPair(const uint32_t*w,size_t at){const uint64_t bits=wordPair(w,at);
double value;
std::memcpy(&value,&bits,sizeof(value));
return value;}
// The bank is an already committed GPU checkpoint. Decode/copy only its valid
// prefix and its GPU-selected publication slices; no tracker transition/ID,
// prediction, count, or display-window computation occurs here.
inline std::string trackerCheckpoint(uint64_t timestamp,View<uint32_t>bank,const CheckpointBinding&binding){
 shape(bank);
 require(bank.count>=32,"Short GPU tracker header");
 const auto*w=bank.data;
 require(binding.capacity&&w[0]==1&&w[1]==8224&&w[2]==binding.capacity&&w[3]==255,"GPU tracker schema/capacity differs");
 const uint64_t total=wordPair(w,4),published=wordPair(w,24);
 require(total<=binding.capacity&&total<=(SIZE_MAX-32)/2056&&bank.count==32+2056*size_t(total),"GPU tracker valid prefix differs");
 require(w[8]<65535&&w[9]==0&&wordPair(w,10)==binding.epoch&&wordPair(w,12)==binding.generation&&wordPair(w,14)==binding.consumedSequence,"GPU tracker checkpoint binding differs");
 for(size_t i=26;i<32;++i)require(w[i]==0,"GPU tracker reserved header nonzero");
 const std::array<uint64_t,4>counts{wordPair(w,16),wordPair(w,18),wordPair(w,20),wordPair(w,22)};
 require(counts[0]<=total&&counts[1]<=total-counts[0]&&counts[2]<=total-counts[0]-counts[1]&&counts[3]==total-counts[0]-counts[1]-counts[2]&&published==total-counts[0],"GPU tracker header totals differ");
 std::vector<PublishedTrack>rows(size_t(published),PublishedTrack{});
 struct Storage {std::vector<double>d,f;
 std::vector<uint32_t>s;};
 std::vector<Storage>history(size_t(published),Storage{});
 std::array<uint64_t,4>observed{};
 size_t ordinal=0;
 for(size_t i=0;i<size_t(total);++i){const size_t base=32+2056*i;
 const uint32_t state=w[base+14],packed=w[base+13],head=packed&255,length=(packed>>8)&255,start=(packed>>16)&255,display=packed>>24;
  require(state<4&&w[base+12]<65535&&head<255&&length&&start<255&&display&&display<=100&&display<=length,"GPU tracker record metadata invalid");
  // Validate the supplied slice, but copy start/display directly below.
  require(display==(length>=100?100:length)&&start==(head+length-display)%255&&(length==255||head==0),"GPU tracker display metadata inconsistent");
  ++observed[state];
  finite(realPair(w,base));
  finite(realPair(w,base+2));
  finite(realPair(w,base+4));
  finite(realPair(w,base+6));
  require(w[base+15]==(state?ordinal:UINT32_MAX),"GPU tracker publication ordinal differs");
  if(!state)continue;
  require(ordinal<rows.size(),"GPU tracker publication exceeds header");
  auto&h=history[ordinal];
  h.d.reserve(display);
  h.f.reserve(display);
  h.s.reserve(display);
  for(uint32_t j=0;j<display;++j){const size_t at=base+16+8*((start+j)%255);
  const double d=realPair(w,at),f=realPair(w,at+2);
  finite(d);
  finite(f);
  finite(realPair(w,at+4));
  stateName(w[at+6]);
  require(w[at+7]==0,"GPU history reserved word nonzero");
  h.d.push_back(d);
  h.f.push_back(f);
  h.s.push_back(w[at+6]);}
  rows[ordinal]={w[base+12],state,realPair(w,base),realPair(w,base+2),realPair(w,base+6),wordPair(w,base+8),{{h.d.data(),h.d.size()},{h.f.data(),h.f.size()},{h.s.data(),h.s.size()}}};
  ++ordinal;
 }
 require(observed==counts&&ordinal==published,"GPU tracker record/header counts differ");
 return tracks({timestamp,total,counts[0],counts[1],counts[2],counts[3],{rows.data(),rows.size()}});
}
} // namespace blah2::gpu_result_json
