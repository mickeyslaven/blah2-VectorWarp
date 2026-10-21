#pragma once
#include "TrackBeforeDetect.private.h"
// Private stateless real-fused-map seam. Host work is setup/control/byte copies.
struct PostprocessConfig {
 bool enabled=false,detection=false,tracker=false;
 double pfa=0,minDoppler=0,centroidDopplerResolution=0;
 int32_t guard=0,train=0,minDelay=0;
 uint16_t centroidDelay=0,centroidDoppler=0;
 bool interpolateDelay=true,interpolateDoppler=true;
 uint32_t integrationCpis=1;
 uint32_t integrationDelayMotionBins=0,integrationDopplerMotionBins=0;
 uint32_t integrationBeamTolerance=UINT32_MAX,beamCount=0;
 uint32_t bearingExclusionBins=0;
 uint32_t bearingMethod=0,bearingReferenceAnchor=0,bearingScanBeams=0,bearingCandidatePeaks=3;
 uint32_t bearingGuardRadius=1,bearingTrainingRadius=3;
 float bearingDiagonalLoading=.01f;
 std::vector<uint32_t> bearingChannelMapping;
 std::vector<std::complex<float>> bearingManifoldResponses;
 std::vector<int32_t> delayAxis;
 std::vector<double> dopplerAxis;
 uint64_t epoch=0;
};
struct PostprocessDetection { double delay=0,doppler=0,snr=0,delayKm=0; };
static_assert(sizeof(PostprocessDetection)==32,"Postprocess record layout changed");
struct PostprocessMetrics { double noisePower=0,maxPower=0;
uint32_t cfarCount=0,centroidCount=0,detectionCount=0; };
struct PostprocessOutputs {
 PostprocessMetrics* metrics=nullptr;
 double* display=nullptr;
 size_t displayCount=0;
 PostprocessDetection* detections=nullptr;
 size_t detectionCapacity=0;
 // Optional qualifier outputs. Fixed capacities are M; no truncation.
 double* levels=nullptr;
 size_t levelCount=0;
 double* unflooredSnr=nullptr;
 size_t unflooredSnrCount=0;
 PostprocessDetection* cfar=nullptr;
 size_t cfarCapacity=0;
 PostprocessDetection* centroid=nullptr;
 size_t centroidCapacity=0;
 uint32_t* decisionFlags=nullptr;
 size_t decisionFlagCount=0; // grid bits: CFAR1, centroid2, final4
 uint32_t* detectionCells=nullptr;
 size_t detectionCellCapacity=0; // final stable original grid identities
};
namespace gpu_postprocess {
enum Counter : uint32_t {error=0,levelCells=1,metricCells=2,displayCells=3,cfarCells=4,
 trainingAdds=5,cfarAccepted=6,centroidExamined=7,centroidKept=8,interpolationExamined=9,
 interpolationRejected=10,detectionCount=11,compactCells=12,recordCells=13,diagnosticCount=14,
 refinedCandidates=15,cfarDiagnosticRecords=16,centroidDiagnosticRecords=17,
 compactTileCounts=23,compactPrefixTiles=24,compactScatterCells=25,compactKinds=26,statusWords=32};
struct Layout {
 uint32_t cells=0,levels=0,powers=0,snr=0,flags=0,keep=0,indices=0,candidates=0,direction=0,records=0,
  metricPartials=0,metricPartialCount=0;
 uint64_t bytes=0;
};
inline Layout layout(uint32_t rows,uint32_t columns){
 const uint64_t count=uint64_t(rows)*columns;
 if(!rows||!columns||rows>65535||columns>65535||count>UINT32_MAX/25)
  throw std::invalid_argument("GPU_POSTPROCESS_UNSUPPORTED index capacity");
 Layout l;
 l.cells=uint32_t(count);
 l.powers=uint32_t(2*count);
 l.snr=uint32_t(4*count);
 l.flags=uint32_t(6*count);
 l.keep=uint32_t(7*count);
 l.indices=uint32_t(8*count);
 l.candidates=uint32_t((9*count+1)&~uint64_t(1));
 l.direction=l.candidates+6*l.cells;
 l.records=l.direction+l.cells;
 l.metricPartials=l.records+8*l.cells;
 l.metricPartialCount=(l.cells+127u)/128u;
 l.bytes=uint64_t(l.metricPartials+4*l.metricPartialCount)*4;
 return l;
}
inline Layout validate(const PostprocessConfig& c,uint32_t rows,uint32_t columns,double fs){
 const auto l=layout(rows,columns);
 if(c.tracker||!c.enabled||!std::isfinite(fs)||fs<=0||fs>UINT32_MAX||fs!=std::floor(fs)||
    c.delayAxis.size()!=columns||c.dopplerAxis.size()!=rows)
  throw std::invalid_argument("GPU_POSTPROCESS_UNSUPPORTED flags, sample rate or axes");
 for(uint32_t j=0;j<columns;++j)if(int64_t(c.delayAxis[j])!=int64_t(c.delayAxis[0])+j)
  throw std::invalid_argument("GPU_POSTPROCESS_UNSUPPORTED non-unit delay axis");
 for(uint32_t i=0;i<rows;++i)if(!std::isfinite(c.dopplerAxis[i])||(i&&!(c.dopplerAxis[i]>c.dopplerAxis[i-1])))
  throw std::invalid_argument("GPU_POSTPROCESS_UNSUPPORTED unordered Doppler axis");
 const bool tbd=c.integrationCpis>2;
 if(tbd&&l.cells>UINT32_MAX/128u)
  throw std::invalid_argument("GPU_POSTPROCESS_UNSUPPORTED TBD workgroup count");
 if((c.integrationCpis!=1&&c.integrationCpis!=2&&c.integrationCpis!=4&&c.integrationCpis!=8)||
    c.integrationDelayMotionBins>(tbd?2u:1u)||c.integrationDopplerMotionBins>(tbd?16u:1u)||
    (!tbd&&c.integrationCpis!=2&&(c.integrationDelayMotionBins||c.integrationDopplerMotionBins))||
    (tbd&&(!c.detection||!c.beamCount))||
    (c.bearingScanBeams&&(!tbd||c.bearingScanBeams<c.beamCount||c.bearingScanBeams>127||
      c.bearingScanBeams%c.beamCount||c.bearingCandidatePeaks!=3||c.bearingMethod>1||
      c.bearingGuardRadius>3||c.bearingTrainingRadius<=c.bearingGuardRadius||
      c.bearingTrainingRadius>6||!std::isfinite(c.bearingDiagonalLoading)||
      c.bearingDiagonalLoading<=0||c.bearingDiagonalLoading>1))||
    (!c.bearingManifoldResponses.empty()&&c.bearingManifoldResponses.size()!=uint64_t(c.bearingScanBeams)*c.bearingChannelMapping.size())||
    (c.integrationBeamTolerance!=UINT32_MAX&&(!c.beamCount||c.integrationBeamTolerance>c.beamCount/2))||
    (c.detection&&(!std::isfinite(c.pfa)||c.pfa<=0||c.pfa>=1||c.guard<0||c.guard>127||
    c.train<=0||c.train>127||c.minDelay<INT8_MIN||c.minDelay>INT8_MAX||
    !std::isfinite(c.minDoppler)||c.minDoppler<0||!std::isfinite(c.centroidDopplerResolution)||c.centroidDopplerResolution<=0||
    !std::isfinite(double(c.centroidDoppler)*c.centroidDopplerResolution))))
  throw std::invalid_argument("GPU_POSTPROCESS_UNSUPPORTED detector configuration");
 const uint32_t detectorPasses=c.integrationCpis>2?track_before_detect::detectorPassCount(c.integrationCpis):1u;
 if(c.detection&&uint64_t(l.cells)*uint32_t(2*c.train)*detectorPasses>UINT32_MAX)
  throw std::invalid_argument("GPU_POSTPROCESS_UNSUPPORTED counter capacity");
 return l;
}
const char* common=R"glsl(#version 450
#extension GL_ARB_gpu_shader_fp64 : require
layout(local_size_x=128) in;
layout(binding=0) buffer Metadata { uint m[]; };
layout(binding=1) readonly buffer Fused { dvec2 fused[]; };
layout(binding=2) buffer Scratch { uint s[]; };
layout(binding=3) buffer Integration { double history[]; };
layout(binding=4) buffer CurrentPower { double currentPower[]; };
layout(binding=5) buffer TbdBanks { double tbdBanks[]; };
layout(binding=6) readonly buffer ChannelMaps { vec2 channelMaps[]; };
layout(binding=7) readonly buffer RefineWeights { vec2 refineWeights[]; };
layout(binding=8) buffer AngleHistory { uint angleHistory[]; };
layout(push_constant) uniform P { uint count; uint a; uint b; uint c; int d; } p;
uint indexOf(){return gl_GlobalInvocationID.x+gl_GlobalInvocationID.y*gl_NumWorkGroups.x*128;}
bool finiteD(double x){return !isnan(x)&&!isinf(x);}
void reject(uint code){atomicCompSwap(m[0],0u,code);}
double readD(uint at){return packDouble2x32(uvec2(m[at],m[at+1]));}
void writeD(uint at,double value){uvec2 bits=unpackDouble2x32(value);m[at]=bits.x;m[at+1]=bits.y;}
double readS(uint at){return packDouble2x32(uvec2(s[at],s[at+1]));}
void writeS(uint at,double value){uvec2 bits=unpackDouble2x32(value);s[at]=bits.x;s[at+1]=bits.y;}
double dopplerAt(uint row){return readD(m[44]+2u*row);}
double delayAt(uint column){return double(int(m[m[45]+column]));}
// Exact accepted FP64 logarithm; no float conversion or host callback.
double positiveLog10(double x){
 if(!finiteD(x)||!(x>0.0LF)){reject(101u);return 0.0LF;}
 uvec2 bits=unpackDouble2x32(x);int adjust=0;
 if((bits.y&0x7ff00000u)==0u){x=x*4503599627370496.0LF;bits=unpackDouble2x32(x);adjust=-52;}
 int exponent=int((bits.y>>20)&2047u)-1023+adjust;
 double mantissa=packDouble2x32(uvec2(bits.x,(bits.y&0xfffffu)|0x3ff00000u));
 precise double t=(mantissa-1.0LF)/(mantissa+1.0LF);
 precise double square=t*t,term=t,sum=0.0LF;
 for(uint k=0u;k<16u;++k){sum=sum+term/double(2u*k+1u);term=term*square;}
 precise double high=double(exponent)*0.69314718055994528622676398299518LF;
 precise double low=double(exponent)*2.319046813846299558417771099e-17LF;
 precise double value=((high+2.0LF*sum)+low)*0.43429448190325182765112891891661LF;
 return value;
}
)glsl";
const char* levelsBody=R"glsl(
void main(){uint i=indexOf();if(i>=m[34])return;dvec2 value=fused[i];
 if(!finiteD(value.x)||!finiteD(value.y)){reject(201u);return;}
 uint beam=uint(value.y);bool validBeam=m[75]==0u?value.y==0.0LF:
  (value.y>=0.0LF&&value.y<double(m[75])&&double(beam)==value.y);
 if(i==0u){m[18]=m[72];m[19]=m[73];m[20]=m[49];m[21]=m[32];m[22]=m[33];m[27]=m[74];m[28]=m[78]!=0u?(m[79]+1u>=m[74]?1u:0u):(m[74]==2u&&finiteD(history[i])?1u:0u);m[29]=m[76]|(m[93]<<16u);m[30]=m[77];m[31]=m[75];}
 if(value.x<0.0LF||!validBeam){reject(201u);return;}
 precise double current=value.x*value.x;currentPower[i]=current;s[m[39]+i]=m[75]==0u?0u:(beam+1u)<<8u;
 if(m[78]!=0u){uint historyAt=(m[79]%m[74])*m[34]+i;history[historyAt]=current;
  if(m[77]!=0xffffffffu)angleHistory[historyAt]=m[75]==0u?0u:beam+1u;}
 double prior=history[i];
 bool historyReady=m[74]==2u&&finiteD(history[0]);
 if(historyReady&&(m[76]!=0u||m[93]!=0u)){
  uint row=i/m[33],column=i%m[33];prior=-1.0LF;
  for(int dr=-int(m[93]);dr<=int(m[93]);++dr)for(int dc=-int(m[76]);dc<=int(m[76]);++dc){
   int rr=int(row)+dr,cc=int(column)+dc;if(rr<0||cc<0||rr>=int(m[32])||cc>=int(m[33]))continue;
   uint at=uint(rr)*m[33]+uint(cc);double candidate=history[at];if(!finiteD(candidate))continue;
   if(candidate>prior)prior=candidate;
  }
 }
 precise double power=m[49]!=0u?(historyReady&&finiteD(prior)&&prior>=0.0LF?0.5LF*(current+prior):current):0.0LF;
 double amplitude=sqrt(max(power,0.0LF));
 double level=amplitude<=1e-30LF?-300.0LF:10.0LF*positiveLog10(amplitude);
 if(!finiteD(level)||!finiteD(power)){reject(202u);return;}
 writeS(m[36]+2u*i,level);writeS(m[37]+2u*i,power);atomicAdd(m[1],1u);
}
)glsl";
const char* tbdBody=R"glsl(
bool angleCompatible(uint left,uint right){if(m[77]==0xffffffffu||m[75]==0u||left==0u||right==0u)return true;
 uint a=left-1u,b=right-1u,delta=a>b?a-b:b-a;return min(delta,m[75]-delta)<=m[77];}
