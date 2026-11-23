#pragma once
#include "TemporalCandidateCell.h"
#include "data/Detection.h"
// GPU-result views only: validation and legacy JSON formatting; no DSP math.
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstring>
#include <limits>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string>
#include <array>
#include <algorithm>
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
View<uint32_t> state,measured;};
struct PublishedTrack {uint32_t id=0,state=0;
double delay=0,doppler=0,acceleration=0;
uint64_t associated=0;
HistoryView history;
bool measurementKnown=false,measured=false;
uint32_t source=0;
Detection::Evidence measurementEvidence{};}; // 0 prediction/legacy, 1 detector
// Actual accepted input identities, oldest first, bound to this checkpoint.
struct MeasurementWindow {uint64_t epoch=0,consumedSequence=0;View<uint64_t> sequences;};
struct TrackView {uint64_t timestamp=0,total=0,tentative=0,associated=0,active=0,coasting=0;
View<PublishedTrack> published;};
inline void require(bool yes,const char*why){if(!yes)throw std::invalid_argument(why);}
// Startup-only product oracle. Preserve exact structural/integer checks and
// two-place display tolerance; report one bounded mismatch, never whole maps.
inline void compareOracleJson(const rapidjson::Value& expected,const rapidjson::Value& actual,
 const char* label,const std::string& path="$"){
 const auto type=[](const rapidjson::Value& value){
  if(value.IsNumber())return value.IsInt64()?"int64":value.IsUint64()?"uint64":"double";
  const char* names[]={"null","bool","bool","object","array","string","number"};
  return names[value.GetType()];
 };
 const auto fail=[&](const char* reason,double tolerance=0,const std::string& location=""){
  std::ostringstream message;message<<std::setprecision(17)<<label<<" path="<<(location.empty()?path:location)
   <<" reason="<<reason<<" expected_type="<<type(expected)<<" actual_type="<<type(actual);
  const auto value=[&](const rapidjson::Value& v){
   if(v.IsInt64())message<<v.GetInt64();else if(v.IsUint64())message<<v.GetUint64();
   else if(v.IsNumber())message<<v.GetDouble();else if(v.IsArray())message<<"size:"<<v.Size();
   else if(v.IsObject())message<<"members:"<<v.MemberCount();
   else if(v.IsString())message<<"length:"<<v.GetStringLength();
   else if(v.IsBool())message<<(v.GetBool()?"true":"false");else message<<"null";
  };
  message<<" expected=";value(expected);message<<" actual=";value(actual);
  if(expected.IsNumber()&&actual.IsNumber())message<<" tolerance="<<tolerance;
  throw std::invalid_argument(message.str());
 };
 if(expected.GetType()!=actual.GetType())fail("type");
 if(expected.IsObject()){
  if(expected.MemberCount()!=actual.MemberCount())fail("member-count");
  for(auto member=expected.MemberBegin();member!=expected.MemberEnd();++member){
   const auto child=(path+"."+std::string(member->name.GetString(),
    std::min<rapidjson::SizeType>(member->name.GetStringLength(),64))).substr(0,192);
   if(!actual.HasMember(member->name.GetString()))fail("missing-member",0,child);
   compareOracleJson(member->value,actual[member->name.GetString()],label,child);
  }
 }else if(expected.IsArray()){
  if(expected.Size()!=actual.Size())fail("array-size");
  for(rapidjson::SizeType i=0;i<expected.Size();++i)
   compareOracleJson(expected[i],actual[i],label,(path+"["+std::to_string(i)+"]").substr(0,192));
 }else if(expected.IsNumber()){
  const double left=expected.GetDouble(),right=actual.GetDouble();
  if(expected.IsInt64()&&actual.IsInt64()){
   if(expected.GetInt64()!=actual.GetInt64())fail("integer");
  }else if(expected.IsUint64()&&actual.IsUint64()){
   if(expected.GetUint64()!=actual.GetUint64())fail("integer");
  }else{
   const double tolerance=0.010001+1e-7*std::max({1.0,std::abs(left),std::abs(right)});
   if(!std::isfinite(left)||!std::isfinite(right)||std::abs(left-right)>tolerance)
    fail("number",tolerance);
  }
 }else if(expected.IsString()){
  if(std::string(expected.GetString())!=actual.GetString())fail("string");
 }else if(expected.IsBool()){
  if(expected.GetBool()!=actual.GetBool())fail("boolean");
 }else if(!expected.IsNull()||!actual.IsNull())fail("null");
}
inline void compareOracleJson(const std::string& expected,const std::string& actual,const char* label){
 rapidjson::Document left,right;
 left.Parse<rapidjson::kParseFullPrecisionFlag>(expected.c_str());
 right.Parse<rapidjson::kParseFullPrecisionFlag>(actual.c_str());
 require(!left.HasParseError()&&!right.HasParseError(),label);
 compareOracleJson(left,right,label);
}
// Fail-only lineage receipt: one candidate, bounded scalar fields and at most
// eight support identifiers. Never dump maps or alter the comparator's verdict.
inline std::string oracleLineageDiagnostic(const rapidjson::Value& expected,
 const rapidjson::Value& actual,View<uint32_t> flags,View<uint32_t> cells,
 uint32_t rows,uint32_t columns,uint32_t maximum,size_t& index){
 index=SIZE_MAX;
 if(!expected.IsObject()||!actual.IsObject()||!expected.HasMember("evidence")||
  !actual.HasMember("evidence")||!expected["evidence"].IsArray()||!actual["evidence"].IsArray())return {};
 const auto count=std::min(expected["evidence"].Size(),actual["evidence"].Size());
 for(rapidjson::SizeType i=0;i<count;++i){
  try{compareOracleJson(expected["evidence"][i],actual["evidence"][i],"lineage");}
  catch(const std::invalid_argument&){index=i;break;}
 }
 if(index==SIZE_MAX)return {};
 std::ostringstream out;out<<std::setprecision(17)<<" lineage_index="<<index;
 if(cells.data&&index<cells.count){const uint32_t cell=cells.data[index];out<<" gpu_cell="<<cell;
  if(flags.data&&cell<flags.count){const uint32_t flag=flags.data[cell],bank=(flag>>3)&15u;
   out<<" raw_flags=0x"<<std::hex<<flag<<std::dec<<" decoded_bank="
    <<((bank&8)?4:(bank&4)?3:(bank&2)?2:(bank&1)?1:0)
    <<" decoded_delay_motion="<<(int((flag>>17)&7)-2)<<" decoded_doppler_motion="<<(int((flag>>20)&63)-16);
   try{out<<" gpu_source_cell="<<temporal_candidate::sourceCell(cell,flag,rows,columns,maximum);}
   catch(const std::invalid_argument&){out<<"invalid";}
  }
 }
 const auto scalar=[&](const rapidjson::Value& value){
  if(value.IsInt64())out<<value.GetInt64();else if(value.IsUint64())out<<value.GetUint64();
  else if(value.IsNumber())out<<value.GetDouble();
  else if(value.IsString())out<<std::string(value.GetString(),std::min<rapidjson::SizeType>(value.GetStringLength(),64));
  else out<<"invalid";
 };
 const auto candidate=[&](const char* label,const rapidjson::Value& product){out<<' '<<label<<"={";
  for(const char* field:{"delay","doppler","snr"})if(product.HasMember(field)&&product[field].IsArray()&&index<product[field].Size()){
   out<<field<<'=';scalar(product[field][rapidjson::SizeType(index)]);out<<',';}
  const auto& evidence=product["evidence"][rapidjson::SizeType(index)];
  if(evidence.IsObject())for(const char* field:{"kind","epoch_exact","newest_sequence_exact","cpis","bank","delay_motion_bins","doppler_motion_bins","support_sequences_exact"})
   if(evidence.HasMember(field)){out<<field<<'=';const auto& value=evidence[field];
    if(value.IsArray()){out<<'[';for(rapidjson::SizeType i=0;i<std::min<rapidjson::SizeType>(value.Size(),8);++i){if(i)out<<',';scalar(value[i]);}out<<']';}
    else {scalar(value);}out<<',';}
  out<<'}';
 };
 candidate("cpu",expected);candidate("gpu",actual);return out.str();
}
inline bool validFineSpatialEvidence(uint32_t spatial,uint32_t fineBeams){
 const uint32_t primary=spatial&127u,secondary=(spatial>>7u)&127u,tertiary=(spatial>>14u)&127u;
 return primary>0&&primary<=fineBeams&&secondary<=fineBeams&&tertiary<=fineBeams;
}
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
 shape(t.history.measured);
 require(t.history.delay.count==t.history.doppler.count&&t.history.delay.count==t.history.state.count&&t.history.delay.count<=100,"GPU selected display history shape differs");
 require(!t.history.measured.count||t.history.measured.count==t.history.state.count,"GPU selected measurement history shape differs");
 require(!t.measurementKnown||t.source<=1,"GPU measurement provenance invalid");
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
 if(t.measurementKnown){w.Key("measured");w.Bool(t.measured);
  w.Key("source");w.String(t.source==1?"detector":"prediction");
  if(t.measured){w.Key("measurement_evidence");rapidjson::Document storage;
   Detection::evidence_json(t.measurementEvidence,storage.GetAllocator(),true).Accept(w);}}
 w.Key("associated_delay");
 array(w,t.history.delay);
 w.Key("associated_doppler");
 array(w,t.history.doppler);
 w.Key("associated_state");
 w.StartArray();
 for(size_t j=0;j<t.history.state.count;++j)w.String(stateName(t.history.state.data[j]));
 w.EndArray();
 if(t.measurementKnown||t.history.measured.count){w.Key("associated_measured");w.StartArray();
  for(size_t j=0;j<t.history.measured.count;++j)w.Bool(t.history.measured.data[j]!=0);
  w.EndArray();}
 w.EndObject();}w.EndArray();
 w.EndObject();
 return {b.GetString(),b.GetSize()};}

