#pragma once
#include "GpuReferenceSpectrum.private.h"
#include "GpuPostprocess.private.h"
#include "GpuTracker.private.h"
// Private direct-call arithmetic seam. Included after ClutterPipeline, inside
// blah2's private namespace. No main/worker ABI or production route is changed.
// Host work is immutable input/output byte copies, status checks and submission.
enum ResidentFrameStage : uint32_t {
  residentSuppliedReference = 1u,
  residentClutter = 2u,
  residentAmbiguity = 4u,
  residentFusion = 8u,
  residentSupportedStages = 15u,
  residentCapturedReference = 16u,
  residentSpectrum = 32u,
  residentCapturedSupportedStages = 62u,
  residentMapDisplay = 64u,
  residentDetection = 128u,
  residentTracker = 256u
};
struct ResidentFrameConfig {
  GpuGeometry geometry{};
  uint32_t correlation = 0;
  double sampleRate = 0;
  double middle = 0;
  struct Fusion {
    uint32_t mode = 0, beams = 0;
    double frequencyHz = 0, radiusM = 0, element0Rad = 0, elementStepRad = 0,
      directPathAzimuthRad = 0;
  } fusion;
  // Supplied canonical reference, native clutter, ambiguity, shift/fusion.
  // Captured mode explicitly adds GPU synthesis/dedicated selection and spectrum.
  // Display/detection/tracker are explicit; main/worker routing remains unsupported.
  uint32_t stages = residentSupportedStages;
  // Private boundary tests may lower the batch ceiling; zero keeps automatic.
  // This never removes channels or changes any allocator limit.
  uint32_t clutterBatchLimit = 0;
  ReferenceSpectrumConfig reference;
  PostprocessConfig postprocess;
  TrackerConfig tracker;
};
struct ResidentFrameOutputs {
  // Original unshifted layout: channel, delay, Doppler (FP32 diagnostic maps).
  std::complex<float>* maps = nullptr;
  size_t mapCount = 0;
  // Published row-major shifted Doppler, delay; FP64 configured fusion.
  std::complex<double>* fused = nullptr;
  size_t fusedCount = 0;
  // Optional diagnostics; candidates are invalid whenever finishRadar is false.
  std::complex<double>* filtered = nullptr;
  size_t filteredCount = 0;
  std::complex<double>* tail = nullptr;
  size_t tailCount = 0;
  PostprocessOutputs postprocess;
  TrackerOutputs tracker;
  uint64_t trackerTimestamp=0,consumedInputSequence=0;
};
namespace gpu_resident {
enum Status : uint32_t {
  error=0, referenceSamples=1, referenceLive=2, surveillanceSamples=3,
  subtractedSamples=4, surveillanceLive=5, shiftedCells=6, fusedCells=7,
  words=16
};
// Status is GPU-written coverage, not a count inferred from host submissions.
// Each workgroup's lane 0 counts its exact covered interval. This also handles
// dispatch rows, padding, inactive short-batch slots and nonmultiple tails.
const char* referenceSource=R"glsl(#version 450
#extension GL_ARB_gpu_shader_fp64 : require
layout(local_size_x=128) in;
layout(binding=0) readonly buffer Canonical { dvec2 raw[]; };
layout(binding=1) writeonly buffer Clutter { vec2 ref[]; };
layout(binding=2) buffer Status { uint s[]; };
layout(push_constant) uniform P { uint count; uint samples; uint unused; uint rotation; int ignored; } p;
void main(){
 uint i=gl_GlobalInvocationID.x+gl_GlobalInvocationID.y*gl_NumWorkGroups.x*128;
 if(i>=p.count)return;
 uint at=i>=p.rotation?i-p.rotation:i+(p.samples-p.rotation);
 dvec2 x=raw[at]; vec2 y=vec2(x);
 if(any(isnan(x))||any(isinf(x))||any(isnan(y))||any(isinf(y)))atomicCompSwap(s[0],0u,1u);
 ref[i]=y;
 if((i&127u)==0u)atomicAdd(s[1],min(128u,p.count-i));
})glsl";
const char* referencePackSource=R"glsl(#version 450
#extension GL_ARB_gpu_shader_fp64 : require
layout(local_size_x=128) in;
layout(binding=0) readonly buffer Canonical { dvec2 raw[]; };
layout(binding=1) writeonly buffer Reference { vec2 ref[]; };
layout(binding=2) buffer Status { uint s[]; };
layout(push_constant) uniform P { uint count; uint range; uint correlation; uint unused; int ignored; } p;
uint live(uint at){return (at/p.range)*p.correlation+min(at%p.range,p.correlation);}
void main(){
 uint i=gl_GlobalInvocationID.x+gl_GlobalInvocationID.y*gl_NumWorkGroups.x*128;
 if(i>=p.count)return;
 uint r=i%p.range; ref[i]=r<p.correlation?vec2(raw[(i/p.range)*p.correlation+r]):vec2(0);
 if((i&127u)==0u)atomicAdd(s[2],live(min(i+128u,p.count))-live(i));
})glsl";
const char* surveillanceSource=R"glsl(#version 450
#extension GL_ARB_gpu_shader_fp64 : require
layout(local_size_x=128) in;
layout(binding=0) readonly buffer Canonical { dvec2 raw[]; };
layout(binding=1) writeonly buffer Surveillance { vec2 surv[]; };
layout(binding=2) buffer Status { uint s[]; };
layout(push_constant) uniform P { uint count; uint samples; uint first; uint activeCount; int ignored; } p;
void main(){
 uint i=gl_GlobalInvocationID.x+gl_GlobalInvocationID.y*gl_NumWorkGroups.x*128;
 if(i>=p.count)return;
 if(i>=p.samples*p.activeCount){surv[i]=vec2(0);return;}
 dvec2 x=raw[p.first*p.samples+i]; vec2 y=vec2(x);
 if(any(isnan(x))||any(isinf(x))||any(isnan(y))||any(isinf(y)))atomicCompSwap(s[0],0u,2u);
 surv[i]=y;
 if((i&127u)==0u)atomicAdd(s[3],min(128u,p.samples*p.activeCount-i));
})glsl";
const char* subtractSource=R"glsl(#version 450
#extension GL_ARB_gpu_shader_fp64 : require
layout(local_size_x=128) in;
layout(binding=0) buffer Canonical { dvec2 raw[]; };
layout(binding=1) readonly buffer Estimate { vec2 estimate[]; };
layout(binding=2) buffer Status { uint s[]; };
layout(push_constant) uniform P { uint count; uint samples; uint first; uint unused; int ignored; } p;
void main(){
 uint i=gl_GlobalInvocationID.x+gl_GlobalInvocationID.y*gl_NumWorkGroups.x*128;
 if(i>=p.count)return;
 // Exactly the original parent boundary: original FP64 y - promoted FP32 z.
 precise dvec2 value=raw[p.first*p.samples+i]-dvec2(estimate[i]);
 if(any(isnan(value))||any(isinf(value)))atomicCompSwap(s[0],0u,3u);
 raw[p.first*p.samples+i]=value;
 if((i&127u)==0u)atomicAdd(s[4],min(128u,p.count-i));
})glsl";
const char* surveillancePackSource=R"glsl(#version 450
#extension GL_ARB_gpu_shader_fp64 : require
layout(local_size_x=128) in;
layout(binding=0) readonly buffer Canonical { dvec2 raw[]; };
layout(binding=1) writeonly buffer Surveillance { vec2 surv[]; };
layout(binding=2) buffer Status { uint s[]; };
layout(push_constant) uniform P { uint count; uint range; uint correlation; uint samples; int first; } p;
// The dispatch is one channel. Its output descriptor spans the complete frame.
uint live(uint at){return (at/p.range)*p.correlation+min(at%p.range,p.correlation);}
void main(){
 uint i=gl_GlobalInvocationID.x+gl_GlobalInvocationID.y*gl_NumWorkGroups.x*128;
 if(i>=p.count)return;
 uint r=i%p.range; vec2 value=vec2(0);
 if(r<p.correlation)value=vec2(raw[uint(p.first)*p.samples+(i/p.range)*p.correlation+r]);
 if(any(isnan(value))||any(isinf(value)))atomicCompSwap(s[0],0u,4u);
 surv[uint(p.first)*p.count+i]=value;
 if((i&127u)==0u)atomicAdd(s[5],live(min(i+128u,p.count))-live(i));
})glsl";
const char* fusionSource=R"glsl(#version 450
#extension GL_ARB_gpu_shader_fp64 : require
layout(local_size_x=128) in;
layout(binding=0) readonly buffer Doppler { vec2 maps[]; };
layout(binding=1) writeonly buffer Fused { dvec2 fused[]; };
layout(binding=2) buffer Status { uint s[]; };
layout(push_constant) uniform P { uint count; uint doppler; uint delays; uint channels; int ignored; } p;
void main(){
 uint i=gl_GlobalInvocationID.x+gl_GlobalInvocationID.y*gl_NumWorkGroups.x*128;
 if(i>=p.count)return;
 uint row=i/p.delays, col=i%p.delays;
 uint shifted=(row+p.doppler/2u+1u)%p.doppler;
 precise double power=0.0LF;
 for(uint c=0u;c<p.channels;++c){
  dvec2 value=dvec2(maps[c*p.count+col*p.doppler+shifted]);
  if(any(isnan(value))||any(isinf(value)))atomicCompSwap(s[0],0u,5u);
  precise double square=value.x*value.x+value.y*value.y;
  power=power+square;
 }
 precise double result=sqrt(power/double(p.channels));
 if(isnan(result)||isinf(result))atomicCompSwap(s[0],0u,6u);
 fused[i]=dvec2(result,0.0LF);
 if((i&127u)==0u){uint covered=min(128u,p.count-i);atomicAdd(s[6],covered*p.channels);atomicAdd(s[7],covered);}
})glsl";
const char* coherentFusionSource=R"glsl(#version 450
#extension GL_ARB_gpu_shader_fp64 : require
layout(local_size_x=128) in;
layout(binding=0) readonly buffer Doppler { vec2 maps[]; };
layout(binding=1) writeonly buffer Fused { dvec2 fused[]; };
layout(binding=2) buffer Status { uint s[]; };
layout(binding=3) readonly buffer Weights { vec2 weights[]; };
layout(push_constant) uniform P { uint count; uint doppler; uint delays; uint channels; int beams; } p;
void main(){
 uint i=gl_GlobalInvocationID.x+gl_GlobalInvocationID.y*gl_NumWorkGroups.x*128;
 if(i>=p.count)return;
 uint row=i/p.delays, col=i%p.delays;
 uint shifted=(row+p.doppler/2u+1u)%p.doppler;
 precise double best=-1.0LF;
 for(uint b=0u;b<uint(p.beams);++b){
  dvec2 sum=dvec2(0.0LF);
  for(uint c=0u;c<p.channels;++c){
   dvec2 value=dvec2(maps[c*p.count+col*p.doppler+shifted]);
   dvec2 weight=dvec2(weights[b*p.channels+c]);
   if(any(isnan(value))||any(isinf(value)))atomicCompSwap(s[0],0u,5u);
   sum+=dvec2(weight.x*value.x-weight.y*value.y,
              weight.x*value.y+weight.y*value.x);
  }
  precise double power=sum.x*sum.x+sum.y*sum.y;
  if(power>best)best=power;
 }
 precise double result=sqrt(best);
 if(isnan(result)||isinf(result))atomicCompSwap(s[0],0u,6u);
 // Keep the same real-amplitude interface as noncoherent fusion.
 fused[i]=dvec2(result,0.0LF);
 if((i&127u)==0u){uint covered=min(128u,p.count-i);atomicAdd(s[6],covered*p.channels);atomicAdd(s[7],covered);}
})glsl";
struct Commands {
 Context& context;
 std::vector<VkCommandBuffer> handles;
 explicit Commands(Context& value):context(value){}
 Commands(const Commands&)=delete;
 Commands& operator=(const Commands&)=delete;
 ~Commands(){for(auto command:handles)if(command)vkFreeCommandBuffers(context.device,context.pool,1,&command);}
 void allocate(uint32_t count){
  handles.assign(count,VK_NULL_HANDLE);
  VkCommandBufferAllocateInfo info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  info.commandPool=context.pool;
  info.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  info.commandBufferCount=count;
  check(vkAllocateCommandBuffers(context.device,&info,handles.data()),"resident commands");
 }
};
inline void hostWrite(VkCommandBuffer command){
 VkMemoryBarrier b{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
 b.srcAccessMask=VK_ACCESS_HOST_WRITE_BIT;
 b.dstAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;
 vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_HOST_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,1,&b,0,nullptr,0,nullptr);
}
inline void hostRead(VkCommandBuffer command){
 VkMemoryBarrier b{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
 b.srcAccessMask=VK_ACCESS_MEMORY_WRITE_BIT;
 b.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
 vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&b,0,nullptr,0,nullptr);
}
inline ResidentFrameConfig validate(ResidentFrameConfig config){
 const auto& g=config.geometry;
 const bool supplied=config.reference.mode==ReferenceMode::supplied;
 const uint32_t expectedStages=(supplied?residentSupportedStages:residentCapturedSupportedStages)|
  (config.postprocess.enabled?residentMapDisplay:0u)|(config.postprocess.detection?residentDetection:0u)|(config.tracker.enabled?residentTracker:0u);
 if(config.postprocess.tracker||(!config.postprocess.enabled&&config.postprocess.detection)||(config.tracker.enabled&&(!config.postprocess.enabled||!config.postprocess.detection))||
    config.clutterBatchLimit>g.channels || config.stages!=expectedStages || config.middle!=0 || !std::isfinite(config.sampleRate) || !(config.sampleRate>0))
  throw std::invalid_argument("GPU_RESIDENT_UNSUPPORTED stage mask or nonzero middle");
 if(config.fusion.mode>2 || (config.fusion.mode!=0 &&
    (config.fusion.beams<4||config.fusion.beams>360||!std::isfinite(config.fusion.frequencyHz)||
     config.fusion.frequencyHz<=0||!std::isfinite(config.fusion.radiusM)||config.fusion.radiusM<=0||
     !std::isfinite(config.fusion.element0Rad)||!std::isfinite(config.fusion.elementStepRad)||
     config.fusion.elementStepRad==0)) ||
    (config.fusion.mode==0&&(config.fusion.beams||config.fusion.frequencyHz||config.fusion.radiusM||
     config.fusion.element0Rad||config.fusion.elementStepRad||
     config.fusion.directPathAzimuthRad)) ||
    (config.fusion.mode==1&&config.fusion.directPathAzimuthRad!=0))
  throw std::invalid_argument("GPU_RESIDENT_UNSUPPORTED surveillance fusion");
 if(g.kind!=GpuWorkKind::radar || g.firSamples || g.firTaps || g.firFft || g.firPercent ||
    !g.range || g.range>65535 || !g.doppler || g.doppler>65535 || !g.delays || g.delays>65535 ||
    !g.channels || g.channels>8 || !g.clutterSamples || g.clutterSamples>INT32_MAX ||
    !g.clutterBins || g.clutterBins>gpu_native::fft || g.clutterBins>g.clutterSamples ||
    !config.correlation || config.correlation>g.range ||
    uint64_t(config.correlation)*g.doppler>g.clutterSamples ||
    g.delayMin<=-int64_t(config.correlation) || int64_t(g.delayMin)+g.delays-1>=config.correlation)
  throw std::invalid_argument("GPU_RESIDENT_UNSUPPORTED geometry");
 const uint64_t nc=uint64_t(g.clutterSamples)*g.channels;
 const uint64_t rc=uint64_t(g.range)*g.doppler*g.channels;
 const uint64_t mc=uint64_t(g.delays)*g.doppler*g.channels;
 if(nc>UINT32_MAX/2 || rc>UINT32_MAX/2 || mc>UINT32_MAX/2 || nc>SIZE_MAX/16)
  throw std::invalid_argument("GPU_RESIDENT_UNSUPPORTED index capacity");
 if(!supplied)gpu_reference::validate(config.reference,g.clutterSamples,g.channels,config.sampleRate);
 if(config.postprocess.enabled)gpu_postprocess::validate(config.postprocess,g.doppler,g.delays,config.sampleRate);
 if(config.tracker.enabled){gpu_tracker::validate(config.tracker,g.doppler*g.delays);
  const double actualCpi=(double(config.correlation)*g.doppler)/config.sampleRate;
  if(!gpu_tracker::sameDouble(config.tracker.cpi,actualCpi))throw std::invalid_argument("GPU_RESIDENT_UNSUPPORTED tracker actual CPI mismatch");}
 return config;
}
struct HostRange { uintptr_t first=0,last=0; };
inline HostRange hostRange(const void* p,size_t count,size_t element){
 if(!p || !count || count>SIZE_MAX/element)throw std::invalid_argument("Resident output dimensions changed");
 const uintptr_t begin=reinterpret_cast<uintptr_t>(p);
 const size_t bytes=count*element;
 if(bytes>UINTPTR_MAX-begin)throw std::invalid_argument("Resident host address overflow");
 return {begin,begin+bytes};
}
inline bool overlaps(HostRange a,HostRange b){return a.first<b.last && b.first<a.last;}
} // namespace gpu_resident