shared double pathBest2[128],pathBest4[128],pathBest8[128];
void main(){uint i=gl_WorkGroupID.x+gl_WorkGroupID.y*gl_NumWorkGroups.x,lane=gl_LocalInvocationID.x;if(i>=m[34]||m[0]!=0u||m[78]==0u)return;
 uint row=i/m[33],column=i%m[33],available=m[79]+1u,limit=min(available,m[74]),currentBeam=uint(fused[i].y)+1u;
 if(p.a==0u){
  if(lane!=0u)return;
  if(available>=2u){uint priorSlot=(m[79]+m[74]-1u)%m[74];
   uint priorAt=priorSlot*m[34]+i;double baseline=(m[77]==0xffffffffu||angleCompatible(currentBeam,angleHistory[priorAt]))?
    0.5LF*(currentPower[i]+history[priorAt]):-1.0LF;
   if(baseline>=0.0LF&&!finiteD(baseline)){reject(208u);return;}tbdBanks[i]=baseline;
  }else tbdBanks[i]=packDouble2x32(uvec2(0u,0x7ff80000u));
  for(uint bank=0u;bank<m[80];++bank){uint length=2u<<bank;
   tbdBanks[(bank+1u)*m[34]+i]=available>=length?0.0LF:packDouble2x32(uvec2(0u,0x7ff80000u));}
  return;
 }
 // One workgroup owns one map cell. Its lanes evaluate distinct motion paths
 // concurrently, then lane zero reduces exact 2/4/8-CPI maxima in shared
 // memory. Large configured envelopes use ordered 128-path chunks.
 if(p.a==1u){uint delayWidth=2u*m[76]+1u,total=(2u*m[93]+1u)*delayWidth,path=p.b+lane;
  double best2=-1.0LF,best4=-1.0LF,best8=-1.0LF;
  if(path<total){int dr=int(path/delayWidth)-int(m[93]),dc=int(path%delayWidth)-int(m[76]);
   precise double sum=0.0LF;bool compatible=true;
   for(uint age=0u;age<limit;++age){int rr=int(row)-dr*int(age),cc=int(column)-dc*int(age);
    if(rr<0||cc<0||rr>=int(m[32])||cc>=int(m[33]))break;
    uint slot=(m[79]+m[74]-(age%m[74]))%m[74],at=slot*m[34]+uint(rr)*m[33]+uint(cc);
    double value=history[at];compatible=compatible&&(m[77]==0xffffffffu||angleCompatible(currentBeam,angleHistory[at]));
    if(!compatible||!finiteD(value)||value<0.0LF)break;sum+=value;uint length=age+1u;
    if(length==2u)best2=sum/2.0LF;
    else if(length==4u&&m[74]>=4u)best4=sum/4.0LF;
    else if(length==8u&&m[74]>=8u)best8=sum/8.0LF;
   }
  }
  pathBest2[lane]=best2;pathBest4[lane]=best4;pathBest8[lane]=best8;barrier();
  if(lane==0u){uint count=min(128u,total-p.b);double chunkBest[3]=double[3](-1.0LF,-1.0LF,-1.0LF);
   for(uint j=0u;j<count;++j){chunkBest[0]=max(chunkBest[0],pathBest2[j]);chunkBest[1]=max(chunkBest[1],pathBest4[j]);chunkBest[2]=max(chunkBest[2],pathBest8[j]);}
   for(uint bank=0u;bank<m[80];++bank){double candidate=chunkBest[bank],prior=tbdBanks[(bank+1u)*m[34]+i];
    if(candidate>prior)tbdBanks[(bank+1u)*m[34]+i]=candidate;}
  }
  return;
 }
 if(lane!=0u)return;
 double selected=available<2u?currentPower[i]:max(0.0LF,tbdBanks[i]);
 for(uint bank=0u;bank<m[80];++bank){uint length=2u<<bank;if(available>=length){
  double best=tbdBanks[(bank+1u)*m[34]+i];if(!finiteD(best)){reject(208u);return;}selected=max(selected,best);}}
 double amplitude=sqrt(max(selected,0.0LF));double level=amplitude<=1e-30LF?-300.0LF:10.0LF*positiveLog10(amplitude);
 if(!finiteD(level)||!finiteD(selected)){reject(209u);return;}
 writeS(m[36]+2u*i,level);writeS(m[37]+2u*i,selected);
}
)glsl";
const char* advanceBody=R"glsl(
void main(){if(indexOf()==0u&&m[78]!=0u&&m[0]==0u)m[79]=min(m[79]+1u,0xfffffffeu);}
)glsl";
const char* metricsBody=R"glsl(
shared double metricTile[128];
void main(){
 // Preserve ordered FP64 additions inside each contiguous tile while allowing
 // every tile to be reduced independently. A short ordered second pass below
 // combines only ceil(cells/128) partials instead of serializing the full map.
 if(m[0]!=0u)return;uint lane=gl_LocalInvocationID.x,first=indexOf()-lane,tile=first/128u;
 uint count=first>=m[34]?0u:min(128u,m[34]-first);
 metricTile[lane]=lane<count?readS(m[36]+2u*(first+lane)):0.0LF;barrier();
 if(lane!=0u||count==0u)return;precise double total=0.0LF;double maximum=metricTile[0];
 for(uint j=0u;j<count;++j){double value=metricTile[j];total=total+value;maximum=maximum<value?value:maximum;}
 uint at=m[91]+4u*tile;writeS(at,total);writeS(at+2u,maximum);atomicAdd(m[2],count);
}
)glsl";
const char* metricsFinishBody=R"glsl(
void main(){if(indexOf()!=0u||m[0]!=0u)return;precise double total=0.0LF;double maximum=readS(m[91]+2u);
 for(uint tile=0u;tile<m[92];++tile){uint at=m[91]+4u*tile;double subtotal=readS(at),peak=readS(at+2u);
  total=total+subtotal;maximum=maximum<peak?peak:maximum;}
 precise double noise=total/(double(m[32])*double(m[33]));precise double peak=maximum-noise;
 if(!finiteD(total)||!finiteD(noise)||!finiteD(peak)){reject(203u);return;}
 writeD(64u,noise);writeD(66u,peak);
}
)glsl";
const char* displayBody=R"glsl(
void main(){uint i=indexOf();if(i>=m[34]||m[0]!=0u)return;double noise=readD(64u);
 if(p.a==0u||p.a==2u){
 // Qualifier-only mode2 preserves unusable-neighbor semantics without running
 // the whole-frame finite-map admission. Production always uses mode0 here.
  if(p.a==2u&&(!(fused[i].x>0.0LF)||!finiteD(fused[i].x)||!finiteD(fused[i].y)||!finiteD(noise))){writeS(m[38]+2u*i,packDouble2x32(uvec2(0u,0x7ff80000u)));return;}
  // levels is the current amplitude map on the legacy path and the strongest
  // motion-integrated bank on the TBD path.  Using it here keeps centroiding
  // and interpolation on the same surface that CFAR actually qualified.
  precise double snr=readS(m[36]+2u*i)-noise;
  if(!finiteD(snr)){reject(204u);return;}writeS(m[38]+2u*i,snr);}
 else {if(m[78]!=0u){double amplitude=sqrt(max(currentPower[i],0.0LF));
   precise double level=amplitude<=1e-30LF?-300.0LF:10.0LF*positiveLog10(amplitude);
   precise double value=level-noise;if(!finiteD(value)){reject(204u);return;}writeS(m[37]+2u*i,value);}
  else {precise double value=readS(m[36]+2u*i)-noise;if(!finiteD(value)){reject(204u);return;}writeS(m[37]+2u*i,value);}
  atomicAdd(m[3],1u);}
}
)glsl";
const char* cfarBody=R"glsl(
double detectorPower(uint i){return p.a==0u?readS(m[37]+2u*i):tbdBanks[(p.a-1u)*m[34]+i];}
void main(){uint i=indexOf();if(i>=m[34]||m[0]!=0u)return;if(p.b!=0u){s[m[39]+i]=s[m[39]+i]&0xffffff00u;s[m[40]+i]=0u;}
 uint row=i/m[33],column=i%m[33];atomicAdd(m[4],1u);
 if(abs(dopplerAt(row))<readD(52u)||delayAt(column)<double(int(m[51])))return;
 uint at=m[46]+m[84]*column,n=m[at+4u];if(n==0u)return;atomicAdd(m[5],n);double cut=detectorPower(i);if(!finiteD(cut))return;
 precise double train=0.0LF;for(uint j=m[at];j<m[at+1u];++j)train=train+detectorPower(row*m[33]+j);
 for(uint j=m[at+2u];j<m[at+3u];++j)train=train+detectorPower(row*m[33]+j);
 train=train/double(n);uint alphaIndex=p.a==0u?0u:p.a-1u;precise double threshold=readD(at+6u+2u*alphaIndex)*train;
 if(!finiteD(train)||!finiteD(threshold)){reject(205u);return;}
 if(p.a==0u&&m[74]==2u&&m[28]==0u)return;
 if(cut>threshold){uint previous=atomicOr(s[m[39]+i],1u);if((previous&1u)==0u)atomicAdd(m[6],1u);
  uint record=m[42]+6u*i;writeS(record,delayAt(column));writeS(record+2u,dopplerAt(row));writeS(record+4u,readS(m[38]+2u*i));}
}
)glsl";
const char* refineBody=R"glsl(
vec2 cmul(vec2 a,vec2 b){return vec2(a.x*b.x-a.y*b.y,a.x*b.y+a.y*b.x);}
vec2 cdiv(vec2 a,vec2 b){float d=dot(b,b);return vec2(a.x*b.x+a.y*b.y,a.y*b.x-a.x*b.y)/d;}
float beamScore(uint beam,uint channels,uint cell,uint shifted,bool adaptive,vec2 inverse[64]){
 vec2 values[8],filtered[8];for(uint c=0u;c<channels;++c)values[c]=channelMaps[c*m[34]+cell%m[33]*m[32]+shifted];
 if(adaptive){for(uint a=0u;a<channels;++a){filtered[a]=vec2(0.0);for(uint b=0u;b<channels;++b)filtered[a]+=cmul(inverse[a*8u+b],values[b]);}}
 vec2 numerator=vec2(0.0);for(uint c=0u;c<channels;++c)numerator+=cmul(refineWeights[beam*channels+c],adaptive?filtered[c]:values[c]);
 if(!adaptive)return dot(numerator,numerator);
 vec2 denominator=vec2(0.0);for(uint a=0u;a<channels;++a){vec2 z=vec2(0.0);
  for(uint b=0u;b<channels;++b){vec2 steering=refineWeights[beam*channels+b];steering.y=-steering.y;z+=cmul(inverse[a*8u+b],steering);}
  denominator+=cmul(refineWeights[beam*channels+a],z);}
 return dot(numerator,numerator)/max(abs(denominator.x),1e-12);
}
bool excluded(uint beam,uint selected,uint fine,uint radius){uint d=beam>selected?beam-selected:selected-beam;return min(d,fine-d)<=radius;}
void main(){uint i=indexOf();if(i>=m[34]||m[0]!=0u||m[78]==0u||(s[m[39]+i]&1u)==0u)return;
 uint row=i/m[33],column=i%m[33],factor=m[81],channels=m[82],fine=m[83];
 uint shifted=(row+m[32]/2u+1u)%m[32],bestBeam=0u,secondBeam=0u,thirdBeam=0u;float best=-1.0,second=-1.0,third=-1.0;
 vec2 covariance[64],inverse[64];for(uint k=0u;k<64u;++k){covariance[k]=vec2(0.0);inverse[k]=vec2(0.0);}
 bool adaptive=m[87]==1u;uint samples=0u;
 if(adaptive){int training=int(m[89]),guard=int(m[88]);
  for(int dr=-training;dr<=training;++dr)for(int dc=-training;dc<=training;++dc){if(abs(dr)<=guard&&abs(dc)<=guard)continue;
   int rr=int(row)+dr,cc=int(column)+dc;if(rr<0||cc<0||rr>=int(m[32])||cc>=int(m[33]))continue;
   uint sh=(uint(rr)+m[32]/2u+1u)%m[32];vec2 v[8];for(uint c=0u;c<channels;++c)v[c]=channelMaps[c*m[34]+uint(cc)*m[32]+sh];
   for(uint a=0u;a<channels;++a)for(uint b=0u;b<channels;++b)covariance[a*8u+b]+=vec2(v[a].x*v[b].x+v[a].y*v[b].y,v[a].y*v[b].x-v[a].x*v[b].y);samples++;}
  float trace=0.0;for(uint c=0u;c<channels;++c)trace+=covariance[c*8u+c].x;
  float load=max(trace/max(float(samples*channels),1.0)*uintBitsToFloat(m[90]),1e-12);
  for(uint a=0u;a<channels;++a){for(uint b=0u;b<channels;++b)covariance[a*8u+b]/=max(float(samples),1.0);covariance[a*8u+a].x+=load;inverse[a*8u+a]=vec2(1.0,0.0);}
  for(uint pivot=0u;pivot<channels;++pivot){uint choice=pivot;float magnitude=dot(covariance[pivot*8u+pivot],covariance[pivot*8u+pivot]);
   for(uint r=pivot+1u;r<channels;++r){float candidate=dot(covariance[r*8u+pivot],covariance[r*8u+pivot]);if(candidate>magnitude){magnitude=candidate;choice=r;}}
   if(!(magnitude>1e-20)){adaptive=false;break;}if(choice!=pivot)for(uint c=0u;c<channels;++c){vec2 q=covariance[pivot*8u+c];covariance[pivot*8u+c]=covariance[choice*8u+c];covariance[choice*8u+c]=q;q=inverse[pivot*8u+c];inverse[pivot*8u+c]=inverse[choice*8u+c];inverse[choice*8u+c]=q;}
   vec2 divisor=covariance[pivot*8u+pivot];for(uint c=0u;c<channels;++c){covariance[pivot*8u+c]=cdiv(covariance[pivot*8u+c],divisor);inverse[pivot*8u+c]=cdiv(inverse[pivot*8u+c],divisor);}
   for(uint r=0u;r<channels;++r)if(r!=pivot){vec2 multiplier=covariance[r*8u+pivot];for(uint c=0u;c<channels;++c){covariance[r*8u+c]-=cmul(multiplier,covariance[pivot*8u+c]);inverse[r*8u+c]-=cmul(multiplier,inverse[pivot*8u+c]);}}
  }
 }
 for(uint beam=0u;beam<fine;++beam){float power=beamScore(beam,channels,i,shifted,adaptive,inverse);if(power>best){best=power;bestBeam=beam;}}
 for(uint beam=0u;beam<fine;++beam){if(excluded(beam,bestBeam,fine,m[85]))continue;float power=beamScore(beam,channels,i,shifted,adaptive,inverse);if(power>second){second=power;secondBeam=beam;}}
 for(uint beam=0u;beam<fine;++beam){if(excluded(beam,bestBeam,fine,m[85])||excluded(beam,secondBeam,fine,m[85]))continue;float power=beamScore(beam,channels,i,shifted,adaptive,inverse);if(power>third){third=power;thirdBeam=beam;}}
 double refined=double(sqrt(max(best,0.0))),base=fused[i].x;uint record=m[42]+6u*i;
 if(m[87]==0u&&refined>base&&base>0.0LF){double gain=20.0LF*positiveLog10(refined/base);double snr=readS(record+4u)+gain;
  if(!finiteD(snr)){reject(210u);return;}writeS(record+4u,snr);writeS(m[38]+2u*i,readS(m[38]+2u*i)+gain);}
 uint ratio2=second>0.0?uint(clamp(round(31.0*second/best),0.0,31.0)):0u;
 uint ratio3=third>0.0?uint(clamp(round(31.0*third/best),0.0,31.0)):0u;
 // Versioned per-frame three-lobe evidence. Detection/tracker association
 // derives its coarse beam from the primary; legacy public track JSON ignores it.
 s[m[86]+i]=0x80000000u|(bestBeam+1u)|((secondBeam+1u)<<7u)|((thirdBeam+1u)<<14u)|(ratio2<<21u)|(ratio3<<26u);
 atomicAdd(m[15],1u);
}
)glsl";
const char* centroidBody=R"glsl(
bool finiteRecord(uint at){return finiteD(readS(at))&&finiteD(readS(at+2u))&&finiteD(readS(at+4u));}
void main(){uint i=indexOf();if(i>=p.count||m[0]!=0u)return;
 // p.a=1 is a direct-record qualifier binding; production always uses zero.
 if(p.a==0u&&(s[m[39]+i]&1u)==0u)return;s[m[40]+i]=0u;uint record=m[42]+6u*i;
 if(!finiteRecord(record))return;atomicAdd(m[7],1u);double centreSnr=readS(record+4u);bool retain=true;
 if(p.a==0u){uint row=i/m[33],column=i%m[33],rb=m[47]+2u*row,cb=m[48]+2u*column;
  for(uint r=m[rb];r<m[rb+1u]&&retain;++r)for(uint c=m[cb];c<m[cb+1u];++c){
   uint other=r*m[33]+c;if(other!=i&&(s[m[39]+other]&1u)!=0u&&centreSnr<readS(m[38]+2u*other)){retain=false;break;}}}
 else {double delay=readS(record),doppler=readS(record+2u);
  precise double lowDelay=delay-readD(68u),highDelay=delay+readD(68u),lowDoppler=doppler-readD(70u),highDoppler=doppler+readD(70u);
  for(uint j=0u;j<p.count;++j){uint other=m[42]+6u*j;if(i==j||!finiteRecord(other))continue;
   double d=readS(other),v=readS(other+2u);if(d>lowDelay&&d<highDelay&&v>lowDoppler&&v<highDoppler&&centreSnr<readS(other+4u)){retain=false;break;}}}
 if(retain){s[m[40]+i]=1u;atomicAdd(m[8],1u);}
}
)glsl";
const char* interpolationBody=R"glsl(
bool usable(uint i){return fused[i].x>0.0LF&&finiteD(fused[i].x)&&finiteD(fused[i].y)&&finiteD(readS(m[38]+2u*i));}
bool fit(double left,double centre,double right,out double offset,out double peak){
 precise double denominator=2.0LF*(left-2.0LF*centre+right);
 if(!finiteD(denominator)||abs(denominator)<1e-12LF)return false;
 precise double fittedOffset=(left-right)/denominator;offset=fittedOffset;if(!finiteD(offset)||abs(offset)>1.0LF)return false;
 precise double fittedPeak=centre-((left-right)*offset/4.0LF);peak=fittedPeak;return finiteD(peak);
}
void main(){uint i=indexOf();if(i>=p.count||m[0]!=0u||s[m[40]+i]==0u)return;
 uint record=m[42]+6u*i;double delay=readS(record),doppler=readS(record+2u),snr=readS(record+4u);
 if(!finiteD(delay)||!finiteD(doppler)||!finiteD(snr)){s[m[40]+i]=0u;return;}
 atomicAdd(m[9],1u);s[m[39]+i]=s[m[39]+i]|2u;
 uint row=i/m[33],column=i%m[33];bool known=true;
 if(p.a!=0u){row=m[32];column=m[33];for(uint r=0u;r<m[32];++r)if(doppler==dopplerAt(r)){row=r;break;}
  for(uint c=0u;c<m[33];++c)if(delay==delayAt(c)){column=c;break;}known=row<m[32]&&column<m[33];}
 uint cell=known?row*m[33]+column:0u;
 precise double fittedDelay=delay,fittedDoppler=doppler,delayPeak=snr,dopplerPeak=snr;bool nonPeak=false;
 if(known&&(m[50]&1u)!=0u&&column>0u&&column+1u<m[33]&&usable(cell-1u)&&usable(cell)&&usable(cell+1u)){
  double left=readS(m[38]+2u*(cell-1u)),centre=readS(m[38]+2u*cell),right=readS(m[38]+2u*(cell+1u)),offset,peak;
  if(centre<left||centre<right)nonPeak=true;else if(fit(left,centre,right,offset,peak)){fittedDelay=delay+offset;delayPeak=peak;}}
 if(!nonPeak&&known&&(m[50]&2u)!=0u&&row>0u&&row+1u<m[32]&&usable(cell-m[33])&&usable(cell)&&usable(cell+m[33])){
  double left=readS(m[38]+2u*(cell-m[33])),centre=readS(m[38]+2u*cell),right=readS(m[38]+2u*(cell+m[33])),offset,peak;
  if(centre<left||centre<right)nonPeak=true;else if(fit(left,centre,right,offset,peak)){fittedDoppler=doppler+(dopplerAt(row+1u)-doppler)*offset;dopplerPeak=peak;}}
 if(nonPeak){s[m[40]+i]=0u;atomicAdd(m[10],1u);return;}
 double finalSnr=max(max(delayPeak,dopplerPeak),snr);
 if(!finiteD(fittedDelay)||!finiteD(fittedDoppler)||!finiteD(finalSnr)){reject(206u);return;}
 uint at=m[42]+6u*i;writeS(at,fittedDelay);writeS(at+2u,fittedDoppler);writeS(at+4u,finalSnr);s[m[39]+i]=s[m[39]+i]|4u;
}
)glsl";
// Integer-only stable compaction. The record region is dead until recordsBody;
// its first ceil(M/128) words hold tile counts, then exclusive tile offsets.
const char* compactBody=R"glsl(
shared uint retained[128];
void main(){uint i=indexOf(),lane=gl_LocalInvocationID.x,tile=i/128u,tiles=(m[34]+127u)/128u;
 if(tile>=tiles||m[0]!=0u)return;
 bool retain=false;if(i<m[34])retain=p.a==0u?s[m[40]+i]!=0u:(s[m[39]+i]&p.a)!=0u;
 retained[lane]=retain?1u:0u;barrier();
 for(uint width=64u;width>0u;width>>=1u){if(lane<width)retained[lane]+=retained[lane+width];barrier();}
 if(lane==0u){s[m[43]+tile]=retained[0];atomicAdd(m[23],1u);}
}
)glsl";
const char* compactPrefixBody=R"glsl(
void main(){if(indexOf()!=0u||m[0]!=0u)return;uint count=0u,tiles=(m[34]+127u)/128u;
 for(uint tile=0u;tile<tiles;++tile){uint amount=s[m[43]+tile];s[m[43]+tile]=count;count+=amount;}
 if(p.a==0u){m[11]=count;m[12]=m[34];}else m[14]=count;
 atomicAdd(m[24],tiles);atomicOr(m[26],1u<<p.a);
}
)glsl";
const char* compactScatterBody=R"glsl(
shared uint prefix[128];
void main(){uint i=indexOf(),lane=gl_LocalInvocationID.x,tile=i/128u,tiles=(m[34]+127u)/128u;
 if(tile>=tiles||m[0]!=0u)return;
 bool retain=false;if(i<m[34])retain=p.a==0u?s[m[40]+i]!=0u:(s[m[39]+i]&p.a)!=0u;
 prefix[lane]=retain?1u:0u;barrier();
 for(uint distance=1u;distance<128u;distance<<=1u){uint addend=lane>=distance?prefix[lane-distance]:0u;
  barrier();prefix[lane]+=addend;barrier();}
 if(retain)s[m[41]+s[m[43]+tile]+prefix[lane]-1u]=i;
 if(lane==0u)atomicAdd(m[25],min(128u,m[34]-tile*128u));
}
)glsl";
const char* recordsBody=R"glsl(
void main(){uint i=indexOf();if(i>=(p.a==0u?m[11]:m[14])||m[0]!=0u)return;uint cell=s[m[41]+i],source=m[42]+6u*cell,target=m[43]+8u*i;
 double delay=p.a==0u?readS(source):delayAt(cell%m[33]),doppler=p.a==0u?readS(source+2u):dopplerAt(cell/m[33]),snr=p.a==0u?readS(source+4u):readS(m[38]+2u*cell);
 precise double km=1.0LF*delay*(299792458.0LF/double(m[60]))/1000.0LF;
 if(!finiteD(km)){reject(207u);return;}
 writeS(target,delay);writeS(target+2u,doppler);writeS(target+4u,snr);writeS(target+6u,km);
 atomicAdd(m[p.a==0u?13u:(p.a==1u?16u:17u)],1u);
}
)glsl";
inline std::vector<std::pair<std::string,std::string>> shaderSources(){
 const std::pair<const char*,const char*> bodies[]={{"levels",levelsBody},{"metrics",metricsBody},{"display",displayBody},{"cfar",cfarBody},{"centroid",centroidBody},{"interpolation",interpolationBody},{"compact",compactBody},{"records",recordsBody},{"compact_prefix",compactPrefixBody},{"compact_scatter",compactScatterBody},{"tbd",tbdBody},{"advance",advanceBody},{"refine",refineBody},{"metrics_finish",metricsFinishBody}};
 std::vector<std::pair<std::string,std::string>> result;
 for(const auto& b:bodies)result.emplace_back(b.first,std::string(common)+b.second);
 return result;
}
// Shared by the production seam and test-local binding of these same kernels.
inline void appendStableCompaction(VkCommandBuffer command,const std::vector<std::unique_ptr<Kernel>>& kernels,uint32_t cells,uint32_t kind){
 kernels[6]->append(command,{cells,kind,0,0,0});
 barrier(command);
 kernels[8]->append(command,{1,kind,0,0,0});
 barrier(command);
 kernels[9]->append(command,{cells,kind,0,0,0});
 barrier(command);
}
class Stage {
 std::array<uint32_t,32> evidenceStatus_{};
 Context& context_;
 const PostprocessConfig config_;
 const uint32_t rows_,columns_;
 const Layout layout_;
 Buffer& fused_;
 Buffer& scratch_;
 Buffer& staging_;
 std::unique_ptr<Buffer> metadata_;
 std::unique_ptr<Buffer> integration_;
 std::unique_ptr<Buffer> currentPower_,angleHistory_;
 std::unique_ptr<Buffer> tbdBanks_;
 std::unique_ptr<gpu_native::Timestamps> timestamps_;
 std::vector<std::unique_ptr<Kernel>> kernels_;
 uint32_t expectedTrainingAdds_=0;
 static void putD(uint32_t* words,uint32_t at,double value){std::memcpy(words+at,&value,8);}
 static double getD(const uint32_t* words,uint32_t at){double value;
 std::memcpy(&value,words+at,8);
 return value;}
 static void hostRead(VkCommandBuffer command){VkMemoryBarrier b{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
 b.srcAccessMask=VK_ACCESS_MEMORY_WRITE_BIT;
 b.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
  vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&b,0,nullptr,0,nullptr);}
 void copy(uint64_t offset,void* target,uint64_t bytes){
  for(uint64_t done=0;done<bytes;){const uint64_t chunk=std::min(staging_.bytes,bytes-done);
   gpu_reference::CommandOwner command(context_);
   command.allocate();
   VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
   check(vkBeginCommandBuffer(command.command,&begin),"postprocess readback recording");
   barrier(command.command);
   VkBufferCopy region{offset+done,0,chunk};
   vkCmdCopyBuffer(command.command,scratch_.handle,staging_.handle,1,&region);
   barrier(command.command);
   hostRead(command.command);
   check(vkEndCommandBuffer(command.command),"postprocess readback recording");
   VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
   submit.commandBufferCount=1;
   submit.pCommandBuffers=&command.command;
   check(gpu_host::call(context_.hostTiming,"vkQueueSubmit",0,[&]{return vkQueueSubmit(context_.queue,1,&submit,context_.fence);}),"postprocess readback submit");
   check(gpu_host::call(context_.hostTiming,"vkWaitForFences",0,[&]{return vkWaitForFences(context_.device,1,&context_.fence,VK_TRUE,UINT64_MAX);}),"postprocess readback fence");
   check(gpu_host::call(context_.hostTiming,"vkResetFences",0,[&]{return vkResetFences(context_.device,1,&context_.fence);}),"postprocess readback fence reset");
   staging_.invalidate(0,chunk);
   std::memcpy(static_cast<unsigned char*>(target)+done,staging_.mapped,size_t(chunk));
   done+=chunk;
  }
 }
public:
 const auto& evidenceStatus()const{return evidenceStatus_;}
 const gpu_native::Timestamps& evidenceTimes()const{return *timestamps_;}
 Stage(Context& context,const PostprocessConfig& config,uint32_t rows,uint32_t columns,uint32_t fs,Buffer& fused,Buffer& scratch,Buffer& staging,
  Buffer* channelMaps=nullptr,Buffer* refinementWeights=nullptr,uint32_t channels=0,uint32_t refinementBeams=0)
  :context_(context),config_(config),rows_(rows),columns_(columns),layout_(validate(config,rows,columns,fs)),fused_(fused),scratch_(scratch),staging_(staging){
  const bool tbd=config.integrationCpis>2;
  const uint32_t banks=tbd?track_before_detect::bankCount(config.integrationCpis):0;
  const uint32_t detectorPasses=tbd?track_before_detect::detectorPassCount(config.integrationCpis):1u;
  if(tbd&&(!channelMaps||!refinementWeights||!channels||
    refinementBeams!=(config.bearingScanBeams?config.bearingScanBeams:config.beamCount*4u)))
   throw std::invalid_argument("GPU_POSTPROCESS_UNSUPPORTED track-before-detect refinement binding");
  if(config.bearingScanBeams){std::vector<bool> used(channels,false);
   if(config.bearingChannelMapping.size()!=channels)throw std::invalid_argument("GPU_POSTPROCESS_UNSUPPORTED bearing channel mapping");
   for(auto channel:config.bearingChannelMapping){if(channel>=channels||used[channel])throw std::invalid_argument("GPU_POSTPROCESS_UNSUPPORTED bearing channel mapping");used[channel]=true;}}
  if(fused.bytes<uint64_t(layout_.cells)*16||scratch.bytes<layout_.bytes||staging.bytes<8||!staging.mapped||staging.bytes%8||
     fused.handle==scratch.handle||fused.handle==staging.handle||scratch.handle==staging.handle)
   throw std::invalid_argument("GPU_POSTPROCESS_UNSUPPORTED scratch/staging binding");
  const uint32_t doppler=96,delay=doppler+2*rows,windows=(delay+columns+1)&~1u,windowWords=tbd?14u:8u,
   rowBounds=windows+windowWords*columns,colBounds=rowBounds+2*rows;
  std::vector<uint32_t> words(colBounds+2*columns,0);
  words[32]=rows;
  words[33]=columns;
  words[34]=layout_.cells;
  words[36]=layout_.levels;
  words[37]=layout_.powers;
  words[38]=layout_.snr;
  words[39]=layout_.flags;
  words[40]=layout_.keep;
  words[41]=layout_.indices;
  words[42]=layout_.candidates;
  words[43]=layout_.records;
  words[86]=layout_.direction;
  words[44]=doppler;
  words[45]=delay;
  words[46]=windows;
  words[47]=rowBounds;
  words[48]=colBounds;
  words[49]=config.detection?1:0;
  words[50]=(config.interpolateDelay?1u:0u)|(config.interpolateDoppler?2u:0u);
  words[51]=uint32_t(config.minDelay);
  putD(words.data(),52,config.minDoppler);
  words[60]=fs;
  words[72]=uint32_t(config.epoch);
  words[73]=uint32_t(config.epoch>>32);
  words[74]=config.integrationCpis;
  words[75]=config.beamCount;
  words[76]=config.integrationDelayMotionBins;
  words[77]=config.integrationBeamTolerance;
  words[78]=tbd?1u:0u;
  words[79]=0;
  words[80]=banks;
  words[81]=tbd?(config.bearingScanBeams?config.bearingScanBeams/config.beamCount:4u):0u;
  words[82]=channels;
  words[83]=refinementBeams;
  words[84]=windowWords;
  words[85]=config.bearingExclusionBins;
  words[87]=config.bearingMethod;
  words[88]=config.bearingGuardRadius;
  words[89]=config.bearingTrainingRadius;
  words[91]=layout_.metricPartials;
  words[92]=layout_.metricPartialCount;
  words[93]=config.integrationDopplerMotionBins;
  std::memcpy(words.data()+90,&config.bearingDiagonalLoading,4);
  putD(words.data(),68,double(config.centroidDelay));
  putD(words.data(),70,config.centroidDoppler*config.centroidDopplerResolution);
  for(uint32_t r=0;r<rows;++r)putD(words.data(),doppler+2*r,config.dopplerAxis[r]);
  for(uint32_t j=0;j<columns;++j)words[delay+j]=uint32_t(config.delayAxis[j]);
  if(config.detection){
   for(uint32_t j=0;j<columns;++j){const int column=int(j),n=int(columns);
   uint32_t at=windows+windowWords*j;
    const int lb=std::max(0,column-config.guard-config.train),le=std::max(0,column-config.guard),rb=std::min(n,column+config.guard+1),re=std::min(n,column+config.guard+config.train+1),count=le-lb+re-rb;
    words[at]=uint32_t(lb);
    words[at+1]=uint32_t(le);
    words[at+2]=uint32_t(rb);
    words[at+3]=uint32_t(re);
    words[at+4]=uint32_t(count);
    for(uint32_t pass=0;pass<detectorPasses;++pass){const uint32_t cpis=tbd?(pass?track_before_detect::bankLength(pass-1u):2u):config.integrationCpis;
     const double pfaForPass=tbd?track_before_detect::passPfa(config.pfa,config.integrationCpis,
      config.integrationDelayMotionBins,config.integrationDopplerMotionBins,pass):config.pfa;
     const double alpha=count?(cpis==1?count*(std::pow(pfaForPass,-1.0/count)-1):
      track_before_detect::cfarAlpha(pfaForPass,uint32_t(count),cpis)):0;
     if(!std::isfinite(alpha)||alpha<0)throw std::invalid_argument("GPU_POSTPROCESS_UNSUPPORTED CFAR alpha");
     putD(words.data(),at+6+2*pass,alpha);
    }
    if(config.delayAxis[j]>=config.minDelay)for(uint32_t r=0;r<rows;++r)if(std::abs(config.dopplerAxis[r])>=config.minDoppler)expectedTrainingAdds_+=uint32_t(count);
    const double low=double(config.delayAxis[j])-double(config.centroidDelay),high=double(config.delayAxis[j])+double(config.centroidDelay);
    uint32_t first=0,last=0;
    while(first<columns&&!(double(config.delayAxis[first])>low))++first;
    last=first;
    while(last<columns&&double(config.delayAxis[last])<high)++last;
    words[colBounds+2*j]=first;
    words[colBounds+2*j+1]=last;
   }
   const double width=config.centroidDoppler*config.centroidDopplerResolution;
   for(uint32_t r=0;r<rows;++r){const double low=config.dopplerAxis[r]-width,high=config.dopplerAxis[r]+width;
    if(!std::isfinite(low)||!std::isfinite(high))throw std::invalid_argument("GPU_POSTPROCESS_UNSUPPORTED centroid extent");
    uint32_t first=0,last=0;
    while(first<rows&&!(config.dopplerAxis[first]>low))++first;
    last=first;
    while(last<rows&&config.dopplerAxis[last]<high)++last;
    words[rowBounds+2*r]=first;
    words[rowBounds+2*r+1]=last;
   }
  }
  metadata_=std::make_unique<Buffer>(context,words.size()*4,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,VK_MEMORY_PROPERTY_HOST_COHERENT_BIT|VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
  std::memcpy(metadata_->mapped,words.data(),words.size()*4);
  metadata_->flush(0,metadata_->bytes);
  integration_=std::make_unique<Buffer>(context,uint64_t(layout_.cells)*(tbd?config.integrationCpis:1u)*8,
   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,VK_MEMORY_PROPERTY_HOST_COHERENT_BIT|VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
  auto* history=static_cast<double*>(integration_->mapped);
  std::fill(history,history+layout_.cells*(tbd?config.integrationCpis:1u),std::numeric_limits<double>::quiet_NaN());
  integration_->flush(0,integration_->bytes);
  currentPower_=std::make_unique<Buffer>(context,uint64_t(layout_.cells)*8,
   VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  const uint64_t angleWords=track_before_detect::angleHistoryWords(layout_.cells,
   config.integrationCpis,config.integrationBeamTolerance);
  angleHistory_=std::make_unique<Buffer>(context,angleWords*4,
   VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  if(tbd)tbdBanks_=std::make_unique<Buffer>(context,uint64_t(layout_.cells)*(banks+1u)*8,
   VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  timestamps_=std::make_unique<gpu_native::Timestamps>(context);
  Buffer* bankBinding=tbd?tbdBanks_.get():currentPower_.get();
  Buffer* mapBinding=tbd?channelMaps:&fused_;
  Buffer* weightBinding=tbd?refinementWeights:currentPower_.get();
  for(const auto& source:shaderSources())kernels_.push_back(std::make_unique<Kernel>(context,source.second.c_str(),std::vector<Buffer*>{metadata_.get(),&fused_,&scratch_,integration_.get(),currentPower_.get(),bankBinding,mapBinding,weightBinding,angleHistory_.get()}));
 }
 const Layout& scratchLayout()const{return layout_;}
 Buffer& metadataBuffer(){return *metadata_;} // Tracker reads final count before diagnostic reconstruction.
 Buffer& integrationBuffer(){return *integration_;} // Rolling power history; remains device resident.
 Buffer& currentPowerBuffer(){return *currentPower_;} // Current CPI power map; remains device resident.
 void append(VkCommandBuffer command){
  const bool tbd=config_.integrationCpis>2;
  const uint32_t banks=tbd?track_before_detect::bankCount(config_.integrationCpis):0;
  vkCmdFillBuffer(command,metadata_->handle,0,statusWords*4,0);
  barrier(command);
  timestamps_->begin(command,0);
  kernels_[0]->append(command,{layout_.cells,0,0,0,0});
  barrier(command);
  if(tbd){
   const uint32_t work=layout_.cells*128u;
   kernels_[10]->append(command,{work,0,0,0,0});barrier(command);
   const uint32_t trajectories=(2u*config_.integrationDelayMotionBins+1u)*(2u*config_.integrationDopplerMotionBins+1u);
   for(uint32_t first=0;first<trajectories;first+=128u){kernels_[10]->append(command,{work,1,first,0,0});barrier(command);}
   kernels_[10]->append(command,{work,2,0,0,0});barrier(command);
  }
  else {VkBufferCopy powerCopy{0,0,integration_->bytes};
   vkCmdCopyBuffer(command,currentPower_->handle,integration_->handle,1,&powerCopy);barrier(command);}
  kernels_[1]->append(command,{layout_.cells,0,0,0,0});
  barrier(command);
  kernels_[13]->append(command,{1,0,0,0,0});
  barrier(command);
  if(config_.detection){kernels_[2]->append(command,{layout_.cells,0,0,0,0});
  barrier(command);}timestamps_->end(command,0);
  if(config_.detection){
   timestamps_->begin(command,1);
   if(tbd)for(uint32_t pass=0;pass<=banks;++pass){kernels_[3]->append(command,{layout_.cells,pass+1u,pass==0u?1u:0u,0,0});barrier(command);}
   else {kernels_[3]->append(command,{layout_.cells,0,1,0,0});barrier(command);}
   if(tbd){kernels_[12]->append(command,{layout_.cells,0,0,0,0});barrier(command);}
   timestamps_->end(command,1);
   for(uint32_t stage=2;stage<=3;++stage){timestamps_->begin(command,stage);
    kernels_[stage+2]->append(command,{layout_.cells,0,0,0,0});barrier(command);timestamps_->end(command,stage);}
   timestamps_->begin(command,4);
   appendStableCompaction(command,kernels_,layout_.cells,0);
   kernels_[7]->append(command,{layout_.cells,0,0,0,0});
   barrier(command);
   timestamps_->end(command,4);
  }
  timestamps_->begin(command,5);
  kernels_[2]->append(command,{layout_.cells,1,0,0,0});
  barrier(command);
  timestamps_->end(command,5);
  if(tbd){kernels_[11]->append(command,{1,0,0,0,0});barrier(command);}
  hostRead(command);
 }
 std::array<uint32_t,statusWords> checkStatus(){
  metadata_->invalidate(0,metadata_->bytes);
  std::array<uint32_t,statusWords> s{};
  std::memcpy(s.data(),metadata_->mapped,sizeof(s));
  evidenceStatus_=s;
  if(s[error])throw ClutterRejected("GPU postprocess numeric rejection code="+std::to_string(s[error]));
  const uint32_t motionEcho=config_.integrationDelayMotionBins|(config_.integrationDopplerMotionBins<<16u);
  if(s[18]!=uint32_t(config_.epoch)||s[19]!=uint32_t(config_.epoch>>32)||s[20]!=(config_.detection?1u:0u)||s[21]!=rows_||s[22]!=columns_||s[27]!=config_.integrationCpis||s[29]!=motionEcho||s[30]!=config_.integrationBeamTolerance||s[31]!=config_.beamCount)throw std::runtime_error("GPU postprocess configuration echo mismatch");
  if(s[levelCells]!=layout_.cells||s[metricCells]!=layout_.cells||s[displayCells]!=layout_.cells)
   throw std::runtime_error("GPU postprocess map coverage mismatch");
  const bool tbd=config_.integrationCpis>2;
  const uint32_t detectorPasses=tbd?track_before_detect::detectorPassCount(config_.integrationCpis):1u;
  if(config_.detection){if(s[cfarCells]!=layout_.cells*detectorPasses||s[trainingAdds]!=expectedTrainingAdds_*detectorPasses||s[cfarAccepted]>layout_.cells||
    s[centroidExamined]!=s[cfarAccepted]||s[centroidKept]>s[centroidExamined]||s[interpolationExamined]!=s[centroidKept]||
    s[interpolationRejected]>s[interpolationExamined]||s[detectionCount]!=s[interpolationExamined]-s[interpolationRejected]||
    s[compactCells]!=layout_.cells||s[recordCells]!=s[detectionCount]||(tbd&&s[refinedCandidates]!=s[cfarAccepted]))throw std::runtime_error("GPU postprocess detection coverage mismatch");}
  else for(uint32_t i=cfarCells;i<=centroidDiagnosticRecords;++i)if(s[i])throw std::runtime_error("Disabled GPU detection executed work");
  const uint32_t kinds=s[compactKinds],passes=(kinds&1u)+((kinds>>1u)&1u)+((kinds>>2u)&1u);
  if((config_.detection?(!(kinds&1u)||(kinds&~7u)):(kinds!=0u))||
     s[compactTileCounts]!=passes*((layout_.cells+127u)/128u)||s[compactPrefixTiles]!=s[compactTileCounts]||
     s[compactScatterCells]!=passes*layout_.cells)
   throw std::runtime_error("GPU stable compaction coverage mismatch");
  if(s[cfarDiagnosticRecords]&&s[cfarDiagnosticRecords]!=s[cfarAccepted])throw std::runtime_error("GPU CFAR diagnostic coverage mismatch");
  if(s[centroidDiagnosticRecords]&&s[centroidDiagnosticRecords]!=s[centroidKept])throw std::runtime_error("GPU centroid diagnostic coverage mismatch");
  return s;
 }
 void validateOutputs(PostprocessOutputs out)const{
  const auto optional=[&](const void* p,size_t n){return (!p&&!n)||(p&&n==layout_.cells);};
  if(!optional(out.levels,out.levelCount)||!optional(out.unflooredSnr,out.unflooredSnrCount)||
     !optional(out.cfar,out.cfarCapacity)||!optional(out.centroid,out.centroidCapacity)||
     !optional(out.decisionFlags,out.decisionFlagCount)||!optional(out.detectionCells,out.detectionCellCapacity)||
     (!config_.detection&&(out.unflooredSnr||out.cfar||out.centroid||out.decisionFlags||out.detectionCells))||
     !out.metrics||!out.display||out.displayCount!=layout_.cells||
     (config_.detection?(!out.detections||out.detectionCapacity!=layout_.cells):(out.detections||out.detectionCapacity)))
   throw std::invalid_argument("GPU postprocess output dimensions changed");
  std::vector<std::pair<uintptr_t,uintptr_t>> ranges;
  const auto add=[&](const void* p,size_t count,size_t width){if(!p)return;
   const uintptr_t start=reinterpret_cast<uintptr_t>(p);
   if(count>SIZE_MAX/width||count*width>UINTPTR_MAX-start)throw std::invalid_argument("GPU postprocess host range overflow");
   ranges.emplace_back(start,start+count*width);};
  add(out.metrics,1,sizeof(PostprocessMetrics));
  add(out.display,out.displayCount,8);
  add(out.detections,out.detectionCapacity,32);
  add(out.levels,out.levelCount,8);
  add(out.unflooredSnr,out.unflooredSnrCount,8);
  add(out.cfar,out.cfarCapacity,32);
  add(out.centroid,out.centroidCapacity,32);
  add(out.decisionFlags,out.decisionFlagCount,4);
  add(out.detectionCells,out.detectionCellCapacity,4);
  add(fused_.mapped,size_t(fused_.bytes),1);
  add(scratch_.mapped,size_t(scratch_.bytes),1);
  add(staging_.mapped,size_t(staging_.bytes),1);
  add(metadata_->mapped,size_t(metadata_->bytes),1);
  add(integration_->mapped,size_t(integration_->bytes),1);
  for(size_t i=0;i<ranges.size();++i)for(size_t j=0;j<i;++j)if(ranges[i].first<ranges[j].second&&ranges[j].first<ranges[i].second)
   throw std::invalid_argument("GPU postprocess host buffers overlap");
 }
 void copyOutputs(PostprocessOutputs out){
  validateOutputs(out);
  const auto s=checkStatus();
  copy(uint64_t(layout_.powers)*4,out.display,uint64_t(layout_.cells)*8);
  if(s[detectionCount])copy(uint64_t(layout_.records)*4,out.detections,uint64_t(s[detectionCount])*32);
  if(out.levels)copy(uint64_t(layout_.levels)*4,out.levels,uint64_t(layout_.cells)*8);
  if(out.unflooredSnr)copy(uint64_t(layout_.snr)*4,out.unflooredSnr,uint64_t(layout_.cells)*8);
  if(out.decisionFlags){copy(uint64_t(layout_.flags)*4,out.decisionFlags,uint64_t(layout_.cells)*4);
   for(size_t i=0;i<out.decisionFlagCount;++i)out.decisionFlags[i]&=7u;}
  if(out.detectionCells&&s[detectionCount])copy(uint64_t(layout_.indices)*4,out.detectionCells,uint64_t(s[detectionCount])*4);
  if(out.cfar)copyDiagnostic(1,out.cfar,s[cfarAccepted]);
  if(out.centroid)copyDiagnostic(2,out.centroid,s[centroidKept]);
  const auto* words=static_cast<const uint32_t*>(metadata_->mapped);
  out.metrics->noisePower=getD(words,64);
  out.metrics->maxPower=getD(words,66);
  out.metrics->cfarCount=s[cfarAccepted];
  out.metrics->centroidCount=s[centroidKept];
  out.metrics->detectionCount=s[detectionCount];
 }
private:
 // Optional qualifier materialization: original flags/SNRs survive interpolation.
 // Final records must be copied first; this reuses their now-dead device region.
 void copyDiagnostic(uint32_t kind,PostprocessDetection* target,uint32_t expected){
  gpu_reference::CommandOwner command(context_);
  command.allocate();
  VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  check(vkBeginCommandBuffer(command.command,&begin),"postprocess diagnostic recording");
  barrier(command.command);
  appendStableCompaction(command.command,kernels_,layout_.cells,kind);
  kernels_[7]->append(command.command,{layout_.cells,kind,0,0,0});
  barrier(command.command);
  hostRead(command.command);
  check(vkEndCommandBuffer(command.command),"postprocess diagnostic recording");
  VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  submit.commandBufferCount=1;
  submit.pCommandBuffers=&command.command;
  check(gpu_host::call(context_.hostTiming,"vkQueueSubmit",0,[&]{return vkQueueSubmit(context_.queue,1,&submit,context_.fence);}),"postprocess diagnostic submit");
  check(gpu_host::call(context_.hostTiming,"vkWaitForFences",0,[&]{return vkWaitForFences(context_.device,1,&context_.fence,VK_TRUE,UINT64_MAX);}),"postprocess diagnostic wait");
  check(gpu_host::call(context_.hostTiming,"vkResetFences",0,[&]{return vkResetFences(context_.device,1,&context_.fence);}),"postprocess diagnostic fence");
  const auto s=checkStatus();
  if(s[diagnosticCount]!=expected||s[kind==1?cfarDiagnosticRecords:centroidDiagnosticRecords]!=expected)throw std::runtime_error("GPU postprocess diagnostic count mismatch");
  if(expected)copy(uint64_t(layout_.records)*4,target,uint64_t(expected)*32);
 }
public:
 void log(uint64_t frame){const auto s=checkStatus();
 context_.diagnostics.stream()<<std::setprecision(12)<<"GPU_POSTPROCESS_READY frame="<<frame<<" epoch="<<config_.epoch<<" detection="<<config_.detection<<" tracker=0"
  <<" device_epoch_low="<<s[18]<<" device_epoch_high="<<s[19]<<" device_detection="<<s[20]<<" device_rows="<<s[21]<<" device_columns="<<s[22]
  <<" device_level_cells="<<s[levelCells]<<" device_metric_cells="<<s[metricCells]<<" device_display_cells="<<s[displayCells]
  <<" device_cfar_cells="<<s[cfarCells]<<" device_training_adds="<<s[trainingAdds]<<" device_cfar_accepted="<<s[cfarAccepted]
  <<" device_centroid_examined="<<s[centroidExamined]<<" device_centroid_kept="<<s[centroidKept]
  <<" device_interpolation_examined="<<s[interpolationExamined]<<" device_interpolation_rejected="<<s[interpolationRejected]
  <<" device_compact_cells="<<s[compactCells]<<" device_records="<<s[recordCells]<<" detection_count="<<s[detectionCount]
  <<" device_cfar_diagnostic_records="<<s[cfarDiagnosticRecords]<<" device_centroid_diagnostic_records="<<s[centroidDiagnosticRecords]
  <<" device_compact_tile_counts="<<s[compactTileCounts]<<" device_compact_prefix_tiles="<<s[compactPrefixTiles]
  <<" device_compact_scatter_cells="<<s[compactScatterCells]<<" device_compact_kinds="<<s[compactKinds]
  <<" integration_cpis="<<s[27]<<" delay_motion_bins="<<(s[29]&0xffffu)<<" doppler_motion_bins="<<(s[29]>>16u)<<" integration_beam_tolerance="<<s[30]<<" beam_count="<<s[31]
  <<" track_before_detect="<<(config_.integrationCpis>2)<<" tbd_banks="<<(config_.integrationCpis>2?track_before_detect::bankCount(config_.integrationCpis):0)
  <<" joint_angle_tbd="<<(config_.integrationCpis>2&&config_.integrationBeamTolerance!=UINT32_MAX)
  <<" bearing_manifold="<<(config_.bearingManifoldResponses.empty()?"ideal":"measured")
  <<" bearing_exclusion_bins="<<config_.bearingExclusionBins
  <<" coherent_refined_candidates="<<s[refinedCandidates]<<" scratch_bytes="<<layout_.bytes<<" gpu_map_ms="<<timestamps_->milliseconds(0)<<" gpu_display_ms="<<timestamps_->milliseconds(5);
  if(config_.detection)context_.diagnostics.stream()<<" gpu_cfar_ms="<<timestamps_->milliseconds(1)<<" gpu_centroid_ms="<<timestamps_->milliseconds(2)<<" gpu_interpolation_ms="<<timestamps_->milliseconds(3)<<" gpu_compact_ms="<<timestamps_->milliseconds(4);
  context_.diagnostics.stream()<<" cpu_map_calls=0 cpu_cfar_calls=0 cpu_centroid_calls=0 cpu_interpolation_calls=0 cpu_conversion_calls=0\n";
 }
};
} // namespace gpu_postprocess
