#include "GpuResidentWire.h"
#include "TemporalCandidateCell.h"
#include "GpuResidentInput.h"
#include "GpuResidentProof.h"
#include "CpuEvidenceDouble.h"
#include "process/tracker/TrackerDiagnosticsJson.h"
#include "ResidentReplayFixture.h"
#include "../../core/TrackBeforeDetect.private.h"
#include <algorithm>
#include <iostream>
#include <functional>
#include <vector>

using namespace blah2::resident;
namespace {
// ABI19 binds current-CPI preference and lossless tracker initiation SNR.
static_assert(offsetof(TrackerConfig,reservedPolicy)==80&&
 offsetof(TrackerConfig,normalizedInnovationGate)==83&&
 offsetof(TrackerConfig,reservedLimit)==84&&sizeof(TrackerConfig)==88,
 "Resident tracker configuration padding changed");
static_assert(offsetof(OutputViews,causalTrace)==offsetof(OutputViews,reservedOutput)+
 sizeof(Span<const uint32_t>),"Resident output padding changed");
unsigned checked=0, rejected=0;
void check(bool ok,const char* label){++checked;if(!ok)throw std::runtime_error(label);}
template<class F>void reject(F f,const char*label,const char*reason=nullptr){bool failed=false;
 try{f();}catch(const std::invalid_argument& error){failed=true;
  if(reason)check(std::string(error.what())==reason,"Wrong refusal reason");}
 check(failed,label);++rejected;}
template<size_t N>ConstBytes bytes(const std::array<uint8_t,N>& a){return {a.data(),a.size()};}
Config config(){
 Config c;c.inputChannels=2;auto&g=c.geometry;g.kind=blah2::GpuWorkKind::radar;
 g.range=12;g.doppler=7;g.delays=3;g.channels=2;g.delayMin=-1;
 g.clutterSamples=64;g.clutterBins=4;g.clutterDelayMin=0;
 c.correlation=9;c.sampleRate=128000;c.middle=0;c.epoch=9;
 c.surveillanceChannels={0,1};c.delayAxis={-1,0,1};
 const double resolution=1.0/(63.0/128000);c.dopplerAxis={-3*resolution,-2*resolution,-resolution,0,resolution,2*resolution,3*resolution};c.spectrumBins=64;
 c.reference.channels={0,1};c.reference.analysisSamples=64;
 c.detector.enabled=true;c.detector.guard=1;c.detector.train=2;
 c.detector.pfa=.001;c.detector.centroidDopplerResolution=2;c.detector.integrationCpis=2;
 c.tracker.enabled=true;c.tracker.capacity=4;c.tracker.m=2;c.tracker.n=3;c.tracker.nDelete=2;
 c.tracker.cpi=63.0/128000;c.tracker.maxAccInit=0;c.tracker.rangeRes=1;c.tracker.lambda=1;
 c.device="4098:27039:1";c.presence=wire::mandatoryPresence(c);
 for(unsigned i=16;i<=24;++i)c.presence|=uint64_t(1)<<i;
 return c;
}
void packetCases(){
 static_assert(moduleAbi==19&&protocol==0x42475213,"Current-CPI ABI/protocol changed without test review");
 validateModuleAbi(19);
 for(unsigned abi:{0u,14u,15u,16u,17u,18u,20u})reject([&]{validateModuleAbi(abi);},"Legacy/unknown module ABI accepted current-CPI configuration");
 for(uint32_t depth:{1u,2u,4u,8u}){
  track_before_detect::validateBeamHistory(depth,UINT32_MAX);
  if(depth==2)reject([&]{track_before_detect::validateBeamHistory(depth,0);},"Two-CPI beam history silently disabled");
  else track_before_detect::validateBeamHistory(depth,0);
 }
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
 bad32(0,0x42475004);bad32(0,0x4247520c);bad32(0,0x4247520e);bad32(0,0x4247520f);bad32(4,159);bad32(8,0);bad32(8,21);bad32(12,1);
 bad32(80,33);bad32(84,6);bad32(128,8);bad32(132,2);bad32(136,1);bad32(140,513);bad32(144,1);bad32(144,uint32_t(bit(Section::filtered)));bad32(152,1);
 auto b=raw;wire::Writer w{{b.data(),b.size()}};w.u64(16,0);w.u64(24,0);reject([&]{decode(bytes(b));},"Zero nonce accepted");
 b=raw;w.u32(8,uint32_t(Op::begin));reject([&]{decode(bytes(b));},"Non-restore input slot accepted");
 b=raw;w.u64(72,replaySharedLimit+1);reject([&]{decode(bytes(b));},"Oversize mapping accepted");
 reject([&]{decode({raw.data(),159});},"Short packet accepted");
 reject([&]{decode({nullptr,packetBytes});},"Null packet accepted");
 for(const auto op:{Op::resetInput,Op::inputReset}){
  auto reset=p;reset.op=op;reset.restoreTrackerSlot=0;reset.frame={};reset.tracker={};
  const auto decoded=decode(bytes(encode(reset)));
  check(decoded.op==op&&!decoded.frame.logicalId&&wire::same(decoded.tracker,{}),
   "Input reset operation did not round trip");
 }
}
void trackerDiagnosticsCases(){
 const auto c=config();const auto layout=makeLayout(c,4096);
 std::vector<uint8_t> storage(layout.bytes);Mapping map{storage.data(),storage.size(),layout};
 Packet packet;packet.phase=Phase::prepared;packet.frame={1,2,3000};packet.session={7,8};packet.epoch=c.epoch;
 CpuEvidenceDouble fixture{c,map};fixture.prepared(packet);
 auto raw=map.bytes(Section::preparedEvidence);wire::Writer writer{raw};
 TrackerFrameDiagnostics expected;expected.noOp=true;uint64_t value=UINT64_C(0x100000000);
 for(const auto& field:trackerDiagnosticFields)expected.*field.member=++value;
 wire::writeTrackerDiagnostics(writer,evidence::prepared::trackerDiagnostics,expected);
 writer.u32(evidence::prepared::trackerPreparedStatus+64*4,71);
 writer.u32(evidence::prepared::trackerPreparedStatus+65*4,73);
 const auto proof=preparedProof({raw.data,raw.size});
 check(proof.trackerStatus[64]==71&&proof.trackerStatus[65]==73&&proof.gpuTimeMask==16383,
  "Prepared tracker status overlapped diagnostics or timings");
 const auto publication=annotateTrackerFrameDiagnostics("{\"timestamp\":3000,\"n\":0}",proof.trackerDiagnostics);
 rapidjson::Document json;json.Parse(publication.c_str());check(!json.HasParseError(),"Tracker publication invalid JSON");
 check(json["trackerDiagnostics"]["no_op"].GetBool(),"Prepared no-op flag lost in publication");
 for(const auto& field:trackerDiagnosticFields)
  check(json["trackerDiagnostics"][field.name].GetUint64()==expected.*field.member,
   "Prepared tracker counter lost precision or omitted from publication");
 writer.u32(evidence::prepared::trackerDiagnostics,0);
 reject([&]{preparedProof({raw.data,raw.size});},"Absent diagnostics accepted as all-zero measurements");
 writer.u32(evidence::prepared::trackerDiagnostics,1);writer.u32(evidence::prepared::trackerDiagnostics+4,2);
 reject([&]{preparedProof({raw.data,raw.size});},"Malformed no-op flag accepted");
}
void temporalProofCases(){
 for(uint32_t maximum:{4u,8u}){
  auto c=config();c.detector.integrationCpis=maximum;c.tracker.n=maximum+1;
  c.tracker.nDelete=maximum-1;
  c.fusion={FusionMode::coherentRegularPolygon,12,539000000,.2,0,-1.2566370614359172,-.75,6};
  c.presence|=wire::mandatoryPresence(c);validateConfig(c);
  const auto layout=makeLayout(c,4096);std::vector<uint8_t> storage(layout.bytes);
  Mapping map{storage.data(),storage.size(),layout};CpuEvidenceDouble fixture{c,map};fixture.device();
  Packet packet;packet.phase=Phase::prepared;packet.session={7,8};packet.epoch=c.epoch;
  packet.presence=c.presence;packet.mappedBytes=layout.bytes;
  for(uint32_t available:{1u,maximum,1u}){
   ++packet.frame.logicalId;++packet.frame.consumedSequence;packet.frame.signalTimestampMs+=1000;
   packet.tracker={c.epoch,17,packet.frame.logicalId,packet.frame.consumedSequence};
   fixture.ready(packet);fixture.prepared(packet);
   wire::Writer writer{map.bytes(Section::preparedEvidence)};
   writer.u32(evidence::prepared::postprocessStatus+28*4,available==maximum);
   validateEvidence(c,packet,map.read(Section::readyEvidence),map.read(Section::preparedEvidence),map.read(Section::device));
   writer.u32(evidence::prepared::postprocessStatus+4*4,21*(track_before_detect::detectorPassCount(maximum)-1));
   reject([&]{validateEvidence(c,packet,map.read(Section::readyEvidence),map.read(Section::preparedEvidence),map.read(Section::device));},
    "Resident proof accepted omitted current-CPI detector work");
  }
 }
}
void configCases(){
 const auto c=config();validateConfig(c);const auto canonical=encodeConfig(c);
 std::array<uint8_t,12>d{};std::array<uint8_t,56>f{};
 for(size_t i=0;i<c.delayAxis.size();++i)wire::Writer{{d.data(),d.size()}}.u32(i*4,uint32_t(c.delayAxis[i]));
 for(size_t i=0;i<c.dopplerAxis.size();++i)wire::Writer{{f.data(),f.size()}}.f64(i*8,c.dopplerAxis[i]);
 const auto decoded=decodeConfig(bytes(canonical),bytes(d),bytes(f));
 check(encodeConfig(decoded)==canonical&&decoded.delayAxis==c.delayAxis&&decoded.dopplerAxis==c.dopplerAxis,"Config/axes did not preserve canonical values");
 auto coherent=c;coherent.fusion={FusionMode::coherentRegularPolygon,12,539000000,.2,0,-1.2566370614359172,-.75,6};
 coherent.detector.integrationMotionBins=1;coherent.detector.integrationBeamTolerance=1;
 reject([&]{validateConfig(coherent);},"Unsupported two-CPI beam gate silently ignored");
 coherent.detector.integrationCpis=4;coherent.tracker.n=5;coherent.tracker.nDelete=4;
 const auto coherentBytes=encodeConfig(coherent);
 const auto coherentDecoded=decodeConfig(bytes(coherentBytes),bytes(d),bytes(f));
 check(encodeConfig(coherentDecoded)==coherentBytes&&
  coherentDecoded.fusion.mode==FusionMode::coherentRegularPolygon&&
  coherentDecoded.fusion.beams==12&&coherentDecoded.fusion.frequencyHz==539000000&&
  coherentDecoded.fusion.directPathAzimuthRad==-.75&&coherentDecoded.fusion.directPathNullDb==6&&
  coherentDecoded.detector.integrationMotionBins==1&&coherentDecoded.detector.integrationBeamTolerance==1,
  "Coherent resident fusion configuration did not round trip");
 check(wire::configHash(coherent)!=wire::configHash(c),
  "Coherent resident fusion is not bound to config identity");
 auto measuredFusion=coherent;
 for(uint32_t beam=0;beam<measuredFusion.fusion.beams;++beam)
  for(uint32_t channel=0;channel<measuredFusion.geometry.channels;++channel)
   measuredFusion.fusion.manifoldResponses.push_back(
    std::polar(1.0f,float(.11*beam-.23*channel)));
 measuredFusion.fusion.directResponse={{1,.25f},{.5f,-.75f}};
 const auto measuredFusionBytes=encodeConfig(measuredFusion);
 const auto measuredFusionDecoded=decodeConfig(bytes(measuredFusionBytes),bytes(d),bytes(f));
 check(encodeConfig(measuredFusionDecoded)==measuredFusionBytes&&
  measuredFusionDecoded.fusion.manifoldResponses==measuredFusion.fusion.manifoldResponses&&
  measuredFusionDecoded.fusion.directResponse==measuredFusion.fusion.directResponse&&
  wire::configHash(measuredFusion)!=wire::configHash(coherent),
  "Measured surveillance manifold did not round trip or bind config identity");
 auto changedAnchor=measuredFusion;changedAnchor.fusion.directResponse[0]+=
  std::complex<float>(.125f,-.25f);
 check(wire::configHash(changedAnchor)!=wire::configHash(measuredFusion),
  "Measured surveillance direct anchor did not bind config identity");
 auto tbd=coherent;tbd.detector.integrationCpis=8;
 tbd.tracker.n=9;tbd.tracker.nDelete=7;
 tbd.detector.integrationMotionBins=track_before_detect::encodeMotionBins(1,4);
 const auto tbdBytes=encodeConfig(tbd);
 const auto tbdDecoded=decodeConfig(bytes(tbdBytes),bytes(d),bytes(f));
 check(encodeConfig(tbdDecoded)==tbdBytes&&tbdDecoded.detector.integrationCpis==8&&
  tbdDecoded.detector.integrationMotionBins==track_before_detect::encodeMotionBins(1,4),
  "Track-before-detect configuration did not round trip");
 auto expandedTbd=tbd;
 expandedTbd.detector.integrationMotionBins=track_before_detect::encodeMotionBins(2,16);
 validateConfig(expandedTbd);
 const auto expandedTbdBytes=encodeConfig(expandedTbd);
 const auto expandedTbdDecoded=decodeConfig(bytes(expandedTbdBytes),bytes(d),bytes(f));
 check(encodeConfig(expandedTbdDecoded)==expandedTbdBytes&&
  expandedTbdDecoded.detector.integrationMotionBins==track_before_detect::encodeMotionBins(2,16),
  "Expanded track-before-detect motion bounds did not round trip");
 check(wire::configHash(tbd)!=wire::configHash(coherent),
  "Track-before-detect is not bound to config identity");
 auto fractional=tbd;
 fractional.detector.integrationMotionBins=track_before_detect::encodeMotionBins(1,4,true);
 const auto fractionalBytes=encodeConfig(fractional);
 check(encodeConfig(decodeConfig(bytes(fractionalBytes),bytes(d),bytes(f)))==fractionalBytes&&
   wire::configHash(fractional)!=wire::configHash(tbd),
   "Fractional delay mode did not round trip or bind identity");
 for(uint32_t unknown:{0x20000000u,0x00002000u,0x00000004u}){
  auto bad=fractional;bad.detector.integrationMotionBins|=unknown;
  reject([&]{validateConfig(bad);},"Unknown fractional motion bits admitted");
 }
 {auto bad=fractional;bad.detector.integrationCpis=2;
  reject([&]{validateConfig(bad);},"Fractional delay without TBD admitted");}
 auto bearing=tbd;bearing.bearing={true,BearingMethod::covarianceMvdr,
  BearingReferenceAnchor::fixedSite,60,3,1,3,.01,{1,0}};
 const auto bearingBytes=encodeConfig(bearing);
 const auto bearingDecoded=decodeConfig(bytes(bearingBytes),bytes(d),bytes(f));
 check(encodeConfig(bearingDecoded)==bearingBytes&&bearingDecoded.bearing.enabled&&
  bearingDecoded.bearing.method==BearingMethod::covarianceMvdr&&
  bearingDecoded.bearing.referenceAnchor==BearingReferenceAnchor::fixedSite&&
  bearingDecoded.bearing.scanBeams==60&&
  bearingDecoded.bearing.channelMapping==std::vector<uint32_t>({1,0}),
  "Adaptive bearing configuration did not round trip");
 check(wire::configHash(bearing)!=wire::configHash(tbd),
  "Bearing method/mapping is not bound to config identity");
 for(uint32_t depth:{4u,8u}){
  auto boundary=tbd;boundary.detector.integrationCpis=depth;
  boundary.fusion.beams=31;validateConfig(boundary);
  check(decodeConfig(bytes(encodeConfig(boundary)),bytes(d),bytes(f)).fusion.beams==31,
    "Default refinement boundary was silently clamped");
  boundary.fusion.beams=32;
  reject([&]{validateConfig(boundary);},"Default128-beam refinement admitted");
  boundary=bearing;boundary.detector.integrationCpis=depth;
  boundary.fusion.beams=127;boundary.bearing.scanBeams=127;validateConfig(boundary);
  check(decodeConfig(bytes(encodeConfig(boundary)),bytes(d),bytes(f)).bearing.scanBeams==127,
    "Explicit127-beam refinement was silently clamped");
  boundary.fusion.beams=128;boundary.bearing.scanBeams=128;
  reject([&]{validateConfig(boundary);},"Explicit128-beam refinement admitted");
 }
 for(uint32_t depth:{1u,2u}){
  auto ordinary=coherent;ordinary.detector.integrationCpis=depth;
  ordinary.detector.integrationMotionBins=0;ordinary.detector.integrationBeamTolerance=UINT32_MAX;
  ordinary.fusion.beams=360;validateConfig(ordinary);
  ordinary.bearing=bearing.bearing;
  reject([&]{validateConfig(ordinary);},"Bearing output without4/8-CPI candidate maps admitted");
 }
 // Temporal admission belongs to the wire boundary even when detection is off.
 // The preceding round trips cover coherent1/8-CPI and motion(2,16) acceptance.
 const char* temporalReason="Production replay temporal/TBD contract is invalid";
 for(bool enabled:{false,true}){
  auto invalid=c;invalid.detector.enabled=enabled;invalid.tracker={};
  invalid.detector.integrationCpis=1;invalid.detector.integrationMotionBins=1;
  reject([&]{validateConfig(invalid);},"Single-CPI motion accepted at wire boundary",temporalReason);
  invalid.detector.integrationCpis=4;invalid.detector.integrationMotionBins=0;
  invalid.presence=wire::mandatoryPresence(invalid);
  if(enabled)validateConfig(invalid);
  else reject([&]{validateConfig(invalid);},"Disabled noncoherent4-CPI detector accepted",temporalReason);
 }
 for(uint32_t depth:{2u,4u,8u}){
  auto detectorOnly=c;detectorOnly.tracker={};detectorOnly.detector.integrationCpis=depth;
  detectorOnly.reference.centreHz=101100000;
  detectorOnly.detector.integrationMotionBins=depth==2?1:track_before_detect::encodeMotionBins(2,4,true);
  detectorOnly.presence=wire::mandatoryPresence(detectorOnly);validateConfig(detectorOnly);
  const auto encoded=encodeConfig(detectorOnly);
  const auto decoded=decodeConfig(bytes(encoded),bytes(d),bytes(f));
  check(encodeConfig(decoded)==encoded&&!decoded.tracker.enabled&&decoded.temporalCarrierHz()==101100000,
   "Noncoherent detector-only temporal carrier/ABI round trip differs");
  if(depth>2){
   auto bad=detectorOnly;bad.reference.centreHz=0;
   reject([&]{validateConfig(bad);},"Fractional noncoherent detector omitted its carrier");
   bad=detectorOnly;bad.tracker=c.tracker;bad.presence=wire::mandatoryPresence(bad);
   reject([&]{validateConfig(bad);},"Unqualified noncoherent resident tracker admitted",temporalReason);
  }
 }
 auto measured=bearing;for(uint32_t beam=0;beam<60;++beam)for(uint32_t channel=0;channel<2;++channel)
  measured.bearing.manifoldResponses.push_back(std::polar(1.0f,float(.1*beam+.2*channel)));
 const auto measuredBytes=encodeConfig(measured);const auto measuredDecoded=decodeConfig(bytes(measuredBytes),bytes(d),bytes(f));
 check(encodeConfig(measuredDecoded)==measuredBytes&&measuredDecoded.bearing.manifoldResponses==measured.bearing.manifoldResponses&&
  wire::configHash(measured)!=wire::configHash(bearing),"Measured bearing manifold did not round trip or bind config identity");
 check(decoded.detector.integrationCpis==2,"Multi-CPI detector configuration did not round trip");
 auto adaptive=c;adaptive.tracker.initiationMode=TrackerInitiationMode::adaptiveGate;
 const auto adaptiveBytes=encodeConfig(adaptive);
 const auto adaptiveDecoded=decodeConfig(bytes(adaptiveBytes),bytes(d),bytes(f));
 check(adaptiveDecoded.tracker.initiationMode==TrackerInitiationMode::adaptiveGate&&
  encodeConfig(adaptiveDecoded)==adaptiveBytes,"Adaptive tracker initiation did not round trip");
 check(wire::configHash(adaptive)!=wire::configHash(c),
  "Adaptive tracker initiation is not bound to config identity");
 auto conservative=adaptive;conservative.tracker.minInitiationSnrDb=7;
 const auto conservativeBytes=encodeConfig(conservative);
 const auto conservativeDecoded=decodeConfig(bytes(conservativeBytes),bytes(d),bytes(f));
 check(conservativeDecoded.tracker.minInitiationSnrDb==7&&
  encodeConfig(conservativeDecoded)==conservativeBytes&&
  wire::configHash(conservative)!=wire::configHash(adaptive),
 "Tracker initiation SNR did not round trip or bind config identity");
 auto exact=tbd;exact.tracker.minInitiationSnrDb=9.1;exact.detector.currentCpiPreferenceSnr=9.1;
 const auto exactBytes=encodeConfig(exact);
 const auto exactDecoded=decodeConfig(bytes(exactBytes),bytes(d),bytes(f));
 check(exactDecoded.tracker.minInitiationSnrDb==9.1&&exactDecoded.detector.currentCpiPreferenceSnr==9.1&&
  encodeConfig(exactDecoded)==exactBytes&&wire::configHash(exact)!=wire::configHash(tbd),
  "Noninteger tracker/merge threshold lost precision or config binding");
 auto mismatch=exact;mismatch.detector.currentCpiPreferenceSnr=9;
 reject([&]{encodeConfig(mismatch);},"Mismatched tracker/merge threshold admitted");
 auto oldConfig=exactBytes;wire::Writer{{oldConfig.data(),oldConfig.size()}}.u32(0,7);
 reject([&]{decodeConfig(bytes(oldConfig),bytes(d),bytes(f));},"Old config schema admitted new merge threshold");
 auto clutterAware=adaptive;clutterAware.tracker.stationaryClutter=true;
 clutterAware.tracker.stationaryPersistenceCpis=8;clutterAware.tracker.stationaryMaxDopplerHz=15;
 const auto clutterBytes=encodeConfig(clutterAware);
 const auto clutterDecoded=decodeConfig(bytes(clutterBytes),bytes(d),bytes(f));
 check(clutterDecoded.tracker.stationaryClutter&&
  clutterDecoded.tracker.stationaryPersistenceCpis==8&&
  clutterDecoded.tracker.stationaryMaxDopplerHz==15&&
  encodeConfig(clutterDecoded)==clutterBytes,
  "Persistent clutter initiation configuration did not round trip");
 check(wire::configHash(clutterAware)!=wire::configHash(adaptive),
  "Persistent clutter initiation is not bound to config identity");
 auto normalized=tbd;normalized.tracker.normalizedInnovationGate=true;
 const auto normalizedBytes=encodeConfig(normalized);
 const auto normalizedDecoded=decodeConfig(bytes(normalizedBytes),bytes(d),bytes(f));
 check(normalizedDecoded.tracker.normalizedInnovationGate&&
  encodeConfig(normalizedDecoded)==normalizedBytes,
  "Normalized tracker gate did not preserve its configuration encoding");
 const wire::Reader normalizedWire{bytes(normalizedBytes)};
 check(moduleAbi==19&&protocol==0x42475213&&normalizedWire.u32(604)==0&&
  normalizedWire.u32(608)==0&&normalizedWire.u32(612)==4,
  "Default merge policy or supported tracker policy offset changed");
 for(const auto offset:{604u,608u,612u})for(const auto flag:{1u,2u}){
  auto retired=normalizedBytes;wire::Writer{{retired.data(),retired.size()}}.u32(offset,flag);
  reject([&]{decodeConfig(bytes(retired),bytes(d),bytes(f));},
   "Invalid current-CPI threshold or retired guided tracker wire flag accepted");}
 auto impossible=tbd;impossible.tracker.m=4;impossible.tracker.n=6;
 reject([&]{encodeConfig(impossible);},"detector-only disjoint 4-of-6 accepted");
 impossible.tracker.n=25;encodeConfig(impossible);impossible.tracker.nDelete=6;
 reject([&]{encodeConfig(impossible);},"eight-CPI bank admitted an incompatible coast horizon");
 auto whitened=coherent;
 whitened.fusion.mode=FusionMode::noiseWhitenedRegularPolygon;
 const auto whitenedBytes=encodeConfig(whitened);
 const auto whitenedDecoded=decodeConfig(bytes(whitenedBytes),bytes(d),bytes(f));
 check(encodeConfig(whitenedDecoded)==whitenedBytes&&
  whitenedDecoded.fusion.mode==FusionMode::noiseWhitenedRegularPolygon,
  "Noise-whitened resident fusion configuration did not round trip");
 auto mutate=[&](const std::function<void(Config&)>&change){auto v=c;change(v);reject([&]{validateConfig(v);},"Invalid config admitted");};
 mutate([](auto&v){v.tracker.reservedPolicy[0]=1;});
 mutate([](auto&v){v.tracker.reservedLimit=1;});
 mutate([](auto&v){v.presence|=bit(Section::reserved30);});
 mutate([](auto&v){v.surveillanceChannels={1,0};});mutate([](auto&v){v.delayAxis[1]=1;});
 mutate([](auto&v){v.dopplerAxis[1]=v.dopplerAxis[0];});mutate([](auto&v){v.reference.channels={1,0};});
 mutate([](auto&v){v.dopplerAxis.back()+=.5;}); // Still increasing, but not the true-CPI axis.
 mutate([](auto&v){--v.correlation;});mutate([](auto&v){v.geometry.range=10;});
 mutate([](auto&v){v.geometry.doppler=6;v.dopplerAxis.pop_back();});
 mutate([](auto&v){v.reference.analysisInterval=0;});mutate([](auto&v){v.reference.covarianceSmoothing=1;});
 mutate([](auto&v){v.spectrumBins=63;});mutate([](auto&v){v.tracker.cpi=.5;});
 mutate([](auto&v){v.tracker.capacity=0;});mutate([](auto&v){v.tracker.n=256;});
 mutate([](auto&v){v.tracker.initiationMode=TrackerInitiationMode(2);});
 mutate([](auto&v){v.tracker.stationaryClutter=true;v.tracker.stationaryPersistenceCpis=0;v.tracker.stationaryMaxDopplerHz=15;});
 mutate([](auto&v){v.tracker.stationaryClutter=true;v.tracker.stationaryPersistenceCpis=1;v.tracker.stationaryMaxDopplerHz=15;});
 mutate([](auto&v){v.detector.enabled=false;});mutate([](auto&v){v.sampleRate=128000.5;});
 mutate([](auto&v){v.detector.integrationCpis=3;});
 mutate([](auto&v){v.detector.integrationCpis=6;});
 mutate([](auto&v){v.detector.integrationCpis=8;});
 mutate([](auto&v){v.detector.integrationMotionBins=2;});
 mutate([](auto&v){v.detector.integrationCpis=1;v.detector.integrationMotionBins=1;});
 mutate([](auto&v){v.detector.integrationBeamTolerance=0;});
 mutate([](auto&v){v.presence&=~bit(Section::canonical);});mutate([](auto&v){v.presence|=uint64_t(1)<<63;});
 mutate([](auto&v){v.device=std::string(96,'x');});mutate([](auto&v){v.geometry.kind=blah2::GpuWorkKind::retiredFir;});
 mutate([](auto&v){v.fusion.mode=FusionMode::coherentRegularPolygon;});
 mutate([&](auto&v){v=coherent;v.fusion.manifoldResponses={{1,0}};});
 mutate([&](auto&v){v=coherent;v.fusion.manifoldResponses.assign(24,{1,0});});
 mutate([&](auto&v){v=measuredFusion;v.fusion.directResponse.pop_back();});
 mutate([&](auto&v){v=measuredFusion;v.fusion.directResponse[0]={0,0};});
 mutate([&](auto&v){v=coherent;v.fusion.manifoldResponses.assign(24,{1,0});
  v.fusion.manifoldResponses[0]={0,0};});
 mutate([&](auto&v){v=coherent;v.fusion.manifoldResponses.assign(24,{1,0});
  v.fusion.manifoldResponses[0]={std::numeric_limits<float>::infinity(),0};});
 mutate([](auto&v){v.fusion.manifoldResponses={{1,0}};});
 mutate([&](auto&v){v=bearing;v.bearing.channelMapping={0,0};});
 mutate([&](auto&v){v=bearing;v.bearing.scanBeams=59;});
 mutate([&](auto&v){v=bearing;v.bearing.manifoldResponses={{1,0}};});
 for(size_t at:{size_t(20),size_t(132),size_t(360),size_t(464),size_t(176)}){
  auto b=canonical;b[at]=1;reject([&]{decodeConfig(bytes(b),bytes(d),bytes(f));},"Noncanonical reserved config accepted");
 }
 auto b=canonical;wire::Writer{{b.data(),b.size()}}.u32(232,2);reject([&]{decodeConfig(bytes(b),bytes(d),bytes(f));},"Nonbinary feature accepted");
 b=canonical;wire::Writer{{b.data(),b.size()}}.u32(620,9);reject([&]{decodeConfig(bytes(b),bytes(d),bytes(f));},"Oversize direct-anchor count accepted");
 b=canonical;std::fill(b.begin()+368,b.begin()+464,'x');reject([&]{decodeConfig(bytes(b),bytes(d),bytes(f));},"Unterminated selector accepted");
 reject([&]{decodeConfig(bytes(canonical),{d.data(),8},bytes(f));},"Short delay axis accepted");
 auto changed=c;changed.sampleRate=256000;changed.tracker.cpi=63.0/256000;
 for(auto&axis:changed.dopplerAxis)axis*=2;
 check(wire::configHash(changed)!=wire::configHash(c),"Valid rate/axis change not bound to config hash");
}
void mappingCases(){
 auto c=config();const auto l=makeLayout(c,4096);check(l.count==29&&l.bytes%4096==0&&l.bytes<=sharedLimit,"Layout shape invalid");
 check(trackerHistoryCapacity(c.tracker)==100&&trackerRecordWords(c.tracker)==820&&
  l.at(Section::tracker0).capacityBytes==128+uint64_t(c.tracker.capacity)*3280,
  "Short confirmation window did not use compact tracker history");
 auto longHistory=c;longHistory.tracker.n=255;longHistory.tracker.m=2;
 const auto longLayout=makeLayout(longHistory,4096);
 check(trackerHistoryCapacity(longHistory.tracker)==255&&trackerRecordWords(longHistory.tracker)==2060&&
  longLayout.at(Section::tracker0).capacityBytes==128+uint64_t(c.tracker.capacity)*8240,
  "Maximum confirmation window did not retain the full tracker history");
 check(l.at(Section::readyEvidence).capacityBytes==4096&&l.at(Section::preparedEvidence).capacityBytes==62224&&l.at(Section::controlEvidence).capacityBytes==512,"Evidence maximum capacity missing");
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
 auto large=[&](uint32_t channels,uint32_t doppler){auto v=c;v.geometry.range=3072;
  v.geometry.doppler=doppler;v.geometry.clutterSamples=2048*doppler;v.correlation=2048;
  v.geometry.delays=1024;v.geometry.delayMin=0;v.geometry.channels=channels;v.inputChannels=channels;
  v.surveillanceChannels.resize(channels);for(uint32_t i=0;i<channels;++i)v.surveillanceChannels[i]=i;
  v.reference.channels=v.surveillanceChannels;v.delayAxis.resize(1024);v.dopplerAxis.resize(doppler);
  for(int i=0;i<1024;++i)v.delayAxis[i]=i;
  for(uint32_t i=0;i<doppler;++i)v.dopplerAxis[i]=(int64_t(i)-int64_t(doppler/2))*2.0;
  v.sampleRate=4096*doppler;v.spectrumBins=1+(v.geometry.clutterSamples-1)/1000;v.tracker.cpi=.5;
  return v;};
 auto huge=large(8,1025);validateConfig(huge);
 reject([&]{makeLayout(huge,4096);},"Oversize shared mapping admitted");
 auto replayMiddle=large(4,1131);validateConfig(replayMiddle);
 reject([&]{makeLayout(replayMiddle,4096);},"Ordinary mapping above 512 MiB admitted");
 blah2::resident_replay::test::enable(replayMiddle);
 for(unsigned i=16;i<=24;++i)replayMiddle.presence|=uint64_t(1)<<i;
 validateConfig(replayMiddle);
 const auto middle=makeLayout(replayMiddle,4096);
 check(middle.bytes>sharedLimit&&middle.bytes<=replaySharedLimit,
  "Explicit replay mapping did not occupy the admitted 512 MiB--1 GiB interval");
 auto replayOver=large(4,1167);blah2::resident_replay::test::enable(replayOver);
 for(unsigned i=16;i<=24;++i)replayOver.presence|=uint64_t(1)<<i;
 check(blah2::resident_replay::requiredBytes(replayOver)<=blah2::resident_replay::byteLimit,
  "Replay-over-limit fixture exceeded the independent snapshot byte limit");
 validateConfig(replayOver);
 reject([&]{makeLayout(replayOver,4096);},"Replay mapping above 1 GiB admitted");
 reject([&]{wire::add(UINT64_MAX,1);},"Addition wrapped");reject([&]{wire::mul(UINT64_MAX,2);},"Multiply wrapped");reject([&]{makeLayout(c,0);},"Zero page size accepted");
}
void topologyCases(){
 auto c=config();c.geometry.channels=1;c.reference.mode=ReferenceMode::capturedDedicated;
 c.reference.channels={0};c.surveillanceChannels={1};validateConfig(c);
 const auto l=makeLayout(c,4096);
 check(l.at(Section::canonical).capacityBytes==64*2*16&&
  l.at(Section::filtered).capacityBytes==64*16&&l.at(Section::maps).capacityBytes==21*8,
  "Dedicated layout confused canonical input with surveillance output");
 std::vector<uint8_t> data(l.bytes);Mapping mapping{data.data(),data.size(),l};
 initializeMapping(mapping,c,{7,8});validateMapping(mapping,c,{7,8});
 const auto decoded=decodeConfig(mapping.read(Section::config),mapping.read(Section::delayAxis),mapping.read(Section::dopplerAxis));
 check(decoded.inputChannels==2&&decoded.geometry.channels==1&&
  decoded.reference.mode==ReferenceMode::capturedDedicated&&decoded.reference.channels==c.reference.channels&&
  decoded.surveillanceChannels==c.surveillanceChannels,"Dedicated topology did not round trip");
 auto reversed=c;reversed.reference.channels={1};reversed.surveillanceChannels={0};validateConfig(reversed);
 check(wire::configHash(c)!=wire::configHash(reversed),"Physical channel selection missing from identity");
 auto sparse=c;sparse.inputChannels=4;sparse.reference.channels={2};sparse.geometry.channels=2;sparse.surveillanceChannels={3,0};validateConfig(sparse);
 check(wire::configHash(c)!=wire::configHash(sparse),"Canonical input count missing from identity");
 auto bad=[&](auto mutate){auto value=c;mutate(value);reject([&]{validateConfig(value);},"Malformed dedicated topology accepted");};
 bad([](auto&v){v.inputChannels=0;});bad([](auto&v){v.inputChannels=9;});
 bad([](auto&v){v.surveillanceChannels={0};});bad([](auto&v){v.surveillanceChannels={2};});
 bad([](auto&v){v.geometry.channels=2;v.surveillanceChannels={1,1};});
 bad([](auto&v){v.reference.channels={2};});bad([](auto&v){v.reference.channels={0,0};});
 bad([](auto&v){v.reference.channels={};});bad([](auto&v){v.reference.mode=ReferenceMode(3);});
 bad([](auto&v){v.reference.mode=ReferenceMode::capturedArray;});
 bad([](auto&v){blah2::resident_replay::test::enable(v);});
 for(const auto& field:std::array<std::pair<size_t,uint32_t>,5>{{{0,6},{32,0},{112,3},{168,2},{200,2}}}){
  auto raw=encodeConfig(c);wire::Writer{{raw.data(),raw.size()}}.u32(field.first,field.second);
  reject([&]{decodeConfig(bytes(raw),mapping.read(Section::delayAxis),mapping.read(Section::dopplerAxis));},"Malformed topology wire accepted");
 }
 // Exercise the same copy boundary as Coordinator::begin with distinct input
 // samples. Reference selection must never reorder the canonical capture.
 std::vector<std::vector<std::complex<double>>> capture(2,std::vector<std::complex<double>>(64));
 std::fill(capture[0].begin(),capture[0].end(),std::complex<double>{2,3});
 std::fill(capture[1].begin(),capture[1].end(),std::complex<double>{7,11});
 std::vector<std::complex<double>> output(128);
 const auto samples=[](const auto& values)->const auto&{return values;};
 copyCanonicalInput(c,{output.data(),output.size()},capture,samples);
 check(output[0]==capture[0][0]&&output[64]==capture[1][0],"Coordinator input copy silently remapped capture");
 copyCanonicalInput(reversed,{output.data(),output.size()},capture,samples);
 check(output[0]==capture[0][0]&&output[64]==capture[1][0],"Coordinator input copy followed surveillance order");
 const auto before=output;capture[1].pop_back();
 reject([&]{copyCanonicalInput(c,{output.data(),output.size()},capture,samples);},"Coordinator accepted malformed channel length");
 check(output==before,"Malformed input partially overwrote canonical frame");
 capture.pop_back();
 reject([&]{copyCanonicalInput(c,{output.data(),output.size()},capture,samples);},"Coordinator accepted missing reference input");
 // Borrowed canonical storage uses the same all-channel validation and order,
 // including a non-zero slice offset used by direct and channelized routes.
 auto borrowedStorage=std::make_shared<const std::vector<std::vector<std::complex<double>>>>(
  std::vector<std::vector<std::complex<double>>>(2,
   std::vector<std::complex<double>>(66)));
 auto mutableBorrowed=std::make_shared<std::vector<std::vector<std::complex<double>>>>(*borrowedStorage);
 std::fill((*mutableBorrowed)[0].begin(),(*mutableBorrowed)[0].end(),std::complex<double>{13,17});
 std::fill((*mutableBorrowed)[1].begin(),(*mutableBorrowed)[1].end(),std::complex<double>{19,23});
 borrowedStorage=mutableBorrowed;
 blah2::iq::CoherentSamples coherent;
 blah2::iq::BorrowedSamples::DoubleChannels borrowedViews;
 for(const auto& channel:*borrowedStorage)
  borrowedViews.emplace_back(channel.data(),channel.size());
 coherent.borrowed=blah2::iq::BorrowedSamples{borrowedStorage,std::move(borrowedViews)};
 copyCanonicalSignalInput(c,{output.data(),output.size()},coherent,2);
 check(output[0]==(*borrowedStorage)[0][2]&&output[64]==(*borrowedStorage)[1][2],
  "Borrowed resident input reordered or ignored its slice offset");
 const auto borrowedBefore=output;
 auto shortStorage=std::make_shared<const std::vector<std::vector<std::complex<double>>>>(
  std::vector<std::vector<std::complex<double>>>{
   std::vector<std::complex<double>>(66),std::vector<std::complex<double>>(65)});
 blah2::iq::BorrowedSamples::DoubleChannels shortViews;
 for(const auto& channel:*shortStorage)shortViews.emplace_back(channel.data(),channel.size());
 coherent.borrowed=blah2::iq::BorrowedSamples{shortStorage,std::move(shortViews)};
 reject([&]{copyCanonicalSignalInput(c,{output.data(),output.size()},coherent,2);},
  "Borrowed coordinator accepted malformed later channel");
 check(output==borrowedBefore,"Malformed borrowed input partially overwrote canonical frame");
}
void outputCases(){
 auto c=config();const auto ordinaryHash=wire::configHash(c);c.presence|=bit(Section::causalTrace);
 check(wire::configHash(c)!=ordinaryHash,"Causal opt-in not bound to config identity");
 const auto l=makeLayout(c,4096);
 check(l.at(Section::causalTrace).capacityBytes==8256,"Causal trace fixed byte bound differs");
 std::vector<uint8_t>data(size_t(l.bytes));Mapping map{data.data(),data.size(),l};initializeMapping(map,c,{7,8});
 Packet p;p.epoch=c.epoch;p.mappedBytes=l.bytes;p.presence=c.presence;p.diagnosticPresence=c.presence&diagnosticMask;p.frame={UINT64_C(0x100000002),17,1000};p.phase=Phase::referenceReady;
 auto ready=views(map,c,p);check(ready.spectrum.size==64&&ready.display.size==0&&ready.trackerCheckpoint.size==0&&!ready.causalTrace.data,"Ready exposed later-phase outputs");
 p.phase=Phase::prepared;p.tracker={c.epoch,3,p.frame.logicalId,p.frame.consumedSequence};wire::Writer m{map.bytes(Section::metrics)};
 const uint32_t recordWords=trackerRecordWords(c.tracker),history=trackerHistoryCapacity(c.tracker);
 m.u32(0,1);m.u32(4,128);m.u64(8,p.frame.logicalId);m.u64(16,p.frame.consumedSequence);m.f64(24,-5);m.f64(32,20);m.u64(40,3);m.u64(48,2);m.u64(56,1);m.u64(64,trackerHeaderWords+recordWords);m.u32(72,1);m.u32(76,0);m.u32(80,c.stages());m.u32(84,c.stages());
 m.u32(96,3);m.u32(100,2);m.u32(104,2);
 wire::Writer flags{map.bytes(Section::flags)};flags.u32(0,7u|8u|(130u<<17));
 wire::Writer bank{map.bytes(Section::tracker1)};bank.zero();bank.u32(0,5);bank.u32(108,1);bank.u32(4,recordWords*4);bank.u32(8,c.tracker.capacity);bank.u32(12,history);bank.u64(16,1);bank.u64(40,c.epoch);bank.u64(48,p.frame.logicalId);bank.u64(56,p.frame.consumedSequence);
 auto prepared=views(map,c,p);check(prepared.detections.size==1&&prepared.detectionCells.size==1&&prepared.cfar.size==3&&prepared.centroid.size==2&&prepared.trackerCheckpoint.size==trackerHeaderWords+recordWords&&prepared.trackerSlot==1,"Prepared prefixes/slot differ");
 check(prepared.causalTrace.data&&prepared.causalTrace.size==2064,"Prepared causal trace fixed view missing");
 check(!prepared.reservedOutput.data&&!prepared.reservedOutput.size,
  "Retired resident output padding was populated");
 for(uint32_t i=0;i<l.count;++i)check(l.sections[i].id!=30,
  "Retired resident section was allocated");
 auto ordinary=c;ordinary.detector.integrationCpis=1;
 const auto ordinaryViews=[&]{m.u32(104,1);try{auto result=views(map,ordinary,p);m.u32(104,2);return result;}catch(...){m.u32(104,2);throw;}};
 for(uint32_t schema:{1u,2u,3u,4u}){bank.u32(0,schema);
  reject([&]{ordinaryViews();},"Legacy checkpoint without exact elapsed interval accepted");
  reject([&]{views(map,c,p);},"Temporal detector accepted legacy checkpoint");}
 bank.u32(0,5);reject([&]{ordinaryViews();},"Disjoint lineage accepted by single-CPI policy");
 bank.u32(108,0);check(ordinaryViews().trackerCheckpoint.size==prepared.trackerCheckpoint.size,
  "Ordinary schema5 rejected by matching policy");
 reject([&]{views(map,c,p);},"Temporal detector accepted ordinary lineage");bank.u32(108,1);
 check(views(map,c,p).trackerCheckpoint.size==prepared.trackerCheckpoint.size,
  "Temporal detector rejected disjoint schema5 history");
 bank.u32(0,6);reject([&]{views(map,c,p);},"Unknown tracker checkpoint schema accepted");bank.u32(0,5);
 auto minimal=p;minimal.diagnosticPresence=0;auto hidden=views(map,c,minimal);check(!hidden.filtered.data&&!hidden.reference.data&&!hidden.cfar.data&&!hidden.centroid.data&&!hidden.levels.data&&hidden.detectionCells.size==1&&hidden.flags.size==21&&hidden.detections.size==1&&hidden.maps.size==prepared.maps.size,"Optional diagnostics leaked or mandatory temporal lineage removed");
 m.u32(104,0);reject([&]{views(map,c,p);},"ABI15 metrics without acknowledged readiness accepted");
 m.u32(104,1);reject([&]{views(map,c,p);},"Warmup frame published a two-CPI candidate");m.u32(104,2);
 flags.u32(0,7u|24u|(130u<<17));reject([&]{views(map,c,p);},"Ambiguous OR-of-banks lineage accepted");
 flags.u32(0,7u|32u|(130u<<17));reject([&]{views(map,c,p);},"Unwarmed originating bank accepted");flags.u32(0,7u|8u|(130u<<17));
 auto merged=c;merged.detector.integrationCpis=4;
 reject([&]{views(map,merged,p);},"Merged candidate omitted its originating-cell offset");
 flags.u32(0,7u|8u|(130u<<17)|(6u<<26));
 const auto sourceView=views(map,merged,p);
 check(sourceView.flags.data[0]==(7u|8u|(130u<<17)|(6u<<26))&&
  blah2::temporal_candidate::sourceCell(sourceView.detectionCells.data[0],sourceView.flags.data[0],7,3,4)==1,
  "ABI16 source-cell offset did not survive wire publication");
 for(uint32_t maximum:{4u,8u})for(uint32_t available:{1u,maximum,1u}){
  merged.detector.integrationCpis=maximum;m.u32(104,available);
  flags.u32(0,7u|128u|(130u<<17)|(6u<<26));
  const auto fresh=views(map,merged,p);
  check(fresh.detectionCount==1&&fresh.availableCpis==available&&fresh.flags.data[0]&128u,
   "Current-CPI resident startup/steady/reset detection was rejected");
  flags.u32(0,7u|136u|(130u<<17)|(6u<<26));
  reject([&]{views(map,merged,p);},"Mixed current/temporal source flags admitted");
 }
 merged.detector.integrationCpis=4;m.u32(104,2);
 flags.u32(0,7u|8u|(130u<<17)|(4u<<26));reject([&]{views(map,merged,p);},"Source-cell offset outside grid accepted");
 flags.u32(0,7u|8u|(130u<<17)|(10u<<26));reject([&]{views(map,merged,p);},"Unknown source-cell offset accepted");
 flags.u32(0,7u|8u|(130u<<17));
 auto missing=c;missing.presence&=~bit(Section::flags);reject([&]{validateConfig(missing);},"Temporal flags treated as optional diagnostics");
 auto bad=[&](size_t at,uint64_t value){const auto before=wire::Reader{map.read(Section::metrics)}.u64(at);m.u64(at,value);reject([&]{views(map,c,p);},"Invalid output metadata accepted");m.u64(at,before);};
 bad(8,p.frame.logicalId-1);bad(16,18);bad(40,25);bad(48,4);bad(56,3);bad(64,33);bad(72,2);bad(88,1);bad(92,6);
 auto wrong=p;wrong.tracker.generation--;reject([&]{views(map,c,wrong);},"Stale prepared token accepted");wrong=p;wrong.phase=Phase::idle;reject([&]{views(map,c,wrong);},"Idle exposed outputs");wrong=p;wrong.presence^=bit(Section::canonical);reject([&]{views(map,c,wrong);},"Output presence substitution accepted");
}
}
int main()try{packetCases();trackerDiagnosticsCases();temporalProofCases();configCases();mappingCases();topologyCases();outputCases();std::cout<<"COMPLETE resident-wire CPU-only checks="<<checked<<" rejected="<<rejected<<" gpu_execution=0\n";return 0;}catch(const std::exception&e){std::cerr<<"FAIL resident-wire: "<<e.what()<<'\n';return 1;}
