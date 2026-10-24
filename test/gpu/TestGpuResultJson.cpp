#include "GpuResultJson.private.h"
#include "ManifoldEvidence.h"
#include "data/IqData.h"
#include "data/Map.h"
#include "data/Detection.h"
#include "data/Track.h"
#include "data/meta/Constants.h"
#include <complex>
#include <vector>
#include <deque>
#include <iostream>
#include <limits>
namespace {
using namespace blah2::gpu_result_json;
void check(bool yes,const char*why){if(!yes)throw std::runtime_error(why);}
template<class F>void rejects(F&&f){bool yes=false;try{f();}catch(const std::invalid_argument&){yes=true;}check(yes,"Malformed result view accepted");}
View<double> view(const std::vector<double>&v){return {v.data(),v.size()};}
void same(const std::string&a,const std::string&b){check(a==b,"Legacy GPU-result JSON bytes differ");rapidjson::Document d;d.Parse(a.c_str());check(!d.HasParseError()&&d.IsObject(),"Malformed GPU-result JSON");}
void spectrumCases(){
 for(const std::vector<std::complex<double>>&values:std::vector<std::vector<std::complex<double>>>{{},{{0,0},{1e-15,0},{1,0},{-3,4}},{{1e200,1e200},{std::numeric_limits<double>::max()/2,std::numeric_limits<double>::max()/2}}}){
  IqData cpu(uint32_t(values.size()));std::vector<double>frequency,db;for(size_t i=0;i<values.size();++i){frequency.push_back(101234.5+i*.125);db.push_back(20*std::log10(std::max(1e-15,std::abs(values[i]))));}
  cpu.update_frequency(frequency);cpu.update_spectrum(values);SpectrumView s{UINT64_C(4294967297),view(frequency),view(db)};same(spectrum(s),cpu.to_json(s.timestamp));
 }
 const std::vector<double>one{0};rejects([&]{spectrum({0,view(one),{nullptr,0}});});const std::vector<double>bad{std::numeric_limits<double>::infinity()};rejects([&]{spectrum({0,view(bad),view(one)});});
 std::cout<<"PASS serializer-spectrum legacy_zero_metadata=1 floor=-300 extreme_finite=1\n";
}
void mapCases(){
 constexpr uint32_t fs=2000000;
 for(bool zero:{true,false}){Map<std::complex<double>>cpu(3,3);cpu.delay={-1,0,1};cpu.doppler={-10,0,10};std::vector<double>km,doppler(cpu.doppler.begin(),cpu.doppler.end()),display;
  for(size_t r=0;r<3;++r){for(size_t c=0;c<3;++c){cpu.data[r][c]=zero?std::complex<double>{0,0}:std::complex<double>{double(r+c+1),double(int(r)-int(c))};}}
  cpu.set_metrics();
  for(int d:cpu.delay){km.push_back(1.0*d*(Constants::c/double(fs))/1000);}
  for(const auto&row:cpu.data)for(const auto z:row)display.push_back(10*std::log10(std::max(1e-30,std::abs(z)))-cpu.noisePower);
  MapView v{7,3,3,cpu.noisePower,cpu.maxPower,view(km),view(doppler),view(display)};same(map(v),cpu.to_json_km(7,fs));
  v.display.count-=1;rejects([&]{map(v);});
 }
 // Preserve actual legacy scientific-notation parser/rewrite behavior.
 Map<std::complex<double>>large(1,1);large.delay={1};large.doppler={1e19};large.data[0][0]={1,0};large.noisePower=1e19;large.maxPower=-1e19;const std::vector<double>km{Constants::c/double(fs)/1000},doppler{1e19},display{-1e19};same(map({UINT64_MAX,1,1,1e19,-1e19,view(km),view(doppler),view(display)}),large.to_json_km(UINT64_MAX,fs));
 rejects([&]{map({0,0,0,0,0,{},{},{}});});
 std::cout<<"PASS serializer-map zero_map_finite=1 legacy_km=1 scientific_roundtrip=1 rectangular_order=1\n";
}
void detectionCases(){
 constexpr uint32_t fs=2000000;
 for(const std::vector<double>&d:std::vector<std::vector<double>>{{},{-1,0,.5,2}}){std::vector<double>f(d.size()),s(d.size()),km;for(size_t i=0;i<d.size();++i){f[i]=double(i)*-.125;s[i]=3.14159+i;km.push_back(1.0*d[i]*(Constants::c/double(fs))/1000);}Detection cpu(d,f,s);same(detections({0,view(km),view(f),view(s)}),cpu.to_json_km(0,fs));}
 Detection big(1.125,1e19,-1e20);const std::vector<double>km{1.125*(Constants::c/double(fs))/1000},f{1e19},s{-1e20};same(detections({4294967297,view(km),view(f),view(s)}),big.to_json_km(4294967297,fs));
 rejects([&]{detections({0,view(km),{},view(s)});});const std::vector<double>nan{std::numeric_limits<double>::quiet_NaN()};rejects([&]{detections({0,view(nan),view(f),view(s)});});
 struct Record {double delay,doppler,snr,delayKm;};const std::vector<Record>records{{1.125,1e19,-1e20,km[0]}};same(detectionRecords(UINT64_C(4294967297),View<Record>{records.data(),records.size()}),big.to_json_km(UINT64_C(4294967297),fs));
 std::vector<uint32_t>guided(6);guided[0]=0x1a;guided[1]=0;double guidedDelay=160,guidedDoppler=-38;
 std::memcpy(guided.data()+2,&guidedDelay,8);std::memcpy(guided.data()+4,&guidedDoppler,8);
 rapidjson::Document guidedDocument;guidedDocument.Parse(detectionRecords(100000,
  View<Record>{records.data(),records.size()},{guided.data(),guided.size()},2400000).c_str());
 check(!guidedDocument.HasParseError()&&guidedDocument["track_guided_shadow"].Size()==1&&
  std::string(guidedDocument["track_guided_shadow"][0]["track_id"].GetString())=="001A"&&
  std::string(guidedDocument["track_guided_shadow"][0]["maturity"].GetString())=="tentative"&&
  std::abs(guidedDocument["track_guided_shadow"][0]["delay_km"].GetDouble()-19.9861639)<1e-5&&
  guidedDocument["track_guided_shadow"][0]["doppler_hz"].GetDouble()==-38,
  "Guided recovery JSON differs");
 rejects([&]{detectionRecords(0,View<Record>{records.data(),records.size()},
  {guided.data(),guided.size()-1},2400000);});
 const std::vector<Record>evidenceRecords{{1,20,9,.15}};const std::vector<uint32_t>cells{3};
 // Channel maps use the production delay-major, FFT-shifted layout. Cell 3 is
 // row 1, column 1 in a 3x2 row-major detector grid, hence shifted row 0.
 std::vector<std::complex<float>>channelMaps(12);channelMaps[3]={1,2};channelMaps[9]={3,4};
 const std::vector<std::complex<double>>weights{{.5,.25},{-.5,.75}};
 const std::vector<uint32_t>mapping{0,1};
 const std::vector<double>actualGains{14.2,14.1};
 rapidjson::Document evidence;evidence.Parse(manifoldEvidence(5,539e6,2.4e6,3,2,2,
  View<Record>{evidenceRecords.data(),evidenceRecords.size()},{cells.data(),cells.size()},
  {channelMaps.data(),channelMaps.size()},{weights.data(),weights.size()},20,2,.8,6.1,1.2,1,1,60,0,1,.01,
  {mapping.data(),mapping.size()},{actualGains.data(),actualGains.size()}).c_str());
 check(!evidence.HasParseError()&&std::string(evidence["schema"].GetString())=="vectorwarp-manifold-evidence-v2"&&
  evidence["data"].Size()==1&&evidence["data"][0]["response"][0][0].GetDouble()==1&&
  evidence["data"][0]["response"][1][1].GetDouble()==4&&evidence["reference_weights"].Size()==2&&
  evidence["reference_state"]["updates"].GetUint64()==2&&
  evidence["reference_state"]["coherent_gain_db"].GetDouble()==6.1&&
  evidence["data"][0]["covariance"].Size()==2&&
  std::string(evidence["bearing_replay"]["reference_anchor"].GetString())=="fixed_site"&&
  evidence["bearing_replay"]["channel_mapping"].Size()==2&&
  evidence["actual_gain_db"].Size()==2&&evidence["actual_gain_db"][0].GetDouble()==14.2,
  "Sparse manifold evidence serialization differs");
 // The initial analysis is update one while the completed-frame counter is
 // still zero. Those counters have different meanings and no ordering rule.
 auto initial=manifoldEvidence(5,539e6,2.4e6,3,2,2,
  View<Record>{evidenceRecords.data(),evidenceRecords.size()},{cells.data(),cells.size()},
  {channelMaps.data(),channelMaps.size()},{weights.data(),weights.size()},0,1,.8,6.1,1.2,1,1,60,0,1,.01,
  {mapping.data(),mapping.size()},{actualGains.data(),actualGains.size()});
 check(!initial.empty(),"Initial reference update rejected");
 Map<std::complex<double>>map0(3,2),map1(3,2);map0.delay={0,1};map1.delay=map0.delay;
 map0.doppler={-20,0,20};map1.doppler=map0.doppler;map0.data[2][1]={5,6};map1.data[2][1]={7,8};
 Detection cpuEvidence(1.2,18,9);std::vector<Map<std::complex<double>>*>cpuMaps{&map0,&map1};
 rapidjson::Document cpuDoc;cpuDoc.Parse(blah2::manifold_evidence::cpu(6,539e6,2e6,
  cpuEvidence,cpuMaps,weights,actualGains).c_str());
 check(!cpuDoc.HasParseError()&&cpuDoc["data"][0]["cell"].GetUint()==5&&
  cpuDoc["data"][0]["response"][0][0].GetDouble()==5&&
  cpuDoc["data"][0]["response"][1][1].GetDouble()==8,
  "CPU sparse manifold evidence serialization differs");
 std::cout<<"PASS serializer-detection empty=1 order=1 legacy_km=1 scientific_roundtrip=1\n";
}
void trackCases(){
 Track cpu;cpu.add(Detection(0,0,1));for(int i=1;i<=500;++i){cpu.set_current(0,Detection(i,-i,1));cpu.set_state(0,"ASSOCIATED");}cpu.promote(0,250,255);
 std::vector<double>d,f;std::vector<uint32_t>state;for(int i=401;i<=500;++i){d.push_back(i);f.push_back(-i);state.push_back(i==500?2u:1u);}PublishedTrack row{0,2,500,-500,0,501,{view(d),view(f),{state.data(),state.size()}}};same(tracks({UINT64_MAX,1,0,0,1,0,{&row,1}}),cpu.to_json(UINT64_MAX));
 rapidjson::Document doc;doc.Parse(tracks({0,1,0,0,1,0,{&row,1}}).c_str());const auto&data=doc["data"][0];check(data["n"].GetUint64()==501&&data["associated_delay"].Size()==100&&data["associated_doppler"].Size()==100&&data["associated_state"].Size()==100&&data["associated_delay"][0].GetDouble()==401&&data["associated_delay"][99].GetDouble()==500&&std::string(data["associated_state"][99].GetString())=="ACTIVE","Original100-history assertions");

 // The compact raw-bank layout retains exactly the 100 public trail points
 // when N is smaller; the lifetime association count remains independent.
 constexpr size_t historyCapacity=100,recordWords=16+8*historyCapacity;
 std::vector<uint32_t>bank(32+recordWords,0);auto pair=[&](size_t at,uint64_t v){bank[at]=uint32_t(v);bank[at+1]=uint32_t(v>>32);};auto real=[&](size_t at,double v){uint64_t bits;std::memcpy(&bits,&v,8);pair(at,bits);};
 const CheckpointBinding binding{1,9,UINT64_C(4294967299),44};bank[0]=1;bank[1]=recordWords*4;bank[2]=1;bank[3]=historyCapacity;pair(4,1);pair(6,500);bank[8]=1;pair(10,9);pair(12,binding.generation);pair(14,44);pair(20,1);pair(24,1);
 constexpr size_t base=32;real(base,500);real(base+2,-500);real(base+4,1);real(base+6,0);pair(base+8,501);bank[base+12]=0;bank[base+13]=1u|(100u<<8)|(1u<<16)|(100u<<24);bank[base+14]=2;bank[base+15]=0;
 for(uint32_t value=401;value<=500;++value){const size_t at=base+16+8*(value%historyCapacity);real(at,value);real(at+2,-double(value));real(at+4,1);bank[at+6]=value==500?2:1;}
 auto raw=[&]{return View<uint32_t>{bank.data(),bank.size()};};same(trackerCheckpoint(UINT64_MAX,raw(),binding),cpu.to_json(UINT64_MAX));
 auto spatial=binding;spatial.beamCount=12;bank[base+16+8*(500%historyCapacity)+7]=12;same(trackerCheckpoint(UINT64_MAX,raw(),spatial),cpu.to_json(UINT64_MAX));bank[base+16+8*(500%historyCapacity)+7]=13;rejects([&]{trackerCheckpoint(0,raw(),spatial);});bank[base+16+8*(500%historyCapacity)+7]=0;
 const uint32_t packedSpatial=12u|(48u<<9u)|(4u<<20u);
 bank[base+16+8*(500%historyCapacity)+7]=packedSpatial;
 rapidjson::Document bearing;bearing.Parse(bearingCheckpoint(7,raw(),spatial,{true,12,4,2,80,30}).c_str());
 check(!bearing.HasParseError()&&bearing["data"].Size()==1&&
  bearing["data"][0]["current_bearing_deg_true"].GetDouble()==87.5&&
  bearing["aperture_uncertainty_floor_deg"].GetDouble()==30&&
  bearing["data"][0]["uncertainty_deg"].GetDouble()==30&&
  bearing["data"][0]["peak_separation_db"].GetUint()==4&&
  bearing["data"][0]["current_peak_separation_db"].GetUint()==4,
  "Fine bearing publication differs");
 // Adjacent fine winners belong to one aperture-limited lobe. Confidence is
 // the temporal median, not whichever quality happened to arrive last.
 bank[base+16+8*(498%historyCapacity)+7]=12u|(48u<<9u)|(2u<<20u);
 bank[base+16+8*(499%historyCapacity)+7]=12u|(1u<<9u)|(4u<<20u);
 bank[base+16+8*(500%historyCapacity)+7]=12u|(2u<<9u)|(6u<<20u);
 rapidjson::Document temporal;temporal.Parse(bearingCheckpoint(8,raw(),spatial,{true,12,4,2,80,30}).c_str());
 check(!temporal.HasParseError()&&temporal["data"][0]["consistent_samples"].GetUint()==3&&
  temporal["data"][0]["peak_separation_db"].GetUint()==4&&
  temporal["data"][0]["current_peak_separation_db"].GetUint()==6&&
  temporal["data"][0]["resolved"].GetBool()&&temporal["data"][0]["candidates"].Size()==1,
  "Temporal independent-lobe bearing confidence differs");
 for(uint32_t value=498;value<=500;++value){const size_t at=base+16+8*(value%historyCapacity);
  real(at,250);real(at+2,value==499?1.0:0.0);}
 rapidjson::Document stationary;stationary.Parse(bearingCheckpoint(8,raw(),spatial,
  {true,12,4,2,80,30,3,0,0,.5,0,3,15}).c_str());
 check(!stationary.HasParseError()&&stationary["data"][0]["stationary_clutter"].GetBool()&&
  !stationary["data"][0]["publishable"].GetBool()&&
  !stationary["data"][0]["resolved"].GetBool(),
  "Stationary bearing classification differs");
 rapidjson::Document fullSpanStationary;fullSpanStationary.Parse(bearingCheckpoint(8,raw(),spatial,
  {true,12,4,2,80,30,3,0,0,.5,0,3,0}).c_str());
 check(!fullSpanStationary.HasParseError()&&
  fullSpanStationary["data"][0]["stationary_clutter"].GetBool(),
  "Full-span stationary bearing classification differs");
 real(base+16+8*(499%historyCapacity)+2,4.0);
 rapidjson::Document crossing;crossing.Parse(bearingCheckpoint(8,raw(),spatial,
  {true,12,4,2,80,30,3,0,0,.5,0,3,0}).c_str());
 check(!crossing.HasParseError()&&!crossing["data"][0]["stationary_clutter"].GetBool(),
  "Transient Doppler crossing was classified as stationary");
 for(uint32_t value=498;value<=500;++value){const size_t at=base+16+8*(value%historyCapacity);
  real(at,value);real(at+2,-double(value));}
 rapidjson::Document strict;strict.Parse(bearingCheckpoint(9,raw(),spatial,{true,12,4,2,80,30,5}).c_str());
 check(!strict.HasParseError()&&strict["min_peak_separation_db"].GetUint()==5&&
  !strict["data"][0]["resolved"].GetBool(),"Bearing confidence threshold differs");
 auto multi=spatial;multi.scanBeamCount=60;auto packed=[](uint32_t a,uint32_t b,uint32_t c,uint32_t rb,uint32_t rc){return 0x80000000u|a|(b<<7u)|(c<<14u)|(rb<<21u)|(rc<<26u);};
 bank[base+16+8*(498%historyCapacity)+7]=packed(10,30,50,25,10);
 bank[base+16+8*(499%historyCapacity)+7]=packed(11,30,50,25,10);
 bank[base+16+8*(500%historyCapacity)+7]=packed(10,30,50,25,10);
 rapidjson::Document adaptive;adaptive.Parse(bearingCheckpoint(10,raw(),multi,{true,12,5,2,80,30,3,1,60}).c_str());
 check(!adaptive.HasParseError()&&std::string(adaptive["method"].GetString())=="covariance_mvdr"&&
  adaptive["fine_beams"].GetUint()==60&&adaptive["data"][0]["consistent_samples"].GetUint()==3&&
  adaptive["data"][0]["candidates"].Size()==3&&!adaptive["data"][0]["resolved"].GetBool(),
  "Adaptive multi-lobe bearing publication differs");
 bank[base+16+8*(498%historyCapacity)+7]=packed(10,11,50,25,10);
 bank[base+16+8*(499%historyCapacity)+7]=packed(30,12,50,25,10);
 bank[base+16+8*(500%historyCapacity)+7]=packed(50,13,30,25,10);
 rapidjson::Document moving;moving.Parse(bearingCheckpoint(11,raw(),multi,{true,12,5,2,80,30,3,1,60,.5,20}).c_str());
 check(!moving.HasParseError()&&moving["data"][0]["hypothesis_tracking"].GetBool()&&
  moving["data"][0]["current_bearing_deg_true"].GetDouble()==8&&
  std::abs(moving["data"][0]["bearing_rate_deg_s"].GetDouble()+12)<.001,
  "Motion-constrained multi-hypothesis bearing path differs");
 bank[base+16+8*(498%historyCapacity)+7]=0;bank[base+16+8*(499%historyCapacity)+7]=0;
 bank[base+16+8*(500%historyCapacity)+7]=packedSpatial;
 same(trackerCheckpoint(UINT64_MAX,raw(),spatial),cpu.to_json(UINT64_MAX));
 bank[base+16+8*(500%historyCapacity)+7]=0;
 rejects([&]{auto wrong=binding;++wrong.generation;trackerCheckpoint(0,raw(),wrong);});rejects([&]{auto v=raw();--v.count;trackerCheckpoint(0,v,binding);});
 bank[base+15]=1;rejects([&]{trackerCheckpoint(0,raw(),binding);});bank[base+15]=0;bank[base+13]^=1u<<16;rejects([&]{trackerCheckpoint(0,raw(),binding);});bank[base+13]^=1u<<16;
 pair(20,0);rejects([&]{trackerCheckpoint(0,raw(),binding);});pair(20,1);
 bank[base+12]=65534;rapidjson::Document wrapped;wrapped.Parse(trackerCheckpoint(0,raw(),binding).c_str());check(std::string(wrapped["data"][0]["id"].GetString())=="FFFE","GPU ID copy/hex differs");bank[base+12]=0;
 Track tentative;tentative.add(Detection(1,1,1));same(tracks({0,1,1,0,0,0,{}}),tentative.to_json(0));Track empty;same(tracks({0,0,0,0,0,0,{}}),empty.to_json(0));
 rejects([&]{tracks({0,1,0,1,0,0,{}});});auto bad=row;bad.state=0;rejects([&]{tracks({0,1,0,0,1,0,{&bad,1}});});bad=row;bad.history.state.count-=1;rejects([&]{tracks({0,1,0,0,1,0,{&bad,1}});});
 std::cout<<"PASS serializer-track gpu_selected_slice=100 raw_checkpoint=1 binding_rejects=1 legacy_ids=1 uint64_counts=1 tentative_omitted=1\n";
}
}
int main()try{spectrumCases();mapCases();detectionCases();trackCases();std::cout<<"COMPLETE GPU-result JSON CPU compatibility groups=4 candidate_dsp_math=0 gpu_execution=0\n";return 0;}catch(const std::exception&e){std::cerr<<"FAIL GPU-result JSON: "<<e.what()<<'\n';return 1;}
