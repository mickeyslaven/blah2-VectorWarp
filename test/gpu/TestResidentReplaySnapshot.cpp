#include "ResidentReplayFixture.h"
#include <iostream>
using namespace blah2::resident;
namespace rr=blah2::resident_replay;
unsigned checks=0;
void check(bool ok,const char* what){++checks;if(!ok)throw std::runtime_error(std::string(what)+" check="+std::to_string(checks));}
template<class F>void reject(F f){bool failed=false;try{f();}catch(const std::invalid_argument&){failed=true;}check(failed,"Malformed replay accepted");}
Config configuration(uint32_t cpis,bool coherent){Config c;c.inputChannels=2;auto& g=c.geometry;
 g.range=12;g.doppler=7;g.delays=3;g.channels=2;g.delayMin=-1;g.clutterSamples=64;g.clutterBins=4;
 c.correlation=9;c.sampleRate=128000;c.epoch=9;c.surveillanceChannels={0,1};c.delayAxis={-1,0,1};
 const double r=1.0/(63.0/128000);c.dopplerAxis={-3*r,-2*r,-r,0,r,2*r,3*r};c.spectrumBins=64;c.reference.channels={0,1};c.reference.analysisSamples=64;
 c.detector.enabled=true;c.detector.guard=0;c.detector.train=2;c.detector.pfa=.001;c.detector.centroidDopplerResolution=2;c.detector.integrationCpis=cpis;
 if(coherent)c.fusion={FusionMode::coherentRegularPolygon,12,539000000,.2,0,-1.2566370614359172,-.75,6};
 if(cpis>2){c.detector.integrationMotionBins=track_before_detect::encodeMotionBins(1,1);
  c.detector.integrationBeamTolerance=coherent?1:UINT32_MAX;}
 c.device="cpu-contract-only";rr::test::enable(c);return c;}
