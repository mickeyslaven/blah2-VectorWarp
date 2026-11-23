#pragma once
#include "GpuBackend.h"
#include "GpuProcess.h"
#include "process/tracker/CausalTraceLayout.h"
#include "process/tracker/Tracker.h"
#include "process/analysis/ResidentReplaySnapshot.h"
#include <algorithm>
#include <array>
#include <complex>
#include <cmath>
#include <cstring>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>
#ifdef __APPLE__
#include <unistd.h>
#endif

// Adjacent, explicitly admitted ABI; never reinterpret an older object/mapping.
namespace blah2::resident {
constexpr unsigned moduleAbi=18;
inline void validateModuleAbi(unsigned abi){
 if(abi!=moduleAbi)throw std::invalid_argument("Resident module ABI18 required: older modules lack canonical tracker frame diagnostics");
}

constexpr uint32_t protocol=0x42475212,packetBytes=160,configBytes=32768;
static_assert(configBytes>=640+(360+127+1)*8*sizeof(std::complex<float>)+152,
 "Resident config cannot carry maximum fusion and bearing manifolds");
constexpr uint64_t sharedLimit=512ULL<<20;
// Exact replay snapshots duplicate the complete resident state in the sealed
// mapping.  Keep the ordinary live ceiling unchanged; only an explicitly
// configured replay export may use the larger bounded mapping.
constexpr uint64_t replaySharedLimit=1ULL<<30;
constexpr uint32_t readyEvidenceBytes=4096,preparedEvidenceBytes=62224,
 deviceEvidenceBytes=512,controlEvidenceBytes=512;
template<class T> struct Span {T* data=nullptr;
size_t size=0;};
using Bytes=Span<uint8_t>;
using ConstBytes=Span<const uint8_t>;
using Nonce=std::array<uint64_t,2>;
using Hash=std::array<uint8_t,32>;
enum class Op:uint32_t {initialize=1,initialized=2,restore=3,restored=4,
 begin=5,referenceReady=6,framePrepared=7,commit=8,committed=9,abort=10,
 aborted=11,published=12,settled=13,rejected=14,failure=15,quit=16,
 snapshot=17,snapshotted=18,resetInput=19,inputReset=20};
enum class Phase:uint32_t {idle=0,begun=1,referenceReady=2,prepared=3,
 committed=4,settled=5,rejected=6,poisoned=7};
enum class Status:uint32_t {success=0,numeric=1,capacity=2,unsupported=3,
 protocolError=4,runtime=5};
enum class Section:uint32_t {config=1,delayAxis=2,dopplerAxis=3,canonical=4,
 maps=5,fused=6,display=7,metrics=8,spectrum=9,spectrumDb=10,frequencies=11,
 referenceCheckpoint=12,tracker0=13,tracker1=14,detections=15,filtered=16,
 tail=17,fullReference=18,levels=19,unflooredSnr=20,cfar=21,centroid=22,
 flags=23,detectionCells=24,readyEvidence=25,reason=26,device=27,
 preparedEvidence=28,controlEvidence=29,reserved30=30,replaySnapshot=31,causalTrace=32};
constexpr uint64_t bit(Section s){return uint64_t(1)<<uint32_t(s);}
constexpr uint64_t diagnosticMask=((uint64_t(1)<<25)-1)&~((uint64_t(1)<<16)-1);
inline uint64_t outputPresence(uint64_t capacity,uint64_t requested){
 // ABI16: allocated candidate-cell/flag identities are publication metadata,
 // including on frames which do not request expensive numeric diagnostics.
 return (capacity&(~diagnosticMask|bit(Section::flags)|bit(Section::detectionCells)))|requested;}
enum class ReferenceMode:uint32_t {capturedArray=1,capturedDedicated=2};
struct ReferenceConfig {
 std::vector<uint32_t> channels;
 uint32_t analysisSamples=32768,analysisInterval=10,powerIterations=12;
 double covarianceSmoothing=.8,diagonalLoading=.001,bandwidthHz=2000,centreHz=0;
 ReferenceMode mode=ReferenceMode::capturedArray;
};
enum class FusionMode:uint32_t {noncoherent=0,coherentRegularPolygon=1,
 noiseWhitenedRegularPolygon=2};
struct FusionConfig {
 FusionMode mode=FusionMode::noncoherent;
 uint32_t beams=0;
 double frequencyHz=0,radiusM=0,element0Rad=0,elementStepRad=0,directPathAzimuthRad=0,
  directPathNullDb=0;
 // Optional measured complex response, scan-major then logical channel.
 // Empty retains the ideal regular-polygon manifold.
 std::vector<std::complex<float>> manifoldResponses;
 // Fixed-transmitter response, kept separate from aircraft azimuth beams.
 std::vector<std::complex<float>> directResponse;
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
// Value2 is an explicitly selected native-replay experiment, never a default.
enum class BearingReferenceAnchor:uint32_t {liveEigenbeam=0,fixedSite=1,fixedSiteProjected=2};
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
 double minInitiationSnrDb=-100;
 bool stationaryClutter=false;
 uint32_t stationaryPersistenceCpis=0;
 double stationaryMaxDopplerHz=0;
 // Reserved configuration padding: retired experimental flags remain zero.
 std::array<uint8_t,3> reservedPolicy{};
 bool normalizedInnovationGate=false;
 uint32_t reservedLimit=0;
};
constexpr uint32_t trackerHeaderWords=32,trackerDisplayHistory=100;
inline uint32_t trackerHistoryCapacity(const TrackerConfig& t){
 return std::max(trackerDisplayHistory,t.n);
}
inline uint32_t trackerRecordWords(const TrackerConfig& t){
 return 20+8*trackerHistoryCapacity(t);
}
struct Config {
 GpuGeometry geometry{};
 // Canonical capture order; geometry.channels counts surveillance outputs.
 // Both mappings index this input, never a silently compacted channel list.
 uint32_t inputChannels=0;
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
 resident_replay::ExportConfig replay;
 std::string device;
 // Noncoherent fusion has no steering frequency; motion still follows the
 // actual canonical lane's carrier, not the capture center or another lane.
 double temporalCarrierHz()const{return fusion.mode==FusionMode::noncoherent?
  reference.centreHz:fusion.frequencyHz;}
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
 Span<const uint32_t> flags,detectionCells,trackerCheckpoint;
 // Reserved output padding. Never allocated, exported or populated.
 Span<const uint32_t> reservedOutput;
 Span<const uint32_t> causalTrace;
 double noise=0,maxPower=0;
 uint64_t cfarCount=0,centroidCount=0,detectionCount=0;
 bool detectorObserved=false;
 uint64_t evaluatedCells=0;
 uint32_t availableCpis=0;
 Token trackerToken{};
 bool trackerNoOp=false;
 TrackerFrameDiagnostics trackerDiagnostics{};
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
 // Input discontinuity, not configuration change: retain plans and identity.
 virtual void resetInput(const Packet&)=0;
 virtual void settle(const Packet&)=0;
 virtual void shutdown(const Packet&)=0;
 virtual void snapshot(const Packet&){throw std::logic_error("Resident replay snapshot unsupported");}
};
using Create=Backend*(*)(unsigned,const Config*,const Mapping*);
using Factory=std::function<std::unique_ptr<Backend>(const Config&,const Mapping&)>;
// Session owns one worker/mapping. Exceptions invalidate it; no fallback/retry.
class Session {
public:
 virtual ~Session()=default;
 virtual const Config& config()const=0;
 virtual const Layout& layout()const=0;
 // Immutable config/module/device binding, independent of session nonce and
 // frame IDs. Used only to transfer a completed private numerical receipt to
 // a fresh production instance of exactly the same admitted implementation.
 virtual std::array<uint8_t,96> qualificationIdentity()const=0;
 virtual Phase phase()const=0;
 virtual Span<std::complex<double>> input()=0; // idle/settled only
 virtual void begin(Frame,uint64_t diagnosticPresence=0)=0;
 virtual OutputViews receiveReferenceReady()=0;
 virtual OutputViews receivePrepared()=0;
 virtual void commit(Token)=0;
 virtual void abort(Token)=0;
 virtual void published()=0;
 virtual void restore(ConstBytes reference,Span<const uint32_t> tracker)=0;
 // Idle/settled only. Discard learned/temporal state, never rewind frame IDs.
 virtual void resetInput()=0;
 virtual std::vector<uint8_t> snapshot(){throw std::logic_error("Resident replay snapshot unsupported");}
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
// Adjacent, versioned map-only capability. The existing portable/resident ABIs
// remain unchanged. Consumed by canonical ProductStages and the isolated worker;
// no receiver, reference-conditioning, clutter, fusion or tracker state lives here.
namespace blah2::temporal_gpu {
constexpr unsigned moduleAbi=1;
constexpr uint32_t protocol=0x42545001;
struct Config {
 resident::DetectorConfig detector;
 uint32_t sampleRate=0;
 double delayDriftPerHz=0;
 std::vector<int32_t> delayAxis;
 std::vector<double> dopplerAxis;
 std::string device;
};
struct Record {double delay=0,doppler=0,snr=0,delayKm=0;};
struct Metrics {
 uint32_t available=0,count=0,cfar=0,evaluated=0;
 double noise=0,maximum=0,uploadMs=0,executeMs=0,readbackMs=0;
 std::array<double,6> gpuMs{};
};
struct Output {
 Metrics metrics;
 double* display=nullptr;
 Record* records=nullptr;
 uint32_t* flags=nullptr;
 uint32_t* cells=nullptr;
};
class Backend {
public:
 virtual ~Backend()=default;
 virtual void process(const std::complex<double>*,Output&)=0;
 virtual void reset()=0;
};
using Create=Backend*(*)(unsigned,const Config*);
using Factory=std::function<std::unique_ptr<Backend>(const Config&)>;
class Session {
public:
 virtual ~Session()=default;
 virtual std::complex<double>* input()=0;
 virtual const Output& process(uint64_t sequence)=0;
 virtual void reset()=0;
};
// One sealed, fixed-capacity mapping; outputs are borrowed until the next call.
// Only compact candidate records are decoded by the parent, never JSON or DSP.
struct Header {
 uint32_t tag=protocol,rows=0,columns=0,sampleRate=0;
 resident::DetectorConfig detector{};
 double drift=0;
 uint64_t bytes=0,nonce=0;
 char device[96]{};
};
static_assert(std::is_trivially_copyable_v<Header>);
struct Layout {
 uint64_t delay=0,doppler=0,input=0,display=0,records=0,flags=0,cells=0,bytes=0;
 uint32_t count=0;
};
inline Layout layout(uint32_t rows,uint32_t columns){
 if(!rows||!columns||rows>65535||columns>65535||uint64_t(rows)*columns>(1u<<20))
  throw std::invalid_argument("Temporal GPU map dimensions exceed canonical bound");
 const auto aligned=[](uint64_t n){return (n+63u)&~uint64_t(63u);};
 Layout l;l.count=rows*columns;l.delay=aligned(sizeof(Header));
 l.doppler=aligned(l.delay+4*columns);l.input=aligned(l.doppler+8*rows);
 l.display=l.input+16*l.count;l.records=l.display+8*l.count;
 l.flags=l.records+sizeof(Record)*l.count;l.cells=l.flags+4*l.count;
 l.bytes=aligned(l.cells+4*l.count);
#ifdef __APPLE__
 // Match Darwin's reported POSIX shared-memory length in both processes.
 const long page=sysconf(_SC_PAGESIZE);
 if(page<=0)throw std::runtime_error("Temporal GPU shared-memory page size unavailable");
 l.bytes=((l.bytes+uint64_t(page)-1)/uint64_t(page))*uint64_t(page);
#endif
 return l;
}
inline void validate(const Config& c){
 (void)layout(uint32_t(c.dopplerAxis.size()),uint32_t(c.delayAxis.size()));
 const auto& d=c.detector;
 if(!c.sampleRate||c.device.empty()||c.device.size()>=sizeof(Header::device)||
    !d.enabled||(d.integrationCpis!=2&&d.integrationCpis!=4&&d.integrationCpis!=8)||
    d.integrationBeamTolerance!=UINT32_MAX||!std::isfinite(c.delayDriftPerHz)||
    c.delayDriftPerHz>0||!std::isfinite(d.pfa)||d.pfa<=0||d.pfa>=1||
    d.guard<0||d.guard>127||d.train<1||d.train>127||
    !std::isfinite(d.minDoppler)||d.minDoppler<0||
    !std::isfinite(d.centroidDopplerResolution)||d.centroidDopplerResolution<=0||
    d.centroidDelay>65535||d.centroidDoppler>65535)
  throw std::invalid_argument("Temporal GPU detector contract invalid");
 for(size_t i=0;i<c.delayAxis.size();++i)
  if(int64_t(c.delayAxis[i])!=int64_t(c.delayAxis[0])+int64_t(i))
   throw std::invalid_argument("Temporal GPU delay grid is not contiguous");
 for(size_t i=0;i<c.dopplerAxis.size();++i)
  if(!std::isfinite(c.dopplerAxis[i])||(i&&c.dopplerAxis[i]<=c.dopplerAxis[i-1]))
   throw std::invalid_argument("Temporal GPU Doppler grid is not ordered");
}
struct MemoryReservation {
 uint64_t mappingBytes=0,workingBytes=0,headroomBytes=0;
 uint64_t bytes()const{return mappingBytes+workingBytes+headroomBytes;}
};
// Pure admission estimate for the map-only seam, not a device-specific quota.
// Include every MapStage/Stage buffer, including host-visible staging, before
// sharing a lane with portable CAF/clutter. Actual Vulkan allocation sizes and
// per-heap limits remain authoritative; unusual driver padding fails closed.
inline MemoryReservation memoryReservation(const Config& c,uint64_t pageBytes){
 validate(c);
 if(!pageBytes||(pageBytes&(pageBytes-1))||pageBytes>(1ULL<<30))
  throw std::invalid_argument("Temporal GPU reservation page size is invalid");
 const uint64_t rows=c.dopplerAxis.size(),columns=c.delayAxis.size();
 const auto wire=layout(uint32_t(rows),uint32_t(columns));
 const uint64_t cells=wire.count,depth=c.detector.integrationCpis;
 const bool tbd=depth>2;
 const uint64_t banks=tbd?(depth==8?3u:2u):0u,passes=tbd?banks+1u:1u;
 // Postprocess scratch: 24 words/cell, plus 36 for multi-bank candidates;
 // four reduction words per 128 cells and at most two alignment words.
 const uint64_t scratch=(tbd?240u:96u)*cells+16*((cells+127)/128)+8;
 // Metadata includes axes, training windows, centroid bounds and all CFAR
 // population tables. Reserve the maximum supported 33 Doppler paths for
 // fractional offsets, avoiding a second copy of the motion-tag decoder.
 const uint64_t offsets=tbd&&c.delayDriftPerHz!=0?rows*33*depth:0;
 const uint64_t metadataWords=96+4*rows+(tbd?17u:11u)*columns+1+offsets+
  (tbd?2*passes*(2*uint64_t(c.detector.train)+1)+2*passes:0);
 MemoryReservation result;
 result.mappingBytes=(wire.bytes+pageBytes-1)&~(pageBytes-1);
 result.workingBytes=48*cells+scratch+8*cells*(tbd?depth:1u)+8*cells+
  (tbd?8*cells*(banks+1):0)+4+4*metadataWords;
 // MapStage owns eight buffers (nine with TBD), all counted above, and has no
 // FFT plans or data-dependent scratch. Reserve 16 MiB for their allocation
 // padding rather than a percentage of already-counted map/history storage.
 // Actual Vulkan requirements still fail closed if this allowance is exceeded.
 result.headroomBytes=16ULL<<20;
 return result;
}
inline Output outputs(uint8_t* p,const Layout& l){
 return {{},reinterpret_cast<double*>(p+l.display),reinterpret_cast<Record*>(p+l.records),
  reinterpret_cast<uint32_t*>(p+l.flags),reinterpret_cast<uint32_t*>(p+l.cells)};
}
std::unique_ptr<Session> createProcess(const Config&,const GpuProcessOptions& = {});
int runWorker(const Factory&);
}
