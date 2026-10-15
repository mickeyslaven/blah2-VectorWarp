#include "GpuResidentWire.h"
#include <algorithm>
#include <iostream>
#include <functional>
#include <vector>

using namespace blah2::resident;
namespace {
unsigned checked=0, rejected=0;
void check(bool ok,const char* label){++checked;if(!ok)throw std::runtime_error(label);}
template<class F>void reject(F f,const char*label){bool failed=false;try{f();}catch(const std::invalid_argument&){failed=true;}check(failed,label);++rejected;}
template<size_t N>ConstBytes bytes(const std::array<uint8_t,N>& a){return {a.data(),a.size()};}
Config config(){
 Config c;auto&g=c.geometry;g.kind=blah2::GpuWorkKind::radar;
 g.range=12;g.doppler=7;g.delays=3;g.channels=2;g.delayMin=-1;
 g.clutterSamples=64;g.clutterBins=4;g.clutterDelayMin=0;
 c.correlation=9;c.sampleRate=128000;c.middle=0;c.epoch=9;
 c.surveillanceChannels={0,1};c.delayAxis={-1,0,1};
 const double resolution=1.0/(63.0/128000);c.dopplerAxis={-3*resolution,-2*resolution,-resolution,0,resolution,2*resolution,3*resolution};c.spectrumBins=64;
 c.reference.channels={0,1};c.reference.analysisSamples=64;
 c.detector.enabled=true;c.detector.guard=1;c.detector.train=2;
 c.detector.pfa=.001;c.detector.centroidDopplerResolution=2;
 c.tracker.enabled=true;c.tracker.capacity=4;c.tracker.m=2;c.tracker.n=3;c.tracker.nDelete=2;
 c.tracker.cpi=63.0/128000;c.tracker.maxAccInit=0;c.tracker.rangeRes=1;c.tracker.lambda=1;
 c.device="4098:27039:1";c.presence=wire::mandatoryPresence(c);
 for(unsigned i=16;i<=24;++i)c.presence|=uint64_t(1)<<i;
 return c;
}
void packetCases(){
 Packet p;p.op=Op::restore;p.session={7,8};p.requestId=UINT64_C(0x100000009);
 p.frame={UINT64_C(0x200000005),UINT64_C(0x300000006),UINT64_C(0x400000007)};
 p.epoch=9;p.mappedBytes=4096;p.descriptorCount=29;p.presence=bit(Section::canonical);
 p.tracker={9,UINT64_C(0x500000008),p.frame.logicalId,p.frame.consumedSequence};
 p.phase=Phase::idle;p.restoreTrackerSlot=1;const auto raw=encode(p);const auto q=decode(bytes(raw));
 check(q.requestId==p.requestId&&q.frame.logicalId==p.frame.logicalId&&q.frame.consumedSequence==p.frame.consumedSequence&&q.frame.signalTimestampMs==p.frame.signalTimestampMs&&wire::same(q.tracker,p.tracker),"Packet lost high-word identity");
 check(q.restoreTrackerSlot==1&&raw[132]==1&&raw[133]==0,"Restore slot is not explicit input132");
 auto withDiagnostic=p;withDiagnostic.op=Op::begin;withDiagnostic.restoreTrackerSlot=0;withDiagnostic.presence|=bit(Section::filtered);withDiagnostic.diagnosticPresence=bit(Section::filtered);
 check(decode(bytes(encode(withDiagnostic))).diagnosticPresence==bit(Section::filtered),"Requested diagnostic mask did not round trip");
 auto bad32=[&](size_t at,uint32_t value){auto b=raw;wire::Writer{{b.data(),b.size()}}.u32(at,value);reject([&]{decode(bytes(b));},"Malformed packet accepted");};
 bad32(0,0x42475004);bad32(4,159);bad32(8,0);bad32(8,17);bad32(12,1);
 bad32(80,33);bad32(84,6);bad32(128,8);bad32(132,2);bad32(136,1);bad32(140,513);bad32(144,1);bad32(144,uint32_t(bit(Section::filtered)));bad32(152,1);
 auto b=raw;wire::Writer w{{b.data(),b.size()}};w.u64(16,0);w.u64(24,0);reject([&]{decode(bytes(b));},"Zero nonce accepted");
 b=raw;w.u32(8,uint32_t(Op::begin));reject([&]{decode(bytes(b));},"Non-restore input slot accepted");
 b=raw;w.u64(72,sharedLimit+1);reject([&]{decode(bytes(b));},"Oversize mapping accepted");
 reject([&]{decode({raw.data(),159});},"Short packet accepted");
 reject([&]{decode({nullptr,packetBytes});},"Null packet accepted");
}
void configCases(){
 const auto c=config();validateConfig(c);const auto canonical=encodeConfig(c);
 std::array<uint8_t,12>d{};std::array<uint8_t,56>f{};
 for(size_t i=0;i<c.delayAxis.size();++i)wire::Writer{{d.data(),d.size()}}.u32(i*4,uint32_t(c.delayAxis[i]));
 for(size_t i=0;i<c.dopplerAxis.size();++i)wire::Writer{{f.data(),f.size()}}.f64(i*8,c.dopplerAxis[i]);
 const auto decoded=decodeConfig(bytes(canonical),bytes(d),bytes(f));
 check(encodeConfig(decoded)==canonical&&decoded.delayAxis==c.delayAxis&&decoded.dopplerAxis==c.dopplerAxis,"Config/axes did not preserve canonical values");
 auto coherent=c;coherent.fusion={FusionMode::coherentRegularPolygon,12,539000000,.193548,0,-1.2566370614359172,-.75};
 const auto coherentBytes=encodeConfig(coherent);
 const auto coherentDecoded=decodeConfig(bytes(coherentBytes),bytes(d),bytes(f));
 check(encodeConfig(coherentDecoded)==coherentBytes&&
  coherentDecoded.fusion.mode==FusionMode::coherentRegularPolygon&&
  coherentDecoded.fusion.beams==12&&coherentDecoded.fusion.frequencyHz==539000000&&
  coherentDecoded.fusion.directPathAzimuthRad==-.75,
  "Coherent resident fusion configuration did not round trip");
 check(wire::configHash(coherent)!=wire::configHash(c),
  "Coherent resident fusion is not bound to config identity");
 auto whitened=coherent;
 whitened.fusion.mode=FusionMode::noiseWhitenedRegularPolygon;
 const auto whitenedBytes=encodeConfig(whitened);
 const auto whitenedDecoded=decodeConfig(bytes(whitenedBytes),bytes(d),bytes(f));
 check(encodeConfig(whitenedDecoded)==whitenedBytes&&
  whitenedDecoded.fusion.mode==FusionMode::noiseWhitenedRegularPolygon,
  "Noise-whitened resident fusion configuration did not round trip");
 auto mutate=[&](const std::function<void(Config&)>&change){auto v=c;change(v);reject([&]{validateConfig(v);},"Invalid config admitted");};
 mutate([](auto&v){v.surveillanceChannels={1,0};});mutate([](auto&v){v.delayAxis[1]=1;});
 mutate([](auto&v){v.dopplerAxis[1]=v.dopplerAxis[0];});mutate([](auto&v){v.reference.channels={1,0};});
 mutate([](auto&v){v.dopplerAxis.back()+=.5;}); // Still increasing, but not the true-CPI axis.
 mutate([](auto&v){--v.correlation;});mutate([](auto&v){v.geometry.range=10;});
 mutate([](auto&v){v.geometry.doppler=6;v.dopplerAxis.pop_back();});
 mutate([](auto&v){v.reference.analysisInterval=0;});mutate([](auto&v){v.reference.covarianceSmoothing=1;});
 mutate([](auto&v){v.spectrumBins=63;});mutate([](auto&v){v.tracker.cpi=.5;});
 mutate([](auto&v){v.tracker.capacity=0;});mutate([](auto&v){v.tracker.n=256;});
 mutate([](auto&v){v.detector.enabled=false;});mutate([](auto&v){v.sampleRate=128000.5;});
 mutate([](auto&v){v.presence&=~bit(Section::canonical);});mutate([](auto&v){v.presence|=uint64_t(1)<<63;});
 mutate([](auto&v){v.device=std::string(96,'x');});mutate([](auto&v){v.geometry.kind=blah2::GpuWorkKind::fir;});
 mutate([](auto&v){v.fusion.mode=FusionMode::coherentRegularPolygon;});
 for(size_t at:{size_t(20),size_t(132),size_t(308),size_t(360),size_t(464),size_t(176)}){
  auto b=canonical;b[at]=1;reject([&]{decodeConfig(bytes(b),bytes(d),bytes(f));},"Noncanonical reserved config accepted");
 }
 auto b=canonical;wire::Writer{{b.data(),b.size()}}.u32(232,2);reject([&]{decodeConfig(bytes(b),bytes(d),bytes(f));},"Nonbinary feature accepted");
 b=canonical;std::fill(b.begin()+368,b.begin()+464,'x');reject([&]{decodeConfig(bytes(b),bytes(d),bytes(f));},"Unterminated selector accepted");
 reject([&]{decodeConfig(bytes(canonical),{d.data(),8},bytes(f));},"Short delay axis accepted");
 auto changed=c;changed.sampleRate=256000;changed.tracker.cpi=63.0/256000;
 for(auto&axis:changed.dopplerAxis)axis*=2;
 check(wire::configHash(changed)!=wire::configHash(c),"Valid rate/axis change not bound to config hash");
}
void mappingCases(){
 auto c=config();const auto l=makeLayout(c,4096);check(l.count==29&&l.bytes%4096==0&&l.bytes<=sharedLimit,"Layout shape invalid");
 check(l.at(Section::readyEvidence).capacityBytes==4096&&l.at(Section::preparedEvidence).capacityBytes==61888&&l.at(Section::controlEvidence).capacityBytes==512,"Evidence maximum capacity missing");
 uint64_t end=1280;for(uint32_t i=0;i<l.count;++i){auto d=l.sections[i];check(!d.capacityBytes||d.offset%64==0,"Misaligned section");check(!d.capacityBytes||d.offset>=end,"Overlapping section");if(d.capacityBytes)end=d.offset+d.capacityBytes;}
 std::vector<uint8_t>data(size_t(l.bytes));Mapping map{data.data(),data.size(),l};const Nonce nonce{7,8};initializeMapping(map,c,nonce);validateMapping(map,c,nonce);
 auto corrupt=[&](size_t offset){const auto old=data[offset];data[offset]^=1;reject([&]{validateMapping(map,c,nonce);},"Tampered mapping accepted");data[offset]=old;};
 for(size_t offset:{size_t(8),size_t(24),size_t(32),size_t(48),size_t(80),size_t(256),size_t(268),size_t(272),size_t(280)})corrupt(offset);
 corrupt(size_t(l.at(Section::config).offset)+124);corrupt(size_t(l.at(Section::delayAxis).offset));corrupt(size_t(l.at(Section::dopplerAxis).offset));
 reject([&]{validateMapping(map,c,{7,9});},"Cross-session mapping accepted");
 reject([&]{auto shortMap=map;--shortMap.size;validateMapping(shortMap,c,nonce);},"Truncated mapping accepted");
 auto noTail=c;noTail.geometry.clutterSamples=63;noTail.spectrumBins=63;noTail.sampleRate=126000;noTail.tracker.cpi=63.0/126000;
 noTail.dopplerAxis={-6000,-4000,-2000,0,2000,4000,6000};
 const auto zero=makeLayout(noTail,4096).at(Section::tail);check(zero.capacityBytes==0&&zero.offset==0,"Empty tail not canonical");
 auto huge=c;huge.geometry.range=3072;huge.geometry.doppler=1025;huge.geometry.clutterSamples=2048*1025;huge.correlation=2048;huge.geometry.delays=1024;huge.geometry.delayMin=0;huge.geometry.channels=8;huge.surveillanceChannels={0,1,2,3,4,5,6,7};huge.reference.channels=huge.surveillanceChannels;huge.delayAxis.resize(1024);huge.dopplerAxis.resize(1025);for(int i=0;i<1024;++i)huge.delayAxis[i]=i;for(int i=0;i<1025;++i)huge.dopplerAxis[i]=(i-512)*2.0;huge.sampleRate=4198400;huge.spectrumBins=2100;huge.tracker.cpi=.5;
 validateConfig(huge);reject([&]{makeLayout(huge,4096);},"Oversize shared mapping admitted");
 reject([&]{wire::add(UINT64_MAX,1);},"Addition wrapped");reject([&]{wire::mul(UINT64_MAX,2);},"Multiply wrapped");reject([&]{makeLayout(c,0);},"Zero page size accepted");
}
void outputCases(){
 const auto c=config();const auto l=makeLayout(c,4096);std::vector<uint8_t>data(size_t(l.bytes));Mapping map{data.data(),data.size(),l};initializeMapping(map,c,{7,8});
 Packet p;p.epoch=c.epoch;p.mappedBytes=l.bytes;p.presence=c.presence;p.diagnosticPresence=c.presence&diagnosticMask;p.frame={UINT64_C(0x100000002),17,1000};p.phase=Phase::referenceReady;
 auto ready=views(map,c,p);check(ready.spectrum.size==64&&ready.display.size==0&&ready.trackerCheckpoint.size==0,"Ready exposed later-phase outputs");
 p.phase=Phase::prepared;p.tracker={c.epoch,3,p.frame.logicalId,p.frame.consumedSequence};wire::Writer m{map.bytes(Section::metrics)};
 m.u32(0,1);m.u32(4,128);m.u64(8,p.frame.logicalId);m.u64(16,p.frame.consumedSequence);m.f64(24,-5);m.f64(32,20);m.u64(40,3);m.u64(48,2);m.u64(56,1);m.u64(64,32+2056);m.u32(72,1);m.u32(76,0);m.u32(80,c.stages());m.u32(84,c.stages());
 wire::Writer bank{map.bytes(Section::tracker1)};bank.zero();bank.u32(0,1);bank.u32(4,8224);bank.u32(8,c.tracker.capacity);bank.u32(12,255);bank.u64(16,1);bank.u64(40,c.epoch);bank.u64(48,p.frame.logicalId);bank.u64(56,p.frame.consumedSequence);
 auto prepared=views(map,c,p);check(prepared.detections.size==1&&prepared.cfar.size==3&&prepared.centroid.size==2&&prepared.trackerCheckpoint.size==2088&&prepared.trackerSlot==1,"Prepared prefixes/slot differ");
 auto minimal=p;minimal.diagnosticPresence=0;auto hidden=views(map,c,minimal);check(!hidden.filtered.data&&!hidden.reference.data&&!hidden.cfar.data&&!hidden.centroid.data&&!hidden.levels.data&&hidden.detections.size==1&&hidden.maps.size==prepared.maps.size,"Unrequested optional diagnostics exposed or mandatory results removed");
 auto bad=[&](size_t at,uint64_t value){const auto before=wire::Reader{map.read(Section::metrics)}.u64(at);m.u64(at,value);reject([&]{views(map,c,p);},"Invalid output metadata accepted");m.u64(at,before);};
 bad(8,p.frame.logicalId-1);bad(16,18);bad(40,25);bad(48,4);bad(56,3);bad(64,33);bad(72,2);bad(88,1);
 auto wrong=p;wrong.tracker.generation--;reject([&]{views(map,c,wrong);},"Stale prepared token accepted");wrong=p;wrong.phase=Phase::idle;reject([&]{views(map,c,wrong);},"Idle exposed outputs");wrong=p;wrong.presence^=bit(Section::canonical);reject([&]{views(map,c,wrong);},"Output presence substitution accepted");
}
}
int main()try{packetCases();configCases();mappingCases();outputCases();std::cout<<"COMPLETE resident-wire CPU-only checks="<<checked<<" rejected="<<rejected<<" gpu_execution=0\n";return 0;}catch(const std::exception&e){std::cerr<<"FAIL resident-wire: "<<e.what()<<'\n';return 1;}
