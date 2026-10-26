#include "ResidentReplaySnapshot.h"
#include "process/ambiguity/GpuResidentWire.h"
#include "process/ambiguity/GpuResidentHash.h"
#include "../../../core/TrackBeforeDetect.private.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace blah2::resident_replay {
namespace {
using namespace resident;
using wire::require;
using wire::add;
using wire::mul;
constexpr uint32_t blocks=20;
bool nonzero(const Hash& h){return std::any_of(h.begin(),h.end(),[](auto x){return x!=0;});}
Hash hash(const uint8_t* p,size_t n){Sha256 s;s.add(p,n);return s.finish();}
uint32_t passes(const Config& c){return c.detector.integrationCpis>2?
 track_before_detect::detectorPassCount(c.detector.integrationCpis):1;}
uint32_t refinement(const Config& c){return c.detector.integrationCpis>2?
 (c.bearing.scanBeams?c.bearing.scanBeams:c.fusion.beams*4):0;}
uint64_t metadataBytes(const Config& c){const uint64_t rows=c.geometry.doppler,cols=c.geometry.delays;
 const uint64_t windows=(96+2*rows+cols+1)&~uint64_t(1);
 return 4*(windows+(c.detector.integrationCpis>2?14:8)*cols+2*rows+2*cols);}
std::array<uint64_t,blocks> sizes(const Config& c){const auto& g=c.geometry;
 const uint64_t cells=mul(g.doppler,g.delays),n=g.clutterSamples,ch=g.channels;
 const auto depth=c.detector.integrationCpis>2?c.detector.integrationCpis:1;
 return {256,1024,mul(mul(c.fusion.beams,ch),8),mul(mul(refinement(c),ch),8),
  metadataBytes(c),mul(mul(cells,depth),8),c.detector.integrationCpis<=2||c.detector.integrationBeamTolerance==UINT32_MAX?0:mul(mul(cells,depth),4),
  mul(cells,8),c.detector.integrationCpis>2?mul(mul(cells,passes(c)),8):0,
  mul(mul(cells,std::max(1u,c.fusion.beams)),8),mul(mul(cells,passes(c)),40),
  mul(cells,16),mul(mul(cells,ch),8),mul(mul(n,ch),16),mul(n,16),mul(mul(n,ch),16),
  mul(cells,4),mul(cells,32),c.tracker.enabled?mul(trackerHeaderWords+mul(c.tracker.capacity,trackerRecordWords(c.tracker)),4):0,
  configBytes};}
const Block& block(const ResidentReplaySnapshot& s,BlockKind kind){return s.blocks.at(uint32_t(kind)-1);}
wire::Reader reader(const Block& b){return {{b.bytes.data(),b.bytes.size()}};}
void finite(const Block& b,size_t width,bool positive=false){const auto r=reader(b);
 for(size_t at=0;at<b.bytes.size();at+=width){double v=width==8?r.f64(at):r.f32(at);
  require(std::isfinite(v)&&(!positive||v>=0),"Replay snapshot numeric value invalid");}}
}

uint64_t requiredBytes(const resident::Config& c){uint64_t bytes=headerBytes;
 for(auto n:sizes(c))bytes=add(bytes,n);
 return bytes;}

void validateExportConfig(const resident::Config& c){const auto& x=c.replay;
 if(!x.maxBytes){require(!x.maxFrames&&x.frequencyHz==0&&x.configuredPfa==0&&
  !nonzero(x.sourceHash)&&!nonzero(x.rfHash)&&x.actualGainDb.empty(),"Disabled replay export contains configuration");return;}
 require(x.maxBytes<=byteLimit&&x.maxFrames&&x.maxFrames<=frameLimit&&
  requiredBytes(c)<=x.maxBytes,"Replay export exceeds explicit byte/frame bound");
 require(c.detector.enabled&&std::isfinite(x.frequencyHz)&&x.frequencyHz>0&&
  (c.fusion.mode==resident::FusionMode::noncoherent||x.frequencyHz==c.fusion.frequencyHz)&&
  nonzero(x.sourceHash)&&nonzero(x.rfHash)&&x.actualGainDb.size()==c.geometry.channels,
  "Replay export RF/source/full-gain identity incomplete");
 for(double gain:x.actualGainDb)require(std::isfinite(gain),"Replay export gain is nonfinite");
 require(std::isfinite(x.configuredPfa)&&x.configuredPfa>0&&x.configuredPfa<1&&
  x.configuredPfa/std::max(1u,c.fusion.beams)==c.detector.pfa,
  "Replay export familywise beam correction differs");
}

