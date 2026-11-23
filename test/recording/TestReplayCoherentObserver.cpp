#include "process/analysis/ReplayCoherentObserverViews.h"
#include <rapidjson/document.h>
#include <fstream>
#include <iostream>
#include <limits>
#include <cstdlib>
#include <unistd.h>
using namespace blah2::qualification_observer;
namespace {
void require(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
template<class F> void rejects(F fn){bool rejected=false;try{fn();}catch(const std::exception&){rejected=true;}
  require(rejected,"Observer accepted an invalid request");}
void clutterEvidence(const std::filesystem::path& parent) {
  Config c;c.directory=parent/"clutter";c.clutter=true;
  std::filesystem::create_directory(c.directory);
  auto unsupported=c;unsupported.clutterFilterPolicy="fixed-weights";
  rejects([&]{Observer observer(unsupported,true,true);});
  require(!std::filesystem::exists(std::filesystem::path(c.directory)/"coherent-observer.jsonl"),
    "Unsupported fixed filter policy created evidence");
  Frame f;f.sequence=1;f.captureEpoch=9;f.captureFirst=100;f.captureEnd=104;
  f.frequencyHz=100000000;f.sampleRateHz=100000;f.samplePositionAvailable=true;
  f.channelIds={2,1};f.referenceChannelIds={0};f.backend="cpu";
  const auto sample=[](size_t channel,size_t i){return std::complex<double>(channel+.12345678901234567,i-.5);};
  auto captured=snapshotClutter(4,2,[&](size_t i){return sample(0,i);},
    [&](size_t channel,size_t i){return sample(f.channelIds[channel],i)*.25;},true,-1,2,"cpu",false,true);
  rejects([&]{snapshotClutter(4097,1,[](size_t){return 0.;},[](size_t,size_t){return 0.;},true,0,1,"cpu",false,true);});
  {
    Observer observer(c,true,true);require(observer.observesClutter(),"Clutter observer was not enabled");
    rejects([&]{observer.clutter(f,captured);});
    auto invalid=f;invalid.channelIds={1,1};rejects([&]{observer.input(invalid,3,4,sample);});
    invalid=f;invalid.inputSampleInterval=std::array<uint64_t,2>{101,105};
    rejects([&]{observer.input(invalid,3,4,sample);});
    observer.input(f,3,4,sample);
    Products products;products.rows=1;products.columns=1;products.channels=2;
    products.delayBins={0};products.dopplerHz={0};products.referenceSamples=4;
    products.reference=[&](size_t i){return sample(0,i);};products.caf=sample;
    products.fused=[](size_t){return std::complex<double>(1,0);};
    rejects([&]{observer.products(f,products);});
    invalid=f;invalid.captureEpoch++;rejects([&]{observer.clutter(invalid,captured);});
    invalid=f;invalid.channelIds={1,2};rejects([&]{observer.clutter(invalid,captured);});
    invalid=f;invalid.sampleRateHz++;rejects([&]{observer.clutter(invalid,captured);});
    auto truncated=captured;truncated.filtered[0].pop_back();rejects([&]{observer.clutter(f,truncated);});
    truncated=captured;truncated.reference.pop_back();rejects([&]{observer.clutter(f,truncated);});
    auto nonfinite=captured;nonfinite.filtered[0][0]={std::numeric_limits<double>::quiet_NaN(),0};
    rejects([&]{observer.clutter(f,nonfinite);});
    auto fabricated=captured;fabricated.cpuExecuted=false;rejects([&]{observer.clutter(f,fabricated);});
    fabricated=captured;fabricated.backend="vulkan";rejects([&]{observer.clutter(f,fabricated);});
    fabricated=captured;fabricated.enabled=false;fabricated.cpuExecuted=false;
    rejects([&]{observer.clutter(f,fabricated);});
    observer.clutter(f,captured);rejects([&]{observer.clutter(f,captured);});
    invalid=f;invalid.inputSampleInterval=std::array<uint64_t,2>{101,105};
    rejects([&]{observer.products(invalid,products);});
    observer.products(f,products);
    ++f.sequence;f.captureFirst=104;f.captureEnd=108;observer.input(f,3,4,sample);
    captured.enabled=false;captured.cpuExecuted=false;
    for(size_t channel=0;channel<2;++channel)for(size_t i=0;i<4;++i)
      captured.filtered[channel][i]=sample(f.channelIds[channel],i);
    observer.clutter(f,captured);
  }
  std::ifstream in(std::filesystem::path(c.directory)/"coherent-observer.jsonl");std::string line;unsigned count=0;
  while(std::getline(in,line)){
    rapidjson::Document d;d.Parse(line.c_str());require(!d.HasParseError(),"Invalid clutter JSON");
    if(std::string(d["stage"].GetString())!="clutter")continue;
    require(d["captureEpoch"].GetUint64()==9&&d["channel_ids"][0].GetUint()==2,
      "Clutter lost capture/channel lineage");
    require(d["reference_iq"][0][0].GetDouble()==sample(0,0).real()&&
      d["unfiltered_surveillance"][0][0][0].GetDouble()==sample(2,0).real(),"Clutter changed pre-filter precision/order");
    require(d["delay_min"].GetInt()==-1&&d["delay_max_exclusive"].GetInt()==2&&
      d["relative_diagonal_loading"].GetDouble()==0,"Clutter changed numerical configuration");
    require(std::string(d["filter_policy"].GetString())==(count?"disabled":"refit-weights"),"Clutter invented filter policy");
    require(!d["gpu_executed"].GetBool()&&d["cpu_executed"].GetBool()==(count==0),"Clutter invented execution");
    require(d["filtered_surveillance"][0][0][0].GetDouble()==sample(2,0).real()*(count?1:.25),
      "Clutter changed the production output");++count;
  }
  require(count==2,"Lost or duplicated clutter observations");
}
void fixedClutterEvidence(const std::filesystem::path& parent){
  Config config;config.directory=parent/"fixed-clutter";config.clutter=true;
  std::filesystem::create_directory(config.directory);
  Frame f;f.sequence=3;f.captureEpoch=9;f.captureFirst=106;f.captureEnd=122;
  f.inputSamplesPerOutput=4;f.inputSampleInterval=std::array<uint64_t,2>{108,124};
  f.frequencyHz=100000000;f.sampleRateHz=1000;f.samplePositionAvailable=true;
  f.channelIds={1};f.referenceChannelIds={0};f.backend="vulkan";
  const auto sample=[](size_t,size_t){return std::complex<double>(1,.25);};
  auto captured=snapshotClutter(4,1,[&](size_t i){return sample(0,i);},sample,true,0,1,"vulkan",true,false);
  blah2::clutter_replay::Proof proof{{3,9,108,124},true,std::string(64,'a'),std::string(64,'b')};
  captured.checkpointProof=proof.encode();
  {Observer observer(config,true,true);observer.input(f,2,4,sample);
    auto bad=captured;bad.cpuExecuted=true;rejects([&]{observer.clutter(f,bad);});
    auto stale=proof;stale.frame.first++;bad=captured;bad.checkpointProof=stale.encode();
    rejects([&]{observer.clutter(f,bad);});
    stale=proof;stale.solve=1;bad.checkpointProof=stale.encode();rejects([&]{observer.clutter(f,bad);});
    stale=proof;stale.checkpoint.clear();bad.checkpointProof=stale.encode();rejects([&]{observer.clutter(f,bad);});
    observer.clutter(f,captured);
  }
  std::ifstream in(std::filesystem::path(config.directory)/"coherent-observer.jsonl");std::string line;
  std::getline(in,line);std::getline(in,line);rapidjson::Document doc;doc.Parse(line.c_str());
  require(!doc.HasParseError()&&std::string(doc["filter_policy"].GetString())=="fixed-weights"&&
    std::string(doc["checkpoint_proof"].GetString())==proof.encode(),"Actual checkpoint proof/policy was not serialized");
  require(doc["captureFirst"].GetUint64()==106&&doc["captureEnd"].GetUint64()==122&&
    std::string(doc["input_lineage"]["firstInputSampleExact"].GetString())=="108"&&
    std::string(doc["input_lineage"]["endInputSampleExact"].GetString())=="124",
    "Raw capture and native clutter checkpoint intervals were conflated");
}
// Data-only native wire fixtures exercise the production decoder, never a GPU
// or a claim of physical allocation acceptance.
struct NativeFixture {
  std::vector<uint8_t> evidence=std::vector<uint8_t>(blah2::resident::preparedEvidenceBytes);
  std::array<std::complex<float>,2> maps{};
  std::array<std::complex<double>,2> fused{},reference{},filtered{};
  std::array<double,2> delay{};std::array<double,1> doppler{};
  blah2::resident::Config config;
  blah2::resident::OutputViews output;
  NativeFixture() {
    using namespace blah2::resident;
    config.geometry.channels=1;config.geometry.doppler=1;config.geometry.delays=2;config.sampleRate=100000;
    config.geometry.clutterSamples=2;config.geometry.clutterBins=1;
    output.maps={maps.data(),maps.size()};output.fused={fused.data(),fused.size()};
    output.delayKm={delay.data(),delay.size()};output.doppler={doppler.data(),doppler.size()};
    output.filtered={filtered.data(),filtered.size()};
    output.preparedEvidence={evidence.data(),evidence.size()};
    wire::Writer w{{evidence.data(),evidence.size()}};
    w.u32(0,1);w.u32(4,uint32_t(evidence.size()));w.u32(8,uint32_t(Phase::prepared));
    w.u64(16,0x0123456789abcdefULL);w.u64(24,1);w.u64(40,23);w.u64(56,21);
    w.u64(72,23);w.u64(80,23);w.u32(112,1);w.u32(116,1);
    w.u64(evidence::prepared::nativeReference,23);
    const size_t a=evidence::prepared::allocation;
    w.u32(a,1);w.u32(a+4,832);w.u32(a+8,1);w.u32(a+12,1);w.u32(a+16,1);
    w.u64(a+24,31ULL<<20);w.u64(a+32,2ULL<<20);w.u64(a+40,3ULL<<20);w.u64(a+48,7);
    w.u64(a+64,1ULL<<30);w.u64(a+72,256ULL<<20);w.u64(a+80,2ULL<<20);w.u32(a+88,1);
  }
  Products products(uint64_t mapping,const blah2::resident::LaneBudget* budget) {
    auto p=residentProducts(config,output,mapping,budget);
    p.referenceSamples=reference.size();p.reference=[this](size_t i){return reference.at(i);};
    return p;
  }
};
void nativeAllocationEvidence(const std::filesystem::path& parent) {
  using namespace blah2::resident;
  const LaneBudget budget(8,8,256ULL<<20);const uint64_t mapping=1ULL<<20;
  NativeFixture native;
  native.reference[0]={.12345678901234567,-.25};native.filtered[0]={1.25,-.75};
  const auto clutter=residentClutter(native.config,native.output,{native.reference.data(),native.reference.size()});
  require(clutter.gpuExecuted&&!clutter.cpuExecuted&&clutter.backend=="vulkan-resident"&&
    clutter.reference[0]==native.reference[0]&&clutter.filtered[0][0]==native.filtered[0],
    "Resident clutter adapter changed native samples or execution");
  rejects([&]{residentClutter(native.config,native.output,{native.reference.data(),1});});
  auto truncated=native.output;truncated.filtered.size=1;
  rejects([&]{residentClutter(native.config,truncated,{native.reference.data(),native.reference.size()});});
  auto one=native.products(mapping,&budget);
  require(one.residentAllocation.limitBytes==budget.allocationLimit(0,mapping),"Lost actual reserved limit");
  require(one.residentAllocation.sessionId()=="0123456789abcdef0000000000000001","Lost native session nonce");
  require(one.logicalFrame==23&&one.consumedSequence==21,"Allocation lost native frame lineage");
  require(one.residentAllocation.sessionId()==native.products(mapping,&budget).residentAllocation.sessionId(),
    "A reused session got a different allocation identity");
  auto single=native.products(mapping,nullptr);
  require(!single.residentAllocation.laneBudgetAvailable,"Invented single-lane aggregate budget");
  rejects([&]{native.products(0,&budget);});
  rejects([&]{native.products(2ULL<<20,&budget);});
  wire::Writer w{{native.evidence.data(),native.evidence.size()}};
  w.u64(16,0);w.u64(24,0);rejects([&]{native.products(mapping,&budget);});
  w.u64(16,0x0123456789abcdefULL);w.u64(24,1);
  w.u64(evidence::prepared::allocation+40,32ULL<<20);rejects([&]{native.products(mapping,&budget);});
  w.u64(evidence::prepared::allocation+40,3ULL<<20);
  w.u64(evidence::prepared::allocation+80,1ULL<<20);rejects([&]{native.products(mapping,&budget);});
  w.u64(evidence::prepared::allocation+80,2ULL<<20);
  blah2::qualification_observer::Config c;c.directory=parent/"resident";std::filesystem::create_directory(c.directory);
  blah2::qualification_observer::Frame f;f.sequence=1;f.captureEpoch=9;f.captureFirst=100;f.captureEnd=102;
  f.frequencyHz=100000000;f.sampleRateHz=100000;f.samplePositionAvailable=true;
  f.channelIds={1};f.referenceChannelIds={0};f.backend="vulkan-resident";
  const auto sample=[](size_t,size_t){return std::complex<double>{};};
  {
    Observer observer(c,true,true);
    for(uint32_t lane=0;lane<8;++lane){
      f.lane="lane-"+std::to_string(lane);w.u64(24,lane+1);
      observer.input(f,2,2,sample);
      observer.products(f,native.products(mapping,&budget));
    }
  }
  std::ifstream in(std::filesystem::path(c.directory)/"coherent-observer.jsonl");std::string line;
  std::map<std::string,uint64_t> sessions;unsigned products=0;
  while(std::getline(in,line)){
    rapidjson::Document d;d.Parse(line.c_str());require(!d.HasParseError(),"Invalid allocation JSON");
    if(!d.HasMember("resident_allocation"))continue;
    const auto& a=d["resident_allocation"];
    require(d["captureEpoch"].GetUint64()==9&&d["captureFirst"].GetUint64()==100&&
      d["captureEnd"].GetUint64()==102&&d["sequence"].GetUint64()==1,"Allocation has no exact capture lineage");
    require(d["resident_proof"]["logical_frame"].GetUint64()==23&&
      d["resident_proof"]["consumed_sequence"].GetUint64()==21,"Allocation has no native frame lineage");
    require(a["used_bytes"].GetUint64()==(2ULL<<20)&&a["peak_bytes"].GetUint64()==(3ULL<<20)&&
      a["limit_bytes"].GetUint64()==(31ULL<<20)&&a["live_allocations"].GetUint64()==7,
      "Observer changed native allocator counters");
    require(a["aggregate_cap_bytes"].GetUint64()==budget.totalBytes&&
      a["aggregate_session_cap"].GetUint()==budget.sessions&&
      a["session_reservation_bytes"].GetUint64()==budget.bytesPerSession,
      "Observer changed actual aggregate budget");
    require(a["shared_reservation_bytes"].GetUint64()==mapping,"Lost actual IPC mapping reservation");
    const auto bytes=a["peak_bytes"].GetUint64()+a["shared_reservation_bytes"].GetUint64();
    sessions[a["session_id"].GetString()]=bytes;
    // The same session named by another observation is counted once.
    sessions[a["session_id"].GetString()]=bytes;++products;
  }
  uint64_t total=0;for(const auto& entry:sessions)total+=entry.second;
  require(products==8&&sessions.size()==8&&total==(32ULL<<20)&&total<=budget.totalBytes,
    "Eight-lane unique-session allocation receipt failed");
  c.directory=parent/"single-resident";std::filesystem::create_directory(c.directory);
  {Observer observer(c,true,true);observer.input(f,2,2,sample);observer.products(f,single);}
  std::ifstream singleInput(std::filesystem::path(c.directory)/"coherent-observer.jsonl");
  std::getline(singleInput,line);std::getline(singleInput,line);rapidjson::Document d;d.Parse(line.c_str());
  require(d["resident_allocation"]["aggregate_cap_bytes"].IsNull()&&
    d["resident_allocation"]["session_reservation_bytes"].IsNull()&&
    d["resident_allocation"]["aggregate_session_cap"].IsNull(),"Single session invented aggregate limits");
}
}
int main(){
  char temporary[]="/tmp/vectorwarp-observer-test-XXXXXX";
  const auto* made=mkdtemp(temporary);if(!made)return 1;
  const std::filesystem::path directory(made);
  try{
    Config c;c.directory=directory;c.maxFrames=2;c.maxSamplesPerChannel=2;c.maxMapCells=2;
    rejects([&]{Observer x(c,false,true);});rejects([&]{Observer x(c,true,false);});
    auto invalid=c;invalid.maxFrames=65;rejects([&]{Observer x(invalid,true,true);});
    invalid=c;invalid.maxSamplesPerChannel=4097;rejects([&]{Observer x(invalid,true,true);});
    invalid=c;invalid.maxMapCells=8193;rejects([&]{Observer x(invalid,true,true);});
    invalid=c;invalid.maxBytes=(256ULL<<20)+1;rejects([&]{Observer x(invalid,true,true);});
    Frame f;f.sequence=1;f.captureEpoch=7;f.captureFirst=100;f.captureEnd=102;
    f.frequencyHz=100000000;f.sampleRateHz=100000;f.samplePositionAvailable=true;
    f.channelIds={1};f.referenceChannelIds={0};f.actualGainDb={12.25,-3.5};f.backend="cpu";
    const auto sample=[](size_t ch,size_t i){return std::complex<double>(double(ch)+.12345678901234567,double(i)-.25);};
    Products p;p.rows=1;p.columns=2;p.channels=1;p.delayBins={0,1};p.dopplerHz={0};p.referenceSamples=2;
    p.reference=[&](size_t i){return sample(0,i);};p.caf=sample;p.fused=[](size_t i){return std::complex<double>(double(i)+.5,0);};
    {
      Observer observer(c,true,true);
      require(!observer.observesClutter(),"Clutter observer must be off by default");
      rejects([&]{observer.clutter(f,Clutter{});});
      rejects([&]{Observer overwrite(c,true,true);});
      rejects([&]{observer.products(f,p);});
      auto bad=f;bad.samplePositionAvailable=false;
      rejects([&]{observer.input(bad,2,2,sample);});
      rejects([&]{observer.input(f,2,3,sample);});
      observer.input(f,2,2,sample);
      rejects([&]{observer.input(f,2,2,sample);});
      bad=f;bad.captureEpoch=8;rejects([&]{observer.products(bad,p);});
      auto badProducts=p;badProducts.columns=3;rejects([&]{observer.products(f,badProducts);});
      badProducts=p;badProducts.residentProofAvailable=true;rejects([&]{observer.products(f,badProducts);});
      badProducts=p;badProducts.residentAllocation.available=true;rejects([&]{observer.products(f,badProducts);});
      observer.products(f,p);rejects([&]{observer.products(f,p);});
      ++f.sequence;f.captureFirst=102;f.captureEnd=104;
      observer.input(f,2,2,sample);observer.products(f,p);
      ++f.sequence;f.captureFirst=104;f.captureEnd=106;
      rejects([&]{observer.input(f,2,2,sample);});
    }
    std::ifstream in(directory/"coherent-observer.jsonl");std::string line;unsigned lines=0;
    while(std::getline(in,line)){
      rapidjson::Document d;d.Parse(line.c_str());require(!d.HasParseError(),"Invalid observer JSON");
      require(d["captureEpoch"].GetUint64()==7,"Lost capture epoch");
      require(d["actual_gain_db"][0].GetDouble()==12.25,"Lost exact gain metadata");
      if(d.HasMember("channelized_iq"))require(d["channelized_iq"][0][0][0].GetDouble()==sample(0,0).real(),"Lost full numeric precision");
      if(d.HasMember("fused_beam_id"))require(d["fused_beam_id"].IsNull(),"Invented host beam identity");
      if(d.HasMember("resident_allocation"))require(d["resident_allocation"].IsNull(),"Invented host allocation evidence");
      ++lines;
    }
    require(lines==4,"Lost or duplicated stage records");
    const auto overlap=directory/"one-cpi-overlap";std::filesystem::create_directory(overlap);
    c.directory=overlap;c.maxFrames=2;
    {Observer observer(c,true,true);
      auto first=f;first.sequence=10;first.captureFirst=200;first.captureEnd=202;
      auto next=first;++next.sequence;next.captureFirst=202;next.captureEnd=204;
      observer.input(first,2,2,sample);observer.input(next,2,2,sample);
      observer.products(first,p);observer.products(next,p);
    }
    const auto tiny=directory/"tiny";std::filesystem::create_directory(tiny);c.directory=tiny;c.maxBytes=32;
    {Observer observer(c,true,true);rejects([&]{observer.input(f,2,2,sample);});}
    require(std::filesystem::file_size(tiny/"coherent-observer.jsonl")==0,"Budget overflow published truncated JSON");
    nativeAllocationEvidence(directory);
    clutterEvidence(directory);
    fixedClutterEvidence(directory);
    std::filesystem::remove_all(directory);
    std::cout<<"Replay coherent observer admission, bounds, lineage, precision, allocation and no-overwrite tests passed\n";
  }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
