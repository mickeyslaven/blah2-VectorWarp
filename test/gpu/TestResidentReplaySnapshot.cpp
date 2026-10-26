#include "ResidentReplayFixture.h"
#include "process/analysis/ResidentReplayRecorder.h"
#include <iostream>
using namespace blah2::resident;
namespace rr=blah2::resident_replay;
unsigned checks=0;
void check(bool ok,const char* what){++checks;if(!ok)throw std::runtime_error(std::string(what)+" check="+std::to_string(checks));}
template<class F>void reject(F f){bool failed=false;try{f();}catch(const std::invalid_argument&){failed=true;}check(failed,"Malformed replay accepted");}
Config configuration(uint32_t cpis,bool coherent){Config c;auto& g=c.geometry;
 g.range=12;g.doppler=7;g.delays=3;g.channels=2;g.delayMin=-1;g.clutterSamples=64;g.clutterBins=4;
 c.correlation=9;c.sampleRate=128000;c.epoch=9;c.surveillanceChannels={0,1};c.delayAxis={-1,0,1};
 const double r=1.0/(63.0/128000);c.dopplerAxis={-3*r,-2*r,-r,0,r,2*r,3*r};c.spectrumBins=64;c.reference.channels={0,1};c.reference.analysisSamples=64;
 c.detector.enabled=true;c.detector.guard=0;c.detector.train=2;c.detector.pfa=.001;c.detector.centroidDopplerResolution=2;c.detector.integrationCpis=cpis;
 if(coherent)c.fusion={FusionMode::coherentRegularPolygon,12,539000000,.193548,0,-1.2566370614359172,-.75,6};
 if(cpis>2){c.detector.integrationMotionBins=track_before_detect::encodeMotionBins(1,1);c.detector.integrationBeamTolerance=1;}
 c.device="cpu-contract-only";rr::test::enable(c);return c;}
int main(int argc,char** argv){try{
 for(uint32_t cpis:{1u,2u,4u,8u}){const auto c=configuration(cpis,cpis>2);validateConfig(c);
  for(uint32_t available=1;available<=cpis+2;++available){auto s=rr::test::fixture(c,available);
   const auto encoded=rr::encode(s,c);check(encoded.size()==rr::requiredBytes(c),"Snapshot size differs");
   const auto decoded=rr::decode(encoded.data(),encoded.size(),c,s.identity);check(rr::encode(decoded,c)==encoded,"Snapshot not deterministic");
   check(rr::encode(rr::decodeSelfContained(encoded.data(),encoded.size()),c)==encoded,"Self-contained native decode differs");
   auto retained=encoded;
   for(size_t offset:{size_t(0),size_t(72),size_t(96),size_t(224),size_t(256),size_t(304),size_t(336),size_t(400),size_t(520),size_t(2047),encoded.size()-1}){
    auto bad=encoded;bad[offset]^=1;reject([&]{rr::decode(bad.data(),bad.size(),c,s.identity);});
    reject([&]{rr::decodeSelfContained(bad.data(),bad.size());});}
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
 auto off=configuration(1,false);off.replay={};off.presence=wire::mandatoryPresence(off);validateConfig(off);
 const auto layout=makeLayout(off,4096);check(std::none_of(layout.sections.begin(),layout.sections.begin()+layout.count,[](const auto& d){return d.id==uint32_t(Section::replaySnapshot);}),"Disabled snapshot mapped storage");
 reject([&]{rr::encode(rr::test::fixture(off),off);});
 const auto c=configuration(4,true);
 rr::RecorderConfig recorder{true,"/tmp/native-test",1,3,2ULL<<30,9,0,192};
 rr::validateRecorder(recorder,c,true,true);
 reject([&]{rr::validateRecorder(recorder,c,false,true);});
 reject([&]{rr::validateRecorder(recorder,c,true,false);});
 auto unbounded=recorder;unbounded.maxTotalBytes=1;reject([&]{rr::validateRecorder(unbounded,c,true,true);});
 unbounded=recorder;unbounded.lastFrame=4;reject([&]{rr::validateRecorder(unbounded,c,true,true);});
 rr::checkSource(recorder,c,1,true,9,0,64,c.replay.actualGainDb);
 bool sourceRejected=false;try{rr::checkSource(recorder,c,1,true,9,1,65,c.replay.actualGainDb);}catch(const std::runtime_error&){sourceRejected=true;}
 check(sourceRejected,"Wrong source offset accepted");
 if(argc==2){
  auto bridge=c;bridge.epoch=1;
  for(uint32_t frame=1;frame<=3;++frame){auto s=rr::test::fixture(bridge,frame,{frame,frame,900000+frame});
   s.covarianceValid=true;
   wire::Writer covariance{{s.blocks[1].bytes.data(),s.blocks[1].bytes.size()}};
   covariance.f64(0,123.5);covariance.f64(16,7);covariance.f64(24,-2);
   covariance.f64(32,7);covariance.f64(40,2);covariance.f64(48,90);
   wire::Writer weights{{s.blocks[2].bytes.data(),s.blocks[2].bytes.size()}};
   for(uint32_t b=0;b<c.fusion.beams;++b){weights.f32(b*16,float(b+1));weights.f32(b*16+12,-.25f);}
   const auto cells=c.geometry.doppler*c.geometry.delays;
   wire::Writer powers{{s.blocks[9].bytes.data(),s.blocks[9].bytes.size()}};
   for(uint32_t b=0;b<c.fusion.beams;++b)for(uint32_t i=0;i<cells;++i)powers.f64((b*cells+i)*8,100*b+i);
   wire::Writer maps{{s.blocks[12].bytes.data(),s.blocks[12].bytes.size()}};
   for(uint32_t i=0;i<cells*2;++i){maps.f32(i*8,float(i+1));maps.f32(i*8+4,-float(i+1));}
   wire::Writer stats{{s.blocks[10].bytes.data(),s.blocks[10].bytes.size()}};
   for(size_t at=0;at<s.blocks[10].bytes.size();at+=40){stats.f64(at,2);stats.f64(at+8,3);stats.f64(at+16,6);stats.f64(at+24,4);stats.f64(at+32,1);}
   rr::writeExclusive(std::string(argv[1])+"/frame."+std::to_string(frame)+".rps",rr::encode(s,bridge));
  }
 }
 std::cout<<"COMPLETE replay-snapshot CPU contract checks="<<checks<<" gpu_execution=0\n";return 0;
 }catch(const std::exception& e){std::cerr<<"FAIL replay-snapshot: "<<e.what()<<'\n';return 1;}}
