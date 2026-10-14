#include "GpuResultJson.private.h"
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
  for(size_t r=0;r<3;++r)for(size_t c=0;c<3;++c)cpu.data[r][c]=zero?std::complex<double>{0,0}:std::complex<double>{double(r+c+1),double(int(r)-int(c))};cpu.set_metrics();
  for(int d:cpu.delay)km.push_back(1.0*d*(Constants::c/double(fs))/1000);for(const auto&row:cpu.data)for(const auto z:row)display.push_back(10*std::log10(std::max(1e-30,std::abs(z)))-cpu.noisePower);
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
 Detection big({1.125},{1e19},{-1e20});const std::vector<double>km{1.125*(Constants::c/double(fs))/1000},f{1e19},s{-1e20};same(detections({4294967297,view(km),view(f),view(s)}),big.to_json_km(4294967297,fs));
 rejects([&]{detections({0,view(km),{},view(s)});});const std::vector<double>nan{std::numeric_limits<double>::quiet_NaN()};rejects([&]{detections({0,view(nan),view(f),view(s)});});
 struct Record {double delay,doppler,snr,delayKm;};const std::vector<Record>records{{1.125,1e19,-1e20,km[0]}};same(detectionRecords(UINT64_C(4294967297),View<Record>{records.data(),records.size()}),big.to_json_km(UINT64_C(4294967297),fs));
 std::cout<<"PASS serializer-detection empty=1 order=1 legacy_km=1 scientific_roundtrip=1\n";
}
void trackCases(){
 Track cpu;cpu.add(Detection(0,0,1));for(int i=1;i<=500;++i){cpu.set_current(0,Detection(i,-i,1));cpu.set_state(0,"ASSOCIATED");}cpu.promote(0,250,255);
 std::vector<double>d,f;std::vector<uint32_t>state;for(int i=401;i<=500;++i){d.push_back(i);f.push_back(-i);state.push_back(i==500?2u:1u);}PublishedTrack row{0,2,500,-500,0,501,{view(d),view(f),{state.data(),state.size()}}};same(tracks({UINT64_MAX,1,0,0,1,0,{&row,1}}),cpu.to_json(UINT64_MAX));
 rapidjson::Document doc;doc.Parse(tracks({0,1,0,0,1,0,{&row,1}}).c_str());const auto&data=doc["data"][0];check(data["n"].GetUint64()==501&&data["associated_delay"].Size()==100&&data["associated_doppler"].Size()==100&&data["associated_state"].Size()==100&&data["associated_delay"][0].GetDouble()==401&&data["associated_delay"][99].GetDouble()==500&&std::string(data["associated_state"][99].GetString())=="ACTIVE","Original100-history assertions");

 // Fixed raw-bank layout from the frozen GPU tracker API. CPU Track above is
 // the independent JSON oracle; supplied metadata selects the ring slice.
 std::vector<uint32_t>bank(32+2056,0);auto pair=[&](size_t at,uint64_t v){bank[at]=uint32_t(v);bank[at+1]=uint32_t(v>>32);};auto real=[&](size_t at,double v){uint64_t bits;std::memcpy(&bits,&v,8);pair(at,bits);};
 const CheckpointBinding binding{1,9,UINT64_C(4294967299),44};bank[0]=1;bank[1]=8224;bank[2]=1;bank[3]=255;pair(4,1);pair(6,500);bank[8]=1;pair(10,9);pair(12,binding.generation);pair(14,44);pair(20,1);pair(24,1);
 constexpr size_t base=32;real(base,500);real(base+2,-500);real(base+4,1);real(base+6,0);pair(base+8,501);bank[base+12]=0;bank[base+13]=246u|(255u<<8)|(146u<<16)|(100u<<24);bank[base+14]=2;bank[base+15]=0;
 for(uint32_t value=246;value<=500;++value){const size_t at=base+16+8*(value%255);real(at,value);real(at+2,-double(value));real(at+4,1);bank[at+6]=value==500?2:1;}
 auto raw=[&]{return View<uint32_t>{bank.data(),bank.size()};};same(trackerCheckpoint(UINT64_MAX,raw(),binding),cpu.to_json(UINT64_MAX));
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
