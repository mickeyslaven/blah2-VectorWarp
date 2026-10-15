#pragma once
// Private GPU-only captured reference/spectrum stage; no public/wire ABI.
enum class ReferenceMode : uint32_t { supplied=0, capturedArray=1, capturedDedicated=2 };
struct ReferenceSpectrumConfig {
 ReferenceMode mode=ReferenceMode::supplied;
 std::vector<uint32_t> referenceChannels;
 uint32_t analysisSamples=32768,analysisInterval=10,powerIterations=12;
 double covarianceSmoothing=.8,diagonalLoading=.001;
 double bandwidthHz=2000,centreHz=0;
 uint64_t epoch=0;
};
struct ReferenceCheckpoint {
 ReferenceSpectrumConfig config;
 uint32_t samples=0,channels=0;
 double sampleRate=0;
 uint64_t frames=0,updates=0;
 uint32_t weightCount=0;
 std::array<std::complex<double>,8> weights{};
 double coherentFraction=0,coherentGainDb=0,scale=0;
 // Internal live-state evidence only. It is deliberately not part of the
 // durable 256-byte checkpoint; restored workers use matched weights until
 // the next scheduled covariance analysis.
 bool covarianceValid=false;
 std::array<std::complex<double>,64> covariance{};
};
struct ReferenceOutputs {
 ReferenceCheckpoint* checkpoint=nullptr;
 std::complex<double>* spectrum=nullptr;
 size_t spectrumCount=0;
 double* spectrumDb=nullptr;
 size_t spectrumDbCount=0;
 // Optional full FP64 diagnostic reference, copied in fenced chunks.
 std::complex<double>* reference=nullptr;
 size_t referenceCount=0;
};
namespace gpu_reference {
constexpr uint32_t metaWords=464,pending=64,committed=128,stateWords=52,covariance=192;
constexpr uint32_t error=0,analyzed=1,observations=2,pairs=3,iterations=4,
 referenceSamples=5,scaledSamples=6,chunks=7,foldTerms=8,bins=9,commits=10;
constexpr uint32_t cfgN=16,cfgChannels=17,cfgRefs=18,cfgMode=19,cfgAnalysis=20,
 cfgInterval=21,cfgIterations=22,cfgSpectrum=23,cfgDecimation=24,cfgFolded=25,
 cfgTransform=26,cfgStride=27,cfgObservations=28,cfgChunks=29,cfgSelection=32,
 cfgSmoothing=448,cfgLoading=450;
// Exact same strings are exposed for the existing CPU-only compiler preflight.
const char* common=R"glsl(#version 450
#extension GL_ARB_gpu_shader_fp64 : require
layout(local_size_x=128) in;
layout(binding=0) buffer Metadata { uint m[]; };
layout(binding=1) readonly buffer Canonical { dvec2 raw[]; };
layout(binding=2) buffer Reference { dvec2 ref[]; };
layout(binding=3) buffer Scratch { dvec2 scratch[]; };
layout(binding=4) readonly buffer Phases { dvec2 phases[]; };
layout(binding=5) buffer OutputValues { double result[]; };
layout(push_constant) uniform P { uint count; uint a; uint b; uint c; int d; } p;
uint indexOf(){return gl_GlobalInvocationID.x+gl_GlobalInvocationID.y*gl_NumWorkGroups.x*128;}
double readD(uint at){return packDouble2x32(uvec2(m[at],m[at+1]));}
void writeD(uint at,double v){uvec2 bits=unpackDouble2x32(v);m[at]=bits.x;m[at+1]=bits.y;}
dvec2 readZ(uint at){return dvec2(readD(at),readD(at+2));}
void writeZ(uint at,dvec2 v){writeD(at,v.x);writeD(at+2,v.y);}
bool finiteD(double x){return !isnan(x)&&!isinf(x);}
bool finiteZ(dvec2 x){return all(not(isnan(x)))&&all(not(isinf(x)));}
void reject(uint code){atomicCompSwap(m[0],0u,code);}
dvec2 conjZ(dvec2 x){return dvec2(x.x,-x.y);}
dvec2 multiplyZ(dvec2 a,dvec2 b){precise dvec2 v=dvec2(a.x*b.x-a.y*b.y,a.x*b.y+a.y*b.x);return v;}
double normZ(dvec2 x){precise double v=x.x*x.x+x.y*x.y;return v;}
double magnitudeZ(dvec2 x){
 double hi=max(abs(x.x),abs(x.y)),lo=min(abs(x.x),abs(x.y));
 if(hi==0.0LF)return 0.0LF;
 double upper=packDouble2x32(uvec2(0u,0x5ff00000u)),lower=packDouble2x32(uvec2(0u,0x1ff00000u));
 precise double ratioHi=hi,ratioLo=lo;
 if(hi>upper){ratioHi=hi*lower;ratioLo=lo*lower;}
 else if(hi<lower){ratioHi=hi*upper;ratioLo=lo*upper;}
 precise double r=ratioLo/ratioHi;precise double value=hi*sqrt(1.0LF+r*r);return value;
}
// All finite positive doubles, including subnormals. No FP32 conversion/log.
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
void increment64(uint at){uint lo=m[at]+1u;m[at]=lo;if(lo==0u)m[at+1]=m[at+1]+1u;}
uint modulo64(uint at,uint divisor){
 // Shift a remainder without uint overflow, preserving unsigned64 modulo.
 uint remainder=0u;
 for(int bit=63;bit>=0;--bit){
  uint nextBit=bit>=32?((m[at+1]>>uint(bit-32))&1u):((m[at]>>uint(bit))&1u);
  uint gap=divisor-remainder;
  remainder=remainder>=gap?remainder-gap:remainder+remainder;
  if(nextBit!=0u)remainder=remainder==divisor-1u?0u:remainder+1u;
 }
 return remainder;
}
)glsl";
const char* schedule=R"glsl(
void main(){
 if(indexOf()!=0u)return;
 if(m[19]!=1u)return;
 bool run=m[68]!=m[18];
 if(!run){run=modulo64(64u,m[21])==0u;increment64(64u);}
 m[1]=run?1u:0u;
}
)glsl";
const char* covarianceBody=R"glsl(
void main(){
 uint i=indexOf(),count=m[18];if(m[1]==0u||i>=p.count)return;
 // Phase0 writes contiguous observation-tile partials; phase1 reduces tiles
 // in ascending order. The original FP64 complex product is unchanged.
 if(p.a==0u){
  uint pair=i/p.c,tile=i%p.c,left=pair/count,right=pair%count;
  uint first=tile*p.b,limit=first+min(p.b,m[28]-first),observed=0u;
  precise dvec2 value=dvec2(0.0LF);
  for(uint k=first;k<limit;++k){uint n=k*m[27];
   dvec2 a=raw[m[32+left]*m[16]+n],b=raw[m[32+right]*m[16]+n];
   if(!finiteZ(a)||!finiteZ(b))reject(102u);
   value=value+multiplyZ(a,conjZ(b));++observed;
  }
  if(!finiteZ(value))reject(103u);scratch[i]=value;
  atomicAdd(m[11],1u);if(pair==0u)atomicAdd(m[2],observed);return;
 }
 precise dvec2 value=dvec2(0.0LF);
 for(uint tile=0u;tile<p.c;++tile)value=value+scratch[i*p.c+tile];
 value=value/double(m[28]);if(!finiteZ(value))reject(103u);
 writeZ(192u+4u*i,value);atomicAdd(m[3],1u);atomicAdd(m[12],1u);
}
)glsl";
const char* weightsBody=R"glsl(
void normalizeWeights(inout dvec2 w[8],uint count){
 precise double power=0.0LF;for(uint c=0u;c<count;++c)power=power+normZ(w[c]);
 if(!finiteD(power)||power<0.0LF){reject(112u);return;}
 double lengthValue=sqrt(max(power,1e-30LF));
 if(!finiteD(lengthValue)||!(lengthValue>0.0LF)){reject(112u);return;}
 for(uint c=0u;c<count;++c)w[c]=w[c]/lengthValue;
}
void main(){
 if(indexOf()!=0u||m[1]==0u)return;uint count=m[18];bool retained=m[68]==count;
 precise double trace=0.0LF;for(uint c=0u;c<count;++c)trace=trace+readZ(192u+4u*(c*count+c)).x;
 precise double loading=readD(450u)*trace/double(count);
 if(!finiteD(trace)||trace<0.0LF||!finiteD(loading)||loading<0.0LF){reject(113u);return;}
 for(uint c=0u;c<count;++c){uint at=192u+4u*(c*count+c);dvec2 value=readZ(at);value.x=value.x+loading;writeZ(at,value);}
 precise dvec2 w[8],old[8],next[8];
 for(uint c=0u;c<count;++c){old[c]=readZ(72u+4u*c);w[c]=retained?old[c]:dvec2(1.0LF/sqrt(double(count)),0.0LF);}
 uint completed=0u;for(uint iteration=0u;iteration<m[22];++iteration){
  for(uint left=0u;left<count;++left){precise dvec2 value=dvec2(0.0LF);
   for(uint right=0u;right<count;++right)value=value+multiplyZ(readZ(192u+4u*(left*count+right)),w[right]);next[left]=value;}
  normalizeWeights(next,count);for(uint c=0u;c<count;++c)w[c]=next[c];++completed;
 }
 if(retained){
  precise dvec2 alignment=dvec2(0.0LF);for(uint c=0u;c<count;++c)alignment=alignment+multiplyZ(conjZ(old[c]),w[c]);
  double amplitude=magnitudeZ(alignment);
  if(!finiteZ(alignment)||!finiteD(amplitude)){reject(114u);return;}
  if(amplitude>1e-12LF){dvec2 phase=conjZ(alignment)/amplitude;for(uint c=0u;c<count;++c)w[c]=multiplyZ(w[c],phase);}
  double smoothing=readD(448u);
  for(uint c=0u;c<count;++c)w[c]=smoothing*old[c]+(1.0LF-smoothing)*w[c];normalizeWeights(w,count);
 }
 precise dvec2 projected[8];
 for(uint left=0u;left<count;++left){precise dvec2 value=dvec2(0.0LF);for(uint right=0u;right<count;++right)value=value+multiplyZ(readZ(192u+4u*(left*count+right)),w[right]);projected[left]=value;}
 precise dvec2 eigenvalue=dvec2(0.0LF);for(uint c=0u;c<count;++c)eigenvalue=eigenvalue+multiplyZ(conjZ(w[c]),projected[c]);
 if(!finiteZ(eigenvalue)){reject(114u);return;}
 double principal=max(0.0LF,eigenvalue.x-loading);double fraction=principal/max(trace,1e-30LF);
 double ratio=principal/max(trace/double(count),1e-30LF);double gain=ratio<=1e-12LF?-120.0LF:10.0LF*positiveLog10(ratio);
 if(!finiteD(fraction)||!finiteD(gain))reject(104u);
 for(uint c=0u;c<count;++c){if(!finiteZ(w[c]))reject(105u);writeZ(72u+4u*c,w[c]);}
 writeD(104u,fraction);writeD(106u,gain);m[68]=count;increment64(66u);m[4]=completed;
}
)glsl";
const char* combineBody=R"glsl(
// Neighboring lanes compute neighboring samples. Four independent 32-sample
// chunks share a workgroup; each chunk leader retains every original FP64 add.
shared double sampleInputPower[128*8];
shared double sampleOutputPower[128];
void main(){
 uint n=indexOf(),lane=gl_LocalInvocationID.x,count=m[18];
 if(n<m[16]){
  precise dvec2 value=dvec2(0.0LF);
  if(m[19]==2u){value=raw[m[32]*m[16]+n];}
  else for(uint c=0u;c<count;++c){dvec2 x=raw[m[32+c]*m[16]+n];if(!finiteZ(x))reject(106u);
   value=value+multiplyZ(conjZ(readZ(72u+4u*c)),x);
   precise double contribution=normZ(x)/double(count);sampleInputPower[c*128u+lane]=contribution;}
  if(!finiteZ(value))reject(107u);ref[n]=value;sampleOutputPower[lane]=normZ(value);
 }
 // Every invocation, including inactive tail lanes, reaches this barrier.
 barrier();
 if((lane&31u)!=0u||n>=m[16])return;
 uint amount=min(32u,m[16]-n),chunk=n/32u;
 precise double inputPower=0.0LF,outputPower=0.0LF;
 for(uint j=0u;j<amount;++j){
  if(m[19]==1u)for(uint c=0u;c<count;++c)inputPower=inputPower+sampleInputPower[c*128u+lane+j];
  outputPower=outputPower+sampleOutputPower[lane+j];
 }
 if(m[19]==1u){scratch[chunk]=dvec2(inputPower,outputPower);atomicAdd(m[7],1u);}
 atomicAdd(m[5],amount);
}
)glsl";
const char* scaleBody=R"glsl(
shared dvec2 scaleTile[128];
void main(){
 // One workgroup prefetches; lane zero preserves ascending chunk additions.
 if(gl_WorkGroupID.x!=0u||gl_WorkGroupID.y!=0u||gl_WorkGroupID.z!=0u||m[19]!=1u)return;
 uint lane=gl_LocalInvocationID.x;precise double inputPower=0.0LF,outputPower=0.0LF;
 for(uint first=0u;first<m[29];first+=128u){
  uint count=min(128u,m[29]-first);
  scaleTile[lane]=lane<count?scratch[first+lane]:dvec2(0.0LF);
  barrier();
  if(lane==0u){for(uint j=0u;j<count;++j){dvec2 value=scaleTile[j];inputPower=inputPower+value.x;outputPower=outputPower+value.y;}}
  barrier();
 }
 if(lane!=0u)return;
 if(!finiteD(inputPower)||inputPower<0.0LF||!finiteD(outputPower)||outputPower<0.0LF){reject(108u);return;}
 double divisor=max(outputPower,1e-30LF);
 if(!finiteD(divisor)||!(divisor>0.0LF)){reject(108u);return;}
 double scale=sqrt(inputPower/divisor);if(!finiteD(scale))reject(108u);
 writeD(108u,scale);writeD(110u,inputPower);writeD(112u,outputPower);
}
)glsl";
const char* normalizeBody=R"glsl(
void main(){uint i=indexOf();if(i>=m[16]||m[19]!=1u)return;
 precise dvec2 value=ref[i]*readD(108u);if(!finiteZ(value))reject(109u);ref[i]=value;
 if((i&127u)==0u)atomicAdd(m[6],min(128u,m[16]-i));}
)glsl";
const char* spectrumBody=R"glsl(
void main(){uint i=indexOf();if(i>=m[26])return;
 precise dvec2 value=dvec2(0.0LF);
 if(m[25]!=0u){for(uint block=0u;block<m[24];++block)value=value+multiplyZ(ref[block*m[23]+i],phases[block]);value=multiplyZ(value,phases[m[24]+i]);}
 else value=ref[i];
 if(!finiteZ(value))reject(110u);scratch[i]=value;
 if((i&127u)==0u)atomicAdd(m[8],min(128u,m[26]-i)*(m[25]!=0u?m[24]:1u));
}
)glsl";
const char* readoutBody=R"glsl(
void main(){uint i=indexOf();if(i>=m[23])return;
 uint shift=(m[16]+1u)/2u;
 uint at=m[25]!=0u?(shift/m[24]+i)%m[23]:(i*m[24]+shift)%m[16];
 dvec2 value=scratch[at]/double(m[16]);double amplitude=magnitudeZ(value);
 double level=amplitude<=1e-15LF?-300.0LF:20.0LF*positiveLog10(amplitude);
 if(!finiteZ(value)||!finiteD(amplitude)||!finiteD(level))reject(111u);
 result[2u*i]=value.x;result[2u*i+1u]=value.y;result[2u*m[23]+i]=level;
 if((i&127u)==0u)atomicAdd(m[9],min(128u,m[23]-i));}
)glsl";
const char* commitBody=R"glsl(
void main(){uint i=indexOf();if(i<52u)m[128u+i]=m[64u+i];if(i==0u)m[10]=1u;}
)glsl";
const char* logProbeBody=R"glsl(
void main(){uint i=indexOf();if(i<p.count)result[32u+i]=positiveLog10(result[i]);}
)glsl";
inline std::vector<std::pair<std::string,std::string>> shaderSources(){
 const std::pair<const char*,const char*> bodies[]={{"schedule",schedule},{"covariance",covarianceBody},{"weights",weightsBody},{"combine",combineBody},{"scale",scaleBody},{"normalize",normalizeBody},{"spectrum",spectrumBody},{"readout",readoutBody},{"commit",commitBody},{"positive-log-probe",logProbeBody}};
 std::vector<std::pair<std::string,std::string>> result;
 for(const auto& b:bodies)result.emplace_back(b.first,std::string(common)+b.second);
 return result;
}
inline bool sameConfig(const ReferenceSpectrumConfig& a,const ReferenceSpectrumConfig& b){
 return a.mode==b.mode&&a.referenceChannels==b.referenceChannels&&a.analysisSamples==b.analysisSamples&&
 a.analysisInterval==b.analysisInterval&&a.powerIterations==b.powerIterations&&a.covarianceSmoothing==b.covarianceSmoothing&&
 a.diagonalLoading==b.diagonalLoading&&a.bandwidthHz==b.bandwidthHz&&a.centreHz==b.centreHz&&a.epoch==b.epoch;
}
struct Shape {uint32_t decimation,bins,transform,stride,observations,chunks;
bool folded;};
// Counts only: no host covariance arithmetic. No buffer allocation grows.
struct CovarianceLayout {uint32_t tileObservations,tiles,partials;
uint64_t bytes;};
inline CovarianceLayout covarianceLayout(uint32_t observations,uint32_t refs,uint64_t workspaceBytes){
 if(!observations||!refs||refs>8)throw std::invalid_argument("Invalid covariance partial geometry");
 const uint64_t pairs=uint64_t(refs)*refs,availableTiles=workspaceBytes/16/pairs;
 if(!availableTiles)throw std::invalid_argument("Covariance borrowed workspace unavailable");
 const uint64_t tileObservations=std::max<uint64_t>(128,1+(uint64_t(observations)-1)/availableTiles);
 const uint64_t tiles=1+(uint64_t(observations)-1)/tileObservations,partials=tiles*pairs;
 if(tileObservations>UINT32_MAX||tiles>UINT32_MAX||partials>UINT32_MAX||partials>workspaceBytes/16)
  throw std::invalid_argument("Covariance partial geometry overflow");
 return {uint32_t(tileObservations),uint32_t(tiles),uint32_t(partials),partials*16};
}