void validate(const ResidentReplaySnapshot& s,const resident::Config& c){
 validateExportConfig(c);require(c.replay.maxBytes,"Replay export disabled");
 const auto& id=s.identity;
 require(id.epoch==c.epoch&&id.logicalFrame&&id.generation&&id.consumedSequence&&
  id.configHash==resident::wire::configHash(c)&&nonzero(id.moduleHash)&&nonzero(id.rawIqHash)&&nonzero(id.deviceHash)&&
  bool(id.trackerTransaction)==c.tracker.enabled,"Replay snapshot identity invalid");
 require(s.blocks.size()==blocks,"Replay snapshot block coverage incomplete");
 const auto expected=sizes(c);uint64_t total=headerBytes;
 for(size_t i=0;i<blocks;++i){const auto& b=s.blocks[i];
  require(uint32_t(b.kind)==i+1&&b.bytes.size()==expected[i],"Replay snapshot block dimensions/order differ");
  total=add(total,b.bytes.size());}
 require(total<=c.replay.maxBytes,"Replay snapshot byte bound exceeded");
 const auto& raw=block(s,BlockKind::rawIq);
 require(hash(raw.bytes.data(),raw.bytes.size())==id.rawIqHash,"Replay snapshot raw IQ hash differs");
 const auto config=resident::encodeConfig(c);
 require(std::equal(config.begin(),config.end(),block(s,BlockKind::configuration).bytes.begin()),"Replay snapshot config bytes differ");
 const auto cp=reader(block(s,BlockKind::referenceCheckpoint));
 require(cp.u32(0)==1&&cp.u32(4)==256&&cp.u64(8)==c.epoch&&cp.u64(16)==id.logicalFrame&&
  cp.u64(24)==c.geometry.clutterSamples&&cp.u64(32)==c.geometry.channels&&
  cp.u64(56)==c.reference.channels.size(),"Replay snapshot reference checkpoint differs");
 cp.zeros(216,40);
 for(size_t i=c.reference.channels.size();i<8;++i)require(cp.f64(64+16*i)==0&&cp.f64(72+16*i)==0,"Replay unused reference weight nonzero");
 require(cp.f64(192)>=0&&cp.f64(192)<=1&&cp.f64(208)>0,"Replay reference scaling invalid");
 for(auto kind:{BlockKind::referenceCheckpoint}){const auto r=reader(block(s,kind));
  for(size_t at=64;at<216;at+=8)require(std::isfinite(r.f64(at)),"Replay reference numeric state invalid");}
 for(auto kind:{BlockKind::covariance,BlockKind::rawIq,BlockKind::referenceIq,BlockKind::filteredIq,
   BlockKind::fused})finite(block(s,kind),8);
 for(auto kind:{BlockKind::fusionWeights,BlockKind::refinementWeights,BlockKind::channelMaps})finite(block(s,kind),4);
 finite(block(s,BlockKind::currentPower),8,true);finite(block(s,BlockKind::beamPower),8,true);
 const auto fused=reader(block(s,BlockKind::fused));
 for(size_t at=0;at<fused.b.size;at+=16){const double beam=fused.f64(at+8);
  require(fused.f64(at)>=0&&beam>=0&&beam<std::max(1u,c.fusion.beams)&&beam==std::floor(beam),"Replay fused beam identity invalid");}
 const auto m=reader(block(s,BlockKind::postprocessMetadata));
 const auto cells=uint64_t(c.geometry.doppler)*c.geometry.delays;
 const auto motion=track_before_detect::decodeMotionBins(c.detector.integrationMotionBins);
 require(!m.u32(0)&&m.u32(32*4)==c.geometry.doppler&&m.u32(33*4)==c.geometry.delays&&
  m.u32(34*4)==cells&&m.u32(72*4)==uint32_t(c.epoch)&&m.u32(73*4)==uint32_t(c.epoch>>32)&&
  m.u32(74*4)==c.detector.integrationCpis&&m.u32(75*4)==c.fusion.beams&&
  m.u32(76*4)==motion.delay&&m.u32(93*4)==motion.doppler&&
  m.u32(77*4)==c.detector.integrationBeamTolerance&&m.u32(11*4)<=cells,
  "Replay snapshot temporal/detector metadata differs");
 const bool tbd=c.detector.integrationCpis>2;
 const uint32_t available=m.u32(79*4),depth=tbd?c.detector.integrationCpis:1;
 require(tbd?(available>0):available==0,"Replay snapshot temporal advance invalid");
 const auto history=reader(block(s,BlockKind::powerHistory));
 for(uint64_t i=0;i<cells*depth;++i){const double v=history.f64(i*8);
  const bool unused=tbd&&available<depth&&i/cells>=available;
  require(unused?std::isnan(v):(std::isfinite(v)&&v>=0),"Replay snapshot history/warmup differs");}
 const auto angle=reader(block(s,BlockKind::angleHistory));
 for(size_t at=0;at<angle.b.size;at+=4){const auto value=angle.u32(at);
  require(value<=c.fusion.beams,"Replay snapshot beam history invalid");}
 const auto temporal=reader(block(s,BlockKind::temporalBanks));
 for(size_t at=0;at<temporal.b.size;at+=8){const uint32_t pass=uint32_t(at/8/cells);
  const uint32_t length=pass?track_before_detect::bankLength(pass-1):2;
  const double v=temporal.f64(at);require(available<length?std::isnan(v):
   (std::isfinite(v)&&v>=-1),"Replay snapshot temporal bank invalid");}
 const auto windows=(96+2*c.geometry.doppler+c.geometry.delays+1)&~1u;
 require(m.u32(46*4)==windows&&m.u32(84*4)==(tbd?14u:8u),"Replay snapshot CFAR layout differs");
 for(uint32_t row=0;row<c.geometry.doppler;++row)require(m.f64((96+2*row)*4)==c.dopplerAxis[row],"Replay Doppler axis differs");
 for(uint32_t j=0;j<c.geometry.delays;++j){const int column=int(j),n=int(c.geometry.delays),g=c.detector.guard,t=c.detector.train;
  require(m.u32((96+2*c.geometry.doppler+j)*4)==uint32_t(c.delayAxis[j]),"Replay delay axis differs");
  const uint32_t bounds[]={uint32_t(std::max(0,column-g-t)),uint32_t(std::max(0,column-g)),
   uint32_t(std::min(n,column+g+1)),uint32_t(std::min(n,column+g+t+1))};
  const auto at=windows+(tbd?14u:8u)*j,count=bounds[1]-bounds[0]+bounds[3]-bounds[2];
  for(uint32_t i=0;i<4;++i)require(m.u32((at+i)*4)==bounds[i],"Replay snapshot training membership differs");
  require(m.u32((at+4)*4)==count,"Replay snapshot training count differs");
  for(uint32_t pass=0;pass<passes(c);++pass){const auto cpis=tbd?(pass?track_before_detect::bankLength(pass-1):2):c.detector.integrationCpis;
   const double pfa=tbd?track_before_detect::passPfa(c.detector.pfa,c.detector.integrationCpis,motion.delay,motion.doppler,pass):c.detector.pfa;
   const double alpha=count?(cpis==1?count*(std::pow(pfa,-1.0/count)-1):track_before_detect::cfarAlpha(pfa,count,cpis)):0;
   require(m.f64((at+6+2*pass)*4)==alpha,"Replay snapshot CFAR alpha differs");}}
 const auto stats=reader(block(s,BlockKind::cfarStatistics));
 for(size_t at=0;at<stats.b.size;at+=40){const auto flags=stats.f64(at+32);
  require(flags==0||flags==1||flags==3||flags==5,"Replay snapshot CFAR decision invalid");
  for(size_t k=0;k<32;k+=8){double v=stats.f64(at+k);require(flags?
   std::isfinite(v):(!std::isinf(v)),"Replay snapshot CFAR numeric statistic invalid");}}
 const auto detections=reader(block(s,BlockKind::detections));const auto detectionCount=m.u32(11*4);
 for(size_t at=0;at<size_t(detectionCount)*32;at+=8)require(std::isfinite(detections.f64(at)),"Replay detection nonfinite");
 detections.zeros(size_t(detectionCount)*32,detections.b.size-size_t(detectionCount)*32);
 if(c.tracker.enabled){const auto tracker=reader(block(s,BlockKind::trackerCheckpoint));
  const auto count=tracker.u64(16),used=trackerHeaderWords+count*trackerRecordWords(c.tracker);
  require(tracker.u32(0)==1&&tracker.u32(4)==trackerRecordWords(c.tracker)*4&&
   tracker.u32(8)==c.tracker.capacity&&tracker.u32(12)==trackerHistoryCapacity(c.tracker)&&
   count<=c.tracker.capacity&&tracker.u64(40)==c.epoch&&tracker.u64(48)==id.generation&&
   tracker.u64(56)==id.consumedSequence,"Replay tracker identity/shape differs");
  tracker.zeros(size_t(used)*4,tracker.b.size-size_t(used)*4);}
}

