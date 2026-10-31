#pragma once
// Private GPU tracker: no host tracking arithmetic or production wire/main path.
#include "GpuTrackerAdmission.private.h"
struct TrackerConfig {
 bool enabled=false;
 uint32_t capacity=0,m=0,n=0,nDelete=0;
 bool adaptiveGate=false;
 double cpi=0,maxAccInit=0,rangeRes=0,lambda=0;
 double minInitiationSnrDb=-100;
 bool stationaryClutter=false;
 uint32_t stationaryPersistenceCpis=0;
 double stationaryMaxDopplerHz=0;
 bool trackGuidedShadow=false;
 bool trackGuidedPromote=false;
 uint32_t trackGuidedShadowMaxTracks=0;
 uint64_t epoch=0;
};
struct TrackerFrame {uint64_t generation=0,consumedSequence=0,timestamp=0;};
struct TrackerToken {uint64_t epoch=0,transaction=0,generation=0,consumedSequence=0;};
struct TrackerResult {TrackerToken token{};
uint32_t checkpointWords=0;
uint32_t guidedRecoveries=0;
bool noOp=false;};
struct TrackerOutputs {uint32_t* checkpoint=nullptr;
size_t wordCapacity=0;
uint32_t* guidedRecoveries=nullptr;
size_t guidedWordCapacity=0;
TrackerResult* result=nullptr;};
namespace gpu_tracker {
constexpr uint32_t headerWords=32,displayHistory=100,statusWords=66,workWords=12,
 clutterHeaderWords=10;
struct Admission {uint32_t bankWords=0,recordWords=0,historyCount=0,nAcc=0,hypotheses=0;
uint64_t stageBytes=0,totalBytes=0;
double resolution=0;};
inline Admission validate(const TrackerConfig& c,uint32_t cells){
 if(!c.enabled||!c.capacity||!cells||cells>UINT32_MAX/24||!c.m||c.m>c.n||c.n>255||!std::isfinite(c.cpi)||c.cpi<=0||
    !std::isfinite(c.maxAccInit)||c.maxAccInit<0||!std::isfinite(c.rangeRes)||c.rangeRes<=0||!std::isfinite(c.lambda)||c.lambda<=0||
    !std::isfinite(c.minInitiationSnrDb)||c.minInitiationSnrDb < -100||c.minInitiationSnrDb > 100||
    !std::isfinite(c.stationaryMaxDopplerHz)||c.stationaryMaxDopplerHz<0||
    (c.stationaryClutter?(c.stationaryPersistenceCpis<2||c.stationaryPersistenceCpis>65535):
      (c.stationaryPersistenceCpis||c.stationaryMaxDopplerHz!=0))||
    (c.trackGuidedShadow?(!c.trackGuidedShadowMaxTracks||c.trackGuidedShadowMaxTracks>c.capacity):
      (c.trackGuidedShadowMaxTracks!=0||c.trackGuidedPromote))||
    c.capacity>(UINT32_MAX-127)/128)
  throw std::invalid_argument("GPU_TRACKER_UNSUPPORTED configuration or index capacity");
 Admission a;
 a.historyCount=std::max(displayHistory,c.n);
 a.recordWords=16+8*a.historyCount;
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
 a.stageBytes=uint64_t(statusWords+uint64_t(workWords)*c.capacity+
  (c.trackGuidedShadow?16u+c.capacity:0u))*4;
 a.totalBytes=uint64_t(a.bankWords)*8+a.stageBytes+
  (c.stationaryClutter?uint64_t(cells+clutterHeaderWords)*4:16);
 if(uint64_t(a.bankWords)*4>SIZE_MAX||a.stageBytes>SIZE_MAX){throw std::invalid_argument("GPU_TRACKER_UNSUPPORTED host byte capacity");}
 return a;
}
inline bool sameToken(const TrackerToken& a,const TrackerToken& b){return a.epoch==b.epoch&&a.transaction==b.transaction&&a.generation==b.generation&&a.consumedSequence==b.consumedSequence;}
inline bool sameDouble(double a,double b){return std::memcmp(&a,&b,sizeof(a))==0;}
inline bool sameConfig(const TrackerConfig& a,const TrackerConfig& b){return a.enabled==b.enabled&&a.capacity==b.capacity&&a.m==b.m&&a.n==b.n&&a.nDelete==b.nDelete&&a.adaptiveGate==b.adaptiveGate&&a.stationaryClutter==b.stationaryClutter&&a.stationaryPersistenceCpis==b.stationaryPersistenceCpis&&a.trackGuidedShadow==b.trackGuidedShadow&&a.trackGuidedPromote==b.trackGuidedPromote&&a.trackGuidedShadowMaxTracks==b.trackGuidedShadowMaxTracks&&a.epoch==b.epoch&&sameDouble(a.cpi,b.cpi)&&sameDouble(a.maxAccInit,b.maxAccInit)&&sameDouble(a.rangeRes,b.rangeRes)&&sameDouble(a.lambda,b.lambda)&&sameDouble(a.minInitiationSnrDb,b.minInitiationSnrDb)&&sameDouble(a.stationaryMaxDopplerHz,b.stationaryMaxDopplerHz);}
const char* common=R"glsl(#version 450
#extension GL_ARB_gpu_shader_fp64 : require
layout(local_size_x=128) in;
layout(binding=0) buffer Metadata {uint m[];};
layout(binding=1) buffer BankA {uint a[];};
layout(binding=2) buffer BankB {uint b[];};
layout(binding=3) buffer Scratch {uint s[];};
layout(binding=4) readonly buffer Postprocess {uint post[];};
layout(binding=5) buffer StationaryClutter {uint clutterPersistence[];};
#if TRACK_GUIDED_SHADOW
layout(binding=6) readonly buffer Integration {double integrationHistory[];};
layout(binding=7) readonly buffer CurrentPower {double currentPower[];};
#endif
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
uint recordWords(){return 16u+8u*historyCapacity();}
uint record(uint i){return 32u+recordWords()*i;}
uint work(uint i){return 66u+12u*i;}
#if TRACK_GUIDED_SHADOW
uint guidedBase(){return 66u+12u*m[3];}
uint guidedRead(uint at){return m[guidedBase()+at];}
void guidedWrite(uint at,uint value){m[guidedBase()+at]=value;}
#endif
uint detectionSpatial(uint i){uint cell=s[post[41]+i],direction=s[post[86]+cell];
 if((direction&0x80000000u)!=0u)return direction;
 uint flags=s[post[39]+cell],coarse=(flags>>8u)&511u,fine=(flags>>17u)&2047u,quality=(flags>>28u)&15u;
 return coarse|(fine<<9u)|(quality<<20u);}
uint coarseBeam(uint spatial){if((spatial&0x80000000u)!=0u){uint fine=spatial&127u;if(fine==0u)return 0u;
  return (((fine-1u)+post[81]/2u)/post[81])%post[75]+1u;}return spatial&511u;}
uint detectionBeam(uint i){return coarseBeam(detectionSpatial(i));}
uint latestSpatial(uint r){uint packed=src(r+13u),head=packed&255u,length=(packed>>8u)&255u,h=historyCapacity();
 return length==0u?0u:src(r+16u+8u*((head+length-1u)%h)+7u);}
uint candidateBeam(uint spatial,uint candidate){if((spatial&0x80000000u)==0u)return candidate==0u?coarseBeam(spatial):0u;
 uint fine=(spatial>>(candidate*7u))&127u;if(fine==0u)return 0u;
 return (((fine-1u)+post[81]/2u)/post[81])%post[75]+1u;}
bool bearingCompatible(uint trackSpatial,uint detectionSpatialValue){uint beams=post[75],tolerance=post[77];
 if(tolerance==0xffffffffu||beams==0u||trackSpatial==0u||detectionSpatialValue==0u)return true;
 for(uint leftCandidate=0u;leftCandidate<3u;++leftCandidate){uint left=candidateBeam(trackSpatial,leftCandidate);if(left==0u)continue;
  for(uint rightCandidate=0u;rightCandidate<3u;++rightCandidate){uint right=candidateBeam(detectionSpatialValue,rightCandidate);if(right==0u)continue;
   uint delta=left>right?left-right:right-left;if(min(delta,beams-delta)<=tolerance)return true;}}
 return false;
}
bool adaptiveGate(){return (m[38]&0x80000000u)!=0u;}
uint hypothesisCount(){return m[38]&0x7fffffffu;}
bool clutterAware(){return clutterPersistence[0]!=0u;}
uint clutterStreak(uint cell){
 uint packed=clutterPersistence[10u+cell],streak=packed&65535u;
 uint last=packed>>16u,current=m[10]&65535u,age=(current-last)&65535u;
 uint missed=age>0u?age-1u:0u;
 // Leaky occupancy tolerates short detector gaps without retaining isolated
 // transients: one evidence credit is lost per four missed CPIs.
 uint leaked=missed/4u;
 return packed==0u||leaked>=streak?0u:streak-leaked;
}
uint clutterRidgeStreak(uint cell){
 uint rows=post[33],cells=m[4];if(rows==0u||cells==0u||cell>=cells)return 0u;
 uint delayBins=cells/rows;if(delayBins==0u)return 0u;
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
 return abs(value-left)<=abs(right-value)?low-1u:low;}
bool dopplerCombBirthEligible(uint i){
 uint at=m[5]+8u*i,row=nearestDopplerRow(sd(at+2u)),cluster=1u,winner=i;
 double delay=sd(at),snr=sd(at+4u);
 uint siblings=0u;for(uint track=0u;track<m[18];++track){uint w=work(track);if(m[w+7u]==0u)continue;
  uint trackRow=nearestDopplerRow(md(w+2u)),rowDelta=row>trackRow?row-trackRow:trackRow-row;
  if(rowDelta<=3u&&abs(md(w)-delay)>=4.0LF&&++siblings>=2u)return false;}
 for(uint j=0u;j<m[19];++j){if(j==i||s[m[6]+j]!=0u)continue;uint other=m[5]+8u*j;
  uint otherRow=nearestDopplerRow(sd(other+2u)),rowDelta=row>otherRow?row-otherRow:otherRow-row;
  if(rowDelta>3u||abs(sd(other)-delay)<4.0LF)continue;cluster++;
  double otherSnr=sd(other+4u);if(otherSnr>snr||(otherSnr==snr&&j<winner)){winner=j;snr=otherSnr;}}
 return cluster<3u||winner==i;
}
bool rangeCombBirthEligible(uint i){
 uint at=m[5]+8u*i,row=nearestDopplerRow(sd(at+2u)),cluster=1u,winner=i;
 double delay=sd(at),snr=sd(at+4u);
 for(uint j=0u;j<m[19];++j){if(j==i||s[m[6]+j]!=0u)continue;uint other=m[5]+8u*j;
  if(abs(sd(other)-delay)>.5LF)continue;
  uint otherRow=nearestDopplerRow(sd(other+2u)),rowDelta=row>otherRow?row-otherRow:otherRow-row;
  if(rowDelta<4u)continue;cluster++;
  double otherSnr=sd(other+4u);if(otherSnr>snr||(otherSnr==snr&&j<winner)){winner=j;snr=otherSnr;}}
 return cluster<3u||winner==i;
}
bool initiationEligible(uint i){if(s[m[6]+i]!=0u)return false;
 uint at=m[5]+8u*i;if(sd(at+4u)<md(50u))return false;
 if(!dopplerCombBirthEligible(i)||!rangeCombBirthEligible(i))return false;
 if(!clutterAware())return true;double doppler=sd(at+2u);
 double maximum=packDouble2x32(uvec2(clutterPersistence[2],clutterPersistence[3]));
 if(maximum>0.0LF&&abs(doppler)>maximum)return true;uint cell=s[post[41]+i];
 if(cell>=m[4]||post[33]==0u)return false;
 return clutterRidgeStreak(cell)<clutterPersistence[1];}
bool quarantinePersistentTrack(uint r,uint match){
 if(!clutterAware()||match==0xffffffffu)return false;
 uint samples=clutterPersistence[4],minimumAssociated=clutterPersistence[5];
 uint ringBits=src(r+13u),head=ringBits&255u,length=(ringBits>>8u)&255u,h=historyCapacity();
 if(samples<2u||samples>h||length+1u<samples||minimumAssociated>samples)return false;
 uint detectionAt=m[5]+8u*match,cell=s[post[41]+match];
 if(cell>=m[4]||post[33]==0u)return false;
 if(clutterRidgeStreak(cell)<clutterPersistence[1])return false;
 double maximumDelaySpan=packDouble2x32(uvec2(clutterPersistence[6],clutterPersistence[7]));
 double maximumDopplerSpan=packDouble2x32(uvec2(clutterPersistence[8],clutterPersistence[9]));
 double newestDelay=sd(detectionAt),newestDoppler=sd(detectionAt+2u);
 double minimumDelay=newestDelay,maximumDelay=newestDelay;
 double minimumDoppler=newestDoppler,maximumDoppler=newestDoppler;uint associated=1u;
 uint first=length-(samples-1u);
 for(uint j=first;j<length;++j){uint at=r+16u+8u*((head+j)%h);
  double delay=sourceD(at),doppler=sourceD(at+2u);uint state=src(at+6u);
  if(!finiteD(delay)||!finiteD(doppler))return false;
  minimumDelay=min(minimumDelay,delay);maximumDelay=max(maximumDelay,delay);
  minimumDoppler=min(minimumDoppler,doppler);maximumDoppler=max(maximumDoppler,doppler);
  if(state==1u||state==2u)associated++;}
 return associated>=minimumAssociated&&maximumDelay-minimumDelay<=maximumDelaySpan&&
  maximumDoppler-minimumDoppler<=maximumDopplerSpan;
}
void header(uint count,uint nextID){
 for(uint k=0u;k<32u;++k)put(k,0u);
 put(0u,1u);put(1u,recordWords()*4u);put(2u,m[3]);put(3u,historyCapacity());put(4u,count);
 put(6u,m[16]);put(7u,m[17]);put(8u,nextID);put(10u,m[14]);put(11u,m[15]);
 put(12u,m[10]);put(13u,m[11]);put(14u,m[12]);put(15u,m[13]);
}
)glsl";
const char* initializeBody=R"glsl(
void main(){if(indexOf()!=0u)return;
 for(uint k=0u;k<32u;++k){a[k]=0u;b[k]=0u;}
 uint words=recordWords(),history=historyCapacity();
 a[0]=1u;b[0]=1u;a[1]=words*4u;b[1]=words*4u;a[2]=m[3];b[2]=m[3];a[3]=history;b[3]=history;
 a[10]=m[14];b[10]=m[14];a[11]=m[15];b[11]=m[15];
}
)glsl";
const char* admitBody=R"glsl(
void main(){if(indexOf()!=0u)return;m[52]=m[10];m[53]=m[11];
 uint count=src(4u),detections=post[11];
 if(post[0]!=0u||src(5u)!=0u||count>m[3]||detections>m[4]||src(8u)>=65535u){reject(301u);return;}
 m[18]=count;m[19]=detections;m[24]=32u+recordWords()*count;
 if(count>0u&&!greater64(pairM(16u),pairS(6u))){m[2]=1u;return;}
 m[2]=2u;precise double elapsed=count>0u?double64(subtract64(pairM(16u),pairS(6u)))/1000.0LF:0.0LF;
 if(!finiteD(elapsed)||(count>0u&&!(elapsed>0.0LF))){reject(302u);return;}writeM(50u,elapsed);
}
)glsl";
const char* predictBody=R"glsl(
void main(){uint i=indexOf();if(m[0]!=0u||m[2]!=2u)return;
 if(i<m[19]){s[m[6]+i]=0u;uint at=m[5]+8u*i;
  uint cell=s[post[41]+i],beam=cell<m[4]?((s[post[39]+cell]>>8u)&511u):0xffffffffu;
  if(!finiteD(sd(at))||!finiteD(sd(at+2u))||!finiteD(sd(at+4u))||cell>=m[4]||beam>post[75])reject(303u);
  else if(post[33]!=0u&&cell%post[33]+1u==post[33]){s[m[6]+i]=2u;atomicAdd(m[65],1u);}
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
  bool adaptiveTentative=adaptiveGate()&&state==0u;
  precise double delayMargin=1.0LF,dopplerMargin=1.0LF/cpi,span=0.0LF,time=md(50u),gateTime=time,q=0.0LF;
  if(adaptiveTentative){gateTime=time*double(src(r+10u)+1u);span=md(48u)*double(m[37]);
   q=md(46u)*0.5LF*gateTime*gateTime/md(44u);delayMargin+=q*span;dopplerMargin+=span*gateTime;}
  precise double lowDelay=dp-delayMargin,highDelay=dp+delayMargin,lowDoppler=vp-dopplerMargin,highDoppler=vp+dopplerMargin;
  bool validGate=finiteD(lowDelay)&&finiteD(highDelay)&&finiteD(lowDoppler)&&finiteD(highDoppler)&&
   (!adaptiveTentative||(gateTime>0.0LF&&q>0.0LF&&finiteD(span)));if(m[19]>0u&&!validGate)reject(305u);
  double best=packDouble2x32(uvec2(0u,0x7ff00000u));uint match=0xffffffffu;
  for(uint j=lane;j<m[19];j+=128u){uint at=m[5]+8u*j;double delay=sd(at),doppler=sd(at+2u);
   bool compatible=validGate&&delay>lowDelay&&delay<highDelay&&doppler>lowDoppler&&doppler<highDoppler;
   precise double fitAcc=0.0LF;
   if(compatible&&adaptiveTentative){precise double dd=delay-dp,dv=doppler-vp,unitDoppler=1.0LF/cpi;
    precise double accelerationLow=max(-span,max((dv-unitDoppler)/gateTime,(-1.0LF-dd)/q));
    precise double accelerationHigh=min(span,min((dv+unitDoppler)/gateTime,(1.0LF-dd)/q));
    compatible=finiteD(accelerationLow)&&finiteD(accelerationHigh)&&accelerationLow<accelerationHigh;
    if(compatible){precise double denominator=q*q+gateTime*gateTime*cpi*cpi;
     fitAcc=clamp((gateTime*cpi*cpi*dv-q*dd)/denominator,accelerationLow,accelerationHigh);}}
   if(compatible&&s[m[6]+j]==0u&&bearingCompatible(previousSpatial,detectionSpatial(j))){
    precise double deltaDelay=delay-dp,deltaDoppler=(doppler-vp)*cpi;
    if(adaptiveTentative){deltaDelay+=q*fitAcc;deltaDoppler=(doppler-vp-fitAcc*gateTime)*cpi;}
    precise double distance=deltaDelay*deltaDelay+deltaDoppler*deltaDoppler;
    if(!finiteD(distance))reject(305u);else if(distance<best){best=distance;match=j;}
   }}
  distances[lane]=best;identities[lane]=match;memoryBarrierBuffer();barrier();
  if(m[0]!=0u)return;
  for(uint width=64u;width>0u;width>>=1u){if(lane<width){uint other=lane+width;
    if(distances[other]<distances[lane]||(distances[other]==distances[lane]&&identities[other]<identities[lane])){
     distances[lane]=distances[other];identities[lane]=identities[other];}}
   barrier();}
  if(lane==0u){match=identities[0];double acc=sourceD(r+6u);bool alive=true,guidedAssociation=false;
#if TRACK_GUIDED_SHADOW
   uint shadow=guidedRead(16u+i)|(src(r+14u)<<8u);
   if(guidedRead(0u)!=0u&&(shadow&2u)!=0u){if(match==0xffffffffu){atomicAdd(m[guidedBase()+7u],1u);atomicAdd(m[guidedBase()+(src(r+14u)<2u?11u:12u)],1u);shadow|=32u;
      if((guidedRead(0u)&2u)!=0u){guidedAssociation=true;shadow|=64u;atomicAdd(m[guidedBase()+13u],1u);}}
     else{atomicAdd(m[guidedBase()+8u],1u);shadow|=16u;}}
   guidedWrite(16u+i,shadow);
#endif
   if(match!=0xffffffffu){precise double updated=adaptiveTentative&&src(r+10u)>0u?
     sourceD(r+6u)+(sd(m[5]+8u*match+2u)-md(w+2u))/gateTime:
     (sd(m[5]+8u*match+2u)-sourceD(r+2u))/md(50u);
    if(!finiteD(updated))reject(306u);acc=updated;s[m[6]+match]=1u;state=(state==2u||state==3u)?2u:1u;m[27]++;
    // Retain the filter and its history, but keep a stable learned cell out of
    // operational publication. If it starts moving, fresh associations leave
    // the nuisance cell and the ordinary M-of-N confirmation path restores it.
    if(quarantinePersistentTrack(r,match)){state=0u;m[64]++;}}
   else if(guidedAssociation){state=(state==2u||state==3u)?2u:1u;m[27]++;}
   else{state=state==2u?3u:(state==1u?0u:state);alive=!greater64(plusOne(pairS(r+10u)),uvec2(m[41],0u));m[28]++;if(!alive)m[29]++;}
   m[w+4u]=match;m[w+5u]=alive?m[20]++:0xffffffffu;m[w+6u]=state;m[w+7u]=alive?1u:0u;writeM(w+8u,acc);m[w+10u]=0u;m[w+11u]=0u;m[55]++;
#if TRACK_GUIDED_SHADOW
   m[w+10u]=guidedAssociation?1u:0u;
#endif
  }
  // Each claim is visible before any lane examines the next ordered track.
  memoryBarrierBuffer();barrier();if(m[0]!=0u)return;
 }
}
)glsl";
const char* unmatchedCountBody=R"glsl(
shared uint values[128];
void main(){uint i=indexOf(),lane=gl_LocalInvocationID.x,tile=i/128u,tiles=(m[19]+127u)/128u;
 if(m[0]!=0u||m[2]!=2u||tile>=tiles)return;values[lane]=i<m[19]&&initiationEligible(i)?1u:0u;barrier();
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
 if(keep)s[m[7]+s[m[7]+m[4]+tile]+values[lane]-1u]=i;
}
)glsl";
const char* persistenceBody=R"glsl(
void main(){uint i=indexOf();if(m[0]!=0u||m[2]!=2u||!clutterAware())return;
 if(i>=m[19])return;uint at=m[5]+8u*i;double doppler=sd(at+2u);
 double maximum=packDouble2x32(uvec2(clutterPersistence[2],clutterPersistence[3]));if(maximum>0.0LF&&abs(doppler)>maximum)return;
 uint cell=s[post[41]+i];if(cell>=m[4]||post[33]==0u){reject(320u);return;}
 // Paint a bounded 9x3 neighborhood. Narrow waveform/site ridges can walk a
 // few quantized delay cells between CFAR appearances while retaining their
 // Doppler identity. Join that delay walk but keep Doppler locality tight so
 // this never becomes a delay-column notch. An independent moving target still
 // has to occupy the neighborhood for the configured persistence interval.
 uint delayBins=post[33],row=cell/delayBins,rows=post[32],delay=cell%delayBins;
 for(int rowOffset=-1;rowOffset<=1;++rowOffset){int candidateRow=int(row)+rowOffset;if(candidateRow<0||candidateRow>=int(rows))continue;
  for(int delayOffset=-4;delayOffset<=4;++delayOffset){int candidateDelay=int(delay)+delayOffset;if(candidateDelay<0||candidateDelay>=int(delayBins))continue;
  uint index=10u+uint(candidateRow)*delayBins+uint(candidateDelay),prior=clutterPersistence[index],current=m[10]&65535u;
  while(true){uint last=prior>>16u,streak=prior&65535u;if(prior!=0u&&last==current)break;
   uint age=(current-last)&65535u,missed=age>0u?age-1u:0u,leaked=missed/4u;
   uint decayed=prior==0u||leaked>=streak?0u:streak-leaked;
   uint next=(current<<16u)|min(65535u,decayed+1u);
   uint observed=atomicCompSwap(clutterPersistence[index],prior,next);if(observed==prior)break;prior=observed;}}}
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
 put(at+6u,state);put(at+7u,beam);put(target+6u,m[w+8u]);put(target+7u,m[w+9u]);
 uvec2 age=plusOne(pairS(source+8u)),inactive=associated?uvec2(0u):plusOne(pairS(source+10u));
 put(target+8u,age.x);put(target+9u,age.y);put(target+10u,inactive.x);put(target+11u,inactive.y);
 uint display=min(length,100u),start=(head+length-display)%h;put(target+13u,head|(length<<8u)|(start<<16u)|(display<<24u));
 if(associated&&length>=m[40]){uint count=0u;for(uint j=length-m[40];j<length;++j){uint value=dst(target+16u+8u*((head+j)%h)+6u);if(value==1u||value==2u)count++;}
  if(count>=m[39]){state=2u;put(at+6u,state);}}
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
 put(target+13u,(1u<<8u)|(1u<<24u));put(target+15u,0xffffffffu);put(target+23u,detectionSpatial(detection));atomicAdd(m[30],1u);atomicAdd(m[31],1u);
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
 if(i==0u){if(checkedWord(0u)!=1u||checkedWord(1u)!=recordWords()*4u||checkedWord(2u)!=m[3]||checkedWord(3u)!=historyCapacity()||
    checkedWord(8u)>=65535u||checkedWord(9u)!=0u||checkedWord(10u)!=m[14]||checkedWord(11u)!=m[15]||m[24]!=32u+recordWords()*count)reject(311u);
  for(uint k=26u;k<32u;++k)if(checkedWord(k)!=0u)reject(311u);
  if(m[2]==2u&&(checkedWord(6u)!=m[16]||checkedWord(7u)!=m[17]||checkedWord(12u)!=m[10]||checkedWord(13u)!=m[11]||checkedWord(14u)!=m[12]||checkedWord(15u)!=m[13]))reject(311u);
 }
 if(i>=count)return;uint r=record(i),ringBits=checkedWord(r+13u),head=ringBits&255u,length=(ringBits>>8u)&255u,start=(ringBits>>16u)&255u,display=ringBits>>24u,state=checkedWord(r+14u),h=historyCapacity();
 if(checkedWord(r+12u)>=65535u||head>=h||length==0u||length>h||(length<h&&head!=0u)||
    display!=min(length,100u)||start!=(head+length-display)%h||state>3u){reject(312u);return;}
 uvec2 inactive=uvec2(checkedWord(r+10u),checkedWord(r+11u));
 if(greater64(inactive,uvec2(m[41],0u))||((state==1u||state==2u)&&any(notEqual(inactive,uvec2(0u))))||(state==3u&&all(equal(inactive,uvec2(0u))))){reject(313u);return;}
 for(uint k=0u;k<8u;k+=2u)if(!finiteD(checkedD(r+k)))reject(314u);
 uint newest=r+16u+8u*((head+length-1u)%h);
 for(uint k=0u;k<6u;++k)if(checkedWord(r+k)!=checkedWord(newest+k))reject(315u);
 if(checkedWord(newest+6u)!=state)reject(315u);
 for(uint j=0u;j<h;++j){uint at=r+16u+8u*j;
  if(length<h&&j>=length){for(uint k=0u;k<8u;++k)if(checkedWord(at+k)!=0u)reject(3161u);}
  else{for(uint k=0u;k<6u;k+=2u)if(!finiteD(checkedD(at+k)))reject(314u);uint spatial=checkedWord(at+7u);
   if((spatial&0x80000000u)!=0u){uint a=spatial&127u,b=(spatial>>7u)&127u,c=(spatial>>14u)&127u;
    if(FINE_SPATIAL_INVALID)reject(3162u);}
   else{uint coarse=spatial&511u,fine=(spatial>>9u)&2047u;if(coarse>post[75]||fine>post[75]*4u||(coarse==0u&&fine!=0u))reject(3163u);}
   if(checkedWord(at+6u)>3u)reject(3164u);}}
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
const char* guidedShadowBody=R"glsl(
#if TRACK_GUIDED_SHADOW
void main(){uint i=indexOf();if(i>=guidedRead(1u)||i>=m[18]||m[0]!=0u||m[2]!=2u)return;
 uint r=record(i),w=work(i),id=src(r+12u)&65535u,state=src(r+14u);guidedWrite(16u+i,id<<16u);
 if(guidedRead(0u)==0u||post[78]==0u||post[74]<4u||post[79]<2u){atomicAdd(m[guidedBase()+6u],1u);return;}
 double currentDelay=md(w),currentDoppler=md(w+2u),priorDelay=sourceD(r),priorDoppler=sourceD(r+2u);
 int currentColumn=int(floor(currentDelay-double(int(post[post[45]]))+.5LF));
 int priorColumn=int(floor(priorDelay-double(int(post[post[45]]))+.5LF));
 if(currentColumn<0||priorColumn<0||currentColumn>=int(post[33])||priorColumn>=int(post[33])||
    !finiteD(currentDelay)||!finiteD(currentDoppler)||!finiteD(priorDelay)||!finiteD(priorDoppler)){
  atomicOr(m[guidedBase()+16u+i],8u);atomicAdd(m[guidedBase()+6u],1u);return;}
 uint currentRow=nearestDopplerRow(currentDoppler),priorRow=nearestDopplerRow(priorDoppler),column=uint(currentColumn);
 uint window=post[46]+post[84]*column,n=post[window+4u];if(n==0u){atomicOr(m[guidedBase()+16u+i],8u);atomicAdd(m[guidedBase()+6u],1u);return;}
 uint currentCell=currentRow*post[33]+column,priorCell=priorRow*post[33]+uint(priorColumn);
 uint priorSlot=(post[79]+post[74]-2u)%post[74];double a=currentPower[currentCell],b=integrationHistory[priorSlot*post[34]+priorCell];
 if(!finiteD(a)||!finiteD(b)||a<0.0LF||b<0.0LF){atomicOr(m[guidedBase()+16u+i],8u);atomicAdd(m[guidedBase()+6u],1u);return;}
 precise double train=0.0LF;uint seen=0u;int shift=priorColumn-currentColumn;
 for(uint side=0u;side<2u;++side){uint first=post[window+2u*side],last=post[window+2u*side+1u];
  for(uint j=first;j<last;++j){int old=int(j)+shift;if(old<0||old>=int(post[33]))continue;
   double x=currentPower[currentRow*post[33]+j],y=integrationHistory[priorSlot*post[34]+priorRow*post[33]+uint(old)];
   if(finiteD(x)&&finiteD(y)&&x>=0.0LF&&y>=0.0LF){train+=.5LF*(x+y);seen++;}}}
 if(seen!=n){atomicOr(m[guidedBase()+16u+i],8u);atomicAdd(m[guidedBase()+6u],1u);return;}
 // The tracker supplies one pre-registered motion path. Use the ordinary
 // two-CPI single-path alpha, not the full-map 27-motion-trial penalty.
 double alpha=packDouble2x32(uvec2(post[window+6u],post[window+7u]));
 precise double cut=.5LF*(a+b),threshold=alpha*(train/double(n));
 if(!finiteD(cut)||!finiteD(threshold)){atomicOr(m[guidedBase()+16u+i],8u);atomicAdd(m[guidedBase()+6u],1u);return;}
 atomicOr(m[guidedBase()+16u+i],1u);atomicAdd(m[guidedBase()+3u],1u);atomicAdd(m[guidedBase()+(state<2u?9u:10u)],1u);
 if(cut>threshold){atomicOr(m[guidedBase()+16u+i],2u);atomicAdd(m[guidedBase()+4u],1u);}
}
#else
void main(){}
#endif
)glsl";
inline std::vector<std::pair<std::string,std::string>> shaderSources(bool guided=false){
 const std::pair<const char*,const char*> bodies[]={{"initialize",initializeBody},{"admit",admitBody},{"predict",predictBody},{"associate",associateBody},
  {"unmatched_count",unmatchedCountBody},{"unmatched_prefix",unmatchedPrefixBody},{"unmatched_scatter",unmatchedScatterBody},{"history",historyBody},
  {"initiate",initiateBody},{"finalize",finalizeBody},{"validate",validateBody},{"validate_totals",validateTotalsBody},{"commit",commitBody},{"persistence",persistenceBody},{"guided_shadow",guidedShadowBody}};
 std::vector<std::pair<std::string,std::string>> result;
 const std::string definition=guided?"#define TRACK_GUIDED_SHADOW 1\n":"#define TRACK_GUIDED_SHADOW 0\n";
 const std::string header=std::string(common),prefix=header.substr(0,13)+definition+header.substr(13);
 for(const auto& body:bodies){std::string source=body.second;
  const auto marker=source.find("FINE_SPATIAL_INVALID");
  if(marker!=std::string::npos)source.replace(marker,20,fineSpatialInvalidGlsl);
  result.emplace_back(body.first,prefix+source);}
 return result;
}
class Stage {
 Context& context_;
 const TrackerConfig config_;
 const Admission admission_;
 const uint32_t cells_;
 const gpu_postprocess::Layout layout_;
 Buffer& post_;
 Buffer& scratch_;
 Buffer& integration_;
 Buffer& currentPower_;
 std::unique_ptr<Buffer> banks_[2],metadata_,clutterPersistence_;
 std::unique_ptr<gpu_native::Timestamps> timestamps_;
 std::vector<std::unique_ptr<Kernel>> kernels_;
 uint32_t active_=0;
 uint64_t serial_=0;
 bool pending_=false,poisoned_=false,lastCommitted_=false,noOp_=false,copied_=false;
 bool guidedBudgetExceeded_=false;
 double guidedLastMs_=0;
 std::array<uint32_t,statusWords> controlEvidence_{};
 double evidenceHostCommitMs_=0;
 TrackerToken pendingToken_{},lastToken_{};
 std::array<uint32_t,statusWords> ready_{};
 static void put64(uint32_t* target,uint32_t at,uint64_t value){target[at]=uint32_t(value);
 target[at+1]=uint32_t(value>>32);}
 static void putDouble(uint32_t* target,uint32_t at,double value){std::memcpy(target+at,&value,8);}
 static void hostRead(VkCommandBuffer command){VkMemoryBarrier b{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
 b.srcAccessMask=VK_ACCESS_MEMORY_WRITE_BIT;
 b.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
  vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&b,0,nullptr,0,nullptr);}
 void healthy()const{if(poisoned_)throw std::logic_error("GPU tracker is poisoned");}
 std::array<uint32_t,statusWords> status(){metadata_->invalidate(0,statusWords*4);
 std::array<uint32_t,statusWords>s{};
 std::memcpy(s.data(),metadata_->mapped,sizeof(s));
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
  s[38]=admission_.hypotheses|(config_.adaptiveGate?0x80000000u:0u);
  s[39]=config_.m;
  s[40]=config_.n;
  s[41]=config_.nDelete;
  putDouble(s.data(),42,config_.cpi);
  putDouble(s.data(),44,config_.rangeRes);
  putDouble(s.data(),46,config_.lambda);
  putDouble(s.data(),48,admission_.resolution);
  // Words 50-51 are configuration scratch. 52-53 echo generation and 58 is
  // the commit acknowledgement, so neither can safely carry this threshold.
  putDouble(s.data(),50,config_.minInitiationSnrDb);
  std::memcpy(metadata_->mapped,s.data(),sizeof(s));
  metadata_->flush(0,sizeof(s));
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
 void checkTokenEcho(const std::array<uint32_t,statusWords>& s,const TrackerToken& token)const{
  if(s[3]!=config_.capacity||s[4]!=cells_||s[8]!=uint32_t(token.transaction)||s[9]!=uint32_t(token.transaction>>32)||
     s[10]!=uint32_t(token.generation)||s[11]!=uint32_t(token.generation>>32)||s[12]!=uint32_t(token.consumedSequence)||s[13]!=uint32_t(token.consumedSequence>>32)||
     s[14]!=uint32_t(config_.epoch)||s[15]!=uint32_t(config_.epoch>>32)||s[52]!=uint32_t(token.generation)||s[53]!=uint32_t(token.generation>>32))throw std::runtime_error("GPU tracker transaction echo mismatch");
 }
 void checkCoverage(const std::array<uint32_t,statusWords>& s)const{
  uint32_t guidedPromoted=0;
  if(config_.trackGuidedPromote){const uint64_t guidedOffset=uint64_t(statusWords+workWords*config_.capacity)*4;
   metadata_->invalidate(guidedOffset,uint64_t(16+config_.capacity)*4);
   guidedPromoted=static_cast<const uint32_t*>(metadata_->mapped)[statusWords+workWords*config_.capacity+13];}
  if(s[1]!=active_||s[24]<headerWords||s[24]>admission_.bankWords||s[54]>config_.capacity||s[24]!=headerWords+admission_.recordWords*s[54]||
     uint64_t(s[33])+s[34]+s[35]+s[36]!=s[54]||s[32]!=s[34]+s[35]+s[36])throw std::runtime_error("GPU tracker snapshot coverage mismatch");
  if(s[2]==1){for(uint32_t k=26;k<=31;++k)if(s[k])throw std::runtime_error("GPU tracker no-op performed tracking work");
   if(s[55]||s[56]||s[57]||s[54]!=s[18])throw std::runtime_error("GPU tracker no-op coverage mismatch");}
 else if(s[2]!=2||s[26]!=s[18]||s[27]+s[28]!=s[18]||s[27]<guidedPromoted||s[27]-guidedPromoted>s[19]||s[65]>s[19]-(s[27]-guidedPromoted)||s[21]>s[19]-(s[27]-guidedPromoted)-s[65]||s[29]>s[18]||!validQuarantineCoverage(s[27],s[64])||s[20]!=s[18]-s[29]||s[55]!=s[18]||
    s[57]!=s[19]||s[56]!=(s[19]+127)/128||s[30]!=s[22]||s[31]!=s[23]||s[23]!=s[20]+s[22]||s[54]!=s[23])
   throw std::runtime_error("GPU tracker executed-work coverage mismatch");
  if(s[2]==2){const auto admission=boundInitiations(config_.capacity,s[20],s[21],admission_.hypotheses);
   if(s[22]!=admission.initiatedTracks||s[23]!=admission.totalTracks)
    throw std::runtime_error("GPU tracker bounded initiation coverage mismatch");}
 }
 void validateOutputs(TrackerOutputs out)const{
  if(!out.checkpoint||out.wordCapacity!=admission_.bankWords||!out.result)throw std::invalid_argument("GPU tracker output dimensions changed");
  if(config_.trackGuidedShadow?(!out.guidedRecoveries||out.guidedWordCapacity!=uint64_t(config_.trackGuidedShadowMaxTracks)*6):
    (out.guidedRecoveries||out.guidedWordCapacity))throw std::invalid_argument("GPU tracker guided output dimensions changed");
  const auto range=[](const void* p,size_t bytes){uintptr_t start=reinterpret_cast<uintptr_t>(p);
  if(bytes>UINTPTR_MAX-start)throw std::invalid_argument("GPU tracker output overflow");
  return std::pair<uintptr_t,uintptr_t>{start,start+bytes};};
  const auto a=range(out.checkpoint,out.wordCapacity*4),z=range(out.result,sizeof(TrackerResult));
  const auto overlaps=[](auto x,auto y){return x.first<y.second&&y.first<x.second;};
  if(overlaps(a,z))throw std::invalid_argument("GPU tracker outputs overlap");
  if(out.guidedRecoveries){const auto g=range(out.guidedRecoveries,out.guidedWordCapacity*4);
   if(overlaps(a,g)||overlaps(z,g))throw std::invalid_argument("GPU tracker guided outputs overlap");}
  for(const Buffer* b:{banks_[0].get(),banks_[1].get(),metadata_.get(),clutterPersistence_.get(),&post_,&scratch_,&integration_,&currentPower_})if(b->mapped){const auto x=range(b->mapped,size_t(b->bytes));
  if(overlaps(a,x)||overlaps(z,x))throw std::invalid_argument("GPU tracker output aliases device storage");}
 }
 void copyBank(uint32_t bank,uint32_t words,TrackerOutputs out,const TrackerToken& token,bool noOp){
  validateOutputs(out);
  if(words<headerWords||words>admission_.bankWords)throw std::runtime_error("GPU tracker checkpoint size invalid");
  banks_[bank]->invalidate(0,uint64_t(words)*4);
  std::memcpy(out.checkpoint,banks_[bank]->mapped,size_t(words)*4);
  *out.result={token,words,0,noOp};
 }
 void logPrepared(const std::array<uint32_t,statusWords>& s){context_.diagnostics.stream()<<std::setprecision(12)<<"GPU_TRACKER_PREPARED transaction="<<pendingToken_.transaction<<" generation="<<pendingToken_.generation<<" consumed_sequence="<<pendingToken_.consumedSequence<<" epoch="<<config_.epoch
  <<" device_generation_low="<<s[52]<<" device_generation_high="<<s[53]<<" noop="<<(s[2]==1)<<" capacity="<<config_.capacity<<" tracks="<<s[54]<<" detections="<<s[19]<<" adaptive_gate="<<config_.adaptiveGate<<" acceleration_hypotheses="<<admission_.hypotheses<<" device_predicted="<<s[26]<<" device_associated="<<s[27]<<" device_missed="<<s[28]<<" device_removed="<<s[29]<<" device_initiated="<<s[30]
  <<" device_retained_history_appends="<<s[31]<<" device_published="<<s[32]<<" device_stationary_quarantined="<<s[64]<<" device_stationary_retired=0 device_association_visits="<<s[55]<<" device_unmatched_tiles="<<s[56]<<" device_detection_checks="<<s[57]<<" device_validated_records="<<s[54]<<" checkpoint_words="<<s[24];
  if(s[2]==2){const auto admission=boundInitiations(config_.capacity,s[20],s[21],admission_.hypotheses);
   uint32_t guidedPromoted=0;if(config_.trackGuidedPromote){const auto*g=static_cast<const uint32_t*>(metadata_->mapped)+statusWords+workWords*config_.capacity;guidedPromoted=g[13];}
   context_.diagnostics.stream()<<" unmatched_detections="<<admission.unmatched
    <<" boundary_rejected="<<s[65]
    <<" stationary_clutter_suppressed="<<(s[19]-(s[27]-guidedPromoted)-s[65]-admission.unmatched)
    <<" admitted_detections="<<admission.admittedDetections<<" dropped_capacity_detections="<<admission.droppedDetections;}
  for(uint32_t phase=0;phase<6;++phase)context_.diagnostics.stream()<<" gpu_phase"<<phase<<"_ms="<<timestamps_->milliseconds(phase);
  if(config_.trackGuidedShadow){const uint64_t guidedOffset=uint64_t(statusWords+workWords*config_.capacity)*4;
   metadata_->invalidate(guidedOffset,uint64_t(16+config_.capacity)*4);const auto*g=static_cast<const uint32_t*>(metadata_->mapped)+statusWords+workWords*config_.capacity;
   if(!guidedBudgetExceeded_){guidedLastMs_=timestamps_->milliseconds(8);if(guidedLastMs_>10.0)guidedBudgetExceeded_=true;}
   context_.diagnostics.stream()<<" guided_shadow=1 guided_max_tracks="<<g[1]<<" guided_evaluated="<<g[3]<<" guided_above_threshold="<<g[4]
    <<" guided_stationary_rejected="<<g[5]<<" guided_invalid="<<g[6]<<" guided_would_recover="<<g[7]
    <<" guided_already_detected="<<g[8]<<" guided_tentative_evaluated="<<g[9]<<" guided_established_evaluated="<<g[10]
    <<" guided_tentative_would_recover="<<g[11]<<" guided_established_would_recover="<<g[12]
    <<" guided_promote="<<config_.trackGuidedPromote<<" guided_promoted="<<g[13]
    <<" guided_auto_disabled="<<guidedBudgetExceeded_<<" gpu_guided_shadow_ms="<<guidedLastMs_<<" guided_recovery_track_ids=";
   uint32_t written=0;for(uint32_t i=0;i<std::min(s[18],g[1]);++i)if((g[16+i]&32u)!=0u){if(written++)context_.diagnostics.stream()<<',';context_.diagnostics.stream()<<(g[16+i]>>16u);}
   if(!written)context_.diagnostics.stream()<<"none";
  }
  context_.diagnostics.stream()<<" cpu_predict_calls=0 cpu_associate_calls=0 cpu_history_calls=0 cpu_initiate_calls=0\n";
 }
public:
 const auto& evidencePrepared()const{return ready_;}
 const auto& evidenceControl()const{return controlEvidence_;}
 const gpu_native::Timestamps& evidenceTimes()const{return *timestamps_;}
 double evidenceHostCommitMs()const{return evidenceHostCommitMs_;}
 uint32_t activeBank()const{return active_;}
 Stage(Context& context,const TrackerConfig& config,uint32_t cells,Buffer& post,Buffer& scratch,const gpu_postprocess::Layout& layout,
  Buffer& integration,Buffer& currentPower)
  :context_(context),config_(config),admission_(validate(config,cells)),cells_(cells),layout_(layout),post_(post),scratch_(scratch),integration_(integration),currentPower_(currentPower){
  const auto& limit=context_.candidate.properties.limits;
  const auto*postWords=static_cast<const uint32_t*>(post.mapped);
  if(layout.cells!=cells||post.bytes<gpu_postprocess::statusWords*4||scratch.bytes<layout.bytes||
     uint64_t(layout.keep)+cells>layout.indices||uint64_t(layout.indices)+cells>layout.candidates||
     uint64_t(layout.candidates)+cells+(uint64_t(cells)+127)/128>layout.records||uint64_t(layout.records)+uint64_t(cells)*8>layout.bytes/4||
     !post.mapped||integration.bytes<uint64_t(cells)*(config.trackGuidedShadow?4u:1u)*8||currentPower.bytes<uint64_t(cells)*8||
     post.handle==scratch.handle||post.handle==integration.handle||post.handle==currentPower.handle||integration.handle==currentPower.handle||
     (config.trackGuidedShadow&&(!postWords[78]||postWords[74]<4u))||
     std::max({uint64_t(admission_.bankWords)*4,admission_.stageBytes,config.stationaryClutter?uint64_t(cells+clutterHeaderWords)*4:16})>limit.maxStorageBufferRange||limit.maxComputeSharedMemorySize<2048||limit.maxPerStageDescriptorStorageBuffers<(config.trackGuidedShadow?8u:6u)||limit.maxDescriptorSetStorageBuffers<(config.trackGuidedShadow?8u:6u))
   throw std::invalid_argument("GPU_TRACKER_UNSUPPORTED buffers or device limits");
  constexpr auto host=VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
  constexpr auto preferred=VK_MEMORY_PROPERTY_HOST_COHERENT_BIT|VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
  banks_[0]=std::make_unique<Buffer>(context,uint64_t(admission_.bankWords)*4,host,preferred);
  banks_[1]=std::make_unique<Buffer>(context,uint64_t(admission_.bankWords)*4,host,preferred);
  metadata_=std::make_unique<Buffer>(context,admission_.stageBytes,host,preferred);
  clutterPersistence_=std::make_unique<Buffer>(context,config.stationaryClutter?uint64_t(cells+clutterHeaderWords)*4:16,host,preferred);
  std::memset(clutterPersistence_->mapped,0,size_t(clutterPersistence_->bytes));
  auto* persistence=static_cast<uint32_t*>(clutterPersistence_->mapped);
  persistence[0]=config.stationaryClutter;
  persistence[1]=config.stationaryPersistenceCpis;
  std::memcpy(persistence+2,&config.stationaryMaxDopplerHz,8);
  const auto retirement=stationaryRetirementPolicy(config.stationaryClutter,
    config.cpi,config.stationaryPersistenceCpis);
  persistence[4]=retirement.samples;
  persistence[5]=retirement.minimumAssociated;
  std::memcpy(persistence+6,&retirement.maxDelaySpan,8);
  std::memcpy(persistence+8,&retirement.maxDopplerSpanHz,8);
  clutterPersistence_->flush(0,clutterPersistence_->bytes);
  std::memset(metadata_->mapped,0,size_t(metadata_->bytes));
  if(config.trackGuidedShadow){auto* guided=static_cast<uint32_t*>(metadata_->mapped)+statusWords+workWords*config.capacity;
   guided[0]=1u|(config.trackGuidedPromote?2u:0u);guided[1]=config.trackGuidedShadowMaxTracks;}
  metadata_->flush(0,metadata_->bytes);
  timestamps_=std::make_unique<gpu_native::Timestamps>(context);
  std::vector<Buffer*> bindings{metadata_.get(),banks_[0].get(),banks_[1].get(),&scratch_,&post_,clutterPersistence_.get()};
  if(config.trackGuidedShadow){bindings.push_back(&integration_);bindings.push_back(&currentPower_);}
  for(const auto& source:shaderSources(config.trackGuidedShadow))kernels_.push_back(std::make_unique<Kernel>(context,source.second.c_str(),bindings));
  setup({},0);
  commands([&](VkCommandBuffer command){timestamps_->begin(command,6);append(command,0,1);timestamps_->end(command,6);});
  std::cerr<<"GPU_TRACKER_READY capacity="<<config.capacity<<" history="<<admission_.historyCount<<" record_bytes="<<admission_.recordWords*4<<" bank_bytes="<<uint64_t(admission_.bankWords)*4<<" stage_bytes="<<admission_.stageBytes<<" requested_bytes="<<admission_.totalBytes<<" track_guided_shadow="<<config.trackGuidedShadow<<" guided_promote="<<config.trackGuidedPromote<<" guided_max_tracks="<<config.trackGuidedShadowMaxTracks<<" gpu_initialize_ms="<<timestamps_->milliseconds(6)<<" selected_live_capacity=0\n";
 }
 bool pending()const{return pending_;}
 TrackerToken preparedToken()const{healthy();
 if(!pending_)throw std::logic_error("GPU tracker has no prepared token");
 return pendingToken_;}
 void checkOutputs(TrackerOutputs out)const{validateOutputs(out);}
 uint32_t checkpointWordCapacity()const{return admission_.bankWords;}const TrackerConfig& config()const{return config_;}
 bool prepare(TrackerFrame frame){healthy();
 if(pending_)throw std::logic_error("GPU tracker already has a prepared transaction");
 if(serial_==UINT64_MAX)throw std::overflow_error("GPU tracker transaction exhausted");
  copied_=false;
  pendingToken_={config_.epoch,++serial_,frame.generation,frame.consumedSequence};
  try{setup(pendingToken_,frame.timestamp);
  commands([&](VkCommandBuffer command){
    timestamps_->begin(command,0);append(command,1,1);append(command,2,std::max(cells_,config_.capacity));timestamps_->end(command,0);
    if(config_.trackGuidedShadow){const VkDeviceSize offset=uint64_t(statusWords+workWords*config_.capacity)*4+8;
     vkCmdFillBuffer(command,metadata_->handle,offset,metadata_->bytes-offset,0);barrier(command);
     if(!guidedBudgetExceeded_){timestamps_->begin(command,8);append(command,14,config_.trackGuidedShadowMaxTracks);timestamps_->end(command,8);}}
    timestamps_->begin(command,1);append(command,3,1);timestamps_->end(command,1);
    timestamps_->begin(command,2);append(command,13,cells_);append(command,4,cells_);append(command,5,1);append(command,6,cells_);timestamps_->end(command,2);
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
  out.result->guidedRecoveries=0;
  if(config_.trackGuidedShadow&&!noOp_){
   const uint64_t workBytes=uint64_t(workWords)*config_.capacity*4;
   metadata_->invalidate(uint64_t(statusWords)*4,workBytes+uint64_t(16+config_.capacity)*4);
   const auto* words=static_cast<const uint32_t*>(metadata_->mapped);
   const auto* guided=words+statusWords+workWords*config_.capacity;
   for(uint32_t i=0;i<std::min(ready_[18],config_.trackGuidedShadowMaxTracks);++i){const uint32_t flags=guided[16+i];
    if((flags&32u)==0u)continue;
    uint32_t* target=out.guidedRecoveries+6u*out.result->guidedRecoveries++;
    target[0]=flags>>16u;target[1]=(flags>>8u)&3u;
    std::memcpy(target+2,words+statusWords+workWords*i,8);
    std::memcpy(target+4,words+statusWords+workWords*i+2,8);
   }
  }
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
   pending_=false;
   copied_=false;
   lastCommitted_=true;
   lastToken_=token;
   evidenceHostCommitMs_=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-began).count();
   std::cerr<<std::setprecision(12)<<"GPU_TRACKER_COMMIT transaction="<<token.transaction<<" generation="<<token.generation<<" epoch="<<token.epoch<<" noop="<<noOp_<<" device_commit=1 gpu_commit_ms="<<gpuMs<<" host_commit_ms="<<std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-began).count()<<" cpu_tracker_calls=0\n";
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
  for(const Buffer* buffer:{banks_[0].get(),banks_[1].get(),metadata_.get(),clutterPersistence_.get(),&post_,&scratch_,&integration_,&currentPower_})if(buffer->mapped){
   const uintptr_t at=reinterpret_cast<uintptr_t>(buffer->mapped);
   if(buffer->bytes>UINTPTR_MAX-at||(first<at+buffer->bytes&&at<first+bytes))
    throw std::invalid_argument("GPU tracker checkpoint aliases device storage");}
  if(checkpoint[5]||checkpoint[4]>config_.capacity||words!=headerWords+admission_.recordWords*checkpoint[4])throw std::invalid_argument("GPU tracker checkpoint count invalid");
  if(serial_==UINT64_MAX)throw std::overflow_error("GPU tracker transaction exhausted");
  try{setup({},0,3);
  static_cast<uint32_t*>(metadata_->mapped)[24]=uint32_t(words);
  metadata_->flush(0,statusWords*4);
   std::memcpy(banks_[1-active_]->mapped,checkpoint,words*4);
   banks_[1-active_]->flush(0,words*4);
   commands([&](VkCommandBuffer command){timestamps_->begin(command,5);append(command,10,config_.capacity);append(command,11,1);timestamps_->end(command,5);});
   auto s=status();
   if(s[0])throw std::invalid_argument("GPU tracker checkpoint invariants rejected code="+std::to_string(s[0]));
   if(s[54]!=checkpoint[4])throw std::runtime_error("GPU tracker restore coverage mismatch");
   commands([&](VkCommandBuffer command){timestamps_->begin(command,7);append(command,12,1);timestamps_->end(command,7);});
   s=status();
   if(s[0]||s[58]!=1||s[1]!=1-active_){throw std::runtime_error("GPU tracker restore commit invalid");}
   active_=s[1];
   ++serial_;
   lastCommitted_=false;
   copied_=false;
   std::cerr<<"GPU_TRACKER_RESTORE tracks="<<s[54]<<" device_validated_records="<<s[54]<<" device_commit=1 gpu_validate_ms="<<timestamps_->milliseconds(5)<<" gpu_commit_ms="<<timestamps_->milliseconds(7)<<'\n';
  }catch(const std::invalid_argument&){throw;}catch(...){poisoned_=true;
  throw;}
 }
};
} // namespace gpu_tracker
