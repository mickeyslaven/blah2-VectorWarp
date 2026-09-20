#include "GpuPartialAmbiguity.h"
// The blocked-correlation and FIR experiments include the same pinned helper
// in other translation units. Give this instantiation private external names.
#define blah2 vectorwarp_ambig_gpu
#include "hardware-vulkan-support.h"
#undef blah2
#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstring>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
using Float=std::complex<float>;
using Clock=std::chrono::steady_clock;
double ms(Clock::time_point a,Clock::time_point b){
  return std::chrono::duration<double,std::milli>(b-a).count();
}
constexpr uint32_t samples=1000000,rows=301,nCorr=samples/rows,nfft=4096,delays=411;
constexpr int32_t delayMin=-10;
constexpr uint32_t gpuRows=GpuPartialAmbiguity::gpuRows;
static_assert(nCorr==3322 && rows*nCorr==999922 && gpuRows==75);
void barrier(VkCommandBuffer c,VkPipelineStageFlags from,VkPipelineStageFlags to,
    VkAccessFlags src,VkAccessFlags dst){
  VkMemoryBarrier b{VK_STRUCTURE_TYPE_MEMORY_BARRIER};b.srcAccessMask=src;b.dstAccessMask=dst;
  vkCmdPipelineBarrier(c,from,to,0,1,&b,0,nullptr,0,nullptr);
}
constexpr char multiplyShader[]=R"glsl(#version 450
layout(local_size_x=128) in;
layout(binding=0) readonly buffer Reference {vec2 referenceData[];};
layout(binding=1) buffer Surveillance {vec2 surveillanceData[];};
layout(push_constant) uniform P {uint count;uint range;uint doppler;uint delays;int delayMin;} p;
void main(){uint i=gl_GlobalInvocationID.x+gl_GlobalInvocationID.y*gl_NumWorkGroups.x*128;
 if(i>=p.count)return;
 vec2 a=surveillanceData[i],b=referenceData[i];
 surveillanceData[i]=vec2(a.x*b.x+a.y*b.y,a.y*b.x-a.x*b.y);
})glsl";
constexpr char gatherShader[]=R"glsl(#version 450
layout(local_size_x=128) in;
layout(binding=0) readonly buffer Surveillance {vec2 surveillanceData[];};
layout(binding=1) writeonly buffer Lags {vec2 lagData[];};
layout(push_constant) uniform P {uint count;uint range;uint doppler;uint delays;int delayMin;} p;
void main(){uint i=gl_GlobalInvocationID.x+gl_GlobalInvocationID.y*gl_NumWorkGroups.x*128;
 if(i>=p.count)return;
 uint row=i/p.delays, d=i%p.delays;
 int lag=p.delayMin+int(d);
 uint bin=uint(lag<0?int(p.range)+lag:lag);
 lagData[i]=surveillanceData[row*p.range+bin];
})glsl";
}

