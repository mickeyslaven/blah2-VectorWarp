#pragma once
// Private GPU tracker: no host tracking arithmetic or production wire/main path.
#include "GpuTrackerAdmission.private.h"
#include "process/tracker/AssociationContract.h"
struct TrackerConfig {
 bool enabled=false;
 uint32_t capacity=0,m=0,n=0,nDelete=0;
 bool adaptiveGate=false;
 double cpi=0,maxAccInit=0,rangeRes=0,lambda=0;
 double minInitiationSnrDb=-100;
 bool stationaryClutter=false;
 uint32_t stationaryPersistenceCpis=0;
 double stationaryMaxDopplerHz=0;
 bool normalizedInnovationGate=false;
 uint64_t epoch=0;
 bool causalDiagnostics=false;
 uint32_t maximumStationaryCells=tracker_policy::residentStationaryCapacity;
};
struct TrackerFrame {uint64_t generation=0,consumedSequence=0,timestamp=0;};
struct TrackerToken {uint64_t epoch=0,transaction=0,generation=0,consumedSequence=0;};
struct TrackerResult {TrackerToken token{};
uint32_t checkpointWords=0;
bool noOp=false;};
struct TrackerOutputs {uint32_t* checkpoint=nullptr;
size_t wordCapacity=0;
TrackerResult* result=nullptr;};
namespace gpu_tracker {
constexpr uint32_t headerWords=32,displayHistory=100,statusWords=66,workWords=12,
 clutterHeaderWords=persistenceHeaderWords;
// Keep the public evidence/wire header fixed. Private configuration precedes
// per-frame exclusive rejection counters; neither is tracker checkpoint state.
enum RejectionReason : uint32_t {minimumSnr,dopplerComb,rangeComb,persistentOccupancy,
 boundary,capacity,lineageOverlap,stationaryCapacity,rejectionWords};
constexpr uint32_t minInitiationSnrWord=statusWords,rejectionBase=statusWords+2,
 acceptedFrameWord=rejectionBase+rejectionWords,stationaryOrdinalWord=acceptedFrameWord+1,
 metadataHeaderWords=stationaryOrdinalWord+2;
struct Admission {uint32_t bankWords=0,recordWords=0,historyCount=0,nAcc=0,hypotheses=0;
uint64_t stageBytes=0,totalBytes=0;
double resolution=0;};
inline Admission validate(const TrackerConfig& c,uint32_t cells,uint32_t maximumDepth=1){
 if(!c.enabled||!c.capacity||!cells||cells>UINT32_MAX/24||!c.m||c.m>c.n||c.n>255||!std::isfinite(c.cpi)||c.cpi<=0||
    !std::isfinite(c.maxAccInit)||c.maxAccInit<0||!std::isfinite(c.rangeRes)||c.rangeRes<=0||!std::isfinite(c.lambda)||c.lambda<=0||
    !std::isfinite(c.minInitiationSnrDb)||c.minInitiationSnrDb < -100||c.minInitiationSnrDb > 100||
    !std::isfinite(c.stationaryMaxDopplerHz)||c.stationaryMaxDopplerHz<0||
    (c.stationaryClutter?(c.stationaryPersistenceCpis<2||c.stationaryPersistenceCpis>65535):
      (c.stationaryPersistenceCpis||c.stationaryMaxDopplerHz!=0))||
    c.capacity>(UINT32_MAX-127)/128||!c.maximumStationaryCells||c.maximumStationaryCells>1048576)
  throw std::invalid_argument("GPU_TRACKER_UNSUPPORTED configuration or index capacity");
 Admission a;
 tracker_confirmation::validate(c.m,c.n,c.nDelete,maximumDepth);
 a.historyCount=std::max(displayHistory,c.n);
 a.recordWords=20+8*a.historyCount;
 if(c.capacity>(UINT32_MAX-headerWords)/a.recordWords)
  throw std::invalid_argument("GPU_TRACKER_UNSUPPORTED record index capacity");
 a.resolution=1/(c.cpi*c.cpi);
 if(!std::isfinite(a.resolution)||a.resolution<=0)throw std::invalid_argument("GPU_TRACKER_UNSUPPORTED acceleration resolution");
 const double bins=std::floor(c.maxAccInit/a.resolution);
 if(!std::isfinite(bins)||bins<0||bins>65535)throw std::invalid_argument("GPU_TRACKER_UNSUPPORTED acceleration hypotheses");
 a.nAcc=uint32_t(bins);
 a.hypotheses=c.adaptiveGate?1:2*a.nAcc+1;
 if(!std::isfinite(a.resolution*double(a.nAcc))||!std::isfinite(1/c.cpi))throw std::invalid_argument("GPU_TRACKER_UNSUPPORTED derived configuration");
 a.bankWords=headerWords+a.recordWords*c.capacity;
 a.stageBytes=uint64_t(metadataHeaderWords+uint64_t(workWords)*c.capacity+
  (c.causalDiagnostics?2064u:0u))*4;
 a.totalBytes=uint64_t(a.bankWords)*8+a.stageBytes+
  persistenceBytes(c.stationaryClutter,cells);
 if(uint64_t(a.bankWords)*4>SIZE_MAX||a.stageBytes>SIZE_MAX){throw std::invalid_argument("GPU_TRACKER_UNSUPPORTED host byte capacity");}
 return a;
}
inline bool sameToken(const TrackerToken& a,const TrackerToken& b){return a.epoch==b.epoch&&a.transaction==b.transaction&&a.generation==b.generation&&a.consumedSequence==b.consumedSequence;}
inline bool sameDouble(double a,double b){return std::memcmp(&a,&b,sizeof(a))==0;}
inline bool sameConfig(const TrackerConfig& a,const TrackerConfig& b){return a.maximumStationaryCells==b.maximumStationaryCells&&a.enabled==b.enabled&&a.capacity==b.capacity&&a.m==b.m&&a.n==b.n&&a.nDelete==b.nDelete&&a.adaptiveGate==b.adaptiveGate&&a.stationaryClutter==b.stationaryClutter&&a.stationaryPersistenceCpis==b.stationaryPersistenceCpis&&a.normalizedInnovationGate==b.normalizedInnovationGate&&a.epoch==b.epoch&&sameDouble(a.cpi,b.cpi)&&sameDouble(a.maxAccInit,b.maxAccInit)&&sameDouble(a.rangeRes,b.rangeRes)&&sameDouble(a.lambda,b.lambda)&&sameDouble(a.minInitiationSnrDb,b.minInitiationSnrDb)&&sameDouble(a.stationaryMaxDopplerHz,b.stationaryMaxDopplerHz);}
const char* common=R"glsl(#version 450
#extension GL_ARB_gpu_shader_fp64 : require
layout(local_size_x=128) in;
layout(binding=0) buffer Metadata {uint m[];};
layout(binding=1) buffer BankA {uint a[];};
layout(binding=2) buffer BankB {uint b[];};
layout(binding=3) buffer Scratch {uint s[];};
layout(binding=4) readonly buffer Postprocess {uint post[];};
layout(binding=5) buffer StationaryClutter {uint clutterPersistence[];};
layout(push_constant) uniform P {uint count;uint a;uint b;uint c;int d;} p;
uint indexOf(){return gl_GlobalInvocationID.x+gl_GlobalInvocationID.y*gl_NumWorkGroups.x*128u;}
uint groupOf(){return gl_WorkGroupID.x+gl_WorkGroupID.y*gl_NumWorkGroups.x;}
uint src(uint at){return m[1]==0u?a[at]:b[at];}
uint dst(uint at){return m[1]==0u?b[at]:a[at];}
void put(uint at,uint value){if(m[1]==0u)b[at]=value;else a[at]=value;}
uint checkedWord(uint at){return m[2]==1u?src(at):dst(at);}
bool finiteD(double x){return !isnan(x)&&!isinf(x);}
void reject(uint code){atomicCompSwap(m[0],0u,code);}
uvec2 pairM(uint at){return uvec2(m[at],m[at+1]);}
uvec2 pairS(uint at){return uvec2(src(at),src(at+1));}
uvec2 plusOne(uvec2 value){uint low=value.x+1u;return uvec2(low,value.y+uint(low==0u));}
bool greater64(uvec2 left,uvec2 right){return left.y>right.y||(left.y==right.y&&left.x>right.x);}
uvec2 subtract64(uvec2 left,uvec2 right){return uvec2(left.x-right.x,left.y-right.y-uint(left.x<right.x));}
double double64(uvec2 value){precise double high=double(value.y)*4294967296.0LF;precise double sum=high+double(value.x);return sum;}
double md(uint at){return packDouble2x32(uvec2(m[at],m[at+1]));}
double sd(uint at){return packDouble2x32(uvec2(s[at],s[at+1]));}
double sourceD(uint at){return packDouble2x32(uvec2(src(at),src(at+1)));}
double checkedD(uint at){return packDouble2x32(uvec2(checkedWord(at),checkedWord(at+1)));}
void writeM(uint at,double value){uvec2 q=unpackDouble2x32(value);m[at]=q.x;m[at+1]=q.y;}
uint historyCapacity(){return max(m[40],100u);}
uint recordWords(){return 20u+8u*historyCapacity();}
uint timingOffset(){return 16u+8u*historyCapacity();}
uint record(uint i){return 32u+recordWords()*i;}
uint work(uint i){return TRACKER_METADATA_HEADER_WORDS+12u*i;}
// SHARED_CONFIRMATION_CONTRACT
// SHARED_ASSOCIATION_CONTRACT
// SHARED_QUARANTINE_CONTRACT
// SHARED_STATIONARY_CONTRACT
// BEGIN PURE LINEAGE CONTRACT (also compiled as C++ by the focused test).
bool lineageDepthValid(uint depth){return confirmationDepthValid(depth);}
bool lineageCounterNext(uint priorLow,uint priorHigh,uint nextLow,uint nextHigh){
 return !(priorLow==0xffffffffu&&priorHigh==0xffffffffu)&&nextLow==priorLow+1u&&
  nextHigh==priorHigh+uint(nextLow==0u);
}
bool lineageTemporalNext(uint prior,uint next){return prior<0xfffffffeu&&next==prior+1u;}
uint lineageDepth(uint word,bool experimental){return word>>(experimental?5u:4u);}
uint lineageVote(uint word,bool experimental){return (word&(experimental?16u:4u))!=0u?1u:0u;}
bool lineageDisjoint(uint depth,uint recentCredits){
 return confirmationDisjoint(depth,recentCredits);
}
uint lineageWord(uint state,uint provenance,uint depth,bool credit,bool experimental){
 return state|provenance|(depth<<(experimental?5u:4u))|(experimental&&credit?16u:0u);
}
bool lineageWordValid(uint word,bool experimental){
 uint evidence=word&12u,state=word&3u,depth=lineageDepth(word,experimental);
 return word<=(experimental?511u:255u)&&(evidence&8u)==0u&&
  !(state==3u&&(evidence&4u)!=0u)&&
  !(experimental&&(word&16u)!=0u&&evidence==0u)&&
  (evidence==0u?depth==0u:(experimental?lineageDepthValid(depth):depth==1u));
}
// END PURE LINEAGE CONTRACT
bool experimentalLineage(){return (m[38]&0x40000000u)!=0u;}
bool normalizedInnovationGate(){return (m[38]&0x10000000u)!=0u;}
uint checkpointSchema(){return 5u;}
uint detectionSpatial(uint i){uint cell=s[post[41]+i],direction=s[post[86]+cell];
 if((direction&0x80000000u)!=0u)return direction;
 uint flags=s[post[39]+cell],coarse=(flags>>8u)&511u,fine=(flags>>17u)&2047u,quality=(flags>>28u)&15u;
 // ABI16 temporal high bits carry selected-path motion, not a fine beam.
 if(post[74]>1u){fine=0u;quality=0u;}
 return coarse|(fine<<9u)|(quality<<20u);}
uint detectorSupportDepth(uint i){uint cell=s[post[41]+i];if(cell>=m[4])return 1u;
 uint qualified=(s[post[39]+cell]>>3u)&15u;
 if(experimentalLineage()&&post[78]!=0u&&qualified==0u){reject(321u);return 0u;}
 uint depth=(qualified&8u)!=0u?8u:((qualified&4u)!=0u?4u:((qualified&3u)!=0u||post[74]==2u?2u:1u));
 if(experimentalLineage()&&(depth>post[74]||(post[78]!=0u&&depth>post[79]))){reject(321u);return 0u;}
 return depth;}
uint latestSpatial(uint r){uint packed=src(r+13u),head=packed&255u,length=(packed>>8u)&255u,h=historyCapacity();
 return length==0u?0u:src(r+16u+8u*((head+length-1u)%h)+7u);}
bool bearingCompatible(uint trackSpatial,uint detectionSpatialValue){uint beams=post[75],tolerance=post[77];
 return spatialCompatible(trackSpatial,detectionSpatialValue,beams,post[81],tolerance);
}
bool adaptiveGate(){return (m[38]&0x80000000u)!=0u;}
uint hypothesisCount(){return m[38]&0x0fffffffu;}
bool traceEnabled(){return (m[38]&0x20000000u)!=0u;}
uint traceBase(){return TRACKER_METADATA_HEADER_WORDS+12u*m[3]
 ;}
uint traceRecord(uint i){return traceBase()+16u+32u*i;}
uint traceSupportDepth(uint i){uint cell=s[post[41]+i];if(cell>=m[4])return 0u;
 uint qualified=(s[post[39]+cell]>>3u)&15u;
 return (qualified&8u)!=0u?8u:((qualified&4u)!=0u?4u:((qualified&3u)!=0u||post[74]==2u?2u:1u));}
void traceCandidate(uint i,uint reason){if(!traceEnabled()||i>=32u)return;
 uint t=traceRecord(i),at=m[5]+8u*i;m[t]=i;m[t+1u]=reason;m[t+4u]=i;
 // Read-only provenance extraction must not invoke a rejecting inference helper
 // for a candidate that the ordinary admission path would never consume.
 m[t+5u]=1u|(traceSupportDepth(i)<<8u);
 for(uint k=0u;k<6u;++k)m[t+6u+k]=s[at+k];}
void traceTrack(uint i,uint reason,uint state,uint match,bool credit,uint depth){
 if(!traceEnabled()||i>=32u)return;uint t=traceRecord(32u+i),r=record(i),w=work(i);
 m[t]=src(r+12u)&65535u;m[t+1u]=reason;m[t+2u]=state;m[t+3u]=src(r+14u);m[t+4u]=match;
 m[t+5u]=(m[t+5u]&12u)|(match!=0xffffffffu?1u:0u)|(credit?2u:0u)|(depth<<8u);
 for(uint k=0u;k<6u;++k)m[t+6u+k]=match!=0xffffffffu?s[m[5]+8u*match+k]:(k<4u?m[w+k]:0u);}
bool clutterAware(){return clutterPersistence[0]!=0u;}
uint clutterState(){return 12u+(1u-m[1])*(1u+3u*m[4]);}
uint clutterCell(uint cell){return clutterState()+1u+3u*cell;}
uint clutterStreak(uint cell){uint at=clutterCell(cell);
 return persistentEvidence(clutterPersistence[at],clutterPersistence[at+1u],clutterPersistence[at+2u],m[TRACKER_STATIONARY_ORDINAL_WORD],m[TRACKER_STATIONARY_ORDINAL_WORD+1u]);
}
uint clutterRidgeStreak(uint cell){
 uint rows=post[32],delayBins=post[33],cells=m[4];
 if(rows==0u||delayBins==0u||cells==0u||cells%delayBins!=0u||cells/delayBins!=rows||cell>=cells){reject(320u);return 0u;}
 uint row=cell/delayBins,delay=cell%delayBins,best=0u;
 uint rowFirst=row>0u?row-1u:0u,rowLast=min(rows-1u,row+1u);
 uint delayFirst=delay>4u?delay-4u:0u,delayLast=min(delayBins-1u,delay+4u);
 for(uint r=rowFirst;r<=rowLast;++r)for(uint d=delayFirst;d<=delayLast;++d)
  best=max(best,clutterStreak(r*delayBins+d));
 return best;
}
uint nearestDopplerRow(double value){uint count=post[32],low=0u,high=count;
 while(low<high){uint middle=low+(high-low)/2u;
  if(packDouble2x32(uvec2(post[post[44]+2u*middle],post[post[44]+2u*middle+1u]))<value)low=middle+1u;else high=middle;}
 if(low==0u)return 0u;if(low>=count)return count-1u;
 double right=packDouble2x32(uvec2(post[post[44]+2u*low],post[post[44]+2u*low+1u]));
 double left=packDouble2x32(uvec2(post[post[44]+2u*(low-1u)],post[post[44]+2u*(low-1u)+1u]));
 return nearestAxisLeft(value,left,right)?low-1u:low;}
uint policyCount(){return m[19];}
uint policyTracks(){return m[18];}
double policyDelay(uint i){return sd(m[5]+8u*i);}
double policyRow(uint i){return double(nearestDopplerRow(sd(m[5]+8u*i+2u)));}
double policySnr(uint i){return sd(m[5]+8u*i+4u);}
bool policyUnmatched(uint i){return s[m[6]+i]==0u;}
bool policyAlive(uint i){return m[work(i)+7u]!=0u;}
double policyTrackDelay(uint i){return md(work(i));}
double policyTrackRow(uint i){return double(nearestDopplerRow(md(work(i)+2u)));}
// SHARED_ADMISSION_CONTRACT
uint policyCell(uint i){
 double first=double(int(post[post[45]]));
 uint column=uint(nativeDelayCoordinate(policyDelay(i),first,post[33]));
 return uint(policyRow(i))*post[33]+column;
}
bool stationaryEligible(uint i){return stationaryDopplerEligible(sd(m[5]+8u*i+2u),
 packDouble2x32(uvec2(clutterPersistence[2],clutterPersistence[3])));}
uint priorConfirmationHits(uint r){uint packed=src(r+13u),head=packed&255u,length=(packed>>8u)&255u;
 uint count=0u,window=min(length,m[40]-1u),h=historyCapacity();
 // Schema5 header27 selects ordinary measured hits or disjoint temporal credit.
 for(uint j=length-window;j<length;++j)count+=lineageVote(src(r+16u+8u*((head+j)%h)+6u),experimentalLineage());
 return count;
}
bool independentSupport(uint r,uint depth){uint packed=src(r+13u),head=packed&255u,length=(packed>>8u)&255u,h=historyCapacity(),mask=0u;
 for(uint age=1u;age<=min(length,7u);++age)
  if(lineageVote(src(r+16u+8u*((head+length-age)%h)+6u),true)!=0u)mask|=1u<<(age-1u);
 return lineageDisjoint(depth,mask);
}
// 0 eligible, 1 already associated, 2..6 exclusive rejection reasons in the
// original predicate order. This function is pure: count/scatter both call it.
uint initiationDisposition(uint i){uint claimed=s[m[6]+i];if(claimed!=0u)return claimed==3u?8u:(claimed==2u?6u:1u);
 uint at=m[5]+8u*i;if(sd(at+4u)<md(TRACKER_MIN_INITIATION_SNR_WORD))return 2u;
 if(!dopplerRidgeHasBirthCapacity(i)||!combBirthEligible(i,false))return 3u;
 if(!combBirthEligible(i,true))return 4u;
 if(!clutterAware()||!stationaryEligible(i))return 0u;uint cell=policyCell(i);
 if(cell>=m[4]||post[33]==0u){reject(320u);return 5u;}
 if(clutterRidgeStreak(cell)>=clutterPersistence[1])return 5u;
 return stationaryCapacityBlocked(clutterPersistence[clutterState()],clutterPersistence[11],clutterPersistence[clutterCell(cell)]!=0u)?9u:0u;}
bool initiationEligible(uint i){return initiationDisposition(i)==0u;}
bool quarantinePersistentTrack(uint r,uint match){
 if(!clutterAware()||match==0xffffffffu||!stationaryEligible(match))return false;
 uint samples=clutterPersistence[4],minimumAssociated=clutterPersistence[5];
 uint ringBits=src(r+13u),head=ringBits&255u,length=(ringBits>>8u)&255u,h=historyCapacity();
 if(samples<2u||samples>h||length+1u<samples||minimumAssociated>samples)return false;
 uint detectionAt=m[5]+8u*match,cell=policyCell(match);
 if(cell>=m[4]||post[33]==0u)return false;
 if(clutterRidgeStreak(cell)<clutterPersistence[1])return false;
 double newestDelay=sd(detectionAt),newestDoppler=sd(detectionAt+2u);
 double minimumDelay=newestDelay,maximumDelay=newestDelay;
 double minimumDoppler=newestDoppler,maximumDoppler=newestDoppler;uint associated=1u;
 uint first=length-(samples-1u);
 for(uint j=first;j<length;++j){uint at=r+16u+8u*((head+j)%h);
  double delay=sourceD(at),doppler=sourceD(at+2u);
  if(!finiteD(delay)||!finiteD(doppler))return false;
  minimumDelay=min(minimumDelay,delay);maximumDelay=max(maximumDelay,delay);
  minimumDoppler=min(minimumDoppler,doppler);maximumDoppler=max(maximumDoppler,doppler);
  // Quarantined measurements remain actual associations. Counting publication
  // state here would reopen a hot cell merely because it was hidden last CPI.
  if((src(at+6u)&4u)!=0u)associated++;}
 return quarantineHistory(samples,h,length+1u,associated,maximumDelay-minimumDelay,
  maximumDoppler-minimumDoppler,md(42u));
}
void header(uint count,uint nextID){
 for(uint k=0u;k<32u;++k)put(k,0u);
 put(0u,checkpointSchema());put(1u,recordWords()*4u);put(2u,m[3]);put(3u,historyCapacity());put(4u,count);
 put(6u,m[16]);put(7u,m[17]);put(8u,nextID);put(10u,m[14]);put(11u,m[15]);
 put(12u,m[10]);put(13u,m[11]);put(14u,m[12]);put(15u,m[13]);
 if(experimentalLineage())put(26u,post[79]);
 put(27u,experimentalLineage()?1u:0u);
}
)glsl";
const char* initializeBody=R"glsl(
void main(){if(indexOf()!=0u)return;
 for(uint k=0u;k<32u;++k){a[k]=0u;b[k]=0u;}
 uint words=recordWords(),history=historyCapacity();
 a[0]=checkpointSchema();b[0]=checkpointSchema();a[1]=words*4u;b[1]=words*4u;a[2]=m[3];b[2]=m[3];a[3]=history;b[3]=history;
 a[10]=m[14];b[10]=m[14];a[11]=m[15];b[11]=m[15];
 a[27]=experimentalLineage()?1u:0u;b[27]=a[27];
}
)glsl";
const char* admitBody=R"glsl(
void main(){if(indexOf()!=0u)return;m[52]=m[10];m[53]=m[11];
 uint count=src(4u),detections=post[11];
 if(post[0]!=0u||src(5u)!=0u||count>m[3]||detections>m[4]||src(8u)>=65535u){reject(301u);return;}
 m[18]=count;m[19]=detections;m[24]=32u+recordWords()*count;
 bool accepted=m[TRACKER_ACCEPTED_FRAME_WORD]!=0u;
 if(frameClockNoOp(accepted,m[16],m[17],src(6u),src(7u),m[12],m[13],src(14u),src(15u))){m[2]=1u;return;}
 if(accepted&&experimentalLineage()){
  bool adjacent=lineageCounterNext(src(12u),src(13u),m[10u],m[11u])&&
   lineageCounterNext(src(14u),src(15u),m[12u],m[13u])&&greater64(pairM(16u),pairS(6u))&&
   (post[78]==0u||lineageTemporalNext(src(26u),post[79]));
  if(!adjacent){reject(322u);return;}
 }
 m[2]=2u;precise double elapsed=count>0u?double64(subtract64(pairM(16u),pairS(6u)))/1000.0LF:0.0LF;
 if(!finiteD(elapsed)||(count>0u&&!(elapsed>0.0LF))){reject(302u);return;}writeM(50u,elapsed);
}
)glsl";
const char* predictBody=R"glsl(
void main(){uint i=indexOf();if(m[0]!=0u||m[2]!=2u)return;
 if(i<m[19]){s[m[6]+i]=0u;uint at=m[5]+8u*i;
  uint cell=s[post[41]+i],beam=cell<m[4]?((s[post[39]+cell]>>8u)&511u):0xffffffffu;
  if(!finiteD(sd(at))||!finiteD(sd(at+2u))||!finiteD(sd(at+4u))||cell>=m[4]||beam>post[75])reject(303u);
  atomicAdd(m[57],1u);}
 if(i>=m[18])return;uint r=record(i),w=work(i);double delay=sourceD(r),doppler=sourceD(r+2u),acc=sourceD(r+6u),time=md(50u);
 precise double delayPredict=delay-md(46u)*(doppler*time+0.5LF*acc*time*time)/md(44u);
 precise double dopplerPredict=doppler+(acc*time);
 if(!finiteD(delayPredict)||!finiteD(dopplerPredict)){reject(304u);return;}
 writeM(w,delayPredict);writeM(w+2u,dopplerPredict);atomicAdd(m[26],1u);
}
)glsl";
const char* associateBody=R"glsl(
shared double distances[128];shared uint identities[128];
void main(){uint lane=gl_LocalInvocationID.x;if(m[0]!=0u||m[2]!=2u)return;
 for(uint i=0u;i<m[18];++i){uint r=record(i),w=work(i),previousSpatial=latestSpatial(r),state=src(r+14u);double dp=md(w),vp=md(w+2u),cpi=md(42u);
  // A never-confirmed hypothesis gets at most N frames including its seed.
  // Retire it before claiming a detection on frame N+1. Confirmed/coasting
  // tracks retain the existing deletion convention. Word12 bit30 remembers
  // confirmation independently of current nuisance-quarantine state.
  bool expiredTentative=(src(r+12u)&0x40000000u)==0u&&
    !greater64(uvec2(m[40],0u),pairS(r+8u));
  if(expiredTentative){if(lane==0u){m[w+4u]=0xffffffffu;m[w+5u]=0xffffffffu;
    m[w+6u]=state;m[w+7u]=0u;m[w+10u]=0u;m[w+11u]=0u;m[28]++;m[29]++;m[55]++;
    traceTrack(i,10u,state,0xffffffffu,false,0u);}
   memoryBarrierBuffer();barrier();continue;}
  // Record word 12 uses its low 16 bits for the public ID. Its high bit
  // latches the first real association, so a later miss cannot reopen the
  // broad acceleration-estimation gate.
  bool adaptiveTentative=adaptiveGate()&&state<2u&&
    (src(r+12u)&0x80000000u)==0u;
  precise double delayMargin=normalizedInnovationGate()&&!adaptiveTentative?3.034854258770293LF:1.0LF;
  precise double dopplerMargin=(normalizedInnovationGate()&&!adaptiveTentative?3.034854258770293LF:1.0LF)/cpi;
  precise double span=0.0LF,time=md(50u),gateTime=time,q=0.0LF;
  if(adaptiveTentative){gateTime=associationElapsed(time,sourceD(r+timingOffset()));span=md(48u)*double(m[37]);
   q=md(46u)*0.5LF*gateTime*gateTime/md(44u);delayMargin+=q*span;dopplerMargin+=span*gateTime;}
  precise double lowDelay=dp-delayMargin,highDelay=dp+delayMargin,lowDoppler=vp-dopplerMargin,highDoppler=vp+dopplerMargin;
  bool validGate=finiteD(lowDelay)&&finiteD(highDelay)&&finiteD(lowDoppler)&&finiteD(highDoppler)&&
   (!adaptiveTentative||(gateTime>0.0LF&&q>0.0LF&&finiteD(span)));if(m[19]>0u&&!validGate)reject(305u);
  double best=packDouble2x32(uvec2(0u,0x7ff00000u));uint match=0xffffffffu;
  for(uint j=lane;j<m[19];j+=128u){uint at=m[5]+8u*j;double delay=sd(at),doppler=sd(at+2u);
   AssociationDecision decision=associationDecision(delay,doppler,dp,vp,cpi,adaptiveTentative,
    gateTime,span,q,normalizedInnovationGate(),9.210340371976184LF);
   if(!decision.valid)reject(305u);
   if(decision.accepted&&(s[m[6]+j]==0u||s[m[6]+j]==3u)&&bearingCompatible(previousSpatial,detectionSpatial(j))){
    // Reused source CPIs cannot update kinematics, reset coast, or seed a
    // duplicate. Another track may still claim independently fresh evidence.
    if(experimentalLineage()&&!independentSupport(r,detectorSupportDepth(j))){
     atomicCompSwap(s[m[6]+j],0u,3u);continue;}
    precise double distance=decision.distance;
    if(!finiteD(distance))reject(305u);else if(distance<best){best=distance;match=j;}
   }}
  distances[lane]=best;identities[lane]=match;memoryBarrierBuffer();barrier();
  if(m[0]!=0u)return;
  for(uint width=64u;width>0u;width>>=1u){if(lane<width){uint other=lane+width;
    if(distances[other]<distances[lane]||(distances[other]==distances[lane]&&identities[other]<identities[lane])){
     distances[lane]=distances[other];identities[lane]=identities[other];}}
   barrier();}
  if(lane==0u){match=identities[0];double acc=sourceD(r+6u);bool alive=true;
   // A diagnostic-only nearest candidate within a fixed prefix explains misses
   // without a second inference path. A real match is always used if present.
   // The validity/coverage flags prevent an empty prefix being read as absence.
   if(traceEnabled()&&i<32u){uint t=traceRecord(32u+i),near=match;
    double nearest=packDouble2x32(uvec2(0u,0x7ff00000u));
    if(near==0xffffffffu)for(uint j=0u;j<min(m[19],32u);++j){uint at=m[5]+8u*j;
     double dd=sd(at)-dp,dv=(sd(at+2u)-vp)*cpi,distance=dd*dd+dv*dv;
     if(finiteD(distance)&&distance<nearest){nearest=distance;near=j;}}
    m[t+5u]=validGate?4u:0u;writeM(t+12u,dp);writeM(t+14u,vp);
    writeM(t+20u,delayMargin);writeM(t+22u,dopplerMargin);m[t+25u]=near;
    if(near!=0xffffffffu){m[t+5u]|=8u;uint at=m[5]+8u*near;
     double dd=sd(at)-dp,dv=sd(at+2u)-vp;writeM(t+16u,dd);writeM(t+18u,dv);
     uint rejected=validGate?0u:1u;
     if(!(sd(at)>lowDelay&&sd(at)<highDelay))rejected|=2u;
     if(!(sd(at+2u)>lowDoppler&&sd(at+2u)<highDoppler))rejected|=4u;
     if(normalizedInnovationGate()&&!adaptiveTentative&&
       dd*dd+(dv*cpi)*(dv*cpi)>9.210340371976184LF)rejected|=8u;
     if(adaptiveTentative&&validGate&&span>0.0LF){double low=max(-span,max((dv-1.0LF/cpi)/gateTime,(-1.0LF-dd)/q));
      double high=min(span,min((dv+1.0LF/cpi)/gateTime,(1.0LF-dd)/q));
      if(!(finiteD(low)&&finiteD(high)&&low<high))rejected|=8u;}
     if(s[m[6]+near]!=0u)rejected|=16u;
     if(!bearingCompatible(previousSpatial,detectionSpatial(near)))rejected|=32u;
     m[t+24u]=rejected;}}
   if(match!=0xffffffffu){precise double updated=adaptiveTentative&&sourceD(r+timingOffset())>0.0LF?
     sourceD(r+6u)+(sd(m[5]+8u*match+2u)-md(w+2u))/gateTime:
     (sd(m[5]+8u*match+2u)-sourceD(r+2u))/md(50u);
    if(!finiteD(updated))reject(306u);acc=updated;s[m[6]+match]=1u;state=(state==2u||state==3u)?2u:1u;m[27]++;
    // Retain the filter and its history, but keep a stable learned cell out of
    // operational publication. If it starts moving, fresh associations leave
    // the nuisance cell and the ordinary M-of-N confirmation path restores it.
    bool quarantined=quarantinePersistentTrack(r,match);
    state=quarantineAssociationState(quarantined,src(r+14u));if(quarantined)m[64]++;}
   else{state=state==2u?3u:(state==1u?0u:state);alive=!greater64(plusOne(pairS(r+10u)),uvec2(m[41],0u));m[28]++;if(!alive)m[29]++;}
   // Last chance to confirm is frame N itself. Keep its real detection claim
   // (as the CPU does), but retire an unconfirmed hypothesis immediately.
   uint support=match!=0xffffffffu?detectorSupportDepth(match):0u;
   bool credit=support!=0u&&(!experimentalLineage()||independentSupport(r,support));
   bool confirms=credit&&state!=0u&&
     priorConfirmationHits(r)+1u>=m[39]&&!rangeCombAmbiguous(match);
   if(alive&&(src(r+12u)&0x40000000u)==0u&&
      !greater64(uvec2(m[40],0u),plusOne(pairS(r+8u)))&&!confirms){alive=false;m[29]++;}
   m[w+4u]=match;m[w+5u]=alive?m[20]++:0xffffffffu;m[w+6u]=state;m[w+7u]=alive?1u:0u;writeM(w+8u,acc);m[w+10u]=0u;m[w+11u]=credit?1u:0u;m[55]++;
   traceTrack(i,!alive?(((src(r+12u)&0x40000000u)==0u)?10u:11u):
     (match!=0xffffffffu?1u:9u),state,match,credit,support);
  }
  // Each claim is visible before any lane examines the next ordered track.
  memoryBarrierBuffer();barrier();if(m[0]!=0u)return;
 }
}
)glsl";
const char* unmatchedCountBody=R"glsl(
shared uint values[128];
void main(){uint i=indexOf(),lane=gl_LocalInvocationID.x,tile=i/128u,tiles=(m[19]+127u)/128u;
 if(m[0]!=0u||m[2]!=2u||tile>=tiles)return;
 uint disposition=i<m[19]?initiationDisposition(i):1u;
 if(i<m[19])traceCandidate(i,disposition==0u?8u:(disposition==8u?14u:disposition));
 // Exactly this dispatch counts candidate rejection. Repeated predicate calls
 // during stable scatter cannot inflate any reason.
 if(disposition>=2u)atomicAdd(m[TRACKER_REJECTION_BASE+disposition-2u],1u);
 values[lane]=disposition==0u?1u:0u;barrier();
 for(uint width=64u;width>0u;width>>=1u){if(lane<width)values[lane]+=values[lane+width];barrier();}
 if(lane==0u){s[m[7]+m[4]+tile]=values[0];atomicAdd(m[56],1u);}
}
)glsl";
const char* unmatchedPrefixBody=R"glsl(
void main(){if(indexOf()!=0u||m[0]!=0u||m[2]!=2u)return;uint count=0u;
 for(uint tile=0u;tile<(m[19]+127u)/128u;++tile){uint at=m[7]+m[4]+tile,amount=s[at];s[at]=count;count+=amount;}
 // Preserve existing tracks and admit only complete initiation sets.
 // A detection burst must not invalidate the radar frame when capacity is full.
 uint hypotheses=hypothesisCount(),admitted=min(count,(m[3]-m[20])/hypotheses);
 m[TRACKER_REJECTION_BASE+5u]=count-admitted;
 m[21]=count;uint low=admitted*hypotheses;
 uint required=low+m[20];
 m[22]=low;m[23]=required;m[24]=32u+recordWords()*required;m[25]=(src(8u)+low)%65535u;
}
)glsl";
const char* unmatchedScatterBody=R"glsl(
shared uint values[128];
void main(){uint i=indexOf(),lane=gl_LocalInvocationID.x,tile=i/128u,tiles=(m[19]+127u)/128u;
 if(m[0]!=0u||m[2]!=2u||tile>=tiles)return;bool keep=i<m[19]&&initiationEligible(i);values[lane]=keep?1u:0u;barrier();
 for(uint distance=1u;distance<128u;distance<<=1u){uint addend=lane>=distance?values[lane-distance]:0u;barrier();values[lane]+=addend;barrier();}
 if(keep){uint rank=s[m[7]+m[4]+tile]+values[lane]-1u;s[m[7]+rank]=i;
  if(traceEnabled()&&i<32u)m[traceRecord(i)+1u]=rank<m[22]/hypothesisCount()?8u:7u;}
}
)glsl";
// Prune completes before association. A single painting writer preserves
// detector and row-major neighborhood order at capacity without per-cell locks.
const char* persistencePruneBody=R"glsl(
void main(){uint cell=indexOf();if(m[0]!=0u||m[2]!=2u||!clutterAware()||cell>=m[4])return;
 uint at=clutterCell(cell);if(clutterPersistence[at]!=0u&&clutterStreak(cell)==0u){
  clutterPersistence[at]=0u;clutterPersistence[at+1u]=0u;clutterPersistence[at+2u]=0u;
  atomicAdd(clutterPersistence[clutterState()],0xffffffffu);}
}
)glsl";
const char* persistenceBody=R"glsl(
void main(){if(indexOf()!=0u||m[0]!=0u||m[2]!=2u||!clutterAware())return;
 uint delayBins=post[33],rows=post[32];
 if(rows==0u||delayBins==0u||m[4]==0u||m[4]%delayBins!=0u||m[4]/delayBins!=rows){reject(320u);return;}
 for(uint i=0u;i<m[19];++i){if(s[post[41]+i]>=m[4]){reject(320u);return;}
  if(!stationaryEligible(i))continue;uint cell=policyCell(i),row=cell/delayBins,delay=cell%delayBins;
  for(int y=-1;y<=1;++y){int r=int(row)+y;if(r<0||r>=int(rows))continue;
   for(int x=-4;x<=4;++x){int d=int(delay)+x;if(d<0||d>=int(delayBins))continue;
    uint at=clutterCell(uint(r)*delayBins+uint(d)),streak=clutterPersistence[at];
    if(streak==0u){if(clutterPersistence[clutterState()]>=clutterPersistence[11])continue;++clutterPersistence[clutterState()];}
    clutterPersistence[at]=advancePersistentStreak(streak,clutterPersistence[at+1u],clutterPersistence[at+2u],m[TRACKER_STATIONARY_ORDINAL_WORD],m[TRACKER_STATIONARY_ORDINAL_WORD+1u]);
    clutterPersistence[at+1u]=m[TRACKER_STATIONARY_ORDINAL_WORD];clutterPersistence[at+2u]=m[TRACKER_STATIONARY_ORDINAL_WORD+1u];
   }}
 }
}
)glsl";
const char* historyBody=R"glsl(
void main(){uint i=groupOf(),lane=gl_LocalInvocationID.x;if(m[0]!=0u||m[2]!=2u||i>=m[18])return;uint w=work(i);if(m[w+7u]==0u)return;
 uint source=record(i),target=record(m[w+5u]);for(uint k=lane;k<recordWords();k+=128u)put(target+k,src(source+k));memoryBarrierBuffer();barrier();
 if(lane!=0u)return;uint ringBits=src(source+13u),head=ringBits&255u,length=(ringBits>>8u)&255u,slot,h=historyCapacity();
 if(length<h){slot=(head+length)%h;length++;}else{slot=head;head=(head+1u)%h;}
 uint at=target+16u+8u*slot,match=m[w+4u],state=m[w+6u];bool associated=match!=0xffffffffu||m[w+10u]!=0u;
 uint previousNewest=source+16u+8u*((head+length-2u)%h);
 uint beam=match==0xffffffffu?src(previousNewest+7u):detectionSpatial(match);
 for(uint k=0u;k<6u;++k){uint value=match==0xffffffffu?(k<4u?m[w+k]:0u):s[m[5]+8u*match+k];put(target+k,value);put(at+k,value);}
 // Schema3 retains the ordinary measured-hit contract. Schema4 additionally
 // stores an independent-credit bit4 and moves support depth to bits5..8.
 uint measured=match!=0xffffffffu?4u:0u;
 uint support=match!=0xffffffffu?detectorSupportDepth(match):0u;
 uint evidence=lineageWord(0u,measured,support,m[w+11u]!=0u,experimentalLineage());
 put(at+6u,state|evidence);put(at+7u,beam);put(target+6u,m[w+8u]);put(target+7u,m[w+9u]);
 if(match!=0xffffffffu)put(target+12u,src(source+12u)|0x80000000u);
 uvec2 age=plusOne(pairS(source+8u)),inactive=associated?uvec2(0u):plusOne(pairS(source+10u));
 put(target+8u,age.x);put(target+9u,age.y);put(target+10u,inactive.x);put(target+11u,inactive.y);
 uvec2 timing=unpackDouble2x32(associationMissed(md(50u),sourceD(source+timingOffset()),match!=0xffffffffu));
 put(target+timingOffset(),timing.x);put(target+timingOffset()+1u,timing.y);
 uvec2 measuredCount=pairS(source+timingOffset()+2u);
 if(match!=0xffffffffu)measuredCount=plusOne(measuredCount);
 put(target+timingOffset()+2u,measuredCount.x);put(target+timingOffset()+3u,measuredCount.y);
 uint display=min(length,100u),start=(head+length-display)%h;put(target+13u,head|(length<<8u)|(start<<16u)|(display<<24u));
 // Association can deliberately quarantine a learned nuisance cell by
 // resetting its publication state to tentative. Do not let the M-of-N pass
 // immediately undo that decision from the same retained history.
 if(m[w+11u]!=0u&&state!=0u&&state!=3u){uint count=0u,window=min(length,m[40]);for(uint j=length-window;j<length;++j){uint value=dst(target+16u+8u*((head+j)%h)+6u);count+=lineageVote(value,experimentalLineage());}
  bool ambiguous=match!=0xffffffffu&&rangeCombAmbiguous(match);
  if(count>=m[39]&&!ambiguous){state=2u;put(at+6u,state|evidence);}}
 if(state==2u)put(target+12u,dst(target+12u)|0x40000000u);
 if(traceEnabled()&&i<32u){uint t=traceRecord(32u+i);m[t+2u]=state;
  if(state==2u&&src(source+14u)<2u)m[t+1u]=12u;}
 put(target+14u,state);put(target+15u,0xffffffffu);atomicAdd(m[31],1u);
}
)glsl";
const char* initiateBody=R"glsl(
void main(){uint i=groupOf(),lane=gl_LocalInvocationID.x;if(m[0]!=0u||m[2]!=2u||i>=m[22])return;uint target=record(m[20]+i);
 for(uint k=lane;k<recordWords();k+=128u)put(target+k,0u);memoryBarrierBuffer();barrier();if(lane!=0u)return;
 uint hypotheses=hypothesisCount(),detection=s[m[7]+i/hypotheses],hypothesis=i%hypotheses;
 precise double acc=adaptiveGate()?0.0LF:md(48u)*double(int(hypothesis)-int(m[37]));
 if(!finiteD(acc)){reject(308u);return;}for(uint k=0u;k<6u;++k){uint value=s[m[5]+8u*detection+k];put(target+k,value);put(target+16u+k,value);}
 uvec2 bits=unpackDouble2x32(acc);put(target+6u,bits.x);put(target+7u,bits.y);put(target+8u,1u);put(target+12u,(src(8u)+i)%65535u);
 uint state=m[39]==1u&&!rangeCombAmbiguous(detection)?2u:0u;
 if(traceEnabled()&&detection<32u&&hypothesis==0u){uint t=traceRecord(detection);
  m[t+2u]=state;m[t+5u]|=2u;m[t+26u]=(src(8u)+i)%65535u;m[t+27u]=hypotheses;}
 put(target+14u,state);put(target+22u,lineageWord(state,4u,detectorSupportDepth(detection),true,experimentalLineage()));
 if(state==2u)put(target+12u,dst(target+12u)|0x40000000u);
 put(target+13u,(1u<<8u)|(1u<<24u));put(target+15u,0xffffffffu);put(target+23u,detectionSpatial(detection));atomicAdd(m[30],1u);atomicAdd(m[31],1u);
 put(target+timingOffset()+2u,1u);
}
)glsl";
const char* finalizeBody=R"glsl(
void main(){if(indexOf()!=0u||m[0]!=0u||m[2]!=2u)return;header(m[23],m[25]);uint published=0u;
 for(uint i=0u;i<m[23];++i){uint r=record(i),state=dst(r+14u);if(state>3u){reject(309u);return;}m[33u+state]++;
  put(r+15u,state==0u?0xffffffffu:published);if(state!=0u)published++;}
 m[32]=published;for(uint state=0u;state<4u;++state)put(16u+2u*state,m[33u+state]);put(24u,published);
}
)glsl";
const char* validateBody=R"glsl(
void main(){uint i=indexOf();if(m[0]!=0u)return;uint count=checkedWord(4u);
 if(count>m[3]||checkedWord(5u)!=0u){reject(310u);return;}
 if(i==0u){if(checkedWord(0u)!=checkpointSchema()||checkedWord(1u)!=recordWords()*4u||checkedWord(2u)!=m[3]||checkedWord(3u)!=historyCapacity()||
    checkedWord(8u)>=65535u||checkedWord(9u)!=0u||checkedWord(10u)!=m[14]||checkedWord(11u)!=m[15]||m[24]!=32u+recordWords()*count)reject(311u);
  if(checkedWord(27u)!=(experimentalLineage()?1u:0u)||(!experimentalLineage()&&checkedWord(26u)!=0u))reject(311u);
  for(uint k=28u;k<32u;++k)if(checkedWord(k)!=0u)reject(311u);
  if(m[2]!=1u&&experimentalLineage()&&checkedWord(26u)!=post[79])reject(323u);
  if(m[2]==2u&&(checkedWord(6u)!=m[16]||checkedWord(7u)!=m[17]||checkedWord(12u)!=m[10]||checkedWord(13u)!=m[11]||checkedWord(14u)!=m[12]||checkedWord(15u)!=m[13]))reject(311u);
 }
 if(i>=count)return;uint r=record(i),ringBits=checkedWord(r+13u),head=ringBits&255u,length=(ringBits>>8u)&255u,start=(ringBits>>16u)&255u,display=ringBits>>24u,state=checkedWord(r+14u),h=historyCapacity();
 uint identity=checkedWord(r+12u);
 if((identity&0x3fff0000u)!=0u||(identity&65535u)>=65535u||head>=h||length==0u||length>h||(length<h&&head!=0u)||
    display!=min(length,100u)||start!=(head+length-display)%h||state>3u){reject(312u);return;}
 uvec2 inactive=uvec2(checkedWord(r+10u),checkedWord(r+11u));
 if(greater64(inactive,uvec2(m[41],0u))||((state==1u||state==2u)&&any(notEqual(inactive,uvec2(0u))))){reject(313u);return;}
 for(uint k=0u;k<8u;k+=2u)if(!finiteD(checkedD(r+k)))reject(314u);
 double missed=checkedD(r+timingOffset());if(!finiteD(missed)||missed<0.0LF)reject(314u);
 uvec2 measurements=uvec2(checkedWord(r+timingOffset()+2u),checkedWord(r+timingOffset()+3u));
 if(all(equal(measurements,uvec2(0u)))||greater64(measurements,uvec2(checkedWord(r+8u),checkedWord(r+9u))))reject(314u);
 uint newest=r+16u+8u*((head+length-1u)%h);
 if(state==3u&&all(equal(inactive,uvec2(0u)))&&(checkedWord(newest+6u)&8u)==0u)reject(313u);
 for(uint k=0u;k<6u;++k)if(checkedWord(r+k)!=checkedWord(newest+k))reject(315u);
 if((checkedWord(newest+6u)&3u)!=state)reject(315u);
 if((checkedWord(newest+6u)&4u)!=0u&&missed!=0.0LF)reject(315u);
 if((state==2u||state==3u)&&(identity&0x40000000u)==0u)reject(315u);
 for(uint j=0u;j<h;++j){uint at=r+16u+8u*j;
  if(length<h&&j>=length){for(uint k=0u;k<8u;++k)if(checkedWord(at+k)!=0u)reject(3161u);}
  else{for(uint k=0u;k<6u;k+=2u)if(!finiteD(checkedD(at+k)))reject(314u);uint spatial=checkedWord(at+7u);
   if((spatial&0x80000000u)!=0u){uint a=spatial&127u,b=(spatial>>7u)&127u,c=(spatial>>14u)&127u;
    if(FINE_SPATIAL_INVALID)reject(3162u);}
   else{uint coarse=spatial&511u,fine=(spatial>>9u)&2047u;if(coarse>post[75]||fine>post[75]*4u||(coarse==0u&&fine!=0u))reject(3163u);}
   uint historyState=checkedWord(at+6u);
   if(!lineageWordValid(historyState,experimentalLineage()))reject(3164u);
   if(experimentalLineage()&&(historyState&16u)!=0u){
    uint ordinal=(j+h-head)%h,mask=0u;
    for(uint age=1u;age<=min(ordinal,7u);++age)
     if((checkedWord(r+16u+8u*((j+h-age)%h)+6u)&16u)!=0u)mask|=1u<<(age-1u);
    if(!lineageDisjoint(lineageDepth(historyState,true),mask))reject(3165u);
   }}}
 atomicAdd(m[54],1u);atomicAdd(m[60u+state],1u);if(state!=0u)atomicAdd(m[59],1u);
}
)glsl";
const char* validateTotalsBody=R"glsl(
void main(){if(indexOf()!=0u||m[0]!=0u)return;uint count=checkedWord(4u),published=0u;
 if(m[54]!=count){reject(317u);return;}
 for(uint state=0u;state<4u;++state){if(checkedWord(16u+2u*state)!=m[60u+state]||checkedWord(17u+2u*state)!=0u)reject(317u);m[33u+state]=m[60u+state];}
 for(uint i=0u;i<count;++i){uint r=record(i),state=checkedWord(r+14u),ordinal=checkedWord(r+15u);
  if(ordinal!=(state==0u?0xffffffffu:published))reject(318u);if(state!=0u)published++;}
 if(published!=m[59]||checkedWord(24u)!=published||checkedWord(25u)!=0u)reject(317u);m[32]=published;
}
)glsl";
const char* commitBody=R"glsl(
void main(){if(indexOf()!=0u||m[0]!=0u)return;if(m[2]<1u||m[2]>3u){reject(319u);return;}
 if(m[2]!=1u)m[1]=1u-m[1];m[58]=1u;
}
)glsl";
inline std::vector<std::pair<std::string,std::string>> shaderSources(){
 const std::pair<const char*,const char*> bodies[]={{"initialize",initializeBody},{"admit",admitBody},{"predict",predictBody},{"associate",associateBody},
  {"unmatched_count",unmatchedCountBody},{"unmatched_prefix",unmatchedPrefixBody},{"unmatched_scatter",unmatchedScatterBody},{"history",historyBody},
  {"initiate",initiateBody},{"finalize",finalizeBody},{"validate",validateBody},{"validate_totals",validateTotalsBody},{"commit",commitBody},{"persistence",persistenceBody},{"persistence_prune",persistencePruneBody}};
 std::vector<std::pair<std::string,std::string>> result;
 const std::string definition="#define TRACKER_METADATA_HEADER_WORDS "+std::to_string(metadataHeaderWords)+"u\n"+
  "#define TRACKER_MIN_INITIATION_SNR_WORD "+std::to_string(minInitiationSnrWord)+"u\n"+
  "#define TRACKER_ACCEPTED_FRAME_WORD "+std::to_string(acceptedFrameWord)+"u\n"+
  "#define TRACKER_STATIONARY_ORDINAL_WORD "+std::to_string(stationaryOrdinalWord)+"u\n"+
  "#define TRACKER_REJECTION_BASE "+std::to_string(rejectionBase)+"u\n";
 std::string header=std::string(common);
 const std::string contractMarker="// SHARED_CONFIRMATION_CONTRACT";
 header.replace(header.find(contractMarker),contractMarker.size(),tracker_confirmation::shaderSource);
 const std::string associationMarker="// SHARED_ASSOCIATION_CONTRACT";
 header.replace(header.find(associationMarker),associationMarker.size(),tracker_association::shaderSource);
 const std::string quarantineMarker="// SHARED_QUARANTINE_CONTRACT";
 header.replace(header.find(quarantineMarker),quarantineMarker.size(),tracker_quarantine::shaderSource);
 for(const auto& contract:std::vector<std::pair<std::string,const char*>>{
  {"// SHARED_ADMISSION_CONTRACT",tracker_policy::admissionShaderSource},
  {"// SHARED_STATIONARY_CONTRACT",tracker_policy::stationaryShaderSource}})
  header.replace(header.find(contract.first),contract.first.size(),contract.second);
 const std::string prefix=header.substr(0,13)+definition+header.substr(13);
 for(const auto& body:bodies){std::string source=body.second;
  const auto marker=source.find("FINE_SPATIAL_INVALID");
  if(marker!=std::string::npos)source.replace(marker,20,fineSpatialInvalidGlsl);
  result.emplace_back(body.first,prefix+source);}
 return result;
}
class Stage {
 // Defined only by the native qualification fixture; no production accessor,
 // mutation path, public ABI or runtime switch is exposed.
 friend struct QualificationSnapshot;
 Context& context_;
 const TrackerConfig config_;
 const uint32_t maximumDepth_;
 bool disjointLineage()const{return maximumDepth_>1;}
 const Admission admission_;
 const uint32_t cells_;
 const gpu_postprocess::Layout layout_;
 Buffer& post_;
 Buffer& scratch_;
 std::unique_ptr<Buffer> banks_[2],metadata_,clutterPersistence_;
 std::unique_ptr<gpu_native::Timestamps> timestamps_;
 std::vector<std::unique_ptr<Kernel>> kernels_;
 uint32_t active_=0;
 uint64_t serial_=0,acceptedFrames_=0;
 bool pending_=false,poisoned_=false,lastCommitted_=false,noOp_=false,copied_=false;
 std::array<uint32_t,statusWords> controlEvidence_{};
 double evidenceHostCommitMs_=0;
 TrackerToken pendingToken_{},lastToken_{};
 std::array<uint32_t,statusWords> ready_{};
 std::array<uint32_t,rejectionWords> rejections_{};
 static void put64(uint32_t* target,uint32_t at,uint64_t value){target[at]=uint32_t(value);
 target[at+1]=uint32_t(value>>32);}
 static void putDouble(uint32_t* target,uint32_t at,double value){std::memcpy(target+at,&value,8);}
 static void hostRead(VkCommandBuffer command){VkMemoryBarrier b{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
 b.srcAccessMask=VK_ACCESS_MEMORY_WRITE_BIT;
 b.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
  vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&b,0,nullptr,0,nullptr);}
 void healthy()const{if(poisoned_)throw std::logic_error("GPU tracker is poisoned");}
 std::array<uint32_t,statusWords> status(){metadata_->invalidate(0,metadataHeaderWords*4);
 std::array<uint32_t,statusWords>s{};
 std::memcpy(s.data(),metadata_->mapped,sizeof(s));
 std::memcpy(rejections_.data(),static_cast<const uint32_t*>(metadata_->mapped)+rejectionBase,sizeof(rejections_));
 controlEvidence_=s;
 return s;}
 void setup(const TrackerToken& token,uint64_t timestamp,uint32_t disposition=0){
  std::array<uint32_t,statusWords>s{};
  s[1]=active_;
  s[2]=disposition;
  s[3]=config_.capacity;
  s[4]=cells_;
  s[5]=layout_.records;
  s[6]=layout_.keep;
  s[7]=layout_.candidates;
  put64(s.data(),8,token.transaction);
  put64(s.data(),10,token.generation);
  put64(s.data(),12,token.consumedSequence);
  put64(s.data(),14,config_.epoch);
  put64(s.data(),16,timestamp);
  s[37]=admission_.nAcc;
  s[38]=admission_.hypotheses|(config_.adaptiveGate?0x80000000u:0u)|
    (disjointLineage()?0x40000000u:0u)|
    (config_.causalDiagnostics?0x20000000u:0u)|
    (config_.normalizedInnovationGate?0x10000000u:0u);
  s[39]=config_.m;
  s[40]=config_.n;
  s[41]=config_.nDelete;
  putDouble(s.data(),42,config_.cpi);
  putDouble(s.data(),44,config_.rangeRes);
  putDouble(s.data(),46,config_.lambda);
  putDouble(s.data(),48,admission_.resolution);
  // Words 50-51 hold per-frame working state. The configured birth threshold
  // lives after the fixed evidence header and is never reset or shader-written.
  std::memcpy(metadata_->mapped,s.data(),sizeof(s));
  auto* privateWords=static_cast<uint32_t*>(metadata_->mapped);
  privateWords[acceptedFrameWord]=acceptedFrames_!=0;
  put64(privateWords,stationaryOrdinalWord,acceptedFrames_+1);
  std::memset(static_cast<uint32_t*>(metadata_->mapped)+rejectionBase,0,sizeof(rejections_));
  metadata_->flush(0,metadataHeaderWords*4);
  if(config_.causalDiagnostics){const auto offset=metadata_->bytes-2064u*4u;
   std::memset(static_cast<uint8_t*>(metadata_->mapped)+offset,0,2064u*4u);
   metadata_->flush(offset,2064u*4u);}
 }
 void submit(VkCommandBuffer command){VkSubmitInfo info{VK_STRUCTURE_TYPE_SUBMIT_INFO};
 info.commandBufferCount=1;
 info.pCommandBuffers=&command;
  check(gpu_host::call(context_.hostTiming,"vkQueueSubmit",0,[&]{return vkQueueSubmit(context_.queue,1,&info,context_.fence);}),"tracker submit");
  check(gpu_host::call(context_.hostTiming,"vkWaitForFences",0,[&]{return vkWaitForFences(context_.device,1,&context_.fence,VK_TRUE,UINT64_MAX);}),"tracker wait");
  check(gpu_host::call(context_.hostTiming,"vkResetFences",0,[&]{return vkResetFences(context_.device,1,&context_.fence);}),"tracker fence reset");
 }
 template<class F> void commands(F&& record){gpu_reference::CommandOwner command(context_);
 command.allocate();
 VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  check(vkBeginCommandBuffer(command.command,&begin),"tracker command recording");
  barrier(command.command);
  record(command.command);
  barrier(command.command);
  hostRead(command.command);
  check(vkEndCommandBuffer(command.command),"tracker command recording");
  submit(command.command);}
 void append(VkCommandBuffer command,uint32_t kernel,uint32_t count){kernels_[kernel]->append(command,{count,0,0,0,0});
 barrier(command);}
 void stagePersistence(VkCommandBuffer command){
  if(!config_.stationaryClutter)return;
  const VkDeviceSize bytes=persistenceStateWords(cells_)*4;
  const VkBufferCopy copy{clutterHeaderWords*4+active_*bytes,
   clutterHeaderWords*4+(1-active_)*bytes,bytes};
  vkCmdCopyBuffer(command,clutterPersistence_->handle,clutterPersistence_->handle,1,&copy);
  barrier(command);
 }
 void checkTokenEcho(const std::array<uint32_t,statusWords>& s,const TrackerToken& token)const{
  if(s[3]!=config_.capacity||s[4]!=cells_||s[8]!=uint32_t(token.transaction)||s[9]!=uint32_t(token.transaction>>32)||
     s[10]!=uint32_t(token.generation)||s[11]!=uint32_t(token.generation>>32)||s[12]!=uint32_t(token.consumedSequence)||s[13]!=uint32_t(token.consumedSequence>>32)||
     s[14]!=uint32_t(config_.epoch)||s[15]!=uint32_t(config_.epoch>>32)||s[52]!=uint32_t(token.generation)||s[53]!=uint32_t(token.generation>>32))throw std::runtime_error("GPU tracker transaction echo mismatch");
 }
 void checkCoverage(const std::array<uint32_t,statusWords>& s)const{
  if(s[1]!=active_||s[24]<headerWords||s[24]>admission_.bankWords||s[54]>config_.capacity||s[24]!=headerWords+admission_.recordWords*s[54]||
     uint64_t(s[33])+s[34]+s[35]+s[36]!=s[54]||s[32]!=s[34]+s[35]+s[36])throw std::runtime_error("GPU tracker snapshot coverage mismatch");
  if(s[2]==1){for(uint32_t k=26;k<=31;++k)if(s[k])throw std::runtime_error("GPU tracker no-op performed tracking work");
   for(auto count:rejections_)if(count)throw std::runtime_error("GPU tracker no-op classified detections");
   if(s[55]||s[56]||s[57]||s[54]!=s[18])throw std::runtime_error("GPU tracker no-op coverage mismatch");}
 else if(s[2]!=2||s[26]!=s[18]||s[27]+s[28]!=s[18]||s[27]>s[19]||s[65]>s[19]-(s[27])||s[21]>s[19]-(s[27])-s[65]||s[29]>s[18]||!validQuarantineCoverage(s[27],s[64])||s[20]!=s[18]-s[29]||s[55]!=s[18]||
    s[57]!=s[19]||s[56]!=(s[19]+127)/128||s[30]!=s[22]||s[31]!=s[23]||s[23]!=s[20]+s[22]||s[54]!=s[23])
   throw std::runtime_error("GPU tracker executed-work coverage mismatch");
  if(s[2]==2){const auto admission=boundInitiations(config_.capacity,s[20],s[21],admission_.hypotheses);
   if(s[22]!=admission.initiatedTracks||s[23]!=admission.totalTracks)
    throw std::runtime_error("GPU tracker bounded initiation coverage mismatch");
   uint64_t rejected=0;for(auto count:rejections_)rejected+=count;
   if(rejections_[boundary]!=s[65]||rejections_[capacity]!=admission.droppedDetections||
      rejected+admission.admittedDetections+s[27]!=s[19])
    throw std::runtime_error("GPU tracker exclusive admission-reason coverage mismatch");}
 }
 void validateOutputs(TrackerOutputs out)const{
  if(!out.checkpoint||out.wordCapacity!=admission_.bankWords||!out.result)throw std::invalid_argument("GPU tracker output dimensions changed");
  const auto range=[](const void* p,size_t bytes){uintptr_t start=reinterpret_cast<uintptr_t>(p);
  if(bytes>UINTPTR_MAX-start)throw std::invalid_argument("GPU tracker output overflow");
  return std::pair<uintptr_t,uintptr_t>{start,start+bytes};};
  const auto a=range(out.checkpoint,out.wordCapacity*4),z=range(out.result,sizeof(TrackerResult));
  const auto overlaps=[](auto x,auto y){return x.first<y.second&&y.first<x.second;};
  if(overlaps(a,z))throw std::invalid_argument("GPU tracker outputs overlap");
  for(const Buffer* b:{banks_[0].get(),banks_[1].get(),metadata_.get(),clutterPersistence_.get(),&post_,&scratch_})if(b->mapped){const auto x=range(b->mapped,size_t(b->bytes));
  if(overlaps(a,x)||overlaps(z,x))throw std::invalid_argument("GPU tracker output aliases device storage");}
 }
 void copyBank(uint32_t bank,uint32_t words,TrackerOutputs out,const TrackerToken& token,bool noOp){
  validateOutputs(out);
  if(words<headerWords||words>admission_.bankWords)throw std::runtime_error("GPU tracker checkpoint size invalid");
  banks_[bank]->invalidate(0,uint64_t(words)*4);
  std::memcpy(out.checkpoint,banks_[bank]->mapped,size_t(words)*4);
  *out.result={token,words,noOp};
 }
 void logPrepared(const std::array<uint32_t,statusWords>& s){context_.diagnostics.stream()<<std::setprecision(12)<<"GPU_TRACKER_PREPARED transaction="<<pendingToken_.transaction<<" generation="<<pendingToken_.generation<<" consumed_sequence="<<pendingToken_.consumedSequence<<" epoch="<<config_.epoch
  <<" device_generation_low="<<s[52]<<" device_generation_high="<<s[53]<<" noop="<<(s[2]==1)<<" capacity="<<config_.capacity<<" tracks="<<s[54]<<" detections="<<s[19]<<" adaptive_gate="<<config_.adaptiveGate<<" acceleration_hypotheses="<<admission_.hypotheses<<" device_predicted="<<s[26]<<" device_associated="<<s[27]<<" device_missed="<<s[28]<<" device_removed="<<s[29]<<" device_initiated="<<s[30]
  <<" device_retained_history_appends="<<s[31]<<" device_published="<<s[32]<<" device_stationary_quarantined="<<s[64]<<" device_stationary_retired=0 device_association_visits="<<s[55]<<" device_unmatched_tiles="<<s[56]<<" device_detection_checks="<<s[57]<<" device_validated_records="<<s[54]<<" checkpoint_words="<<s[24];
  if(s[2]==2){const auto admission=boundInitiations(config_.capacity,s[20],s[21],admission_.hypotheses);
   context_.diagnostics.stream()<<" unmatched_detections="<<admission.unmatched
    <<" boundary_rejected="<<s[65]
    <<" admission_reason_version=1"
    <<" min_snr_rejected="<<rejections_[minimumSnr]
    <<" doppler_comb_rejected="<<rejections_[dopplerComb]
    <<" range_comb_rejected="<<rejections_[rangeComb]
    <<" persistent_occupancy_rejected="<<rejections_[persistentOccupancy]
    <<" stationary_capacity_rejected="<<rejections_[stationaryCapacity]
    <<" lineage_overlap_rejected="<<rejections_[lineageOverlap]
    // Retain the legacy log key, but only with its literal stationary meaning.
    <<" stationary_clutter_suppressed="<<rejections_[persistentOccupancy]
    <<" admitted_detections="<<admission.admittedDetections<<" dropped_capacity_detections="<<admission.droppedDetections;}
  for(uint32_t phase=0;phase<6;++phase)context_.diagnostics.stream()<<" gpu_phase"<<phase<<"_ms="<<timestamps_->milliseconds(phase);
  context_.diagnostics.stream()<<" cpu_predict_calls=0 cpu_associate_calls=0 cpu_history_calls=0 cpu_initiate_calls=0\n";
 }
public:
 const auto& evidencePrepared()const{return ready_;}
 const auto& admissionRejections()const{return rejections_;}
 const auto& evidenceControl()const{return controlEvidence_;}
 const gpu_native::Timestamps& evidenceTimes()const{return *timestamps_;}
 double evidenceHostCommitMs()const{return evidenceHostCommitMs_;}
 uint32_t activeBank()const{return active_;}
 Stage(Context& context,const TrackerConfig& config,uint32_t cells,Buffer& post,Buffer& scratch,const gpu_postprocess::Layout& layout,uint32_t maximumDepth=1)
  :context_(context),config_(config),maximumDepth_(maximumDepth),admission_(validate(config,cells,maximumDepth)),cells_(cells),layout_(layout),post_(post),scratch_(scratch){
  const auto& limit=context_.candidate.properties.limits;
  const auto*postWords=static_cast<const uint32_t*>(post.mapped);
  if(layout.cells!=cells||post.bytes<34u*4||!post.mapped||
     !postWords[32]||!postWords[33]||uint64_t(postWords[32])*postWords[33]!=cells||scratch.bytes<layout.bytes||
     uint64_t(layout.keep)+cells>layout.indices||uint64_t(layout.indices)+cells>layout.candidates||
     uint64_t(layout.candidates)+cells+(uint64_t(cells)+127)/128>layout.records||uint64_t(layout.records)+uint64_t(cells)*8>layout.bytes/4||
     post.handle==scratch.handle||
     std::max({uint64_t(admission_.bankWords)*4,admission_.stageBytes,persistenceBytes(config.stationaryClutter,cells)})>limit.maxStorageBufferRange||limit.maxComputeSharedMemorySize<2048||limit.maxPerStageDescriptorStorageBuffers<6u||limit.maxDescriptorSetStorageBuffers<6u)
   throw std::invalid_argument("GPU_TRACKER_UNSUPPORTED buffers or device limits");
  constexpr auto host=VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
  constexpr auto preferred=VK_MEMORY_PROPERTY_HOST_COHERENT_BIT|VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
  banks_[0]=std::make_unique<Buffer>(context,uint64_t(admission_.bankWords)*4,host,preferred);
  banks_[1]=std::make_unique<Buffer>(context,uint64_t(admission_.bankWords)*4,host,preferred);
  metadata_=std::make_unique<Buffer>(context,admission_.stageBytes,host,preferred);
  clutterPersistence_=std::make_unique<Buffer>(context,persistenceBytes(config.stationaryClutter,cells),host,preferred);
  std::memset(clutterPersistence_->mapped,0,size_t(clutterPersistence_->bytes));
  auto* persistence=static_cast<uint32_t*>(clutterPersistence_->mapped);
  persistence[0]=config.stationaryClutter;
  persistence[1]=config.stationaryPersistenceCpis;
  persistence[11]=config.maximumStationaryCells;
  std::memcpy(persistence+2,&config.stationaryMaxDopplerHz,8);
  const auto retirement=stationaryRetirementPolicy(config.stationaryClutter,
    config.cpi,config.stationaryPersistenceCpis);
  persistence[4]=retirement.samples;
  persistence[5]=retirement.minimumAssociated;
  std::memcpy(persistence+6,&retirement.maxDelaySpan,8);
  std::memcpy(persistence+8,&retirement.maxDopplerSpanHz,8);
  clutterPersistence_->flush(0,clutterPersistence_->bytes);
  std::memset(metadata_->mapped,0,size_t(metadata_->bytes));
  putDouble(static_cast<uint32_t*>(metadata_->mapped),minInitiationSnrWord,config.minInitiationSnrDb);
  metadata_->flush(0,metadata_->bytes);
  timestamps_=std::make_unique<gpu_native::Timestamps>(context);
  std::vector<Buffer*> bindings{metadata_.get(),banks_[0].get(),banks_[1].get(),&scratch_,&post_,clutterPersistence_.get()};
  for(const auto& source:shaderSources())kernels_.push_back(std::make_unique<Kernel>(context,source.second.c_str(),bindings));
  setup({},0);
  commands([&](VkCommandBuffer command){timestamps_->begin(command,6);append(command,0,1);timestamps_->end(command,6);});
 std::cerr<<"GPU_TRACKER_READY capacity="<<config.capacity<<" history="<<admission_.historyCount<<" record_bytes="<<admission_.recordWords*4<<" bank_bytes="<<uint64_t(admission_.bankWords)*4<<" stage_bytes="<<admission_.stageBytes<<" requested_bytes="<<admission_.totalBytes<<" normalized_innovation_gate="<<config.normalizedInnovationGate<<" gpu_initialize_ms="<<timestamps_->milliseconds(6)<<" selected_live_capacity=0\n";
 }
 bool pending()const{return pending_;}
 void resetInput(){healthy();
  if(pending_)throw std::logic_error("GPU tracker input reset while pending");
  try{active_=0;setup({},0);
   commands([&](VkCommandBuffer command){
    timestamps_->begin(command,6);
    for(auto& bank:banks_)vkCmdFillBuffer(command,bank->handle,0,bank->bytes,0);
    const VkDeviceSize workOffset=uint64_t(metadataHeaderWords)*4;
    vkCmdFillBuffer(command,metadata_->handle,workOffset,uint64_t(workWords)*config_.capacity*4,0);
    if(config_.stationaryClutter){vkCmdFillBuffer(command,clutterPersistence_->handle,
     clutterHeaderWords*4,clutterPersistence_->bytes-clutterHeaderWords*4,0);}
    barrier(command);append(command,0,1);timestamps_->end(command,6);
   });
   // Preserve serial_: an input discontinuity cannot revive a stale token.
   acceptedFrames_=0;lastCommitted_=noOp_=copied_=false;pendingToken_=lastToken_={};
   ready_={};controlEvidence_={};rejections_={};evidenceHostCommitMs_=0;
  }catch(...){poisoned_=true;throw;}
 }
 TrackerToken preparedToken()const{healthy();
 if(!pending_)throw std::logic_error("GPU tracker has no prepared token");
 return pendingToken_;}
 void checkOutputs(TrackerOutputs out)const{validateOutputs(out);}
 uint32_t checkpointWordCapacity()const{return admission_.bankWords;}const TrackerConfig& config()const{return config_;}
 // Snapshot already-computed device facts; no host tracking arithmetic.
 void copyCausalTrace(uint32_t* target,size_t words){
  if(!config_.causalDiagnostics||!pending_||!target||words!=2064u)
   throw std::invalid_argument("GPU tracker causal trace contract");
  const auto offset=metadata_->bytes-2064u*4u;metadata_->invalidate(offset,2064u*4u);
  std::memcpy(target,static_cast<const uint8_t*>(metadata_->mapped)+offset,2064u*4u);
  target[0]=1u;target[1]=noOp_?0u:ready_[19];target[2]=noOp_?0u:ready_[18];target[3]=noOp_?1u:0u;
  std::memcpy(target+4,ready_.data()+10,8);std::memcpy(target+6,ready_.data()+12,8);
  std::memcpy(target+8,ready_.data()+16,8);std::memcpy(target+10,ready_.data()+14,8);
 }
 bool prepare(TrackerFrame frame){healthy();
 if(pending_)throw std::logic_error("GPU tracker already has a prepared transaction");
 if(serial_==UINT64_MAX)throw std::overflow_error("GPU tracker transaction exhausted");
 if(acceptedFrames_==UINT64_MAX)throw std::overflow_error("GPU tracker accepted-frame clock exhausted");
  copied_=false;
  pendingToken_={config_.epoch,++serial_,frame.generation,frame.consumedSequence};
  try{setup(pendingToken_,frame.timestamp);
  commands([&](VkCommandBuffer command){
    stagePersistence(command);
    timestamps_->begin(command,0);append(command,1,1);append(command,2,std::max(cells_,config_.capacity));timestamps_->end(command,0);
    timestamps_->begin(command,1);append(command,14,cells_);append(command,3,1);timestamps_->end(command,1);
    timestamps_->begin(command,2);append(command,13,1);append(command,4,cells_);append(command,5,1);append(command,6,cells_);timestamps_->end(command,2);
    timestamps_->begin(command,3);append(command,7,128*config_.capacity);append(command,8,128*config_.capacity);append(command,9,1);timestamps_->end(command,3);
    timestamps_->begin(command,4);append(command,10,config_.capacity);timestamps_->end(command,4);
    timestamps_->begin(command,5);append(command,11,1);timestamps_->end(command,5);});
   ready_=status();
   checkTokenEcho(ready_,pendingToken_);
   if(ready_[0]){context_.diagnostics.stream()<<"GPU_TRACKER_REJECT transaction="<<pendingToken_.transaction<<" generation="<<frame.generation<<" code="<<ready_[0]<<" committed_unchanged=1\n";
   return false;}
   checkCoverage(ready_);
   noOp_=ready_[2]==1;
   pending_=true;
   logPrepared(ready_);
   return true;
  }catch(...){poisoned_=true;
  pending_=false;
  throw;}
 }
 void copyPrepared(TrackerOutputs out){healthy();
 if(!pending_)throw std::logic_error("GPU tracker has no prepared transaction");
 copied_=false;
 validateOutputs(out);
  try{copyBank(noOp_?active_:1-active_,ready_[24],out,pendingToken_,noOp_);
  copied_=true;}catch(...){poisoned_=true;
  throw;}}
 void commit(TrackerToken token){healthy();
 if(!pending_){if(lastCommitted_&&sameToken(token,lastToken_))return;
 throw std::logic_error("GPU tracker stale commit token");}
  if(!sameToken(token,pendingToken_))throw std::logic_error("GPU tracker commit token mismatch");
  if(!copied_)throw std::logic_error("GPU tracker commit before successful snapshot copy");
  const auto began=std::chrono::steady_clock::now();
  try{commands([&](VkCommandBuffer command){timestamps_->begin(command,7);append(command,12,1);timestamps_->end(command,7);});
   const auto s=status();
   checkTokenEcho(s,token);
   if(s[0]||s[58]!=1||s[1]!=(noOp_?active_:1-active_))throw std::runtime_error("GPU tracker commit acknowledgement invalid");
   const double gpuMs=timestamps_->milliseconds(7);
   active_=s[1];
   if(!noOp_)++acceptedFrames_;
   pending_=false;
   copied_=false;
   lastCommitted_=true;
   lastToken_=token;
   evidenceHostCommitMs_=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-began).count();
   context_.diagnostics.stream()<<std::setprecision(12)<<"GPU_TRACKER_COMMIT transaction="<<token.transaction<<" generation="<<token.generation<<" epoch="<<token.epoch<<" noop="<<noOp_<<" device_commit=1 gpu_commit_ms="<<gpuMs<<" host_commit_ms="<<std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-began).count()<<" cpu_tracker_calls=0\n";
  }catch(...){poisoned_=true;
  throw;}
 }
 void abort(TrackerToken token){healthy();
 if(!pending_||!sameToken(token,pendingToken_))throw std::logic_error("GPU tracker abort token mismatch");
 pending_=false;
 copied_=false;}
 void copyCommitted(TrackerOutputs out){healthy();
 validateOutputs(out);
 try{banks_[active_]->invalidate(0,headerWords*4);
 const auto* words=static_cast<const uint32_t*>(banks_[active_]->mapped);
  if(words[5]||words[4]>config_.capacity){throw std::runtime_error("GPU tracker committed count invalid");}
   copyBank(active_,headerWords+admission_.recordWords*words[4],out,lastCommitted_?lastToken_:TrackerToken{},false);
  }catch(...){poisoned_=true;
  throw;}}
 void restore(const TrackerConfig& envelope,const uint32_t* checkpoint,size_t words){healthy();
 if(pending_)throw std::logic_error("GPU tracker restore while pending");
  if(!sameConfig(envelope,config_)||!checkpoint||words<headerWords||words>admission_.bankWords)
   throw std::invalid_argument("GPU tracker checkpoint envelope or dimensions invalid");
  const uintptr_t first=reinterpret_cast<uintptr_t>(checkpoint);
  const size_t bytes=words*4;
  if(bytes>UINTPTR_MAX-first||first%alignof(uint32_t))throw std::invalid_argument("GPU tracker checkpoint address invalid");
  // Stage owns only the tracker banks, metadata, persistence and the
  // postprocess/scratch inputs.  Integration/current-power buffers belong to
  // the canonical frontend, not this tracker checkpoint.
  for(const Buffer* buffer:{banks_[0].get(),banks_[1].get(),metadata_.get(),clutterPersistence_.get(),&post_,&scratch_})if(buffer->mapped){
   const uintptr_t at=reinterpret_cast<uintptr_t>(buffer->mapped);
   if(buffer->bytes>UINTPTR_MAX-at||(first<at+buffer->bytes&&at<first+bytes))
    throw std::invalid_argument("GPU tracker checkpoint aliases device storage");}
  if(checkpoint[5]||checkpoint[4]>config_.capacity||words!=headerWords+admission_.recordWords*checkpoint[4])throw std::invalid_argument("GPU tracker checkpoint count invalid");
  // The public kinematic checkpoint contains neither stationary cells nor the
  // accepted-frame clock/rolling temporal buffers. These modes admit only the
  // exact current checkpoint in this warm lifecycle; stale/foreign/reset input
  // cannot reconstruct the missing state.
 if(disjointLineage()||config_.stationaryClutter){
   banks_[active_]->invalidate(0,uint64_t(words)*4);
   post_.invalidate(0,post_.bytes);
   const auto* current=static_cast<const uint32_t*>(banks_[active_]->mapped);
   const auto* postWords=static_cast<const uint32_t*>(post_.mapped);
   if(acceptedFrames_==0||checkpoint[0]!=5u||
      (disjointLineage()&&(checkpoint[27]!=1u||checkpoint[26]!=postWords[79]))||
      std::memcmp(current,checkpoint,words*4)!=0)
    throw std::invalid_argument("GPU tracker restore requires identical current warm lifecycle; reset input instead");
  }
  if(serial_==UINT64_MAX)throw std::overflow_error("GPU tracker transaction exhausted");
  try{setup({},0,3);
  static_cast<uint32_t*>(metadata_->mapped)[24]=uint32_t(words);
  metadata_->flush(0,statusWords*4);
   std::memcpy(banks_[1-active_]->mapped,checkpoint,words*4);
   banks_[1-active_]->flush(0,words*4);
   commands([&](VkCommandBuffer command){stagePersistence(command);timestamps_->begin(command,5);append(command,10,config_.capacity);append(command,11,1);timestamps_->end(command,5);});
   auto s=status();
   if(s[0])throw std::invalid_argument("GPU tracker checkpoint invariants rejected code="+std::to_string(s[0]));
   if(s[54]!=checkpoint[4])throw std::runtime_error("GPU tracker restore coverage mismatch");
   commands([&](VkCommandBuffer command){timestamps_->begin(command,7);append(command,12,1);timestamps_->end(command,7);});
   s=status();
   if(s[0]||s[58]!=1||s[1]!=1-active_){throw std::runtime_error("GPU tracker restore commit invalid");}
   active_=s[1];
   if(acceptedFrames_==0)acceptedFrames_=1;
   ++serial_;
   lastCommitted_=false;
   copied_=false;
   context_.diagnostics.stream()<<"GPU_TRACKER_RESTORE tracks="<<s[54]<<" device_validated_records="<<s[54]<<" device_commit=1 gpu_validate_ms="<<timestamps_->milliseconds(5)<<" gpu_commit_ms="<<timestamps_->milliseconds(7)<<'\n';
  }catch(const std::invalid_argument&){throw;}catch(...){poisoned_=true;
  throw;}
 }
};
} // namespace gpu_tracker