void fractionalDiagnosticGeometry(){
 auto c=configuration(8,true);auto& g=c.geometry;
 g.range=225;g.doppler=601;g.delays=107;g.channels=5;c.inputChannels=5;g.delayMin=-10;
 c.correlation=128;g.clutterSamples=c.correlation*g.doppler;c.sampleRate=2400000;
 c.spectrumBins=1+(g.clutterSamples-1)/uint32_t(std::ceil(
  c.reference.bandwidthHz/(c.sampleRate/g.clutterSamples)));
 c.surveillanceChannels={0,1,2,3,4};c.reference.channels=c.surveillanceChannels;
 c.delayAxis.clear();for(int delay=-10;delay<=96;++delay)c.delayAxis.push_back(delay);
 c.dopplerAxis.clear();const double resolution=1.0/(double(g.clutterSamples)/c.sampleRate);
 for(int row=-300;row<=300;++row)c.dopplerAxis.push_back(row<0?-double(-row)*resolution:double(row)*resolution);
 c.detector.integrationMotionBins=track_before_detect::encodeMotionBins(2,4,true);
 c.detector.integrationBeamTolerance=UINT32_MAX;rr::test::enable(c);validateConfig(c);
 auto s=rr::test::fixture(c);auto& metadata=s.blocks[uint32_t(rr::BlockKind::postprocessMetadata)-1].bytes;
 // Prefix + 601*9*8 signed offsets + valid-population alphas/bank noise.
 check(metadata.size()==190560,"Diagnostic fractional metadata allocation differs");
 check(s.blocks[uint32_t(rr::BlockKind::refinementWeights)-1].bytes.size()==1920,
  "Disabled angle gate removed allocated refinement weights");
 check(s.blocks[uint32_t(rr::BlockKind::angleHistory)-1].bytes.empty(),
  "Disabled angle gate exported unused angle history");
 check(wire::Reader{{metadata.data(),metadata.size()}}.u32(35*4)==4320,
  "Fractional lookup table pointer differs from production allocation");
 check(std::any_of(metadata.begin()+17280,metadata.end(),[](uint8_t byte){return byte!=0;}),
  "Diagnostic fixture did not exercise nonzero signed offsets");
 const auto encoded=rr::encode(s,c);
 const auto decoded=rr::decode(encoded.data(),encoded.size(),c,s.identity);
 check(decoded.blocks[4].bytes==metadata,"Fractional offset table changed in round trip");
 const wire::Reader envelope{{encoded.data(),encoded.size()}};
 check(envelope.u64(512+24*4+16)==190560,"Metadata descriptor omitted temporal tables");
 auto corrupted=encoded;const auto tableByte=size_t(envelope.u64(512+24*4+8))+17280;
 corrupted[tableByte]^=1;reject([&]{rr::decode(corrupted.data(),corrupted.size(),c,s.identity);});
 auto truncated=metadata;metadata.resize(17280);reject([&]{rr::encode(s,c);});metadata=std::move(truncated);
 wire::Writer{{metadata.data(),metadata.size()}}.u32(35*4,0);reject([&]{rr::encode(s,c);});
 wire::Writer{{metadata.data(),metadata.size()}}.u32(35*4,4320);
 std::swap(s.blocks[3],s.blocks[4]);reject([&]{rr::encode(s,c);});std::swap(s.blocks[3],s.blocks[4]);
 s.blocks[uint32_t(rr::BlockKind::refinementWeights)-1].bytes.clear();reject([&]{rr::encode(s,c);});
 auto integer=c;integer.detector.integrationMotionBins=track_before_detect::encodeMotionBins(2,4);
 check(rr::requiredBytes(c)-rr::requiredBytes(integer)==173088,
  "Fractional table not charged to the explicit export/shared-memory bound");
 c.replay.maxBytes=rr::requiredBytes(c)-1;reject([&]{validateConfig(c);});
}
void noncoherentFractionalCarrier(){
 for(uint32_t cpis:{4u,8u}){
  auto c=configuration(cpis,false);
  c.reference.centreHz=509000000;
  c.fusion.frequencyHz=0;
  c.detector.integrationMotionBins=track_before_detect::encodeMotionBins(1,1,true);
  rr::test::enable(c);validateConfig(c);
  auto snapshot=rr::test::fixture(c,cpis);
  const auto encoded=rr::encode(snapshot,c);
  check(rr::encode(rr::decode(encoded.data(),encoded.size(),c,snapshot.identity),c)==encoded,
   "Noncoherent fractional replay round trip differs");
  const wire::Reader metadata{{snapshot.blocks[4].bytes.data(),snapshot.blocks[4].bytes.size()}};
  check(metadata.f64(94*4)==-64.0/c.reference.centreHz,
   "Noncoherent replay used fusion carrier instead of reference carrier");
  wire::Writer{{snapshot.blocks[4].bytes.data(),snapshot.blocks[4].bytes.size()}}.
   f64(94*4,-63.0/c.reference.centreHz);
  reject([&]{rr::encode(snapshot,c);});
 }
}
int main(){try{
 fractionalDiagnosticGeometry();
 noncoherentFractionalCarrier();
 for(uint32_t cpis:{1u,2u,4u,8u}){const auto c=configuration(cpis,cpis>2);validateConfig(c);
  for(uint32_t available=1;available<=cpis+2;++available){auto s=rr::test::fixture(c,available);
   const auto encoded=rr::encode(s,c);check(encoded.size()==rr::requiredBytes(c),"Snapshot size differs");
   const auto decoded=rr::decode(encoded.data(),encoded.size(),c,s.identity);check(rr::encode(decoded,c)==encoded,"Snapshot not deterministic");
   auto retained=encoded;
   for(size_t offset:{size_t(0),size_t(72),size_t(96),size_t(224),size_t(256),size_t(304),size_t(336),size_t(400),size_t(520),size_t(2047),encoded.size()-1}){
    auto bad=encoded;bad[offset]^=1;reject([&]{rr::decode(bad.data(),bad.size(),c,s.identity);});}
   check(retained==encoded,"Failed validation mutated caller state");
   reject([&]{rr::decode(encoded.data(),encoded.size()-1,c,s.identity);});
   auto wrong=s.identity;++wrong.generation;reject([&]{rr::decode(encoded.data(),encoded.size(),c,wrong);});
  }
  auto s=rr::test::fixture(c);auto bad=s;bad.blocks.pop_back();reject([&]{rr::encode(bad,c);});
  bad=s;bad.blocks[4].bytes[4*79]^=1;reject([&]{rr::encode(bad,c);});
  bad=s;wire::Writer{{bad.blocks[11].bytes.data(),bad.blocks[11].bytes.size()}}.f64(8,12.5);reject([&]{rr::encode(bad,c);});
  bad=s;wire::Writer{{bad.blocks[5].bytes.data(),bad.blocks[5].bytes.size()}}.f64(0,std::numeric_limits<double>::infinity());reject([&]{rr::encode(bad,c);});
  auto bounded=c;bounded.replay.maxBytes=rr::requiredBytes(c)-1;reject([&]{validateConfig(bounded);});
  bounded=c;bounded.replay.maxFrames=rr::frameLimit+1;reject([&]{validateConfig(bounded);});
  bounded=c;bounded.replay.actualGainDb.pop_back();reject([&]{validateConfig(bounded);});
  bounded=c;bounded.replay.configuredPfa*=2;reject([&]{validateConfig(bounded);});
 }
 auto tracked=configuration(1,false);tracked.tracker.enabled=true;tracked.tracker.capacity=2;
 tracked.tracker.m=4;tracked.tracker.n=6;tracked.tracker.nDelete=3;
 tracked.tracker.cpi=63.0/128000;tracked.tracker.rangeRes=299792458.0/128000;tracked.tracker.lambda=.6;
 rr::test::enable(tracked);validateConfig(tracked);
 auto trackedSnapshot=rr::test::fixture(tracked,1,{1,101,1001},1);
 auto& trackerBlock=trackedSnapshot.blocks.at(uint32_t(rr::BlockKind::trackerCheckpoint)-1).bytes;
 wire::Writer trackerWriter{{trackerBlock.data(),trackerBlock.size()}};
 const auto encoded=rr::encode(trackedSnapshot,tracked);
 check(rr::encode(rr::decode(encoded.data(),encoded.size(),tracked,trackedSnapshot.identity),tracked)==encoded,
   "Tracker checkpoint replay schema5 round trip differs");
 for(uint32_t schema:{1u,2u,3u,4u}){trackerWriter.u32(0,schema);
  reject([&]{rr::encode(trackedSnapshot,tracked);});}
 trackerWriter.u32(0,5);trackerWriter.u32(108,1);
 reject([&]{rr::encode(trackedSnapshot,tracked);});
 auto promoted=configuration(4,true);promoted.tracker=tracked.tracker;
 promoted.tracker.n=13;rr::test::enable(promoted);validateConfig(promoted);
 auto promotedSnapshot=rr::test::fixture(promoted,1,{1,101,1001},1);
 auto& promotedTracker=promotedSnapshot.blocks.at(
  uint32_t(rr::BlockKind::trackerCheckpoint)-1).bytes;
 wire::Writer promotedWriter{{promotedTracker.data(),promotedTracker.size()}};
 const auto promotedBytes=rr::encode(promotedSnapshot,promoted);
 check(rr::encode(rr::decode(promotedBytes.data(),promotedBytes.size(),promoted,
  promotedSnapshot.identity),promoted)==promotedBytes,
  "Tracker checkpoint replay disjoint schema5 round trip differs");
 for(uint32_t schema:{1u,2u,3u,4u}){promotedWriter.u32(0,schema);
  reject([&]{rr::encode(promotedSnapshot,promoted);});}
 promotedWriter.u32(0,5);promotedWriter.u32(108,0);
 reject([&]{rr::encode(promotedSnapshot,promoted);});
 auto off=configuration(1,false);off.replay={};off.presence=wire::mandatoryPresence(off);validateConfig(off);
 auto fractional=configuration(8,true);
 fractional.detector.integrationMotionBins=track_before_detect::encodeMotionBins(1,1,true);
 validateConfig(fractional);
 auto fractionalSnapshot=rr::test::fixture(fractional,8);
 const auto fractionalBytes=rr::encode(fractionalSnapshot,fractional);
 check(rr::encode(rr::decode(fractionalBytes.data(),fractionalBytes.size(),fractional,
   fractionalSnapshot.identity),fractional)==fractionalBytes,"Fractional replay round trip differs");
 const wire::Reader fractionalMetadata{{fractionalSnapshot.blocks[4].bytes.data(),fractionalSnapshot.blocks[4].bytes.size()}};
 check(fractionalMetadata.f64(94*4)==-64.0/fractional.fusion.frequencyHz,
  "Replay fractional stride omitted consumed correlation tail");
 wire::Writer{{fractionalSnapshot.blocks[4].bytes.data(),fractionalSnapshot.blocks[4].bytes.size()}}.f64(94*4,-63.0/fractional.fusion.frequencyHz);
 reject([&]{rr::encode(fractionalSnapshot,fractional);});
 wire::Writer{{fractionalSnapshot.blocks[4].bytes.data(),fractionalSnapshot.blocks[4].bytes.size()}}.f64(94*4,0);
 reject([&]{rr::encode(fractionalSnapshot,fractional);});
 const auto layout=makeLayout(off,4096);check(std::none_of(layout.sections.begin(),layout.sections.begin()+layout.count,[](const auto& d){return d.id==uint32_t(Section::replaySnapshot);}),"Disabled snapshot mapped storage");
 reject([&]{rr::encode(rr::test::fixture(off),off);});
 std::cout<<"COMPLETE replay-snapshot CPU contract checks="<<checks<<" gpu_execution=0\n";return 0;
 }catch(const std::exception& e){std::cerr<<"FAIL replay-snapshot: "<<e.what()<<'\n';return 1;}}