std::vector<uint8_t> encode(const ResidentReplaySnapshot& s,const resident::Config& c){
 validate(s,c);std::vector<uint8_t> out(size_t(requiredBytes(c)),0);resident::wire::Writer w{{out.data(),out.size()}};
 w.u32(0,0x52505331);w.u32(4,version);w.u32(8,headerBytes);w.u32(12,blocks);w.u64(16,out.size());
 const auto& x=s.identity;const uint64_t values[]={x.epoch,x.logicalFrame,x.generation,x.consumedSequence,x.signalTimestampMs,x.trackerTransaction};
 for(size_t i=0;i<6;++i)w.u64(24+8*i,values[i]);
 w.u32(72,s.covarianceValid);
 w.u32(76,c.geometry.doppler);w.u32(80,c.geometry.delays);w.u32(84,c.geometry.channels);w.u32(88,c.geometry.clutterSamples);w.u32(92,uint32_t(c.reference.channels.size()));
 w.copy(96,x.configHash.data(),32);w.copy(128,x.moduleHash.data(),32);w.copy(160,x.rawIqHash.data(),32);
 w.copy(192,c.replay.sourceHash.data(),32);w.copy(224,c.replay.rfHash.data(),32);
 const auto motion=track_before_detect::decodeMotionBins(c.detector.integrationMotionBins);
 const uint32_t counts[]={c.fusion.beams,refinement(c),c.detector.integrationCpis,motion.delay,motion.doppler,c.detector.integrationBeamTolerance,std::max(1u,c.fusion.beams),passes(c)};
 for(size_t i=0;i<8;++i)w.u32(288+4*i,counts[i]);
 w.f64(320,c.replay.frequencyHz);w.f64(328,c.sampleRate);w.f64(336,c.replay.configuredPfa);w.f64(344,c.detector.pfa);
 for(uint32_t pass=0;pass<passes(c);++pass)w.f64(352+8*pass,c.detector.integrationCpis>2?
  track_before_detect::passPfa(c.detector.pfa,c.detector.integrationCpis,motion.delay,motion.doppler,pass):c.detector.pfa);
 w.f64(384,c.detector.integrationCpis>2?track_before_detect::trialCount(c.detector.integrationCpis,motion.delay,motion.doppler):1);
 for(size_t i=0;i<c.replay.actualGainDb.size();++i)w.f64(400+8*i,c.replay.actualGainDb[i]);
 w.u64(464,c.replay.maxBytes);w.u32(472,c.replay.maxFrames);
 w.copy(480,x.deviceHash.data(),32);
 size_t at=headerBytes;for(size_t i=0;i<s.blocks.size();++i){const auto& b=s.blocks[i];const size_t d=512+24*i;
  w.u32(d,uint32_t(b.kind));w.u64(d+8,at);w.u64(d+16,b.bytes.size());w.copy(at,b.bytes.data(),b.bytes.size());at+=b.bytes.size();}
 // Bind the entire envelope, including covariance-valid and reserved bytes;
 // the digest field itself is still zero while calculating this checksum.
 const auto digest=hash(out.data(),out.size());w.copy(256,digest.data(),32);return out;
}