// Accept the actual ABI DetectionRecord AoS directly; copy supplied GPU km
// metadata, never convert the delay-bin field.
inline std::vector<uint32_t> temporalSourceCells(View<uint32_t> flags,View<uint32_t> cells,
 uint32_t rows,uint32_t columns,uint32_t maximumCpis){
 shape(flags);shape(cells);std::vector<uint32_t> result;result.reserve(cells.count);
 for(size_t i=0;i<cells.count;++i){const auto key=cells.data[i];uint32_t value=0;
  if(maximumCpis>1){
   if(key>=flags.count)throw std::invalid_argument("Resident temporal source flags absent");
   value=flags.data[key];
  }
  result.push_back(temporal_candidate::sourceCell(key,value,rows,columns,maximumCpis));}
 return result;
}
// One lossless ABI lineage decoder serves JSON publication and host tracking.
inline std::vector<Detection::Evidence> temporalCandidates(View<uint32_t> flags,
 View<uint32_t> cells,uint64_t epoch,View<uint64_t> sequences,uint32_t maximum){
 shape(flags);shape(cells);shape(sequences);
 require((maximum==2||maximum==4||maximum==8)&&epoch&&sequences.count&&
  sequences.count<=maximum,"Invalid resident temporal publication history");
 std::vector<Detection::Evidence> evidence; evidence.reserve(cells.count);
 for(size_t i=0;i<cells.count;++i){require(cells.data[i]<flags.count,"Resident temporal publication cell invalid");
  const uint32_t value=flags.data[cells.data[i]],qualified=(value>>3)&15;
  require((value&4)&&qualified&&!(qualified&(qualified-1)),"Resident originating bank ambiguous");
  const uint32_t bank=(qualified&8)?4:(qualified&4)?3:(qualified&2)?2:1,
   depth=bank>2?1u<<(bank-1):2;
  require(depth<=maximum&&depth<=sequences.count,"Resident temporal publication support incomplete");
  require(bank!=1||(((value>>17)&7)==2&&((value>>20)&63)==16),"Resident baseline contains motion lineage");
  Detection::Evidence item{epoch,sequences.data[sequences.count-1],uint8_t(depth),uint8_t(bank),
   int8_t(int((value>>17)&7)-2),int8_t(int((value>>20)&63)-16),{}};
  for(size_t age=0;age<depth;++age){const size_t j=sequences.count-1-age;
   require(sequences.data[j]&&(j==0||sequences.data[j]>sequences.data[j-1]),"Resident temporal publication sequence is not strictly increasing");
   item.sequences[age]=sequences.data[j];}
  require(Detection::valid_evidence(item),"Resident temporal candidate evidence invalid");
  evidence.push_back(item);}
 return evidence;
}
inline std::string temporalEvidence(std::string base,View<uint32_t> flags,
 View<uint32_t> cells,uint64_t epoch,View<uint64_t> sequences,uint32_t maximum){
 const auto candidates=temporalCandidates(flags,cells,epoch,sequences,maximum);
 rapidjson::Document d;d.Parse(base.c_str());require(!d.HasParseError()&&d.IsObject()&&
  d.HasMember("snr")&&d["snr"].IsArray()&&d["snr"].Size()==cells.count,
  "Resident temporal publication candidate count differs");
 auto& allocator=d.GetAllocator();rapidjson::Value evidence(rapidjson::kArrayType);
 d.AddMember("evidence_identity",Detection::identity_json(epoch,sequences.data[sequences.count-1],allocator),allocator);
 for(const auto& item:candidates)evidence.PushBack(Detection::evidence_json(item,allocator),allocator);
 if(cells.count)d.AddMember("evidence",evidence,allocator);
 rapidjson::StringBuffer buffer;rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);d.Accept(writer);return buffer.GetString();
}
inline std::string singleCpiEvidence(std::string base,uint64_t epoch,uint64_t sequence){
 rapidjson::Document d;d.Parse(base.c_str());require(!d.HasParseError()&&d.IsObject()&&
  d.HasMember("snr")&&d["snr"].IsArray(),"Resident single-CPI candidate product invalid");
 Detection::Evidence identity;identity.epoch=epoch;identity.newestSequence=sequence;
 identity.depth=1;identity.sequences[0]=sequence;
 require(Detection::valid_evidence(identity),"Resident single-CPI identity unavailable");
 auto& allocator=d.GetAllocator();rapidjson::Value evidence(rapidjson::kArrayType);
 d.AddMember("evidence_identity",Detection::identity_json(epoch,sequence,allocator),allocator);
 for(rapidjson::SizeType i=0;i<d["snr"].Size();++i)
  evidence.PushBack(Detection::evidence_json(identity,allocator),allocator);
 if(!evidence.Empty())d.AddMember("evidence",evidence,allocator);
 rapidjson::StringBuffer buffer;rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);d.Accept(writer);
 return buffer.GetString();
}
// Unrounded detector ABI handoff. Numeric records and acknowledged input
// support are copied, never recomputed from a display grid or JSON product.
template<class Record> inline Detection hostDetections(View<Record> records,
 View<uint32_t> flags,View<uint32_t> cells,uint64_t epoch,
 View<uint64_t> sequences,uint32_t maximum,bool inputLineageAvailable=true){
 shape(records);shape(sequences);
 std::vector<double> delay,doppler,snr;
 delay.reserve(records.count);doppler.reserve(records.count);snr.reserve(records.count);
 for(size_t i=0;i<records.count;++i){const auto& d=records.data[i];
  finite(d.delay);finite(d.doppler);finite(d.snr);
  delay.push_back(d.delay);doppler.push_back(d.doppler);snr.push_back(d.snr);}
 Detection result(std::move(delay),std::move(doppler),std::move(snr));
 if(maximum>1){
  require(records.count==cells.count,"Resident host detector candidate count differs");
  result.set_evidence(temporalCandidates(flags,cells,epoch,sequences,maximum));
 }else if(inputLineageAvailable){
  require(maximum==1&&sequences.count==1,"Resident host single-CPI support invalid");
  Detection::Evidence identity{epoch,sequences.data[0],1,0,0,0,{}};
  identity.sequences[0]=identity.newestSequence;
  require(Detection::valid_evidence(identity),"Resident host single-CPI evidence invalid");
  result.set_evidence(std::vector<Detection::Evidence>(records.count,identity));
 }
 if(maximum>1||inputLineageAvailable)
  result.set_evidence_identity(epoch,sequences.data[sequences.count-1]);
 return result;
}
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
template<class Record> inline std::string manifoldEvidence(uint64_t timestamp,
 double frequencyHz,double sampleRate,uint32_t rows,uint32_t columns,uint32_t channels,
 View<Record> detectionsView,View<uint32_t> cells,
 View<std::complex<float>> channelMaps,View<std::complex<double>> referenceWeights,
 uint64_t referenceFrames,uint64_t referenceUpdates,double coherentFraction,double coherentGainDb,double referenceScale,
 uint32_t bearingMethod,uint32_t bearingReferenceAnchor,uint32_t bearingScanBeams,
 uint32_t bearingGuardRadius,uint32_t bearingTrainingRadius,
 double bearingDiagonalLoading,View<uint32_t> bearingChannelMapping,
 View<double> actualGainDb){
 shape(detectionsView);shape(cells);shape(channelMaps);shape(referenceWeights);shape(actualGainDb);
 require(std::isfinite(frequencyHz)&&frequencyHz>0&&std::isfinite(sampleRate)&&sampleRate>0&&
  rows&&columns&&channels>=2&&channels<=8,"Invalid manifold evidence geometry");
 const uint64_t count=uint64_t(rows)*columns;
 require(count<=SIZE_MAX/channels&&detectionsView.count==cells.count&&
  channelMaps.count==size_t(count)*channels&&referenceWeights.count<=8,
  "Manifold evidence shape differs");
 require(std::isfinite(coherentFraction)&&coherentFraction>=0&&
  std::isfinite(coherentGainDb)&&std::isfinite(referenceScale)&&referenceScale>=0,
  "Invalid reference diagnostic state");
 require(bearingMethod<=1&&bearingReferenceAnchor<=2&&bearingScanBeams>=4&&
  bearingScanBeams<=127&&bearingChannelMapping.count==channels&&actualGainDb.count==channels,
  "Invalid manifold replay configuration");
 for(size_t i=0;i<actualGainDb.count;++i)finite(actualGainDb.data[i]);
 std::array<bool,8> used{};for(size_t i=0;i<bearingChannelMapping.count;++i){
  require(bearingChannelMapping.data[i]<channels&&!used[bearingChannelMapping.data[i]],
   "Invalid manifold replay mapping");used[bearingChannelMapping.data[i]]=true;}
 if(bearingMethod==1)require(bearingGuardRadius<=3&&bearingTrainingRadius>bearingGuardRadius&&
  bearingTrainingRadius<=6&&std::isfinite(bearingDiagonalLoading)&&
  bearingDiagonalLoading>0&&bearingDiagonalLoading<=1,
  "Invalid adaptive manifold replay configuration");
 // Preserve exact applied-gain doubles for the v3 manifold fingerprint and
 // the later runtime actual-gain equality check.
 rapidjson::StringBuffer b;Writer w(b);
 w.StartObject();w.Key("schema");w.String("vectorwarp-manifold-evidence-v2");
 w.Key("precision_contract");w.String("binary64-json-roundtrip-v1");
 w.Key("timestamp");w.Uint64(timestamp);w.Key("frequency_hz");w.Double(frequencyHz);
 w.Key("sample_rate_hz");w.Double(sampleRate);w.Key("channels");w.Uint(channels);
 w.Key("actual_gain_db");w.StartArray();
 for(size_t i=0;i<actualGainDb.count;++i)w.Double(actualGainDb.data[i]);
 w.EndArray();
 w.Key("bearing_replay");w.StartObject();w.Key("method");
 w.String(bearingMethod==1?"covariance_mvdr":"coherent_beam_bank");
 w.Key("reference_anchor");w.String(bearingReferenceAnchor==2?"fixed_site_projected":
  bearingReferenceAnchor==1?"fixed_site":"live_eigenbeam");
 w.Key("scan_beams");w.Uint(bearingScanBeams);
 w.Key("guard_radius");w.Uint(bearingGuardRadius);w.Key("training_radius");w.Uint(bearingTrainingRadius);
 w.Key("diagonal_loading");w.Double(bearingDiagonalLoading);w.Key("channel_mapping");w.StartArray();
 for(size_t i=0;i<bearingChannelMapping.count;++i)w.Uint(bearingChannelMapping.data[i]);
 w.EndArray();w.EndObject();
 w.Key("reference_weights");w.StartArray();
 for(size_t i=0;i<referenceWeights.count;++i){const auto value=referenceWeights.data[i];
  finite(value.real());finite(value.imag());w.StartArray();w.Double(value.real());w.Double(value.imag());w.EndArray();}
 w.EndArray();w.Key("reference_state");w.StartObject();
 w.Key("frames");w.Uint64(referenceFrames);w.Key("updates");w.Uint64(referenceUpdates);
 w.Key("coherent_fraction");w.Double(coherentFraction);w.Key("coherent_gain_db");w.Double(coherentGainDb);
 w.Key("scale");w.Double(referenceScale);w.EndObject();w.Key("data");w.StartArray();
 for(size_t i=0;i<detectionsView.count;++i){const auto& detection=detectionsView.data[i];
  finite(detection.delay);finite(detection.delayKm);finite(detection.doppler);finite(detection.snr);
  const uint32_t cell=cells.data[i];require(cell<count,"Manifold evidence cell out of range");
  const uint32_t row=cell/columns,column=cell%columns;
  const uint32_t shifted=(row+rows/2+1)%rows;
  w.StartObject();w.Key("cell");w.Uint(cell);w.Key("delay_km");w.Double(detection.delayKm);
  w.Key("doppler_hz");w.Double(detection.doppler);w.Key("snr_db");w.Double(detection.snr);
  w.Key("response");w.StartArray();
  for(uint32_t channel=0;channel<channels;++channel){
   const auto value=channelMaps.data[uint64_t(channel)*count+uint64_t(column)*rows+shifted];
   finite(value.real());finite(value.imag());w.StartArray();w.Double(value.real());w.Double(value.imag());w.EndArray();}
  w.EndArray();
  if(bearingMethod==1){
   std::array<std::complex<double>,64> covariance{};uint32_t samples=0;
   const int training=int(bearingTrainingRadius),guard=int(bearingGuardRadius);
   for(int dr=-training;dr<=training;++dr)for(int dc=-training;dc<=training;++dc){
    if(std::abs(dr)<=guard&&std::abs(dc)<=guard)continue;
    const int rr=int(row)+dr,cc=int(column)+dc;
    if(rr<0||cc<0||rr>=int(rows)||cc>=int(columns))continue;
    const uint32_t sh=(uint32_t(rr)+rows/2+1)%rows;
    for(uint32_t a=0;a<channels;++a){
     const auto va=std::complex<double>(channelMaps.data[uint64_t(a)*count+uint64_t(cc)*rows+sh]);
     for(uint32_t c=0;c<channels;++c){
      const auto vc=std::complex<double>(channelMaps.data[uint64_t(c)*count+uint64_t(cc)*rows+sh]);
      covariance[a*8+c]+=va*std::conj(vc);}}
    ++samples;
   }
   double trace=0;for(uint32_t channel=0;channel<channels;++channel)trace+=covariance[channel*8+channel].real();
   const double load=std::max(trace/std::max(double(samples*channels),1.0)*bearingDiagonalLoading,1e-12);
   w.Key("covariance_samples");w.Uint(samples);w.Key("covariance");w.StartArray();
   for(uint32_t a=0;a<channels;++a){w.StartArray();for(uint32_t c=0;c<channels;++c){
    auto value=covariance[a*8+c]/std::max(double(samples),1.0);
    if(a==c)value+=load;
    finite(value.real());finite(value.imag());
    w.StartArray();w.Double(value.real());w.Double(value.imag());w.EndArray();}w.EndArray();}
   w.EndArray();
  }
  w.EndObject();}
 w.EndArray();w.EndObject();return {b.GetString(),b.GetSize()};
}
struct CheckpointBinding {uint32_t capacity=0;
uint64_t epoch=0,generation=0,consumedSequence=0;
uint32_t beamCount=0,scanBeamCount=0;};
struct BearingConfig {bool enabled=false;
 uint32_t beamCount=0,refinementFactor=4,minConsistent=3;
 double xAxisBearingDegTrue=0,minimumUncertaintyDeg=0;
 uint32_t minPeakSeparationDb=3,method=0,scanBeams=0;
 double cpiSeconds=0,maxBearingRateDegPerSecond=0;
 uint32_t stationaryPersistenceCpis=0;
 double stationaryMaxDopplerHz=0;};
