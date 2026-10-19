#pragma once
#include "GpuBackend.h"
#include "GpuProcess.h"
#include <algorithm>
#include <array>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

// Adjacent, explicitly admitted ABI; never reinterpret an older object/mapping.
namespace blah2::resident {
constexpr unsigned moduleAbi=9;
constexpr uint32_t protocol=0x42475209,packetBytes=160,configBytes=8768;
constexpr uint64_t sharedLimit=512ULL<<20;
constexpr uint32_t readyEvidenceBytes=4096,preparedEvidenceBytes=61888,
 deviceEvidenceBytes=512,controlEvidenceBytes=512;
template<class T> struct Span {T* data=nullptr;
size_t size=0;};
using Bytes=Span<uint8_t>;
using ConstBytes=Span<const uint8_t>;
using Nonce=std::array<uint64_t,2>;
using Hash=std::array<uint8_t,32>;
enum class Op:uint32_t {initialize=1,initialized=2,restore=3,restored=4,
 begin=5,referenceReady=6,framePrepared=7,commit=8,committed=9,abort=10,
 aborted=11,published=12,settled=13,rejected=14,failure=15,quit=16};
enum class Phase:uint32_t {idle=0,begun=1,referenceReady=2,prepared=3,
 committed=4,settled=5,rejected=6,poisoned=7};
enum class Status:uint32_t {success=0,numeric=1,capacity=2,unsupported=3,
 protocolError=4,runtime=5};
enum class Section:uint32_t {config=1,delayAxis=2,dopplerAxis=3,canonical=4,
 maps=5,fused=6,display=7,metrics=8,spectrum=9,spectrumDb=10,frequencies=11,
 referenceCheckpoint=12,tracker0=13,tracker1=14,detections=15,filtered=16,
 tail=17,fullReference=18,levels=19,unflooredSnr=20,cfar=21,centroid=22,
 flags=23,detectionCells=24,readyEvidence=25,reason=26,device=27,
 preparedEvidence=28,controlEvidence=29,guidedRecoveries=30};
constexpr uint64_t bit(Section s){return uint64_t(1)<<uint32_t(s);}
constexpr uint64_t diagnosticMask=((uint64_t(1)<<25)-1)&~((uint64_t(1)<<16)-1);
inline uint64_t outputPresence(uint64_t capacity,uint64_t requested){return (capacity&~diagnosticMask)|requested;}
struct ReferenceConfig {
 std::vector<uint32_t> channels;
 uint32_t analysisSamples=32768,analysisInterval=10,powerIterations=12;
 double covarianceSmoothing=.8,diagonalLoading=.001,bandwidthHz=2000,centreHz=0;
};
enum class FusionMode:uint32_t {noncoherent=0,coherentRegularPolygon=1,
 noiseWhitenedRegularPolygon=2};
struct FusionConfig {
 FusionMode mode=FusionMode::noncoherent;
 uint32_t beams=0;
 double frequencyHz=0,radiusM=0,element0Rad=0,elementStepRad=0,directPathAzimuthRad=0,
  directPathNullDb=0;
};
struct DetectorConfig {
 bool enabled=false,interpolateDelay=true,interpolateDoppler=true;
 int32_t guard=0,train=0,minDelay=0;
 uint32_t centroidDelay=0,centroidDoppler=0;
 double pfa=0,minDoppler=0,centroidDopplerResolution=0;
 uint32_t integrationCpis=1;
 uint32_t integrationMotionBins=0,integrationBeamTolerance=UINT32_MAX;
};
enum class BearingMethod:uint32_t {coherentBeamBank=0,covarianceMvdr=1};
enum class BearingReferenceAnchor:uint32_t {liveEigenbeam=0,fixedSite=1};
struct BearingConfig {
 bool enabled=false;
 BearingMethod method=BearingMethod::coherentBeamBank;
 BearingReferenceAnchor referenceAnchor=BearingReferenceAnchor::liveEigenbeam;
 uint32_t scanBeams=0,candidatePeaks=3,guardRadius=1,trainingRadius=3;
 double diagonalLoading=.01;
 // Bearing-only logical-channel to physical-element permutation. Detection
 // fusion keeps its established path and is therefore unaffected.
 std::vector<uint32_t> channelMapping;
 // Optional measured complex response, local CCW scan-major then logical channel.
 // Empty retains the ideal regular-polygon manifold.
 std::vector<std::complex<float>> manifoldResponses;
};
enum class TrackerInitiationMode:uint32_t {hypothesisBank=0,adaptiveGate=1};
struct TrackerConfig {
 bool enabled=false;
 uint32_t capacity=0,m=0,n=0,nDelete=0;
 TrackerInitiationMode initiationMode=TrackerInitiationMode::hypothesisBank;
 double cpi=0,maxAccInit=0,rangeRes=0,lambda=0;
 bool stationaryClutter=false;
 uint32_t stationaryPersistenceCpis=0;
 double stationaryMaxDopplerHz=0;
 bool trackGuidedShadow=false;
 bool trackGuidedPromote=false;
 uint32_t trackGuidedShadowMaxTracks=0;
};
constexpr uint32_t trackerHeaderWords=32,trackerDisplayHistory=100;
inline uint32_t trackerHistoryCapacity(const TrackerConfig& t){
 return std::max(trackerDisplayHistory,t.n);
}
inline uint32_t trackerRecordWords(const TrackerConfig& t){
 return 16+8*trackerHistoryCapacity(t);
}
struct Config {
 GpuGeometry geometry{};
 uint32_t correlation=0,spectrumBins=0;
 double sampleRate=0,middle=0;
 uint64_t epoch=0,presence=0;
 std::vector<uint32_t> surveillanceChannels;
 std::vector<int32_t> delayAxis;
 std::vector<double> dopplerAxis;
 ReferenceConfig reference;
 FusionConfig fusion;
 BearingConfig bearing;
 DetectorConfig detector;
 TrackerConfig tracker;
 std::string device;
 uint32_t stages()const{return 126u+(detector.enabled?128u:0u)+(tracker.enabled?256u:0u);}
};
struct Token {uint64_t epoch=0,transaction=0,generation=0,consumedSequence=0;};
struct Frame {uint64_t logicalId=0,consumedSequence=0,signalTimestampMs=0;};
struct Packet {
 Op op=Op::initialize;
 Nonce session{};
 uint64_t requestId=0;
 Frame frame{};
 uint64_t epoch=0,mappedBytes=0,presence=0,diagnosticPresence=0;
 uint32_t descriptorCount=0;
 Status status=Status::success;
 Token tracker{};
 Phase phase=Phase::idle;
 uint32_t restoreTrackerSlot=0,errorCode=0,reasonLength=0;
};
struct Descriptor {
 uint32_t id=0,type=0,elementBytes=0,access=0;
 uint64_t offset=0,capacityBytes=0;
};
struct Layout {
 std::array<Descriptor,32> sections{};
 uint32_t count=0;
 uint64_t bytes=0;
 const Descriptor& at(Section id)const;
};
struct Mapping {
 uint8_t* data=nullptr;
 size_t size=0;
 Layout layout{};
 Bytes bytes(Section)const;
 ConstBytes read(Section s)const{auto b=bytes(s);
 return {b.data,b.size};}
};
struct DetectionRecord {double delay=0,doppler=0,snr=0,delayKm=0;};
struct OutputViews {
 Span<const std::complex<float>> maps;
 Span<const std::complex<double>> fused,spectrum,filtered,tail,reference;
 Span<const double> display,spectrumDb,frequencies,levels,unflooredSnr;
 // Parent Session-owned immutable setup metadata; no new wire/GPU allocation.
 Span<const double> delayKm,doppler;
 Span<const DetectionRecord> detections,cfar,centroid;
 Span<const uint32_t> flags,detectionCells,trackerCheckpoint,guidedRecoveryWords;
 double noise=0,maxPower=0;
 uint64_t cfarCount=0,centroidCount=0,detectionCount=0;
 Token trackerToken{};
 bool trackerNoOp=false;
 uint32_t trackerSlot=0;
 ConstBytes referenceCheckpoint,readyEvidence,preparedEvidence,deviceEvidence,controlEvidence;
};
// Module calls are synchronous; caller owns wire-phase responses and deadlines.
// The Mapping is validated identically in both processes before factory entry.
class Backend {
public:
 virtual ~Backend()=default;
 virtual void initializeEvidence(const Packet&)=0;
 virtual bool prepareReference(const Packet&)=0;
 virtual bool finishRadar(const Packet&)=0;
 // False stage results remain whole-frame rejections; capacity is explicit.
 virtual Status rejectionStatus()const{return Status::numeric;}
 virtual Token preparedToken()const=0;
 virtual void commit(const Packet&)=0;
 virtual void abort(const Packet&)=0;
 virtual void restore(const Packet&)=0;
 virtual void settle(const Packet&)=0;
 virtual void shutdown(const Packet&)=0;
};
using Create=Backend*(*)(unsigned,const Config*,const Mapping*);
using Factory=std::function<std::unique_ptr<Backend>(const Config&,const Mapping&)>;
// Session owns one worker/mapping. Exceptions invalidate it; no fallback/retry.
class Session {
public:
 virtual ~Session()=default;
 virtual const Config& config()const=0;
 virtual const Layout& layout()const=0;
 virtual Phase phase()const=0;
 virtual Span<std::complex<double>> input()=0; // idle/settled only
 virtual void begin(Frame,uint64_t diagnosticPresence=0)=0;
 virtual OutputViews receiveReferenceReady()=0;
 virtual OutputViews receivePrepared()=0;
 virtual void commit(Token)=0;
 virtual void abort(Token)=0;
 virtual void published()=0;
 virtual void restore(ConstBytes reference,Span<const uint32_t> tracker)=0;
 virtual void stop()noexcept=0;
};
std::unique_ptr<Session> createProcess(const Config&,const GpuProcessOptions& = {});
int runWorker(Factory);
void validateConfig(const Config&);
Layout makeLayout(const Config&,uint64_t pageBytes);
std::array<uint8_t,packetBytes> encode(const Packet&);
Packet decode(ConstBytes);
std::array<uint8_t,configBytes> encodeConfig(const Config&);
Config decodeConfig(ConstBytes config,ConstBytes delayAxis,ConstBytes dopplerAxis);
void initializeMapping(Mapping,const Config&,Nonce);
void validateMapping(Mapping,const Config&,Nonce);
OutputViews views(Mapping,const Config&,const Packet&);
}