class ResidentFramePipeline {
 friend class ResidentModule;
 using F=std::complex<float>;
 using D=std::complex<double>;
 const ResidentFrameConfig config_; // Validate unsupported modes before device allocation.
 Context context_;
 std::unique_ptr<Buffer> reference_,surveillance_,doppler_,output_;
 std::unique_ptr<Plan> referencePlan_,rangePlan_,dopplerPlan_;
 std::unique_ptr<Kernel> multiply_,gather_;
 struct Workspace {
  std::unique_ptr<ClutterPipeline> clutter;
  std::unique_ptr<Buffer> canonical,fused,status,fusionWeights;
  // Stage descriptors/plans die before the canonical and borrowed native storage.
  std::unique_ptr<gpu_reference::Stage> referenceStage;
  std::unique_ptr<gpu_postprocess::Stage> postprocessStage;
  std::unique_ptr<Kernel> referenceCast,referencePack,surveillanceCast,subtract,surveillancePack,fusion;
  std::unique_ptr<gpu_native::Timestamps> timestamps;
  gpu_resident::Commands commands; // Destroy commands before anything they reference.
  const uint32_t capacity;
  Workspace(ResidentFramePipeline& owner,uint32_t channels):commands(owner.context_),capacity(channels){
   auto& context=owner.context_;
   const auto& config=owner.config_;
   auto g=config.geometry;
   g.channels=channels;
   clutter=std::make_unique<ClutterPipeline>(context,g,false,false,true);
   if(config.reference.mode!=ReferenceMode::supplied)
    referenceStage=std::make_unique<gpu_reference::Stage>(context,config.reference,g.clutterSamples,config.geometry.channels,config.sampleRate,clutter->borrowedReference(),clutter->borrowedWork());
   constexpr auto host=VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
   constexpr auto preferred=VK_MEMORY_PROPERTY_HOST_COHERENT_BIT|VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
   // Late allocations coexist with initialized full ambiguity and clutter plans.
   // Their requirements (not requested bytes) remain charged to the same caps.
   canonical=std::make_unique<Buffer>(context,uint64_t(g.clutterSamples)*config.geometry.channels*16,host,preferred);
   fused=std::make_unique<Buffer>(context,uint64_t(g.doppler)*g.delays*16,host,preferred);
   status=std::make_unique<Buffer>(context,gpu_resident::words*4,host,preferred);
   if(referenceStage)referenceStage->bind(*canonical);
   if(config.postprocess.enabled)postprocessStage=std::make_unique<gpu_postprocess::Stage>(context,config.postprocess,
    config.geometry.doppler,config.geometry.delays,uint32_t(config.sampleRate),*fused,clutter->borrowedReference(),*owner.output_);
   Buffer* referenceInput=referenceStage?&referenceStage->referenceBuffer():canonical.get();
   referenceCast=std::make_unique<Kernel>(context,gpu_resident::referenceSource,
     std::vector<Buffer*>{referenceInput,&clutter->residentReference(),status.get()});
   referencePack=std::make_unique<Kernel>(context,gpu_resident::referencePackSource,
     std::vector<Buffer*>{referenceInput,owner.reference_.get(),status.get()});
   surveillanceCast=std::make_unique<Kernel>(context,gpu_resident::surveillanceSource,
     std::vector<Buffer*>{canonical.get(),&clutter->residentSurveillance(),status.get()});
   subtract=std::make_unique<Kernel>(context,gpu_resident::subtractSource,
     std::vector<Buffer*>{canonical.get(),&clutter->residentEstimate(),status.get()});
   surveillancePack=std::make_unique<Kernel>(context,gpu_resident::surveillancePackSource,
     std::vector<Buffer*>{canonical.get(),owner.surveillance_.get(),status.get()});
   if(config.fusion.mode!=0){
    const uint64_t weightCount=uint64_t(config.fusion.beams)*config.geometry.channels;
    fusionWeights=std::make_unique<Buffer>(context,weightCount*sizeof(std::complex<float>),host,preferred);
    auto* target=static_cast<std::complex<float>*>(fusionWeights->mapped);
    const double waveNumber=6.283185307179586476925286766559*config.fusion.frequencyHz/299792458.0;
    const double normalization=1.0/std::sqrt(double(config.geometry.channels));
    for(uint32_t beam=0;beam<config.fusion.beams;++beam){
     const double beamAngle=6.283185307179586476925286766559*double(beam)/config.fusion.beams;
     for(uint32_t channel=0;channel<config.geometry.channels;++channel){
      const double elementAngle=config.fusion.element0Rad+channel*config.fusion.elementStepRad;
      const double phase=waveNumber*config.fusion.radiusM*std::cos(elementAngle-beamAngle);
      target[uint64_t(beam)*config.geometry.channels+channel]=
       std::complex<float>(normalization*std::cos(phase),normalization*std::sin(phase));
     }
    }
    fusionWeights->flush(0,fusionWeights->bytes);
    fusion=std::make_unique<Kernel>(context,gpu_resident::coherentFusionSource,
      std::vector<Buffer*>{owner.doppler_.get(),fused.get(),status.get(),fusionWeights.get()});
   }else fusion=std::make_unique<Kernel>(context,gpu_resident::fusionSource,
     std::vector<Buffer*>{owner.doppler_.get(),fused.get(),status.get()});
   timestamps=std::make_unique<gpu_native::Timestamps>(context);
   const uint32_t batches=(config.geometry.channels+channels-1)/channels;
   commands.allocate(2+2*batches);
   VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
   auto command=commands.handles[0];
   check(vkBeginCommandBuffer(command,&begin),"resident reference recording");
   gpu_resident::hostWrite(command);
   barrier(command);
   if(referenceStage)referenceStage->append(command);
   timestamps->begin(command,0);
   const uint32_t n=g.clutterSamples;
   int64_t rotation=int64_t(g.clutterDelayMin)%n;
   if(rotation<0)rotation+=n;
   referenceCast->append(command,{n,n,0,uint32_t(rotation),0});
   barrier(command);
   referencePack->append(command,{g.range*g.doppler,g.range,config.correlation,0,0});
   barrier(command);
   timestamps->end(command,0);
   gpu_resident::hostRead(command);
   check(vkEndCommandBuffer(command),"resident reference recording");
   for(uint32_t batch=0,first=0;first<config.geometry.channels;++batch,first+=channels){
    const uint32_t active=std::min(channels,config.geometry.channels-first);
    command=commands.handles[2+2*batch];
    check(vkBeginCommandBuffer(command,&begin),"resident input recording");
    gpu_resident::hostWrite(command);
    barrier(command);
    timestamps->begin(command,1);
    surveillanceCast->append(command,{n*channels,n,first,active,0});
    barrier(command);
    timestamps->end(command,1);
    gpu_resident::hostRead(command);
    check(vkEndCommandBuffer(command),"resident input recording");
    command=commands.handles[3+2*batch];
    check(vkBeginCommandBuffer(command,&begin),"resident subtraction recording");
    barrier(command);
    timestamps->begin(command,2);
    subtract->append(command,{n*active,n,first,0,0});
    barrier(command);
    // Separate dispatch after a full memory barrier: no in-place read/write race.
    for(uint32_t c=first;c<first+active;++c)
     surveillancePack->append(command,{g.range*g.doppler,g.range,config.correlation,n,int32_t(c)});
    barrier(command);
    timestamps->end(command,2);
    gpu_resident::hostRead(command);
    check(vkEndCommandBuffer(command),"resident subtraction recording");
   }
   command=commands.handles[1];
   check(vkBeginCommandBuffer(command,&begin),"resident ambiguity recording");
   barrier(command);
   timestamps->begin(command,3);
   // No legacy host-input copy here. Consume precisely the GPU-packed buffers.
   owner.referencePlan_->append(command,-1);
   owner.rangePlan_->append(command,-1);
   barrier(command);
   const auto& full=config.geometry;
   Push push{full.range*full.doppler*full.channels,full.range,full.doppler,full.delays,full.delayMin};
   owner.multiply_->append(command,push);
   barrier(command);
   owner.rangePlan_->append(command,1);
   barrier(command);
   push.count=full.doppler*full.delays*full.channels;
   owner.gather_->append(command,push);
   barrier(command);
   owner.dopplerPlan_->append(command,-1);
   barrier(command);
   timestamps->end(command,3);
   timestamps->begin(command,4);
   fusion->append(command,{full.doppler*full.delays,full.doppler,full.delays,full.channels,
    int32_t(config.fusion.mode!=0?config.fusion.beams:0)});
   barrier(command);
   VkBufferCopy mapCopy{0,0,owner.doppler_->bytes};
   vkCmdCopyBuffer(command,owner.doppler_->handle,owner.output_->handle,1,&mapCopy);
   barrier(command);
   timestamps->end(command,4);
   // All native baseline/reference consumers are finished. The postprocess
   // scratch borrows that storage; existing map staging stays intact until copy.
   if(postprocessStage)postprocessStage->append(command);
   gpu_resident::hostRead(command);
   check(vkEndCommandBuffer(command),"resident ambiguity recording");
  }
  void calibrateFusion(const ReferenceCheckpoint& checkpoint){
   if(config.fusion.mode!=2)return;
   if(checkpoint.weightCount!=config.geometry.channels)
    throw std::runtime_error("Calibrated coherent fusion reference weight count changed");
   double norm=0;
   for(uint32_t channel=0;channel<config.geometry.channels;++channel){
    const auto weight=checkpoint.weights[channel];
    if(!std::isfinite(weight.real())||!std::isfinite(weight.imag()))
     throw std::runtime_error("Calibrated coherent fusion received nonfinite reference weights");
    norm+=std::norm(weight);
   }
   if(!(norm>0)||!std::isfinite(norm))
    throw std::runtime_error("Calibrated coherent fusion received zero-norm reference weights");
   const double scale=1.0/std::sqrt(norm);
   const double waveNumber=6.283185307179586476925286766559*config.fusion.frequencyHz/299792458.0;
   auto* target=static_cast<std::complex<float>*>(fusionWeights->mapped);
   for(uint32_t beam=0;beam<config.fusion.beams;++beam){
    const double beamAngle=6.283185307179586476925286766559*double(beam)/config.fusion.beams;
    for(uint32_t channel=0;channel<config.geometry.channels;++channel){
     const double elementAngle=config.fusion.element0Rad+channel*config.fusion.elementStepRad;
     const double relativePhase=waveNumber*config.fusion.radiusM*
      (std::cos(elementAngle-beamAngle)-
       std::cos(elementAngle-config.fusion.directPathAzimuthRad));
     target[uint64_t(beam)*config.geometry.channels+channel]=
      std::complex<float>(scale*std::conj(checkpoint.weights[channel])*
       std::polar(1.0,relativePhase));
    }
   }
   fusionWeights->flush(0,fusionWeights->bytes);
  }
 };
 std::unique_ptr<Workspace> work_; // Released before full ambiguity/context.
 std::unique_ptr<gpu_tracker::Stage> tracker_; // Dies before borrowed postprocess/native storage.
 bool prepared_=false,poisoned_=false;
 uint64_t generation_=0;
 std::array<uint32_t,16> readyEvidence_{},finalEvidence_{};
 struct BatchSnapshot {std::array<uint32_t,16> status{};
 double packMs=0,subtractMs=0;};
 std::array<BatchSnapshot,8> batchEvidence_{};
 double evidenceHostResultMs_=0;
 gpu_resident::HostRange referenceHostRange_{};
 const D* capturedInput_=nullptr; // Address only: no host re-read after upload.
 size_t capturedCount_=0;
 std::chrono::steady_clock::time_point frameStart_{};
 gpu_host::Frame hostTiming_;
 void submit(VkCommandBuffer command,const char* phase){
  VkSubmitInfo info{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  info.commandBufferCount=1;
  info.pCommandBuffers=&command;
  check(gpu_host::call(context_.hostTiming,"vkQueueSubmit",0,[&]{return vkQueueSubmit(context_.queue,1,&info,context_.fence);}),phase);
  check(gpu_host::call(context_.hostTiming,"vkWaitForFences",0,[&]{return vkWaitForFences(context_.device,1,&context_.fence,VK_TRUE,UINT64_MAX);}),phase);
  check(gpu_host::call(context_.hostTiming,"vkResetFences",0,[&]{return vkResetFences(context_.device,1,&context_.fence);}),"resident fence reset");
 }
 std::array<uint32_t,gpu_resident::words> status(){
  std::array<uint32_t,gpu_resident::words> value{};
  work_->status->invalidate(0,work_->status->bytes);
  std::memcpy(value.data(),work_->status->mapped,work_->status->bytes);
  if(value[gpu_resident::error])throw ClutterRejected("GPU resident nonfinite stage="+std::to_string(value[0]));
  return value;
 }
 bool reject(const std::exception& error,const char* phase){
  if(tracker_&&tracker_->pending())tracker_->abort(tracker_->preparedToken());
  context_.hostTiming=nullptr;
  hostTiming_.abandon();
  prepared_=false;
  work_->clutter->invalidateResidentFrame();
  context_.diagnostics.stream()<<"GPU_RESIDENT_END frame="<<generation_<<" valid=0 phase="<<phase<<" reason="<<error.what()<<'\n';
  context_.diagnostics.flush();
  return false;
 }
 void fail(){
  context_.hostTiming=nullptr;
  hostTiming_.abandon();
  poisoned_=true;
  prepared_=false;
  work_->clutter->invalidateResidentFrame();
  context_.diagnostics.stream()<<"GPU_RESIDENT_END frame="<<generation_<<" valid=0 phase=exception poisoned=1\n";
  context_.diagnostics.flush();
 }
 void validateOutputs(const D* input,size_t count,const ResidentFrameOutputs& out){
  const auto& g=config_.geometry;
  const size_t maps=size_t(g.doppler)*g.delays;
  const size_t tail=size_t(g.clutterSamples-uint64_t(config_.correlation)*g.doppler)*g.channels;
  if(count!=size_t(g.clutterSamples)*g.channels || out.mapCount!=maps*g.channels || out.fusedCount!=maps ||
    ((out.filtered!=nullptr || out.filteredCount!=0) && (!out.filtered || out.filteredCount!=count)) ||
    ((out.tail!=nullptr || out.tailCount!=0) && (!out.tail || out.tailCount!=tail || !tail)))
   throw std::invalid_argument("Resident output dimensions changed");
  std::vector<gpu_resident::HostRange> ranges{gpu_resident::hostRange(input,count,16),
   gpu_resident::hostRange(out.maps,out.mapCount,8),gpu_resident::hostRange(out.fused,out.fusedCount,16)};
  if(out.filtered)ranges.push_back(gpu_resident::hostRange(out.filtered,out.filteredCount,16));
  if(out.tail)ranges.push_back(gpu_resident::hostRange(out.tail,out.tailCount,16));
  const auto& post=out.postprocess;
  if(work_->postprocessStage){
   work_->postprocessStage->validateOutputs(post);
   ranges.push_back(gpu_resident::hostRange(post.metrics,1,sizeof(PostprocessMetrics)));
   ranges.push_back(gpu_resident::hostRange(post.display,post.displayCount,8));
   if(post.detections)ranges.push_back(gpu_resident::hostRange(post.detections,post.detectionCapacity,32));
   if(post.levels)ranges.push_back(gpu_resident::hostRange(post.levels,post.levelCount,8));
   if(post.unflooredSnr)ranges.push_back(gpu_resident::hostRange(post.unflooredSnr,post.unflooredSnrCount,8));
   if(post.cfar)ranges.push_back(gpu_resident::hostRange(post.cfar,post.cfarCapacity,32));
   if(post.centroid)ranges.push_back(gpu_resident::hostRange(post.centroid,post.centroidCapacity,32));
   if(post.decisionFlags)ranges.push_back(gpu_resident::hostRange(post.decisionFlags,post.decisionFlagCount,4));
   if(post.detectionCells)ranges.push_back(gpu_resident::hostRange(post.detectionCells,post.detectionCellCapacity,4));
  }else if(post.metrics||post.display||post.displayCount||post.detections||post.detectionCapacity||post.levels||post.levelCount||
           post.unflooredSnr||post.unflooredSnrCount||post.cfar||post.cfarCapacity||post.centroid||post.centroidCapacity||
           post.decisionFlags||post.decisionFlagCount||post.detectionCells||post.detectionCellCapacity)
   throw std::invalid_argument("Disabled postprocess output supplied");
  if(tracker_){tracker_->checkOutputs(out.tracker);
   ranges.push_back(gpu_resident::hostRange(out.tracker.checkpoint,out.tracker.wordCapacity,4));
   ranges.push_back(gpu_resident::hostRange(out.tracker.result,1,sizeof(TrackerResult)));
  }else if(out.tracker.checkpoint||out.tracker.wordCapacity||out.tracker.result||out.trackerTimestamp||out.consumedInputSequence)
   throw std::invalid_argument("Disabled tracker output supplied");
  for(size_t i=0;i<ranges.size();++i)for(size_t j=0;j<i;++j)
   if(gpu_resident::overlaps(ranges[i],ranges[j]))throw std::invalid_argument("Resident host buffers overlap");
  for(size_t i=1;i<ranges.size();++i)
    if(gpu_resident::overlaps(ranges[i],referenceHostRange_))throw std::invalid_argument("Resident output aliases canonical reference");
 }
 void requireCaptured(bool idle)const{
  if(!work_->referenceStage || poisoned_ || (idle&&(prepared_||(tracker_&&tracker_->pending()))))
   throw std::logic_error("Resident captured-reference phase invalid");
 }
 void validateReferenceOutputs(const D* input,size_t count,const ReferenceOutputs& out){
  const auto& g=config_.geometry;
  const size_t bins=work_->referenceStage->spectrumCount();
  if(count!=size_t(g.clutterSamples)*g.channels || !out.checkpoint || out.spectrumCount!=bins || out.spectrumDbCount!=bins ||
   ((out.reference || out.referenceCount) && (!out.reference || out.referenceCount!=g.clutterSamples)))
   throw std::invalid_argument("Captured reference output dimensions changed");
  std::vector<gpu_resident::HostRange> ranges{gpu_resident::hostRange(input,count,16),
   gpu_resident::hostRange(out.checkpoint,1,sizeof(ReferenceCheckpoint)),
   gpu_resident::hostRange(out.spectrum,bins,16),gpu_resident::hostRange(out.spectrumDb,bins,8)};
  if(out.reference)ranges.push_back(gpu_resident::hostRange(out.reference,out.referenceCount,16));
  for(size_t i=0;i<ranges.size();++i)for(size_t j=0;j<i;++j)
   if(gpu_resident::overlaps(ranges[i],ranges[j]))throw std::invalid_argument("Captured reference host buffers overlap");
  // The diagnostic uses existing map staging. Reject pathological transfer counts
  // before dispatch; do not allocate another N-double reference readback.
  const uint64_t bytes=uint64_t(g.clutterSamples)*16;
  if(out.reference && (bytes+output_->bytes-1)/output_->bytes>4096)
   throw std::invalid_argument("Captured reference diagnostic transfer count unsupported");
 }
 void copyReferenceDiagnostic(D* target){
  auto& source=work_->referenceStage->referenceBuffer();
  const uint64_t bytes=uint64_t(config_.geometry.clutterSamples)*16;
  for(uint64_t offset=0;offset<bytes;){
   const uint64_t chunk=std::min(output_->bytes,bytes-offset);
   gpu_reference::CommandOwner copy(context_);
   copy.allocate();
   VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
   check(vkBeginCommandBuffer(copy.command,&begin),"reference diagnostic recording");
   barrier(copy.command);
   VkBufferCopy region{offset,0,chunk};
   vkCmdCopyBuffer(copy.command,source.handle,output_->handle,1,&region);
   barrier(copy.command);
   gpu_resident::hostRead(copy.command);
   check(vkEndCommandBuffer(copy.command),"reference diagnostic recording");
   submit(copy.command,"reference diagnostic readback");
   output_->invalidate(0,chunk);
   std::memcpy(reinterpret_cast<unsigned char*>(target)+offset,output_->mapped,size_t(chunk));
   offset+=chunk;
  }
 }
public:
 ResidentFramePipeline(std::shared_ptr<Instance> instance,Candidate candidate,ResidentFrameConfig config)
  :config_(gpu_resident::validate(config)),context_(std::move(instance),std::move(candidate)){
  const auto& g=config_.geometry;
  const uint64_t r=uint64_t(g.range)*g.doppler,m=uint64_t(g.doppler)*g.delays;
  const uint64_t ambiguityBytes=(r*(1+g.channels)+2*m*g.channels)*8;
  const uint64_t lateBytes=uint64_t(g.clutterSamples)*g.channels*16+m*16+gpu_resident::words*4;
  const auto budget=gpu_memory::heapBudget(context_.candidate.info.memoryBytes);
  const auto& limit=context_.candidate.properties.limits;
  if(ambiguityBytes>budget || lateBytes>budget-ambiguityBytes ||
    std::max({r*g.channels*8,m*g.channels*8,uint64_t(g.clutterSamples)*g.channels*16,m*16})>limit.maxStorageBufferRange ||
    limit.maxComputeWorkGroupInvocations<128 || limit.maxComputeWorkGroupSize[0]<128 || limit.maxComputeWorkGroupCount[0]<65535)
   throw std::runtime_error("GPU_RESIDENT_UNAVAILABLE capacity");
  constexpr auto device=VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
  constexpr auto host=VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
  reference_=std::make_unique<Buffer>(context_,r*8,device);
  surveillance_=std::make_unique<Buffer>(context_,r*g.channels*8,device);
  doppler_=std::make_unique<Buffer>(context_,m*g.channels*8,device);
  output_=std::make_unique<Buffer>(context_,m*g.channels*8,host,VK_MEMORY_PROPERTY_HOST_COHERENT_BIT|VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
  referencePlan_=std::make_unique<Plan>(context_,*reference_,g.range,g.doppler);
  rangePlan_=std::make_unique<Plan>(context_,*surveillance_,g.range,g.doppler*g.channels);
  dopplerPlan_=std::make_unique<Plan>(context_,*doppler_,g.doppler,g.delays*g.channels);
  multiply_=std::make_unique<Kernel>(context_,multiplySource,std::vector<Buffer*>{reference_.get(),surveillance_.get()});
  gather_=std::make_unique<Kernel>(context_,gatherSource,std::vector<Buffer*>{surveillance_.get(),doppler_.get()});
  std::string reason="preflight";
  const uint32_t ceiling=config_.clutterBatchLimit?config_.clutterBatchLimit:g.channels;
  for(uint32_t c=ceiling;c>0;--c){
   auto batch=g;
   batch.channels=c;
   if(ClutterPipeline::requiredBytes(batch,true)>budget-ambiguityBytes-lateBytes)continue;
   try {work_=std::make_unique<Workspace>(*this,c);
   break;}
   catch(const std::exception& error){reason=error.what();
   startupTrace(("resident retry batch="+std::to_string(c)+" reason="+reason).c_str());}
  }
  if(!work_)throw std::runtime_error("GPU_RESIDENT_UNAVAILABLE "+reason);
  // Tracker capacity is explicit and never retried/shrunk. All existing full-frame
  // allocations, plans and transient peaks have already been admitted.
  if(config_.tracker.enabled)tracker_=std::make_unique<gpu_tracker::Stage>(context_,config_.tracker,uint32_t(m),
   work_->postprocessStage->metadataBuffer(),work_->clutter->borrowedReference(),work_->postprocessStage->scratchLayout());
  {std::lock_guard<std::mutex> lock(allocationsMutex);
  const auto& b=deviceAllocations.at(context_.device).budget;
   std::cerr<<"GPU_RESIDENT_READY channels="<<g.channels<<" samples="<<g.clutterSamples<<" capacity="<<work_->capacity
    <<" canonical_bytes="<<work_->canonical->bytes<<" used="<<b.used()<<" peak="<<b.peak()<<" limit="<<b.limit()
    <<" supplied_reference="<<(config_.reference.mode==ReferenceMode::supplied?1:0)<<" stages="<<config_.stages<<" cpu_bulk_calls=0 cpu_solve_calls=0"
    <<" diagnostic_capacity_bytes="<<gpu_diagnostics::FrameLog::capacity<<" diagnostic_host_bytes="<<sizeof(gpu_diagnostics::FrameLog)<<'\n';}
 }
 GpuDevice device()const{return context_.candidate.info;}
 uint32_t batchCapacity()const{return work_->capacity;}
 // All calls are synchronous. Reset cannot free storage under an active command.
 void reset(){if(poisoned_||(tracker_&&tracker_->pending()))throw std::logic_error("Resident reset while poisoned or tracker pending");
 context_.hostTiming=nullptr;
 hostTiming_.abandon();
 prepared_=false;
 work_->clutter->endFrame();
 context_.diagnostics.flush();}
 const gpu_host::Frame& hostTiming()const{return hostTiming_;}
 void configureSubmitContextProbe(const char* path){
  if(prepared_||poisoned_||(tracker_&&tracker_->pending()))throw std::logic_error("Resident submit probe configuration requires healthy idle state");
  hostTiming_.configureSubmitContextProbe(path);
 }
 uint32_t trackerCheckpointWordCapacity()const{if(!tracker_)throw std::logic_error("Resident tracker disabled");
 return tracker_->checkpointWordCapacity();}
 void commitTracker(TrackerToken token){
  if(!tracker_||prepared_||poisoned_)throw std::logic_error("Resident tracker commit phase invalid");
  try{tracker_->commit(token);}catch(const std::logic_error&){throw;}catch(...){fail();
  throw;}
 }
 void abortTracker(TrackerToken token){
  if(!tracker_||prepared_||poisoned_){throw std::logic_error("Resident tracker abort phase invalid");}
   tracker_->abort(token);
 }
 void copyTrackerCommitted(TrackerOutputs out){
  if(!tracker_||prepared_||poisoned_||tracker_->pending())throw std::logic_error("Resident tracker checkpoint phase invalid");
  tracker_->checkOutputs(out);
  try{tracker_->copyCommitted(out);}catch(...){fail();
  throw;}
 }
 void restoreTracker(const TrackerConfig& envelope,const uint32_t* checkpoint,size_t words){
  if(!tracker_||prepared_||poisoned_||tracker_->pending())throw std::logic_error("Resident tracker restore phase invalid");
  try{tracker_->restore(envelope,checkpoint,words);}catch(const std::invalid_argument&){throw;}catch(const std::logic_error&){throw;}catch(...){fail();
  throw;}
 }
 uint32_t spectrumCount()const{requireCaptured(false);
 return work_->referenceStage->spectrumCount();}
 const std::vector<double>& spectrumFrequencies()const{requireCaptured(false);
 return work_->referenceStage->frequencies();}
 ReferenceCheckpoint exportReferenceCheckpoint(){
  requireCaptured(false);
  try{return work_->referenceStage->exportCheckpoint();}catch(...){fail();
  throw;}
 }
 // Restore is an explicit idle checkpoint operation. It never alters an already
 // prepared frame, nor attempts to revive a route with an unknown device failure.
 void restoreReferenceCheckpoint(const ReferenceCheckpoint& cp){
  requireCaptured(true);
  try{work_->referenceStage->restore(cp);work_->calibrateFusion(cp);}
  catch(const std::invalid_argument&){throw;}catch(...){fail();
  throw;}
 }
 void resetReferenceState(){requireCaptured(true);
 try{work_->referenceStage->reset();}catch(...){fail();
 throw;}}
 bool probePositiveLog(const double* input,size_t count,double* result){
  requireCaptured(true);
  try{return work_->referenceStage->probePositiveLog(input,count,result);}
  catch(const std::invalid_argument&){throw;}catch(...){fail();
  throw;}
 }
 bool prepareCaptured(const D* canonical,size_t count,ReferenceOutputs out){
  requireCaptured(true);
  validateReferenceOutputs(canonical,count,out);
  referenceHostRange_=gpu_resident::hostRange(canonical,count,16);
  capturedInput_=canonical;
  capturedCount_=count;
  readyEvidence_={};
  finalEvidence_={};
  batchEvidence_={};
  frameStart_=std::chrono::steady_clock::now();
  ++generation_;
  hostTiming_.begin(generation_);
  context_.hostTiming=&hostTiming_;
  context_.diagnostics.begin();
  bool committing=false;
  try{
   {gpu_host::Scope timer(context_.hostTiming,"captured.upload");
    std::memset(work_->status->mapped,0,work_->status->bytes);
    work_->status->flush(0,work_->status->bytes);
    // The only captured input upload. finishResidentRadar uses this allocation.
    std::memcpy(work_->canonical->mapped,canonical,count*16);
    work_->canonical->flush(0,count*16);}
   {gpu_host::Scope timer(context_.hostTiming,"captured.submit");
   submit(work_->commands.handles[0],"captured reference and spectrum");}
   std::array<uint32_t,gpu_resident::words> s{};
   {gpu_host::Scope timer(context_.hostTiming,"captured.status_outputs");
   work_->referenceStage->checkStatus();
   s=status();
   if(s[gpu_resident::referenceSamples]!=config_.geometry.clutterSamples ||
      s[gpu_resident::referenceLive]!=uint64_t(config_.correlation)*config_.geometry.doppler)
    throw std::runtime_error("Captured reference coverage mismatch");
   if(out.reference)copyReferenceDiagnostic(out.reference);
   work_->referenceStage->copySpectrum(out);}
   // No native cache/baseline writer may run before every reader above finishes.
   // Commit after all numeric checks; later radar failure preserves this state.
   {gpu_host::Scope timer(context_.hostTiming,"captured.commit");
   committing=true;
   *out.checkpoint=work_->referenceStage->commit();
   work_->calibrateFusion(*out.checkpoint);
   prepared_=true;}
   {gpu_host::Scope timer(context_.hostTiming,"captured.query_log");
   work_->referenceStage->log(generation_);
   context_.diagnostics.stream()<<std::setprecision(12)<<"GPU_RESIDENT_REFERENCE frame="<<generation_<<" device_cast_samples="<<s[1]
    <<" device_packed_live="<<s[2]<<" gpu_pack_ms="<<work_->timestamps->milliseconds(0)
    <<" cpu_reference_cast_calls=0 cpu_mix_calls=0\n";}
   readyEvidence_=s;
   return true;
  }catch(const ClutterRejected& e){if(committing){fail();
  throw;}return reject(e,"captured_reference");}
   catch(const std::domain_error& e){if(committing){fail();
   throw;}return reject(e,"captured_reference");}
   catch(...){fail();
   throw;}
 }
 bool finishResidentRadar(ResidentFrameOutputs out){
  requireCaptured(false);
  if(!prepared_)throw std::logic_error("Resident captured-radar phase invalid");
  validateOutputs(capturedInput_,capturedCount_,out);
  return executeRadar(nullptr,capturedCount_,out);
 }
 bool prepareReference(const D* reference,size_t count){
  if(config_.reference.mode!=ReferenceMode::supplied || poisoned_ || prepared_ || (tracker_&&tracker_->pending()))throw std::logic_error("Resident supplied-reference phase invalid");
  if(!reference || count!=config_.geometry.clutterSamples)throw std::invalid_argument("Resident reference dimensions changed");
  referenceHostRange_=gpu_resident::hostRange(reference,count,16);
  readyEvidence_={};
  finalEvidence_={};
  batchEvidence_={};
  frameStart_=std::chrono::steady_clock::now();
  ++generation_;
  hostTiming_.begin(generation_);
  context_.hostTiming=&hostTiming_;
  context_.diagnostics.begin();
  try {
   gpu_host::Scope timer(context_.hostTiming,"supplied.prepare");
   std::memset(work_->status->mapped,0,work_->status->bytes);
   work_->status->flush(0,work_->status->bytes);
   std::memcpy(work_->canonical->mapped,reference,count*16);
   work_->canonical->flush(0,count*16);
   submit(work_->commands.handles[0],"resident reference packing");
   const auto s=status();
   if(s[gpu_resident::referenceSamples]!=count || s[gpu_resident::referenceLive]!=uint64_t(config_.correlation)*config_.geometry.doppler)
    throw std::runtime_error("Resident reference coverage mismatch");
   // Fence above completes every canonical reference reader before raw reuse.
   prepared_=true;
   context_.diagnostics.stream()<<std::setprecision(12)<<"GPU_RESIDENT_REFERENCE frame="<<generation_<<" device_cast_samples="<<s[1]
    <<" device_packed_live="<<s[2]<<" gpu_pack_ms="<<work_->timestamps->milliseconds(0)
    <<" cpu_reference_cast_calls=0 cpu_mix_calls=0\n";
   return true;
  } catch(const ClutterRejected& e){return reject(e,"reference_pack");}
    catch(const std::domain_error& e){return reject(e,"reference_pack");}
    catch(...){fail();
    throw;}
 }
 bool finishRadar(const D* canonicalSurveillance,size_t count,ResidentFrameOutputs out){
  if(config_.reference.mode!=ReferenceMode::supplied || poisoned_ || !prepared_)throw std::logic_error("Resident supplied-radar phase invalid");
  validateOutputs(canonicalSurveillance,count,out);
  return executeRadar(canonicalSurveillance,count,out);
 }
private:
 // A null upload selects already resident captured input; no per-frame CPU DSP.
 bool executeRadar(const D* canonicalSurveillance,size_t count,ResidentFrameOutputs out){
  const auto& g=config_.geometry;
  const uint32_t n=g.clutterSamples,live=config_.correlation*g.doppler,tail=n-live;
  try {
   if(canonicalSurveillance){gpu_host::Scope timer(context_.hostTiming,"radar.upload");
   std::memcpy(work_->canonical->mapped,canonicalSurveillance,count*16);
   work_->canonical->flush(0,count*16);}
   bool nativeReady=false;
   {gpu_host::Scope timer(context_.hostTiming,"radar.native_reference");
   nativeReady=work_->clutter->beginFrameResident();}
   if(!nativeReady){
    prepared_=false;
    context_.hostTiming=nullptr;
    hostTiming_.abandon();
    context_.diagnostics.stream()<<"GPU_RESIDENT_END frame="<<generation_<<" valid=0 phase=native_reference\n";
    context_.diagnostics.flush();
    return false;
   }
   for(uint32_t first=0,batch=0;first<g.channels;first+=work_->capacity,++batch){
    const uint32_t active=std::min(work_->capacity,g.channels-first);
    std::array<uint32_t,gpu_resident::words> s{};
    double packMs=0;
    {gpu_host::Scope timer(context_.hostTiming,"radar.pack",first);
    submit(work_->commands.handles[2+2*batch],"resident surveillance packing");
    s=status();
     if(s[gpu_resident::surveillanceSamples]!=uint64_t(n)*(first+active))throw std::runtime_error("Resident input coverage mismatch");
     packMs=work_->timestamps->milliseconds(1);}
    bool batchReady=false;
    {gpu_host::Scope timer(context_.hostTiming,"radar.native_batch",first);
    batchReady=work_->clutter->processResident(first,active);}
    if(!batchReady){
     prepared_=false;
     context_.hostTiming=nullptr;
     hostTiming_.abandon();
     context_.diagnostics.stream()<<"GPU_RESIDENT_END frame="<<generation_<<" valid=0 phase=native_batch first="<<first<<'\n';
     context_.diagnostics.flush();
     return false;
    }
    {gpu_host::Scope timer(context_.hostTiming,"radar.subtract_pack",first);
    submit(work_->commands.handles[3+2*batch],"resident subtraction and ambiguity packing");
    s=status();}
    if(s[gpu_resident::subtractedSamples]!=uint64_t(n)*(first+active) || s[gpu_resident::surveillanceLive]!=uint64_t(live)*(first+active))
     throw std::runtime_error("Resident filtered coverage mismatch");
    if(out.filtered || out.tail){
     gpu_host::Scope timer(context_.hostTiming,"radar.filtered_tail_copy",first);
     work_->canonical->invalidate(uint64_t(first)*n*16,uint64_t(active)*n*16);
     const auto* filtered=static_cast<const D*>(work_->canonical->mapped);
     if(out.filtered)std::memcpy(out.filtered+size_t(first)*n,filtered+size_t(first)*n,size_t(active)*n*16);
     if(out.tail)for(uint32_t c=first;c<first+active;++c)
      std::memcpy(out.tail+size_t(c)*tail,filtered+size_t(c)*n+live,size_t(tail)*16);
    }
    {gpu_host::Scope timer(context_.hostTiming,"radar.batch_query_log",first);
    context_.diagnostics.stream()<<std::setprecision(12)<<"GPU_RESIDENT_BATCH frame="<<generation_<<" first="<<first<<" active="<<active
     <<" device_cast_total="<<s[3]<<" device_subtract_total="<<s[4]<<" device_packed_live_total="<<s[5]
     <<" gpu_pack_ms="<<packMs<<" gpu_subtract_pack_ms="<<work_->timestamps->milliseconds(2)
     <<" cpu_cast_calls=0 cpu_subtract_calls=0 cpu_ambiguity_pack_calls=0\n";
     if(batch>=batchEvidence_.size()){throw std::runtime_error("Resident batch evidence overflow");}
      batchEvidence_[batch]={s,packMs,work_->timestamps->observed(2)};}
   }
   {gpu_host::Scope timer(context_.hostTiming,"radar.ambiguity_postprocess_submit");
   submit(work_->commands.handles[1],"resident ambiguity and fusion");}
   std::array<uint32_t,gpu_resident::words> s{};
   {gpu_host::Scope timer(context_.hostTiming,"radar.map_status_copy");
   s=status();
   const uint64_t cells=uint64_t(g.doppler)*g.delays;
   if(s[gpu_resident::shiftedCells]!=cells*g.channels || s[gpu_resident::fusedCells]!=cells)
    throw std::runtime_error("Resident map coverage mismatch");
   if(work_->postprocessStage)work_->postprocessStage->checkStatus();
   output_->invalidate(0,output_->bytes);
   work_->fused->invalidate(0,work_->fused->bytes);
   std::memcpy(out.maps,output_->mapped,output_->bytes);
   std::memcpy(out.fused,work_->fused->mapped,work_->fused->bytes);}
   // Final resident detection records must be consumed before optional stage
   // reconstruction reuses them. A remains committed until explicit caller ack.
   if(tracker_){
    {gpu_host::Scope timer(context_.hostTiming,"radar.tracker_prepare");
     if(!tracker_->prepare({generation_,out.consumedInputSequence,out.trackerTimestamp}))throw ClutterRejected("GPU tracker numeric or capacity rejection");}
    {gpu_host::Scope timer(context_.hostTiming,"radar.tracker_copy");
    tracker_->copyPrepared(out.tracker);}
   }
   // Preserve both diagnostic map outputs before reusing the staging buffer.
   if(work_->postprocessStage){
    {gpu_host::Scope timer(context_.hostTiming,"radar.postprocess_copy");
    work_->postprocessStage->copyOutputs(out.postprocess);}
    {gpu_host::Scope timer(context_.hostTiming,"radar.postprocess_query_log");
    work_->postprocessStage->log(generation_);}}
   {gpu_host::Scope timer(context_.hostTiming,"radar.end_frame");
   prepared_=false;
   work_->clutter->endFrame();}
   const double wall=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-frameStart_).count();
   {gpu_host::Scope timer(context_.hostTiming,"radar.result_query_log");
   context_.diagnostics.stream()<<std::setprecision(12)<<"GPU_RESIDENT_END frame="<<generation_<<" valid=1 device_shifted_cells="<<s[6]
    <<" device_fused_cells="<<s[7]<<" gpu_ambiguity_ms="<<work_->timestamps->milliseconds(3)
    <<" gpu_fusion_readback_ms="<<work_->timestamps->milliseconds(4)<<" host_reference_to_result_ms="<<wall
    <<" cpu_bulk_calls=0 cpu_solve_calls=0 cpu_subtract_calls=0 cpu_fusion_calls=0 supplied_reference="<<(config_.reference.mode==ReferenceMode::supplied?1:0)<<" stages="<<config_.stages<<'\n';
    context_.diagnostics.flush();}
   finalEvidence_=s;
   evidenceHostResultMs_=wall;
   hostTiming_.finish();
   context_.hostTiming=nullptr;
   return true;
  } catch(const ClutterRejected& e){return reject(e,"radar");}
    catch(const std::domain_error& e){return reject(e,"radar");}
    catch(...){fail();
    throw;}
 }
};