inline uint64_t wordPair(const uint32_t*w,size_t at){return uint64_t(w[at])|(uint64_t(w[at+1])<<32);}
inline double realPair(const uint32_t*w,size_t at){const uint64_t bits=wordPair(w,at);
double value;
std::memcpy(&value,&bits,sizeof(value));
return value;}
// History-word encoding is independent of checkpoint schema5's record layout.
enum class HistoryEncoding {measurement,disjointCredit};
inline HistoryEncoding historyEncoding(const uint32_t* checkpoint){
 require(checkpoint[0]==5&&checkpoint[27]<=1,"GPU tracker history encoding invalid");
 return checkpoint[27]?HistoryEncoding::disjointCredit:HistoryEncoding::measurement;
}
inline bool validHistoryMetadata(uint32_t word,HistoryEncoding encoding){
 if(encoding!=HistoryEncoding::measurement&&encoding!=HistoryEncoding::disjointCredit)return false;
 const bool disjoint=encoding==HistoryEncoding::disjointCredit;
 const uint32_t evidence=word&12u,state=word&3u,depth=word>>(disjoint?5u:4u);
 return word<=(disjoint?511u:255u)&&(evidence&8u)==0u&&
  !(state==3u&&evidence!=0u)&&
  !(disjoint&&(word&16u)!=0u&&evidence==0u)&&
  (evidence==0u?depth==0u:(depth==1u||depth==2u||depth==4u||depth==8u));
}
// The bank is an already committed GPU checkpoint. Decode/copy only its valid
// prefix and its GPU-selected publication slices; no tracker transition/ID,
// prediction, count, or display-window computation occurs here.
inline std::string trackerCheckpoint(uint64_t timestamp,View<uint32_t>bank,const CheckpointBinding&binding,
 const MeasurementWindow& measurement={}){
 shape(bank);
 shape(measurement.sequences);
 require(!measurement.sequences.count||(measurement.sequences.count<=8&&
  measurement.consumedSequence==binding.consumedSequence),"GPU measurement window binding differs");
 require(bank.count>=32,"Short GPU tracker header");
 const auto*w=bank.data;
 const uint32_t recordBytes=w[1],historyCapacity=w[3];
 require(binding.capacity&&w[0]==5&&historyCapacity>=100&&historyCapacity<=255&&
  recordBytes==(20+8*historyCapacity)*4&&w[2]==binding.capacity,"GPU tracker schema/capacity differs");
 const auto encoding=historyEncoding(w);
 const size_t recordWords=recordBytes/4;
 const uint64_t total=wordPair(w,4),published=wordPair(w,24);
 require(total<=binding.capacity&&total<=(SIZE_MAX-32)/recordWords&&bank.count==32+recordWords*size_t(total),"GPU tracker valid prefix differs");
 require(w[8]<65535&&w[9]==0&&wordPair(w,10)==binding.epoch&&wordPair(w,12)==binding.generation&&wordPair(w,14)==binding.consumedSequence,"GPU tracker checkpoint binding differs");
 for(size_t i=28;i<32;++i)require(w[i]==0,"GPU tracker reserved header nonzero");
 require(w[27]||w[26]==0,"GPU tracker lineage policy invalid");
 if(w[27])require(w[26]<UINT32_MAX,"GPU tracker temporal counter invalid");
 const std::array<uint64_t,4>counts{wordPair(w,16),wordPair(w,18),wordPair(w,20),wordPair(w,22)};
 require(counts[0]<=total&&counts[1]<=total-counts[0]&&counts[2]<=total-counts[0]-counts[1]&&counts[3]==total-counts[0]-counts[1]-counts[2]&&published==total-counts[0],"GPU tracker header totals differ");
 std::vector<PublishedTrack>rows(size_t(published),PublishedTrack{});
 struct Storage {std::vector<double>d,f;
 std::vector<uint32_t>s,m;};
 std::vector<Storage>history(size_t(published),Storage{});
 std::array<uint64_t,4>observed{};
 size_t ordinal=0;
 for(size_t i=0;i<size_t(total);++i){const size_t base=32+recordWords*i;
 const uint32_t state=w[base+14],packed=w[base+13],head=packed&255,length=(packed>>8)&255,start=(packed>>16)&255,display=packed>>24;
  const uint32_t identity=w[base+12];
  require(state<4&&(identity&0x3fff0000u)==0u&&(identity&65535u)<65535&&head<historyCapacity&&length&&length<=historyCapacity&&start<historyCapacity&&display&&display<=100&&display<=length,"GPU tracker record metadata invalid");
  // Validate the supplied slice, but copy start/display directly below.
  require(display==(length>=100?100:length)&&start==(head+length-display)%historyCapacity&&(length==historyCapacity||head==0),"GPU tracker display metadata inconsistent");
  ++observed[state];
  finite(realPair(w,base));
  finite(realPair(w,base+2));
  finite(realPair(w,base+4));
  finite(realPair(w,base+6));
  const double missed=realPair(w,base+16+8*historyCapacity);finite(missed);
  const auto measured=wordPair(w,base+18+8*historyCapacity);
  require(missed>=0&&measured>0&&measured<=wordPair(w,base+8),"GPU tracker elapsed/measurement interval invalid");
  require(w[base+15]==(state?ordinal:UINT32_MAX),"GPU tracker publication ordinal differs");
  if(!state)continue;
  require(ordinal<rows.size(),"GPU tracker publication exceeds header");
  auto&h=history[ordinal];
  h.d.reserve(display);
  h.f.reserve(display);
  h.s.reserve(display);
  h.m.reserve(display);
  for(uint32_t j=0;j<display;++j){const size_t at=base+16+8*((start+j)%historyCapacity);
  const double d=realPair(w,at),f=realPair(w,at+2);
  finite(d);
  finite(f);
  finite(realPair(w,at+4));
  const uint32_t historyState=w[at+6];
  require(validHistoryMetadata(historyState,encoding),"GPU history measurement metadata invalid");
  stateName(historyState&3u);
  const uint32_t spatial=w[at+7];
  if(spatial&0x80000000u){const uint32_t scan=binding.scanBeamCount?binding.scanBeamCount:binding.beamCount*4u;
   require(validFineSpatialEvidence(spatial,scan),"GPU history multi-lobe metadata invalid");}
  else{const uint32_t coarse=spatial&511u,fineBeam=(spatial>>9u)&2047u;
   require(coarse<=binding.beamCount&&fineBeam<=binding.beamCount*4u&&
    !(coarse==0&&fineBeam!=0),"GPU history beam metadata invalid");}
  // Schema5 publishes only genuine measured points within the GPU-selected
  // last-100-update slice. Prediction endpoints remain private state.
  if((historyState&4u)==0u)continue;
  h.d.push_back(d);
  h.f.push_back(f);
  h.s.push_back(historyState&3u);
  h.m.push_back(1u);}
  const uint32_t newestPrivate=w[base+16+8*((head+length-1u)%historyCapacity)+6u];
  rows[ordinal]={identity&65535u,state,realPair(w,base),realPair(w,base+2),realPair(w,base+6),measured,
   {{h.d.data(),h.d.size()},{h.f.data(),h.f.size()},{h.s.data(),h.s.size()},{h.m.data(),h.m.size()}},
   true,(newestPrivate&4u)!=0u,(newestPrivate&4u)!=0u?1u:0u};
  if((newestPrivate&4u)&&measurement.sequences.count){
   auto& evidence=rows[ordinal].measurementEvidence;
   evidence.epoch=measurement.epoch;
   evidence.newestSequence=measurement.sequences.data[measurement.sequences.count-1];
   evidence.depth=uint8_t(newestPrivate>>(encoding==HistoryEncoding::disjointCredit?5u:4u));
   require(evidence.depth<=measurement.sequences.count,"GPU measurement support incomplete");
   for(uint8_t age=0;age<evidence.depth;++age)
    evidence.sequences[age]=measurement.sequences.data[measurement.sequences.count-1-age];
   require(Detection::valid_evidence(evidence),"GPU measurement support invalid");
  }
  ++ordinal;
 }
 require(observed==counts&&ordinal==published,"GPU tracker record/header counts differ");
 return tracks({timestamp,total,counts[0],counts[1],counts[2],counts[3],{rows.data(),rows.size()}});
}
inline double normalizeDegrees(double value){value=std::fmod(value,360.0);
 return value<0?value+360.0:value;}