ResidentReplaySnapshot decode(const uint8_t* data,size_t bytes,const resident::Config& c,const Identity& expected){
 validateExportConfig(c);require(c.replay.maxBytes&&data&&bytes==requiredBytes(c)&&bytes<=c.replay.maxBytes,"Replay snapshot input bound differs");
 resident::wire::Reader r{{data,bytes}};require(r.u32(0)==0x52505331&&r.u32(4)==version&&r.u32(8)==headerBytes&&r.u32(12)==blocks&&r.u64(16)==bytes&&r.u32(72)<=1,"Replay snapshot envelope invalid");
 ResidentReplaySnapshot s;s.identity={r.u64(24),r.u64(32),r.u64(40),r.u64(48),r.u64(56),r.u64(64)};s.covarianceValid=r.u32(72)!=0;
 std::memcpy(s.identity.configHash.data(),data+96,32);std::memcpy(s.identity.moduleHash.data(),data+128,32);std::memcpy(s.identity.rawIqHash.data(),data+160,32);std::memcpy(s.identity.deviceHash.data(),data+480,32);
 const auto& x=s.identity;require(x.epoch==expected.epoch&&x.logicalFrame==expected.logicalFrame&&x.generation==expected.generation&&x.consumedSequence==expected.consumedSequence&&x.signalTimestampMs==expected.signalTimestampMs&&x.trackerTransaction==expected.trackerTransaction&&x.configHash==expected.configHash&&x.moduleHash==expected.moduleHash&&x.rawIqHash==expected.rawIqHash&&x.deviceHash==expected.deviceHash,"Replay snapshot requested identity differs");
 const auto shape=sizes(c);size_t at=headerBytes;
 for(size_t i=0;i<blocks;++i){const size_t d=512+24*i;require(r.u32(d)==i+1&&!r.u32(d+4)&&r.u64(d+8)==at&&r.u64(d+16)==shape[i],"Replay snapshot descriptor differs");
  r.range(at,size_t(shape[i]));s.blocks.push_back({BlockKind(i+1),{data+at,data+at+shape[i]}});at+=size_t(shape[i]);}
 const auto canonical=encode(s,c);require(std::equal(canonical.begin(),canonical.end(),data),"Replay snapshot hash/reserved/header bytes differ");return s;
}

