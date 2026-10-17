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
uint64_t epoch=0,generation=0,consumedSequence=0;
uint32_t beamCount=0,scanBeamCount=0;};
struct BearingConfig {bool enabled=false;
 uint32_t beamCount=0,refinementFactor=4,minConsistent=3;
 double xAxisBearingDegTrue=0,minimumUncertaintyDeg=0;
 uint32_t minPeakSeparationDb=3,method=0,scanBeams=0;};
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
 const uint32_t recordBytes=w[1],historyCapacity=w[3];
 require(binding.capacity&&w[0]==1&&historyCapacity>=100&&historyCapacity<=255&&
  recordBytes==(16+8*historyCapacity)*4&&w[2]==binding.capacity,"GPU tracker schema/capacity differs");
 const size_t recordWords=recordBytes/4;
 const uint64_t total=wordPair(w,4),published=wordPair(w,24);
 require(total<=binding.capacity&&total<=(SIZE_MAX-32)/recordWords&&bank.count==32+recordWords*size_t(total),"GPU tracker valid prefix differs");
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
 for(size_t i=0;i<size_t(total);++i){const size_t base=32+recordWords*i;
 const uint32_t state=w[base+14],packed=w[base+13],head=packed&255,length=(packed>>8)&255,start=(packed>>16)&255,display=packed>>24;
  require(state<4&&w[base+12]<65535&&head<historyCapacity&&length&&length<=historyCapacity&&start<historyCapacity&&display&&display<=100&&display<=length,"GPU tracker record metadata invalid");
  // Validate the supplied slice, but copy start/display directly below.
  require(display==(length>=100?100:length)&&start==(head+length-display)%historyCapacity&&(length==historyCapacity||head==0),"GPU tracker display metadata inconsistent");
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
  for(uint32_t j=0;j<display;++j){const size_t at=base+16+8*((start+j)%historyCapacity);
  const double d=realPair(w,at),f=realPair(w,at+2);
  finite(d);
  finite(f);
  finite(realPair(w,at+4));
  stateName(w[at+6]);
  const uint32_t spatial=w[at+7];
  if(spatial&0x80000000u){const uint32_t scan=binding.scanBeamCount?binding.scanBeamCount:binding.beamCount*4u;
   require((spatial&127u)>0&&(spatial&127u)<=scan&&((spatial>>7u)&127u)>0&&((spatial>>7u)&127u)<=scan&&
    ((spatial>>14u)&127u)>0&&((spatial>>14u)&127u)<=scan,"GPU history multi-lobe metadata invalid");}
  else{const uint32_t coarse=spatial&511u,fineBeam=(spatial>>9u)&2047u;
   require(coarse<=binding.beamCount&&fineBeam<=binding.beamCount*4u&&
    !(coarse==0&&fineBeam!=0),"GPU history beam metadata invalid");}
  h.d.push_back(d);
  h.f.push_back(f);
  h.s.push_back(w[at+6]);}
  rows[ordinal]={w[base+12],state,realPair(w,base),realPair(w,base+2),realPair(w,base+6),wordPair(w,base+8),{{h.d.data(),h.d.size()},{h.f.data(),h.f.size()},{h.s.data(),h.s.size()}}};
  ++ordinal;
 }
 require(observed==counts&&ordinal==published,"GPU tracker record/header counts differ");
 return tracks({timestamp,total,counts[0],counts[1],counts[2],counts[3],{rows.data(),rows.size()}});
}
inline double normalizeDegrees(double value){value=std::fmod(value,360.0);
 return value<0?value+360.0:value;}