// Bearings are a separate, opt-in publication of temporal beam-bank estimates,
// never ADS-B-derived.
inline std::string bearingCheckpoint(uint64_t timestamp,View<uint32_t>bank,
 const CheckpointBinding&binding,const BearingConfig&config){
 shape(bank);
 require(config.enabled&&config.beamCount==binding.beamCount&&config.beamCount>=4&&
  config.refinementFactor>=1&&config.minConsistent>=2&&config.minConsistent<=100&&
  std::isfinite(config.xAxisBearingDegTrue)&&std::isfinite(config.minimumUncertaintyDeg)&&
  config.minimumUncertaintyDeg>=0&&config.minimumUncertaintyDeg<=180&&
  config.minPeakSeparationDb>=1&&config.minPeakSeparationDb<=15&&config.method<=1&&
  std::isfinite(config.cpiSeconds)&&config.cpiSeconds>=0&&
  std::isfinite(config.maxBearingRateDegPerSecond)&&config.maxBearingRateDegPerSecond>=0&&
  (!config.maxBearingRateDegPerSecond||config.cpiSeconds>0)&&
  std::isfinite(config.stationaryMaxDopplerHz)&&config.stationaryMaxDopplerHz>=0&&
  ((!config.stationaryPersistenceCpis&&config.stationaryMaxDopplerHz==0)||
   (config.stationaryPersistenceCpis>=2&&config.cpiSeconds>0)),
  "Invalid bearing publication config");
 require(bank.count>=32,"Short GPU tracker header");
 const auto*w=bank.data;
 const uint32_t historyCapacity=w[3],recordBytes=w[1];
 require(binding.capacity&&w[0]==5&&historyCapacity>=100&&historyCapacity<=255&&
  recordBytes==(20+8*historyCapacity)*4&&w[2]==binding.capacity,"GPU tracker schema/capacity differs");
 const auto encoding=historyEncoding(w);
 const size_t recordWords=recordBytes/4;
 const uint64_t total=wordPair(w,4);
 require(total<=binding.capacity&&bank.count==32+recordWords*size_t(total)&&
  wordPair(w,10)==binding.epoch&&wordPair(w,12)==binding.generation&&
  wordPair(w,14)==binding.consumedSequence,"GPU bearing checkpoint binding differs");
 rapidjson::StringBuffer b;Writer out(b);out.SetMaxDecimalPlaces(3);
 out.StartObject();out.Key("timestamp");out.Uint64(timestamp);
 out.Key("method");out.String(config.method==1?"covariance_mvdr":"coherent_beam_bank");
 out.Key("frame");out.String("clockwise_from_true_north");
 out.Key("coarse_beams");out.Uint(config.beamCount);
 const uint32_t scanBeams=config.scanBeams?config.scanBeams:config.beamCount*config.refinementFactor;
 require(scanBeams>=config.beamCount&&scanBeams<=127&&scanBeams%config.beamCount==0,"Invalid bearing scan count");
 out.Key("fine_beams");out.Uint(scanBeams);
 out.Key("scan_step_deg");out.Double(360.0/double(scanBeams));
 out.Key("aperture_uncertainty_floor_deg");out.Double(config.minimumUncertaintyDeg);
 out.Key("physical_resolution_limited");out.Bool(config.minimumUncertaintyDeg>180.0/double(scanBeams));
 out.Key("min_peak_separation_db");out.Uint(config.minPeakSeparationDb);
 out.Key("data");out.StartArray();
 constexpr char hex[]="0123456789ABCDEF";
 const double step=360.0/double(scanBeams),radians=std::acos(-1.0)/180.0;
 for(size_t i=0;i<size_t(total);++i){const size_t base=32+recordWords*i;
  const uint32_t state=w[base+14];if(state==0)continue;
  const uint32_t packed=w[base+13],head=packed&255u,length=(packed>>8u)&255u,
   start=(packed>>16u)&255u,display=packed>>24u;
  require(state<=3&&head<historyCapacity&&length&&display&&display<=length&&
   start<historyCapacity,"GPU bearing history metadata invalid");
  std::vector<double> histogram(scanBeams,0);
  struct Lobes{std::array<uint32_t,3> bin{};std::array<double,3> weight{};uint32_t sample=0;};
  std::vector<Lobes> frames;uint32_t count=0,current=0,currentQuality=0,legacyCount=0,stationarySamples=0;
  double minimumDelay=std::numeric_limits<double>::infinity(),maximumDelay=-std::numeric_limits<double>::infinity();
  double minimumDoppler=std::numeric_limits<double>::infinity(),maximumDoppler=-std::numeric_limits<double>::infinity();
  std::array<uint32_t,16> legacyQuality{};
  for(uint32_t j=0;j<display;++j){const size_t at=base+16+8*((start+j)%historyCapacity);
   const uint32_t privateHistoryState=w[at+6];
   require(validHistoryMetadata(privateHistoryState,encoding),"GPU bearing measurement metadata invalid");
   const uint32_t historyState=privateHistoryState&3u;stateName(historyState);
   // Predictions cannot create spatial measurements from a copied beam.
   if((privateHistoryState&4u)==0u)continue;
   if(historyState!=1u&&historyState!=2u)continue;
   const uint32_t spatial=w[at+7];Lobes frame;frame.sample=j;
   const double sampleDelay=realPair(w,at),sampleDoppler=realPair(w,at+2);
   if(spatial&0x80000000u){frame.bin={spatial&127u,(spatial>>7u)&127u,(spatial>>14u)&127u};
    frame.weight={1.0,double((spatial>>21u)&31u)/31.0,double((spatial>>26u)&31u)/31.0};
    require(validFineSpatialEvidence(spatial,scanBeams),"GPU bearing multi-lobe metadata invalid");
    current=frame.bin[0];currentQuality=frame.weight[1]>0?uint32_t(std::clamp(std::floor(10*std::log10(1/frame.weight[1])),0.0,15.0)):15u;}
   else{const uint32_t coarse=spatial&511u,fine=(spatial>>9u)&2047u,q=(spatial>>20u)&15u;
    require(coarse<=config.beamCount&&fine<=scanBeams&&!(coarse==0&&fine!=0),"GPU bearing spatial metadata invalid");if(!fine)continue;
    frame.bin={fine,0,0};frame.weight={1,0,0};current=fine;currentQuality=q;legacyQuality[q]++;legacyCount++;}
   if(config.stationaryMaxDopplerHz<=0||std::abs(sampleDoppler)<=config.stationaryMaxDopplerHz){stationarySamples++;
    minimumDelay=std::min(minimumDelay,sampleDelay);maximumDelay=std::max(maximumDelay,sampleDelay);
    minimumDoppler=std::min(minimumDoppler,sampleDoppler);maximumDoppler=std::max(maximumDoppler,sampleDoppler);}
   for(uint32_t k=0;k<3;++k)if(frame.bin[k])histogram[frame.bin[k]-1]+=frame.weight[k];
   frames.push_back(frame);count++;
  }
  if(!count||!current)continue;
  const uint32_t separationBins=std::max(1u,uint32_t(std::floor(config.minimumUncertaintyDeg/step)));
  std::vector<uint32_t> peaks(histogram.size());for(uint32_t k=0;k<peaks.size();++k)peaks[k]=k;
  std::stable_sort(peaks.begin(),peaks.end(),[&](uint32_t a,uint32_t c){return histogram[a]>histogram[c];});
  std::vector<uint32_t> independent;for(uint32_t bin:peaks){if(histogram[bin]<=0)break;bool separated=true;
   for(uint32_t other:independent){uint32_t delta=bin>other?bin-other:other-bin;if(std::min(delta,scanBeams-delta)<=separationBins){separated=false;break;}}
   if(separated){independent.push_back(bin);if(independent.size()==3)break;}}
  const uint32_t winner=independent.front();
  std::vector<uint32_t> selected(frames.size());
  bool hypothesisTracking=config.maxBearingRateDegPerSecond>0&&frames.size()>1;
  bool ratePathValid=true;
  if(hypothesisTracking){
   constexpr double impossible=-1e100;std::vector<std::array<double,3>>score(frames.size());
   std::vector<std::array<uint8_t,3>>previous(frames.size());
   for(uint32_t k=0;k<3;++k)score[0][k]=frames[0].bin[k]?std::log(std::max(frames[0].weight[k],1e-6)):impossible;
   for(size_t frame=1;frame<frames.size();++frame)for(uint32_t k=0;k<3;++k){score[frame][k]=impossible;if(!frames[frame].bin[k])continue;
    const double elapsed=config.cpiSeconds*double(frames[frame].sample-frames[frame-1].sample);
    const double allowed=std::max(step,config.maxBearingRateDegPerSecond*elapsed);
    const uint32_t right=frames[frame].bin[k]-1;for(uint32_t p=0;p<3;++p){if(score[frame-1][p]<=impossible/2||!frames[frame-1].bin[p])continue;
     const uint32_t left=frames[frame-1].bin[p]-1,raw=left>right?left-right:right-left;const double delta=double(std::min(raw,scanBeams-raw))*step;
     if(delta>allowed)continue;
     const double candidate=score[frame-1][p]+std::log(std::max(frames[frame].weight[k],1e-6))-.5*(delta/allowed)*(delta/allowed);
     if(candidate>score[frame][k]){score[frame][k]=candidate;previous[frame][k]=uint8_t(p);}}}
   uint32_t best=0;for(uint32_t k=1;k<3;++k)if(score.back()[k]>score.back()[best])best=k;
   if(score.back()[best]<=impossible/2){hypothesisTracking=false;ratePathValid=false;for(size_t frame=0;frame<frames.size();++frame){uint32_t nearest=0,bestDelta=UINT32_MAX;for(uint32_t k=0;k<3;++k)if(frames[frame].bin[k]){uint32_t bin=frames[frame].bin[k]-1,delta=bin>winner?bin-winner:winner-bin;delta=std::min(delta,scanBeams-delta);if(delta<bestDelta){bestDelta=delta;nearest=k;}}selected[frame]=nearest;}}
   else for(size_t frame=frames.size();frame-->0;){selected[frame]=best;if(frame)best=previous[frame][best];}
  }else for(size_t frame=0;frame<frames.size();++frame){uint32_t nearest=0,bestDelta=UINT32_MAX;for(uint32_t k=0;k<3;++k)if(frames[frame].bin[k]){uint32_t bin=frames[frame].bin[k]-1,delta=bin>winner?bin-winner:winner-bin;delta=std::min(delta,scanBeams-delta);if(delta<bestDelta){bestDelta=delta;nearest=k;}}selected[frame]=nearest;}
  double x=0,y=0,selectedWeight=0,totalWeight=0,bearingRate=0;uint32_t rateSamples=0;
  for(size_t frameIndex=0;frameIndex<frames.size();++frameIndex){const auto&frame=frames[frameIndex];const uint32_t choice=selected[frameIndex];
   const double angle=double(frame.bin[choice]-1)*step,weight=frame.weight[choice];x+=weight*std::cos(angle*radians);y+=weight*std::sin(angle*radians);selectedWeight+=weight;
   if(hypothesisTracking&&frameIndex){double delta=angle-double(frames[frameIndex-1].bin[selected[frameIndex-1]]-1)*step;delta=std::remainder(delta,360.0);
    const double elapsed=config.cpiSeconds*double(frame.sample-frames[frameIndex-1].sample);bearingRate+=delta/elapsed;++rateSamples;}
   for(double weight:frame.weight)totalWeight+=weight;}
  current=frames.back().bin[selected.back()];if(rateSamples)bearingRate/=rateSamples;
  const double concentration=selectedWeight?std::hypot(x,y)/selectedWeight:0;
  const double meanLocal=normalizeDegrees(std::atan2(y,x)/radians);
  const double uncertainty=std::max({step/2.0,config.minimumUncertaintyDeg,std::min(180.0,std::sqrt(std::max(0.0,-2.0*std::log(std::max(concentration,1e-12))))/radians)});
  const bool stationaryClutter=config.stationaryPersistenceCpis&&count>=config.stationaryPersistenceCpis&&
   stationarySamples==count&&maximumDelay-minimumDelay<=1.5&&
   maximumDoppler-minimumDoppler<=1.5/config.cpiSeconds;
  const double runner=independent.size()>1?histogram[independent[1]]:0;
  uint32_t temporalQuality=runner>0?uint32_t(std::clamp(std::floor(10*std::log10(histogram[winner]/runner)),0.0,15.0)):15u;
  if(legacyCount==count){uint32_t seen=0;for(temporalQuality=0;temporalQuality<legacyQuality.size();++temporalQuality){seen+=legacyQuality[temporalQuality];if(seen>(count-1)/2)break;}}
  char id[5]={hex[(w[base+12]>>12)&15],hex[(w[base+12]>>8)&15],hex[(w[base+12]>>4)&15],hex[w[base+12]&15],0};
  out.StartObject();out.Key("id");out.String(id);out.Key("state");out.String(stateName(state));
  out.Key("delay");out.Double(realPair(w,base));out.Key("doppler");out.Double(realPair(w,base+2));
  out.Key("bearing_deg_true");out.Double(normalizeDegrees(config.xAxisBearingDegTrue-meanLocal));
  out.Key("current_bearing_deg_true");out.Double(normalizeDegrees(config.xAxisBearingDegTrue-double(current-1)*step));
  out.Key("bearing_rate_deg_s");out.Double(-bearingRate);
  out.Key("hypothesis_tracking");out.Bool(hypothesisTracking);
  out.Key("uncertainty_deg");out.Double(uncertainty);out.Key("temporal_consistency");out.Double(concentration);
  out.Key("consistent_samples");out.Uint(count);out.Key("peak_separation_db");out.Uint(temporalQuality);
  out.Key("current_peak_separation_db");out.Uint(currentQuality);
  out.Key("stationary_clutter");out.Bool(stationaryClutter);out.Key("publishable");out.Bool(!stationaryClutter);
  out.Key("resolved");out.Bool(ratePathValid&&!stationaryClutter&&count>=config.minConsistent&&concentration>=.65&&uncertainty<=45&&
   temporalQuality>=config.minPeakSeparationDb);
  out.Key("candidates");out.StartArray();uint32_t emitted=0;
  for(uint32_t bin:independent){if(emitted&&histogram[bin]*4<histogram[winner])break;
   out.StartObject();out.Key("bearing_deg_true");out.Double(normalizeDegrees(config.xAxisBearingDegTrue-double(bin)*step));
   out.Key("support");out.Double(totalWeight?histogram[bin]/totalWeight:0);out.EndObject();if(++emitted==3)break;}
  out.EndArray();out.EndObject();
 }
 out.EndArray();out.EndObject();return {b.GetString(),b.GetSize()};
}
} // namespace blah2::gpu_result_json