ResidentReplaySnapshot decodeSelfContained(const uint8_t* data,size_t bytes){
 require(data&&bytes>=headerBytes&&bytes<=byteLimit,"Replay inspection byte bound differs");
 resident::wire::Reader r{{data,bytes}};
 require(r.u32(0)==0x52505331&&r.u32(4)==version&&r.u32(8)==headerBytes&&
  r.u32(12)==blocks&&r.u64(16)==bytes,"Replay inspection envelope invalid");
 std::array<resident::ConstBytes,blocks> views{};uint64_t at=headerBytes;
 for(size_t i=0;i<blocks;++i){const size_t d=512+24*i;const auto n=r.u64(d+16);
  require(r.u32(d)==i+1&&!r.u32(d+4)&&r.u64(d+8)==at&&n<=bytes-at,
   "Replay inspection descriptor invalid");
  views[i]={data+at,size_t(n)};at+=n;}
 require(at==bytes,"Replay inspection trailing bytes");
 const auto rows=r.u32(76),cols=r.u32(80);
 require(rows&&cols&&uint64_t(rows)*8+uint64_t(cols)*4+384<=views[4].size,
  "Replay inspection axis shape invalid");
 const resident::ConstBytes doppler{views[4].data+384,size_t(rows)*8};
 const resident::ConstBytes delays{views[4].data+384+size_t(rows)*8,size_t(cols)*4};
 const auto c=resident::decodeConfig(views[19],delays,doppler);
 Identity expected{r.u64(24),r.u64(32),r.u64(40),r.u64(48),r.u64(56),r.u64(64)};
 std::memcpy(expected.configHash.data(),data+96,32);
 std::memcpy(expected.moduleHash.data(),data+128,32);
 std::memcpy(expected.rawIqHash.data(),data+160,32);
 std::memcpy(expected.deviceHash.data(),data+480,32);
 return decode(data,bytes,c,expected);
}
}