// A separate, opt-in publication keeps the legacy tracker JSON byte-for-byte
// stable. Bearings are temporal beam-bank estimates, never ADS-B-derived.
inline std::string bearingCheckpoint(uint64_t timestamp,View<uint32_t>bank,
 const CheckpointBinding&binding,const BearingConfig&config){
 shape(bank);
 require(config.enabled&&config.beamCount==binding.beamCount&&config.beamCount>=4&&
  config.refinementFactor>=1&&config.minConsistent>=2&&config.minConsistent<=100&&
  std::isfinite(config.xAxisBearingDegTrue)&&std::isfinite(config.minimumUncertaintyDeg)&&
  config.minimumUncertaintyDeg>=0&&config.minimumUncertaintyDeg<=180&&
  config.minPeakSeparationDb>=1&&config.minPeakSeparationDb<=15&&config.method<=1,
  "Invalid bearing publication config");
 require(bank.count>=32,"Short GPU tracker header");
 const auto*w=bank.data;
 const uint32_t historyCapacity=w[3],recordBytes=w[1];
 require(binding.capacity&&w[0]==1&&historyCapacity>=100&&historyCapacity<=255&&
  recordBytes==(16+8*historyCapacity)*4&&w[2]==binding.capacity,"GPU tracker schema/capacity differs");
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
 out.Key("aperture_uncertainty_floor_deg");out.Double(config.minimumUncertaintyDeg);
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
  struct Lobes{std::array<uint32_t,3> bin{};std::array<double,3> weight{};};
  std::vector<Lobes> frames;uint32_t count=0,current=0,currentQuality=0,legacyCount=0;
  std::array<uint32_t,16> legacyQuality{};
  for(uint32_t j=0;j<display;++j){const size_t at=base+16+8*((start+j)%historyCapacity);
   const uint32_t historyState=w[at+6];stateName(historyState);
   if(historyState!=1u&&historyState!=2u)continue;const uint32_t spatial=w[at+7];Lobes frame;
   if(spatial&0x80000000u){frame.bin={spatial&127u,(spatial>>7u)&127u,(spatial>>14u)&127u};
    frame.weight={1.0,double((spatial>>21u)&31u)/31.0,double((spatial>>26u)&31u)/31.0};
    require(frame.bin[0]>0&&frame.bin[0]<=scanBeams&&frame.bin[1]>0&&frame.bin[1]<=scanBeams&&frame.bin[2]>0&&frame.bin[2]<=scanBeams,"GPU bearing multi-lobe metadata invalid");
    current=frame.bin[0];currentQuality=frame.weight[1]>0?uint32_t(std::clamp(std::floor(10*std::log10(1/frame.weight[1])),0.0,15.0)):15u;}
   else{const uint32_t coarse=spatial&511u,fine=(spatial>>9u)&2047u,q=(spatial>>20u)&15u;
    require(coarse<=config.beamCount&&fine<=scanBeams&&!(coarse==0&&fine!=0),"GPU bearing spatial metadata invalid");if(!fine)continue;
    frame.bin={fine,0,0};frame.weight={1,0,0};current=fine;currentQuality=q;legacyQuality[q]++;legacyCount++;}
   for(uint32_t k=0;k<3;++k)if(frame.bin[k])histogram[frame.bin[k]-1]+=frame.weight[k];frames.push_back(frame);count++;
  }
  if(!count||!current)continue;
  const uint32_t separationBins=std::max(1u,uint32_t(std::floor(config.minimumUncertaintyDeg/step)));
  std::vector<uint32_t> peaks(histogram.size());for(uint32_t k=0;k<peaks.size();++k)peaks[k]=k;
  std::stable_sort(peaks.begin(),peaks.end(),[&](uint32_t a,uint32_t c){return histogram[a]>histogram[c];});
  std::vector<uint32_t> independent;for(uint32_t bin:peaks){if(histogram[bin]<=0)break;bool separated=true;
   for(uint32_t other:independent){uint32_t delta=bin>other?bin-other:other-bin;if(std::min(delta,scanBeams-delta)<=separationBins){separated=false;break;}}
   if(separated){independent.push_back(bin);if(independent.size()==3)break;}}
  const uint32_t winner=independent.front();double x=0,y=0,selectedWeight=0,totalWeight=0;
  for(const auto& frame:frames){uint32_t selected=0,bestDelta=UINT32_MAX;for(uint32_t k=0;k<3;++k)if(frame.bin[k]){uint32_t bin=frame.bin[k]-1,delta=bin>winner?bin-winner:winner-bin;delta=std::min(delta,scanBeams-delta);if(delta<bestDelta){bestDelta=delta;selected=k;}}
   const double angle=double(frame.bin[selected]-1)*step,weight=frame.weight[selected];x+=weight*std::cos(angle*radians);y+=weight*std::sin(angle*radians);selectedWeight+=weight;
   for(double weight:frame.weight)totalWeight+=weight;}
  const double concentration=selectedWeight?std::hypot(x,y)/selectedWeight:0;
  const double meanLocal=normalizeDegrees(std::atan2(y,x)/radians);
  const double uncertainty=std::max({step/2.0,config.minimumUncertaintyDeg,std::min(180.0,std::sqrt(std::max(0.0,-2.0*std::log(std::max(concentration,1e-12))))/radians)});
  const double runner=independent.size()>1?histogram[independent[1]]:0;
  uint32_t temporalQuality=runner>0?uint32_t(std::clamp(std::floor(10*std::log10(histogram[winner]/runner)),0.0,15.0)):15u;
  if(legacyCount==count){uint32_t seen=0;for(temporalQuality=0;temporalQuality<legacyQuality.size();++temporalQuality){seen+=legacyQuality[temporalQuality];if(seen>(count-1)/2)break;}}
  char id[5]={hex[(w[base+12]>>12)&15],hex[(w[base+12]>>8)&15],hex[(w[base+12]>>4)&15],hex[w[base+12]&15],0};
  out.StartObject();out.Key("id");out.String(id);out.Key("state");out.String(stateName(state));
  out.Key("delay");out.Double(realPair(w,base));out.Key("doppler");out.Double(realPair(w,base+2));
  out.Key("bearing_deg_true");out.Double(normalizeDegrees(config.xAxisBearingDegTrue-meanLocal));
  out.Key("current_bearing_deg_true");out.Double(normalizeDegrees(config.xAxisBearingDegTrue-double(current-1)*step));
  out.Key("uncertainty_deg");out.Double(uncertainty);out.Key("temporal_consistency");out.Double(concentration);
  out.Key("consistent_samples");out.Uint(count);out.Key("peak_separation_db");out.Uint(temporalQuality);
  out.Key("current_peak_separation_db");out.Uint(currentQuality);
  out.Key("resolved");out.Bool(count>=config.minConsistent&&concentration>=.65&&uncertainty<=45&&
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
