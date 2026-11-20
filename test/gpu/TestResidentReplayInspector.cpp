#include "ResidentReplayFixture.h"
#include "process/analysis/ResidentReplayInspector.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
using namespace blah2::resident;
namespace rr=blah2::resident_replay;
namespace {
unsigned checks=0;
void check(bool ok,const char* message){++checks;if(!ok)throw std::runtime_error(message);}
template<class F>void reject(F f){bool failed=false;try{f();}catch(const std::invalid_argument&){failed=true;}check(failed,"Invalid sequence accepted");}
Config configuration(){
  Config c;c.inputChannels=2;auto& g=c.geometry;
  g.range=12;g.doppler=7;g.delays=3;g.channels=2;g.delayMin=-1;g.clutterSamples=64;g.clutterBins=4;
  c.correlation=9;c.sampleRate=128000;c.epoch=9;c.surveillanceChannels={0,1};c.delayAxis={-1,0,1};
  const double r=1.0/(63.0/128000);c.dopplerAxis={-3*r,-2*r,-r,0,r,2*r,3*r};
  c.spectrumBins=64;c.reference.channels={0,1};c.reference.analysisSamples=64;
  c.detector.enabled=true;c.detector.guard=0;c.detector.train=2;c.detector.pfa=.001;
  c.detector.centroidDopplerResolution=2;c.detector.integrationCpis=4;
  c.fusion={FusionMode::coherentRegularPolygon,12,539000000,.2,0,-1.2566370614359172,-.75,6,{}, {}};
  c.detector.integrationMotionBins=track_before_detect::encodeMotionBins(1,1);
  c.detector.integrationBeamTolerance=1;
  c.tracker.enabled=true;c.tracker.capacity=2;c.tracker.m=4;c.tracker.n=13;c.tracker.nDelete=3;
  c.tracker.cpi=63.0/128000;c.tracker.rangeRes=299792458.0/128000;c.tracker.lambda=.6;
  c.device="cpu-contract-only";rr::test::enable(c);c.replay.maxFrames=4;validateConfig(c);return c;
}
std::string hex(const rr::Hash& h){
  static const char digits[]="0123456789abcdef";
  std::string result;for(uint8_t b:h){result+=digits[b>>4];result+=digits[b&15];}return result;
}
void fileChecks(const std::vector<rr::Identity>& ids,const std::vector<std::vector<uint8_t>>& frames){
  namespace fs=std::filesystem;
  const auto path=fs::temp_directory_path()/("resident-replay-inspector-test-"+
    std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  check(fs::create_directory(path),"Cannot create isolated test directory");
  struct Cleanup{fs::path path;~Cleanup(){std::error_code ignored;fs::remove_all(path,ignored);}} cleanup{path};
  const auto manifest=path/"expected.txt",snapshot=path/"frame.vwrps";
  {
    std::ofstream out(manifest);out<<"# Independent synthetic contract identities\n\n";
    for(const auto& id:ids)out<<id.epoch<<' '<<id.logicalFrame<<' '<<id.generation<<' '<<id.consumedSequence<<' '
      <<id.signalTimestampMs<<' '<<id.trackerTransaction<<' '<<hex(id.configHash)<<' '<<hex(id.moduleHash)<<' '
      <<hex(id.rawIqHash)<<' '<<hex(id.deviceHash)<<'\n';
  }
  const auto parsed=rr::readInspectorManifest(manifest.string());
  check(parsed.size()==ids.size()&&parsed[3].signalTimestampMs==ids[3].signalTimestampMs&&
    parsed[3].rawIqHash==ids[3].rawIqHash,"Manifest parse differs");
  {std::ofstream out(snapshot,std::ios::binary);out.write(reinterpret_cast<const char*>(frames[0].data()),std::streamsize(frames[0].size()));}
  check(rr::readInspectorSnapshot(snapshot.string())==frames[0],"Snapshot file read differs");
  for(const std::string& value:{std::string("1 2\n"),std::string("-1 1 1 1 1 0 "),std::string("18446744073709551616 1 1 1 1 0 ")}){
    {std::ofstream out(manifest);out<<value<<hex(ids[0].configHash)<<' '<<hex(ids[0].moduleHash)<<' '
      <<hex(ids[0].rawIqHash)<<' '<<hex(ids[0].deviceHash)<<'\n';}
    reject([&]{rr::readInspectorManifest(manifest.string());});
  }
  {std::ofstream out(manifest);out<<std::string(4097,'a');}
  reject([&]{rr::readInspectorManifest(manifest.string());});
  {std::ofstream out(snapshot);out<<"truncated";}
  reject([&]{rr::readInspectorSnapshot(snapshot.string());});
  reject([&]{rr::readInspectorSnapshot(manifest.string());});
  reject([&]{rr::readInspectorManifest((path/"absent").string());});
}
}
int main(){try{
  const auto c=configuration();
  std::vector<rr::Identity> ids;
  std::vector<std::vector<uint8_t>> frames;
  for(uint32_t i=1;i<=4;++i){
    auto s=rr::test::fixture(c,i,{i,100+2*i,1000+i},i);
    s.covarianceValid=i>1;
    auto& metadata=s.blocks.at(uint32_t(rr::BlockKind::postprocessMetadata)-1).bytes;
    wire::Writer{{metadata.data(),metadata.size()}}.u32(11*4,i);
    auto& tracker=s.blocks.at(uint32_t(rr::BlockKind::trackerCheckpoint)-1).bytes;
    wire::Writer{{tracker.data(),tracker.size()}}.u64(16,1);
    ids.push_back(s.identity);frames.push_back(rr::encode(s,c));
  }
  const auto make=[&]{return rr::SequenceInspector(c.replay.sourceHash,c.replay.rfHash,ids,3);};
  fileChecks(ids,frames);
  const auto append=[&](rr::SequenceInspector& inspector,size_t i){return inspector.append(frames[i].data(),frames[i].size());};
  auto inspector=make();
  for(size_t i=0;i<4;++i){const auto f=append(inspector,i);
    check(f.identity.logicalFrame==i+1,"Frame identity differs");
    check(f.warmup==(i<3)&&f.historyReady==(i==3)&&f.historyAvailable==i+1,"Warmup/history reporting differs");
    check(f.covarianceValid==(i>0)&&f.detections==i+1&&f.tracks==1,"Covariance/count reporting differs");}
  inspector.finish();
  reject([&]{append(inspector,3);});
  reject([&]{auto s=make();append(s,0);s.finish();});
  reject([&]{auto s=make();append(s,0);append(s,0);});
  reject([&]{auto s=make();append(s,0);append(s,2);});
  reject([&]{auto s=make();append(s,1);});
  auto wrong=c.replay.sourceHash;wrong[0]^=1;
  reject([&]{rr::SequenceInspector s(wrong,c.replay.rfHash,ids,3);append(s,0);});
  reject([&]{rr::SequenceInspector s(c.replay.sourceHash,wrong,ids,3);append(s,0);});
  reject([&]{rr::SequenceInspector s({},c.replay.rfHash,ids,3);});
  reject([&]{rr::SequenceInspector s(c.replay.sourceHash,c.replay.rfHash,{},0);});
  reject([&]{rr::SequenceInspector s(c.replay.sourceHash,c.replay.rfHash,ids,4);});
  for(size_t field=0;field<10;++field){
    auto wrongIds=ids;
    // A one-frame manifest permits testing every expected identity field
    // independently of sequence validation.
    wrongIds.resize(1);auto& id=wrongIds[0];
    switch(field){case 0:++id.epoch;break;case 1:++id.logicalFrame;break;case 2:++id.generation;break;
      case 3:++id.consumedSequence;break;case 4:++id.signalTimestampMs;break;case 5:++id.trackerTransaction;break;
      case 6:id.configHash[0]^=1;break;case 7:id.moduleHash[0]^=1;break;
      case 8:id.rawIqHash[0]^=1;break;case 9:id.deviceHash[0]^=1;break;}
    reject([&]{rr::SequenceInspector s(c.replay.sourceHash,c.replay.rfHash,wrongIds,0);append(s,0);});
  }
  for(size_t field=0;field<6;++field){auto bad=ids;
    switch(field){case 0:++bad[1].logicalFrame;break;case 1:++bad[1].generation;break;
      case 2:bad[1].consumedSequence=bad[0].consumedSequence;break;
      case 3:bad[1].signalTimestampMs=bad[0].signalTimestampMs-1;break;
      case 4:++bad[1].trackerTransaction;break;case 5:bad[1].deviceHash[0]^=1;break;}
    reject([&]{rr::SequenceInspector s(c.replay.sourceHash,c.replay.rfHash,bad,3);});}
  for(size_t offset:{size_t(72),size_t(256),size_t(512+24*19+8),frames[0].size()-1}){
    auto bad=frames[0];bad[offset]^=1;
    reject([&]{auto s=make();s.append(bad.data(),bad.size());});}
  reject([&]{auto s=make();s.append(frames[0].data(),100);});
  reject([&]{auto s=make();s.append(frames[0].data(),frames[0].size()-1);});
  check(rr::parseInspectorHash(std::string(64,'3'))==c.replay.sourceHash,"Hash parse differs");
  reject([&]{rr::parseInspectorHash(std::string(64,'0'));});
  reject([&]{rr::parseInspectorHash(std::string(64,'x'));});
  reject([&]{rr::parseInspectorHash(std::string(63,'1'));});
  // A production snapshot may have tracking disabled and a one-CPI detector.
  auto simple=c;simple.detector.integrationCpis=1;simple.detector.integrationMotionBins=0;
  simple.detector.integrationBeamTolerance=UINT32_MAX;simple.tracker={};
  rr::test::enable(simple);validateConfig(simple);
  auto snapshot=rr::test::fixture(simple);const auto encoded=rr::encode(snapshot,simple);
  rr::SequenceInspector single(simple.replay.sourceHash,simple.replay.rfHash,{snapshot.identity},0);
  const auto report=single.append(encoded.data(),encoded.size());single.finish();
  check(!report.warmup&&report.historyReady&&report.tracks==0,"Single-CPI/tracker-disabled reporting differs");
  // Two-CPI readiness is emitted separately from the deeper TBD history count.
  // The first frame must remain cold; a reset must become cold again.
  auto pair=c;pair.detector.integrationCpis=2;pair.detector.integrationMotionBins=0;
  pair.detector.integrationBeamTolerance=UINT32_MAX;pair.tracker={};
  rr::test::enable(pair);pair.replay.maxFrames=2;validateConfig(pair);
  auto cold=rr::test::fixture(pair,1,{1,1,1},0),warm=rr::test::fixture(pair,2,{2,2,2},0);
  auto& coldMetadata=cold.blocks.at(uint32_t(rr::BlockKind::postprocessMetadata)-1).bytes;
  auto& warmMetadata=warm.blocks.at(uint32_t(rr::BlockKind::postprocessMetadata)-1).bytes;
  wire::Writer{{coldMetadata.data(),coldMetadata.size()}}.u32(28*4,0);
  wire::Writer{{warmMetadata.data(),warmMetadata.size()}}.u32(28*4,1);
  const auto coldBytes=rr::encode(cold,pair),warmBytes=rr::encode(warm,pair);
  rr::SequenceInspector two(pair.replay.sourceHash,pair.replay.rfHash,
    {cold.identity,warm.identity},1);
  check(!two.append(coldBytes.data(),coldBytes.size()).historyReady,
    "Cold two-CPI frame reported ready");
  check(two.append(warmBytes.data(),warmBytes.size()).historyReady,
    "Warm two-CPI frame did not report ready");
  two.finish();
  rr::SequenceInspector reset(pair.replay.sourceHash,pair.replay.rfHash,{cold.identity},0);
  check(!reset.append(coldBytes.data(),coldBytes.size()).historyReady,
    "Reset two-CPI frame reported ready");
  reset.finish();
  auto invalid=warm;
  auto& invalidMetadata=invalid.blocks.at(uint32_t(rr::BlockKind::postprocessMetadata)-1).bytes;
  wire::Writer{{invalidMetadata.data(),invalidMetadata.size()}}.u32(28*4,2);
  const auto invalidBytes=rr::encode(invalid,pair);
  reject([&]{rr::SequenceInspector malformed(pair.replay.sourceHash,pair.replay.rfHash,
    {invalid.identity},0);malformed.append(invalidBytes.data(),invalidBytes.size());});
  std::cout<<"COMPLETE replay-inspector CPU contract checks="<<checks<<" gpu_execution=0\n";
  return 0;
}catch(const std::exception& e){std::cerr<<"FAIL replay-inspector: "<<e.what()<<'\n';return 1;}}