struct GpuPartialAmbiguity::Impl {
  std::shared_ptr<vectorwarp_ambig_gpu::Instance> owner;
  std::unique_ptr<vectorwarp_ambig_gpu::Context> context;
  std::unique_ptr<vectorwarp_ambig_gpu::Buffer> reference,surveillance,lags;
  std::unique_ptr<vectorwarp_ambig_gpu::Plan> referencePlan,surveillancePlan;
  std::unique_ptr<vectorwarp_ambig_gpu::Kernel> multiply,gather;
  VkCommandBuffer command=VK_NULL_HANDLE;
  bool active=false,poisoned=false;
  Clock::time_point submitted{};
  double packSubmitMs=0;
  std::vector<Float> cached;
  Impl(uint32_t count,uint32_t rowCount,uint32_t corr,uint32_t fft,
      uint32_t delayCount,int32_t firstLag,double middle){
    if(count!=samples||rowCount!=rows||corr!=nCorr||fft!=nfft||
       delayCount!=delays||firstLag!=delayMin||middle!=0)
      throw std::invalid_argument("Partial GPU ambiguity requires exact 1M/301/3322/4096/411/-10/center0 geometry");
    owner=std::make_shared<vectorwarp_ambig_gpu::Instance>();
    auto devices=vectorwarp_ambig_gpu::enumerate(*owner);
    const auto found=std::find_if(devices.begin(),devices.end(),[](const auto& d){
      return d.properties.vendorID==5348 && d.info.name.find("V3D")!=std::string::npos;});
    if(found==devices.end())throw std::runtime_error("Partial GPU ambiguity requires actual Pi V3D");
    context=std::make_unique<vectorwarp_ambig_gpu::Context>(owner,*found);
    auto& ctx=*context;
    constexpr auto required=VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT|VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
    constexpr auto preferred=VK_MEMORY_PROPERTY_HOST_COHERENT_BIT|VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
    reference=std::make_unique<vectorwarp_ambig_gpu::Buffer>(ctx,
      uint64_t(gpuRows)*nfft*sizeof(Float),required,preferred);
    surveillance=std::make_unique<vectorwarp_ambig_gpu::Buffer>(ctx,
      uint64_t(gpuRows)*nfft*sizeof(Float),required,preferred);
    lags=std::make_unique<vectorwarp_ambig_gpu::Buffer>(ctx,
      uint64_t(gpuRows)*delays*sizeof(Float),required,preferred);
    referencePlan=std::make_unique<vectorwarp_ambig_gpu::Plan>(ctx,*reference,nfft,gpuRows,true);
    surveillancePlan=std::make_unique<vectorwarp_ambig_gpu::Plan>(ctx,*surveillance,nfft,gpuRows,true);
    multiply=std::make_unique<vectorwarp_ambig_gpu::Kernel>(ctx,multiplyShader,
      std::vector<vectorwarp_ambig_gpu::Buffer*>{reference.get(),surveillance.get()});
    gather=std::make_unique<vectorwarp_ambig_gpu::Kernel>(ctx,gatherShader,
      std::vector<vectorwarp_ambig_gpu::Buffer*>{surveillance.get(),lags.get()});
    VkCommandBufferAllocateInfo a{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    a.commandPool=ctx.pool;a.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;a.commandBufferCount=1;
    vectorwarp_ambig_gpu::check(vkAllocateCommandBuffers(ctx.device,&a,&command),"partial ambiguity command allocation");
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    vectorwarp_ambig_gpu::check(vkBeginCommandBuffer(command,&begin),"partial ambiguity command begin");
    barrier(command,VK_PIPELINE_STAGE_HOST_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
      VK_ACCESS_HOST_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT);
    referencePlan->append(command,-1);surveillancePlan->append(command,-1);
    barrier(command,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
      VK_ACCESS_SHADER_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT);
    multiply->append(command,{gpuRows*nfft,nfft,rows,delays,delayMin});
    barrier(command,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
      VK_ACCESS_SHADER_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT);
    surveillancePlan->append(command,1);
    barrier(command,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
      VK_ACCESS_SHADER_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT);
    gather->append(command,{gpuRows*delays,nfft,rows,delays,delayMin});
    barrier(command,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_HOST_BIT,
      VK_ACCESS_SHADER_WRITE_BIT,VK_ACCESS_HOST_READ_BIT);
    vectorwarp_ambig_gpu::check(vkEndCommandBuffer(command),"partial ambiguity command end");
    cached.resize(gpuRows*delays);
    std::cerr<<"PARTIAL_AMBIGUITY status=active backend=vulkan device="<<found->info.name
      <<" gpu_rows="<<gpuRows<<" cpu_rows="<<rows-gpuRows
      <<" fft="<<nfft<<" lag_bytes="<<lags->bytes<<"\n";
  }
  ~Impl(){
    abort();
    if(context&&command)vkFreeCommandBuffers(context->device,context->pool,1,&command);
  }
  void abort() noexcept {
    if(!active||!context)return;
    const VkResult wait=vkWaitForFences(context->device,1,&context->fence,VK_TRUE,5000000000ULL);
    if(wait!=VK_SUCCESS || vkResetFences(context->device,1,&context->fence)!=VK_SUCCESS){
      std::cerr<<"PARTIAL_AMBIGUITY status=fatal phase=drain result="<<wait<<'\n';
      std::terminate();
    }
    active=false;
  }
  void start(const std::deque<Complex>& x,const std::deque<Complex>& y,
      const Complex* rotatedX,const Complex* filteredY,uint32_t fullSamples,int32_t rotation){
    if(poisoned||active||x.size()!=samples||y.size()!=samples||
       ((rotatedX||filteredY)&&(fullSamples!=samples||rotation!=delayMin))||
       (rotatedX&&!filteredY))
      throw std::runtime_error("Partial GPU ambiguity ownership/input geometry changed");
    const auto began=Clock::now();auto* r=static_cast<Float*>(reference->mapped);
    auto* s=static_cast<Float*>(surveillance->mapped);
    for(uint32_t row=0;row<gpuRows;++row){
      const uint32_t base=row*nCorr;
      for(uint32_t j=0;j<nCorr;++j){
        const uint32_t index=base+j;
        // The borrowed Wiener x is rotated by -delayMin. Reconstruct the
        // original reference index while packing, before FP32 conversion.
        uint32_t rotatedIndex=0;
        if(rotatedX){
          const uint32_t offset=uint32_t(-rotation); // fixed delayMin=-10
          rotatedIndex=index<offset ? fullSamples+index-offset : index-offset;
        }
        r[row*nfft+j]=Float(rotatedX ? rotatedX[rotatedIndex] : x[index]);
        s[row*nfft+j]=Float(filteredY ? filteredY[index] : y[index]);
      }
      std::fill(r+row*nfft+nCorr,r+(row+1)*nfft,Float{});
      std::fill(s+row*nfft+nCorr,s+(row+1)*nfft,Float{});
    }
    reference->flush(0,reference->bytes);surveillance->flush(0,surveillance->bytes);
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};submit.commandBufferCount=1;
    submit.pCommandBuffers=&command;submitted=Clock::now();
    const VkResult result=vkQueueSubmit(context->queue,1,&submit,context->fence);
    if(result!=VK_SUCCESS){poisoned=true;vectorwarp_ambig_gpu::check(result,"partial ambiguity submit");}
    active=true;packSubmitMs=ms(began,Clock::now());
  }
  void start_borrowed(const Complex* rotatedX, const Complex* filteredY,
      uint32_t fullSamples, int32_t rotation) {
    if (poisoned || active || !rotatedX || !filteredY || fullSamples != samples ||
        rotation != delayMin)
      throw std::runtime_error("Partial GPU ambiguity ownership/input geometry changed");
    const auto began=Clock::now(); auto* r=static_cast<Float*>(reference->mapped);
    auto* s=static_cast<Float*>(surveillance->mapped);
    for(uint32_t row=0;row<gpuRows;++row){
      const uint32_t base=row*nCorr;
      for(uint32_t j=0;j<nCorr;++j){
        const uint32_t index=base+j;
        const uint32_t offset=uint32_t(-rotation);
        const uint32_t rotatedIndex=index<offset ? fullSamples+index-offset : index-offset;
        r[row*nfft+j]=Float(rotatedX[rotatedIndex]);
        s[row*nfft+j]=Float(filteredY[index]);
      }
      std::fill(r+row*nfft+nCorr,r+(row+1)*nfft,Float{});
      std::fill(s+row*nfft+nCorr,s+(row+1)*nfft,Float{});
    }
    reference->flush(0,reference->bytes);surveillance->flush(0,surveillance->bytes);
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};submit.commandBufferCount=1;
    submit.pCommandBuffers=&command;submitted=Clock::now();
    const VkResult result=vkQueueSubmit(context->queue,1,&submit,context->fence);
    if(result!=VK_SUCCESS){poisoned=true;vectorwarp_ambig_gpu::check(result,"partial ambiguity submit");}
    active=true;packSubmitMs=ms(began,Clock::now());
  }
  GpuPartialAmbiguityTiming finish(std::vector<Complex>& output){
    if(poisoned||!active||output.size()!=rows*delays)
      throw std::runtime_error("Partial GPU ambiguity finish ownership/output geometry changed");
    const auto beforeWait=Clock::now();
    const VkResult wait=vkWaitForFences(context->device,1,&context->fence,VK_TRUE,5000000000ULL);
    if(wait!=VK_SUCCESS){poisoned=true;vectorwarp_ambig_gpu::check(wait,"partial ambiguity fence");}
    const auto done=Clock::now();
    const VkResult reset=vkResetFences(context->device,1,&context->fence);
    if(reset!=VK_SUCCESS){poisoned=true;vectorwarp_ambig_gpu::check(reset,"partial ambiguity fence reset");}
    active=false;
    lags->invalidate(0,lags->bytes);
    std::memcpy(cached.data(),lags->mapped,lags->bytes);
    // Reject the entire frame before writing even the first GPU-owned row.
    for(const auto& v:cached)
      if(!std::isfinite(v.real())||!std::isfinite(v.imag())){
        poisoned=true;throw std::runtime_error("Non-finite partial GPU ambiguity lag");
      }
    for(uint32_t i=0;i<gpuRows*delays;++i)output[i]=Complex(cached[i]);
    return {packSubmitMs,ms(submitted,done),ms(beforeWait,done),ms(done,Clock::now())};
  }
};

GpuPartialAmbiguity::GpuPartialAmbiguity(uint32_t count,uint32_t rowCount,uint32_t corr,
    uint32_t fft,uint32_t delayCount,int32_t firstLag,double middle)
  :impl_(std::make_unique<Impl>(count,rowCount,corr,fft,delayCount,firstLag,middle)){}
GpuPartialAmbiguity::~GpuPartialAmbiguity()=default;
void GpuPartialAmbiguity::start(const std::deque<Complex>& x,const std::deque<Complex>& y,
    const Complex* rotatedX,const Complex* filteredY,uint32_t fullSamples,int32_t rotation){
  impl_->start(x,y,rotatedX,filteredY,fullSamples,rotation);
}
void GpuPartialAmbiguity::start_borrowed(const Complex* rotatedReference,
    const Complex* filteredSurveillance,uint32_t fullSamples,int32_t rotation){
  impl_->start_borrowed(rotatedReference,filteredSurveillance,fullSamples,rotation);
}
GpuPartialAmbiguityTiming GpuPartialAmbiguity::finish(std::vector<Complex>& output){return impl_->finish(output);}
void GpuPartialAmbiguity::abort() noexcept {impl_->abort();}