inline Shape validate(const ReferenceSpectrumConfig& c,uint32_t n,uint32_t channels,double sampleRate){
 if(c.mode!=ReferenceMode::capturedArray&&c.mode!=ReferenceMode::capturedDedicated)throw std::invalid_argument("Unsupported captured reference mode");
 const size_t count=c.referenceChannels.size();
 if((c.mode==ReferenceMode::capturedArray&&(count<2||count>8))||(c.mode==ReferenceMode::capturedDedicated&&count!=1)||
 c.analysisSamples<64||!c.analysisInterval||!c.powerIterations||!std::isfinite(c.covarianceSmoothing)||
 c.covarianceSmoothing<0||c.covarianceSmoothing>=1||!std::isfinite(c.diagonalLoading)||c.diagonalLoading<0||
 !std::isfinite(c.bandwidthHz)||!(c.bandwidthHz>0)||!std::isfinite(c.centreHz)||!n||!(sampleRate>0)||!std::isfinite(sampleRate))
  throw std::invalid_argument("Invalid GPU reference/spectrum configuration");
 for(size_t i=0;i<count;++i){if(c.referenceChannels[i]>=channels)throw std::invalid_argument("Reference channel absent from canonical frame");
  for(size_t j=0;j<i;++j)if(c.referenceChannels[i]==c.referenceChannels[j])throw std::invalid_argument("Duplicate reference channel");}
 const double resolution=sampleRate/n;
 if(!std::isfinite(resolution)||!(resolution>0))throw std::invalid_argument("Invalid spectrum resolution");
 Shape s{};
 s.decimation=uint32_t(std::min<double>(n,std::max(1.0,std::ceil(c.bandwidthHz/resolution))));
 s.bins=1+(n-1)/s.decimation;
 s.folded=s.decimation>=8&&n%s.decimation==0;
 s.transform=s.folded?s.bins:n;
 s.stride=std::max(1u,n/c.analysisSamples);
 s.observations=1+(n-1)/s.stride;
 s.chunks=1+(n-1)/32;
 if(uint64_t(s.chunks)*16>uint64_t(gpu_native::tileBlocks)*gpu_native::fft*16||s.transform>gpu_native::tileBlocks*gpu_native::fft)
  throw std::invalid_argument("GPU reference/spectrum borrowed workspace capacity unsupported");
 return s;
}
struct CommandOwner {
 Context& context;
 VkCommandBuffer command=VK_NULL_HANDLE;
 explicit CommandOwner(Context& c):context(c){}
 ~CommandOwner(){if(command)vkFreeCommandBuffers(context.device,context.pool,1,&command);}
 void allocate(){VkCommandBufferAllocateInfo a{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
 a.commandPool=context.pool;
 a.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;
 a.commandBufferCount=1;
 check(vkAllocateCommandBuffers(context.device,&a,&command),"reference auxiliary command");}
};
class Stage {
 std::array<uint32_t,16> evidenceStatus_{};
 Context& context_;
 const ReferenceSpectrumConfig config_;
 const uint32_t samples_,channels_;
 const double sampleRate_;
 const Shape shape_;
 Buffer& reference_;
 Buffer& work_;
 const CovarianceLayout covariance_;
 std::unique_ptr<Buffer> metadata_,phases_,output_;
 std::unique_ptr<NativePlan> spectrumPlan_;
 std::vector<std::unique_ptr<Kernel>> kernels_;
 std::unique_ptr<gpu_native::Timestamps> timestamps_;
 CommandOwner commitCommand_,probeCommand_; // Freed before kernels/borrowed storage.
 std::vector<double> frequencies_;
 bool covarianceValid_=false;
 std::array<std::complex<double>,64> covarianceCache_{};
 void submit(VkCommandBuffer command){VkSubmitInfo s{VK_STRUCTURE_TYPE_SUBMIT_INFO};
 s.commandBufferCount=1;
 s.pCommandBuffers=&command;
  check(gpu_host::call(context_.hostTiming,"vkQueueSubmit",0,[&]{return vkQueueSubmit(context_.queue,1,&s,context_.fence);}),"reference auxiliary submit");
  check(gpu_host::call(context_.hostTiming,"vkWaitForFences",0,[&]{return vkWaitForFences(context_.device,1,&context_.fence,VK_TRUE,UINT64_MAX);}),"reference auxiliary execution");
  check(gpu_host::call(context_.hostTiming,"vkResetFences",0,[&]{return vkResetFences(context_.device,1,&context_.fence);}),"reference auxiliary fence");}
 static void putD(uint32_t* words,uint32_t at,double value){std::memcpy(words+at,&value,8);}
 static double getD(const uint32_t* words,uint32_t at){double value;
 std::memcpy(&value,words+at,8);
 return value;}
 static uint64_t get64(const uint32_t* words,uint32_t at){return uint64_t(words[at])|(uint64_t(words[at+1])<<32);}
 ReferenceCheckpoint decode(uint32_t at){
  metadata_->invalidate(0,metadata_->bytes);
  const auto* m=static_cast<const uint32_t*>(metadata_->mapped);
  ReferenceCheckpoint cp;
  cp.config=config_;
  cp.samples=samples_;
  cp.channels=channels_;
  cp.sampleRate=sampleRate_;
  cp.frames=get64(m,at);
  cp.updates=get64(m,at+2);
  cp.weightCount=m[at+4];
  for(uint32_t c=0;c<8;++c)std::memcpy(static_cast<void*>(&cp.weights[c]),m+at+8+4*c,16);
  cp.coherentFraction=getD(m,at+40);
  cp.coherentGainDb=getD(m,at+42);
  cp.scale=getD(m,at+44);
  cp.covarianceValid=covarianceValid_;
  cp.covariance=covarianceCache_;
  return cp;
 }
public:
 const auto& evidenceStatus()const{return evidenceStatus_;}
 const gpu_native::Timestamps& evidenceTimes()const{return *timestamps_;}
 Stage(Context& context,const ReferenceSpectrumConfig& c,uint32_t n,uint32_t channels,double sampleRate,Buffer& reference,Buffer& work)
  :context_(context),config_(c),samples_(n),channels_(channels),sampleRate_(sampleRate),shape_(validate(c,n,channels,sampleRate)),reference_(reference),work_(work),covariance_(covarianceLayout(shape_.observations,uint32_t(c.referenceChannels.size()),work.bytes)),commitCommand_(context),probeCommand_(context){
  if(reference.bytes<uint64_t(n)*16||work.bytes<std::max(uint64_t(shape_.chunks)*16,uint64_t(shape_.transform)*16)||
   context.candidate.properties.limits.maxPerStageDescriptorStorageBuffers<6||context.candidate.properties.limits.maxDescriptorSetStorageBuffers<6)
   throw std::runtime_error("Reference/spectrum storage unsupported");
  constexpr auto host=VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
  constexpr auto preferred=VK_MEMORY_PROPERTY_HOST_COHERENT_BIT|VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
  metadata_=std::make_unique<Buffer>(context,metaWords*4,host,preferred);
  const uint64_t phaseCount=shape_.folded?uint64_t(shape_.decimation)+shape_.bins:1;
  if(phaseCount*16>context.candidate.properties.limits.maxStorageBufferRange)throw std::runtime_error("Spectrum phase storage unsupported");
  phases_=std::make_unique<Buffer>(context,phaseCount*16,host,preferred);
  output_=std::make_unique<Buffer>(context,std::max(uint64_t(shape_.bins)*24,uint64_t(512)),host,preferred);
  spectrumPlan_=std::make_unique<NativePlan>(context,work_,shape_.transform,1,true,false,true);
  std::array<uint32_t,metaWords> words{};
  words[cfgN]=n;
  words[cfgChannels]=channels;
  words[cfgRefs]=uint32_t(c.referenceChannels.size());
  words[cfgMode]=uint32_t(c.mode);
  words[cfgAnalysis]=c.analysisSamples;
  words[cfgInterval]=c.analysisInterval;
  words[cfgIterations]=c.powerIterations;
  words[cfgSpectrum]=shape_.bins;
  words[cfgDecimation]=shape_.decimation;
  words[cfgFolded]=shape_.folded?1:0;
  words[cfgTransform]=shape_.transform;
  words[cfgStride]=shape_.stride;
  words[cfgObservations]=shape_.observations;
  words[cfgChunks]=shape_.chunks;
  std::copy(c.referenceChannels.begin(),c.referenceChannels.end(),words.begin()+cfgSelection);
  putD(words.data(),cfgSmoothing,c.covarianceSmoothing);
  putD(words.data(),cfgLoading,c.diagonalLoading);
  std::memcpy(metadata_->mapped,words.data(),sizeof(words));
  metadata_->flush(0,metadata_->bytes);
  auto* phases=static_cast<std::complex<double>*>(phases_->mapped);
  if(shape_.folded){const uint32_t remainder=((n+1)/2)%shape_.decimation;
  const double phase=-2.0*std::acos(-1.0)*remainder;
   for(uint32_t block=0;block<shape_.decimation;++block)phases[block]=std::polar(1.0,phase*block/shape_.decimation);
   for(uint32_t bin=0;bin<shape_.bins;++bin)phases[shape_.decimation+bin]=std::polar(1.0,phase*bin/n);
  }else phases[0]={1,0};
  phases_->flush(0,phases_->bytes);
  frequencies_.reserve(shape_.bins);
  for(uint32_t i=0;i<n;i+=shape_.decimation)frequencies_.push_back(((double(i)-n/2)*(sampleRate/n)+c.centreHz)/1000);
  timestamps_=std::make_unique<gpu_native::Timestamps>(context);
 }
 void bind(Buffer& canonical){
  if(!kernels_.empty())throw std::logic_error("Reference stage already bound");
  for(const auto& entry:shaderSources())kernels_.push_back(std::make_unique<Kernel>(context_,entry.second.c_str(),std::vector<Buffer*>{metadata_.get(),&canonical,&reference_,&work_,phases_.get(),output_.get()}));
  commitCommand_.allocate();
  VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  check(vkBeginCommandBuffer(commitCommand_.command,&begin),"reference commit recording");
  barrier(commitCommand_.command);
  kernels_[8]->append(commitCommand_.command,{stateWords,0,0,0,0});
  barrier(commitCommand_.command);
  VkMemoryBarrier read{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
  read.srcAccessMask=VK_ACCESS_MEMORY_WRITE_BIT;
  read.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
  vkCmdPipelineBarrier(commitCommand_.command,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&read,0,nullptr,0,nullptr);
  check(vkEndCommandBuffer(commitCommand_.command),"reference commit recording");
  probeCommand_.allocate();
 }
 void append(VkCommandBuffer command){
  vkCmdFillBuffer(command,metadata_->handle,0,16*4,0);
  barrier(command);
  VkBufferCopy copy{committed*4,pending*4,stateWords*4};
  vkCmdCopyBuffer(command,metadata_->handle,metadata_->handle,1,&copy);
  barrier(command);
  timestamps_->begin(command,0);
  kernels_[0]->append(command,{1,0,0,0,0});
  barrier(command);
  const uint32_t count=uint32_t(config_.referenceChannels.size());
  kernels_[1]->append(command,{covariance_.partials,0,covariance_.tileObservations,covariance_.tiles,0});
  barrier(command);
  kernels_[1]->append(command,{count*count,1,covariance_.tileObservations,covariance_.tiles,0});
  barrier(command);
  kernels_[2]->append(command,{1,0,0,0,0});
  barrier(command);
  timestamps_->end(command,0);
  timestamps_->begin(command,1);
  kernels_[3]->append(command,{samples_,0,0,0,0});
  barrier(command);
  kernels_[4]->append(command,{1,0,0,0,0});
  barrier(command);
  kernels_[5]->append(command,{samples_,0,0,0,0});
  barrier(command);
  timestamps_->end(command,1);
  timestamps_->begin(command,2);
  kernels_[6]->append(command,{shape_.transform,0,0,0,0});
  barrier(command);
  spectrumPlan_->append(command,-1);
  barrier(command);
  kernels_[7]->append(command,{shape_.bins,0,0,0,0});
  barrier(command);
  timestamps_->end(command,2);
 }
 std::array<uint32_t,16> checkStatus(bool committedState=false){
  metadata_->invalidate(0,metadata_->bytes);
  std::array<uint32_t,16> s{};
  std::memcpy(s.data(),metadata_->mapped,sizeof(s));
  evidenceStatus_=s;
  if(s[error])throw ClutterRejected("GPU reference/spectrum numeric rejection code="+std::to_string(s[error]));
  const bool array=config_.mode==ReferenceMode::capturedArray;
  const uint32_t refs=uint32_t(config_.referenceChannels.size());
  if(s[analyzed]>1||(!array&&s[analyzed])||s[observations]!=(s[analyzed]?shape_.observations:0)||s[pairs]!=(s[analyzed]?refs*refs:0)||
   s[11]!=(s[analyzed]?covariance_.partials:0)||s[12]!=(s[analyzed]?refs*refs:0)||
   s[iterations]!=(s[analyzed]?config_.powerIterations:0)||s[referenceSamples]!=samples_||s[scaledSamples]!=(array?samples_:0)||
   s[chunks]!=(array?shape_.chunks:0)||s[foldTerms]!=samples_||s[bins]!=shape_.bins||s[commits]!=(committedState?1u:0u))
   throw std::runtime_error("GPU reference/spectrum coverage mismatch");
  return s;
 }
 ReferenceCheckpoint commit(){const auto status=checkStatus();
  if(status[analyzed]){
   metadata_->invalidate(0,metadata_->bytes);
   const auto* m=static_cast<const uint32_t*>(metadata_->mapped);
   const uint32_t count=uint32_t(config_.referenceChannels.size());
   for(uint32_t i=0;i<count*count;++i)
    std::memcpy(static_cast<void*>(&covarianceCache_[i]),m+covariance+4*i,16);
   covarianceValid_=true;
  }
 submit(commitCommand_.command);
 checkStatus(true);
 return decode(committed);}
 ReferenceCheckpoint exportCheckpoint(){return decode(committed);}
 void restore(const ReferenceCheckpoint& cp){
  if(!sameConfig(cp.config,config_)||cp.samples!=samples_||cp.channels!=channels_||cp.sampleRate!=sampleRate_||(cp.weightCount!=0&&cp.weightCount!=config_.referenceChannels.size())||
   (config_.mode==ReferenceMode::capturedDedicated&&(cp.weightCount||cp.frames||cp.updates||cp.coherentFraction!=0||cp.coherentGainDb!=0||cp.scale!=0))||
   !std::isfinite(cp.coherentFraction)||!std::isfinite(cp.coherentGainDb)||!std::isfinite(cp.scale)||cp.scale<0)
   throw std::invalid_argument("Reference checkpoint identity/state mismatch");
  std::array<uint32_t,stateWords> words{};
  words[0]=uint32_t(cp.frames);
  words[1]=uint32_t(cp.frames>>32);
  words[2]=uint32_t(cp.updates);
  words[3]=uint32_t(cp.updates>>32);
  words[4]=cp.weightCount;
  for(uint32_t c=0;c<8;++c){if(!std::isfinite(cp.weights[c].real())||!std::isfinite(cp.weights[c].imag()))throw std::invalid_argument("Nonfinite reference checkpoint weight");
   if(config_.mode==ReferenceMode::capturedDedicated&&cp.weights[c]!=std::complex<double>{})throw std::invalid_argument("Dedicated reference checkpoint is not inert");
   std::memcpy(words.data()+8+4*c,&cp.weights[c],16);}
  putD(words.data(),40,cp.coherentFraction);
  putD(words.data(),42,cp.coherentGainDb);
  putD(words.data(),44,cp.scale);
  std::memcpy(static_cast<uint32_t*>(metadata_->mapped)+committed,words.data(),sizeof(words));
  metadata_->flush(committed*4,sizeof(words));
  covarianceValid_=false;
 }
 void reset(){ReferenceCheckpoint cp;
 cp.config=config_;
 cp.samples=samples_;
 cp.channels=channels_;
 cp.sampleRate=sampleRate_;
 restore(cp);}
 const std::vector<double>& frequencies()const{return frequencies_;}
 uint32_t spectrumCount()const{return shape_.bins;}
 Buffer& referenceBuffer(){return reference_;}
 void copySpectrum(ReferenceOutputs out){output_->invalidate(0,output_->bytes);
 const auto* data=static_cast<const double*>(output_->mapped);
  std::memcpy(static_cast<void*>(out.spectrum),data,size_t(shape_.bins)*16);
  std::memcpy(out.spectrumDb,data+2*shape_.bins,size_t(shape_.bins)*8);}
 void log(uint64_t frame){const auto s=checkStatus(true);
 const auto cp=exportCheckpoint();
  context_.diagnostics.stream()<<std::setprecision(12)<<"GPU_REFERENCE_READY frame="<<frame<<" mode="<<uint32_t(config_.mode)<<" epoch="<<config_.epoch
   <<" analyzed="<<s[1]<<" device_observations="<<s[2]<<" device_pairs="<<s[3]<<" device_iterations="<<s[4]
   <<" device_reference_samples="<<s[5]<<" device_scaled_samples="<<s[6]<<" device_chunks="<<s[7]<<" device_spectrum_terms="<<s[8]<<" device_spectrum_bins="<<s[9]<<" device_commits="<<s[10]
   <<" device_covariance_partials="<<s[11]<<" device_covariance_reductions="<<s[12]<<" covariance_tile_observations="<<covariance_.tileObservations<<" covariance_tiles="<<covariance_.tiles<<" covariance_scratch_bytes="<<covariance_.bytes
   <<" frames="<<cp.frames<<" updates="<<cp.updates<<" gpu_analysis_ms="<<timestamps_->milliseconds(0)<<" gpu_combine_scale_ms="<<timestamps_->milliseconds(1)<<" gpu_spectrum_ms="<<timestamps_->milliseconds(2)
   <<" cpu_synthesis_calls=0 cpu_spectrum_calls=0 cpu_normalization_calls=0 cpu_log_calls=0\n";
 }
 // Test-only scalar contract, no state commit and no device creation. All math is GPU.
 bool probePositiveLog(const double* values,size_t count,double* results){
  if(!values||!results||!count||count>32)throw std::invalid_argument("Invalid positive-log probe count");
  std::memcpy(output_->mapped,values,count*8);
  output_->flush(0,count*8);
  // Individual command reset is unavailable on the shared pool: free/reallocate.
  if(probeCommand_.command){vkFreeCommandBuffers(context_.device,context_.pool,1,&probeCommand_.command);
  probeCommand_.command=VK_NULL_HANDLE;}probeCommand_.allocate();
  VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  check(vkBeginCommandBuffer(probeCommand_.command,&begin),"log probe recording");
  VkMemoryBarrier host{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
  host.srcAccessMask=VK_ACCESS_HOST_WRITE_BIT;
  host.dstAccessMask=VK_ACCESS_MEMORY_READ_BIT;
  vkCmdPipelineBarrier(probeCommand_.command,VK_PIPELINE_STAGE_HOST_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,1,&host,0,nullptr,0,nullptr);
  vkCmdFillBuffer(probeCommand_.command,metadata_->handle,0,16*4,0);
  barrier(probeCommand_.command);
  kernels_[9]->append(probeCommand_.command,{uint32_t(count),0,0,0,0});
  barrier(probeCommand_.command);
  host.srcAccessMask=VK_ACCESS_MEMORY_WRITE_BIT;
  host.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
  vkCmdPipelineBarrier(probeCommand_.command,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&host,0,nullptr,0,nullptr);
  check(vkEndCommandBuffer(probeCommand_.command),"log probe recording");
  submit(probeCommand_.command);
  metadata_->invalidate(0,64);
  if(static_cast<const uint32_t*>(metadata_->mapped)[0])return false;
  output_->invalidate(256,count*8);
  std::memcpy(results,static_cast<const double*>(output_->mapped)+32,count*8);
  return true;
 }
};
} // namespace gpu_reference
