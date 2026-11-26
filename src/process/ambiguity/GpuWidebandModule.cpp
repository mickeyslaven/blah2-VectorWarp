#include "process/ambiguity/GpuWidebandChannelizer.h"
#include "process/ambiguity/GpuWidebandGather.h"
#include "process/conditioning/SamplePhase.h"
#include <fftw3.h>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <memory>
#include <numeric>
#include <stdexcept>
#include <vector>
#define VECTORWARP_GPU_FP32_MODULE 1
#include "../../../core/VulkanBackend.private.cpp"
#undef VECTORWARP_GPU_FP32_MODULE

namespace blah2
{
namespace
{
constexpr const char* packSource=R"glsl(#version 450
layout(local_size_x=128) in;
layout(binding=0) readonly buffer Raw{vec2 raw[];};
layout(binding=1) readonly buffer Lanes{uvec4 words[];};
layout(binding=2) writeonly buffer Work{vec2 work[];};
layout(binding=3) readonly buffer Turns{vec2 turns[];};
layout(push_constant) uniform P{uint count;uint channels;uint lanes;uint fft;uint hop;uint overlap;uint blocks;uint stride;uint inputSamples;uint inputRate;uint firstBlock;uint activeBlocks;}p;
const float tau=6.283185307179586476925286766559;
int reducePhaseIndex(int index,uint period){uint mask=period-1u;if((period&mask)==0u)return int(uint(index)&mask);int value=index%int(period);return value<0?value+int(period):value;}
void main(){uint i=gl_GlobalInvocationID.x+gl_GlobalInvocationID.y*gl_NumWorkGroups.x*128u;if(i>=p.count)return;
 uint j=i%p.fft,batch=i/p.fft,slot=batch%p.blocks,q=batch/p.blocks,channel=q%p.channels,lane=q/p.channels;if(slot>=p.activeBlocks){work[i]=vec2(0);return;}uint block=p.firstBlock+slot;
 uvec4 b=words[lane*4u+1u],c=words[lane*4u+2u],d=words[lane*4u+3u];int sampleIndex=int(block*p.hop+j)-int(p.overlap);
 if(sampleIndex<0&&uint(-sampleIndex)>b.z){work[i]=vec2(0);return;}if(sampleIndex>=int(p.inputSamples)){work[i]=vec2(0);return;}
 vec2 x=raw[channel*p.stride+uint(int(p.overlap)+sampleIndex)];
 vec2 turn,base;if(d.w!=0u){turn=turns[d.z+uint(reducePhaseIndex(sampleIndex,d.w))];base=vec2(uintBitsToFloat(d.x),uintBitsToFloat(d.y));}else{uint phaseIndex=uint(sampleIndex+int(p.overlap));turn=turns[d.z+(phaseIndex&4095u)];base=turns[c.w+(phaseIndex>>12u)];}
 work[i]=vec2(x.x*(base.x*turn.x-base.y*turn.y)-x.y*(base.x*turn.y+base.y*turn.x),x.x*(base.x*turn.y+base.y*turn.x)+x.y*(base.x*turn.x-base.y*turn.y));})glsl";
constexpr const char* productSource=R"glsl(#version 450
layout(local_size_x=128) in;layout(binding=0) buffer Work{vec2 work[];};layout(binding=1) readonly buffer Spectra{vec2 spectra[];};
layout(push_constant) uniform P{uint count;uint channels;uint lanes;uint fft;uint hop;uint overlap;uint blocks;uint stride;uint inputSamples;uint inputRate;uint firstBlock;uint activeBlocks;}p;
void main(){uint i=gl_GlobalInvocationID.x+gl_GlobalInvocationID.y*gl_NumWorkGroups.x*128u;if(i>=p.count)return;uint lane=(i/p.fft/p.blocks)/p.channels;vec2 a=work[i],b=spectra[lane*p.fft+i%p.fft];work[i]=vec2(a.x*b.x-a.y*b.y,a.x*b.y+a.y*b.x)/float(p.fft);})glsl";
constexpr const char* sharedPackSource=R"glsl(#version 450
layout(local_size_x=128) in;layout(binding=0) readonly buffer Raw{vec2 raw[];};layout(binding=1) readonly buffer Lanes{uvec4 words[];};layout(binding=2) writeonly buffer Forward{vec2 forwardData[];};
layout(push_constant) uniform P{uint count;uint channels;uint lanes;uint fft;uint hop;uint overlap;uint blocks;uint stride;uint inputSamples;uint inputRate;uint firstBlock;uint activeBlocks;}p;
void main(){uint i=gl_GlobalInvocationID.x+gl_GlobalInvocationID.y*gl_NumWorkGroups.x*128u;if(i>=p.count)return;uint j=i%p.fft,batch=i/p.fft,slot=batch%p.blocks,channel=batch/p.blocks;if(slot>=p.activeBlocks){forwardData[i]=vec2(0);return;}uint block=p.firstBlock+slot;int sampleIndex=int(block*p.hop+j)-int(p.overlap);uint history=words[1].z;if(sampleIndex<0&&uint(-sampleIndex)>history){forwardData[i]=vec2(0);return;}if(sampleIndex>=int(p.inputSamples)){forwardData[i]=vec2(0);return;}forwardData[i]=raw[channel*p.stride+uint(int(p.overlap)+sampleIndex)];})glsl";
constexpr const char* sharedProductSource=R"glsl(#version 450
layout(local_size_x=128) in;layout(binding=0) readonly buffer Forward{vec2 forwardData[];};layout(binding=1) readonly buffer Spectra{vec2 spectra[];};layout(binding=2) readonly buffer Lanes{uvec4 words[];};layout(binding=3) writeonly buffer Work{vec2 work[];};layout(binding=4) readonly buffer Turns{vec2 turns[];};
layout(push_constant) uniform P{uint count;uint channels;uint lanes;uint fft;uint hop;uint overlap;uint blocks;uint stride;uint inputSamples;uint inputRate;uint firstBlock;uint activeBlocks;}p;const float tau=6.283185307179586476925286766559;
int reducePhaseIndex(int index,uint period){uint mask=period-1u;if((period&mask)==0u)return int(uint(index)&mask);int value=index%int(period);return value<0?value+int(period):value;}
void main(){uint i=gl_GlobalInvocationID.x+gl_GlobalInvocationID.y*gl_NumWorkGroups.x*128u;if(i>=p.count)return;uint j=i%p.fft,batch=i/p.fft,slot=batch%p.blocks,q=batch/p.blocks,channel=q%p.channels,lane=q/p.channels;if(slot>=p.activeBlocks){work[i]=vec2(0);return;}uvec4 c=words[lane*4u+2u],d=words[lane*4u+3u];uint source=(j+c.z)%p.fft;vec2 a=forwardData[(channel*p.blocks+slot)*p.fft+source],b=spectra[lane*p.fft+j];uint block=p.firstBlock+slot;int relativeSample=int(block*p.hop)-int(p.overlap);vec2 turn,base;if(d.w!=0u){turn=turns[d.z+uint(reducePhaseIndex(relativeSample,d.w))];base=vec2(uintBitsToFloat(d.x),uintBitsToFloat(d.y));}else{uint phaseIndex=block*p.hop;turn=turns[d.z+(phaseIndex&4095u)];base=turns[c.w+(phaseIndex>>12u)];}vec2 rotated=vec2(a.x*(base.x*turn.x-base.y*turn.y)-a.y*(base.x*turn.y+base.y*turn.x),a.x*(base.x*turn.y+base.y*turn.x)+a.y*(base.x*turn.x-base.y*turn.y));work[i]=vec2(rotated.x*b.x-rotated.y*b.y,rotated.x*b.y+rotated.y*b.x)/float(p.fft);})glsl";
constexpr const char* foldedProductSource=R"glsl(#version 450
layout(local_size_x=128) in;layout(binding=0) readonly buffer Forward{vec2 forwardData[];};layout(binding=1) readonly buffer Spectra{vec2 spectra[];};layout(binding=2) readonly buffer Lanes{uvec4 words[];};layout(binding=3) writeonly buffer Work{vec2 work[];};layout(binding=4) readonly buffer Turns{vec2 turns[];};
layout(push_constant) uniform P{uint count;uint channels;uint lanes;uint fft;uint hop;uint overlap;uint blocks;uint stride;uint inputSamples;uint inputRate;uint firstBlock;uint activeBlocks;uint decimation;uint inverse;uint foldRoots;}p;
int reducePhaseIndex(int index,uint period){uint mask=period-1u;if((period&mask)==0u)return int(uint(index)&mask);int value=index%int(period);return value<0?value+int(period):value;}
uint foldPhaseAdvance(uint phase,uint step,uint fft){return phase>=fft-step?phase-(fft-step):phase+step;}
void main(){uint i=gl_GlobalInvocationID.x+gl_GlobalInvocationID.y*gl_NumWorkGroups.x*128u;if(i>=p.count)return;uint k=i%p.inverse,batch=i/p.inverse,slot=batch%p.blocks,q=batch/p.blocks,channel=q%p.channels,lane=q/p.channels;if(slot>=p.activeBlocks){work[i]=vec2(0);return;}uvec4 a0=words[lane*4u],c=words[lane*4u+2u],d=words[lane*4u+3u];uint phaseOffset=a0.z==0u?p.decimation:a0.z,phase=k*phaseOffset,step=(phaseOffset%p.decimation)*p.inverse;vec2 sum=vec2(0);for(uint r=0u;r<p.decimation;++r){uint j=k+r*p.inverse,source=(j+c.z)%p.fft;vec2 a=forwardData[(channel*p.blocks+slot)*p.fft+source],b=spectra[lane*p.fft+j],product=vec2(a.x*b.x-a.y*b.y,a.x*b.y+a.y*b.x),foldTurn=turns[p.foldRoots+phase];sum+=vec2(product.x*foldTurn.x-product.y*foldTurn.y,product.x*foldTurn.y+product.y*foldTurn.x);phase=foldPhaseAdvance(phase,step,p.fft);}uint block=p.firstBlock+slot;int relativeSample=int(block*p.hop)-int(p.overlap);vec2 turn,base;if(d.w!=0u){turn=turns[d.z+uint(reducePhaseIndex(relativeSample,d.w))];base=vec2(uintBitsToFloat(d.x),uintBitsToFloat(d.y));}else{uint phaseIndex=block*p.hop;turn=turns[d.z+(phaseIndex&4095u)];base=turns[c.w+(phaseIndex>>12u)];}vec2 rotation=vec2(base.x*turn.x-base.y*turn.y,base.x*turn.y+base.y*turn.x);work[i]=vec2(sum.x*rotation.x-sum.y*rotation.y,sum.x*rotation.y+sum.y*rotation.x)/float(p.fft);})glsl";
struct WidePush{uint32_t count,channels,lanes,fft,hop,overlap,blocks,stride,inputSamples,inputRate,firstBlock,activeBlocks,decimation,inverse,foldRoots;};

class VulkanWideband final:public wideband_gpu::Backend,
  public wideband_gpu::BorrowedOutputBackend
{
 Context context_;wideband_gpu::Plan plan_;uint32_t fft_=0,inverse_=0,decimation_=0,overlap_=0,hop_=0,blocks_=0,blockBatch_=0,batches_=0,stride_=0,foldRoots_=0;bool sharedForward_=false,decimated_=false;
 std::unique_ptr<Buffer> rawUpload_,raw_,forward_,work_,spectra_,dispatch_,turns_,output_;std::unique_ptr<NativePlan> forwardTransform_,transform_;
 std::unique_ptr<Kernel> pack_,product_,gather_;std::vector<std::complex<float>> history_;std::vector<uint32_t> turnOffsets_,anchorOffsets_;VkCommandBuffer command_=VK_NULL_HANDLE;
 static uint32_t fftSize(uint32_t taps,uint64_t bank,uint64_t target){uint64_t n=1;while(n<uint64_t(taps)*2)n*=2;uint64_t best=0;double bestCost=0;for(;n<=UINT32_MAX&&n*bank*8<=target;n*=2){const double cost=double(n)*std::log2(double(n))/double(n-(taps-1));if(!best||cost<bestCost){best=n;bestCost=cost;}}if(!best)throw std::runtime_error("Wideband FFT minimum batch exceeds device range");return uint32_t(best);}
 static bool smooth(uint64_t n){for(uint64_t p:{2,3,5,7})while(n%p==0)n/=p;return n==1;}
 static uint32_t commonDecimation(const wideband_gpu::Config& config){if(config.lanes.empty())return 0;const uint32_t value=config.lanes.front().decimation;for(const auto& lane:config.lanes)if(lane.decimation!=value)return 0;return value;}
 static uint32_t sharedSize(const wideband_gpu::Config& config,uint32_t taps,uint64_t bank,uint64_t target,uint32_t required){uint64_t period=std::max(1u,required);for(const auto& lane:config.lanes){const int64_t center=std::llround(lane.centerOffsetHz);if(std::abs(lane.centerOffsetHz-double(center))>1e-9)return 0;const uint64_t magnitude=center<0?uint64_t(-(center+1))+1:uint64_t(center);const uint64_t needed=config.inputRateHz/std::gcd<uint64_t>(config.inputRateHz,magnitude%config.inputRateHz);const uint64_t divisor=std::gcd(period,needed);if(period>UINT32_MAX/(needed/divisor))return 0;period*=needed/divisor;}uint64_t best=0;double bestCost=0;const uint64_t first=(uint64_t(taps)*2+period-1)/period;const uint64_t last=std::min<uint64_t>(UINT32_MAX/period,target/(8*bank)/period);for(uint64_t k=first;k<=last;++k){const uint64_t n=k*period;if(!smooth(n))continue;const double cost=double(n)*std::log2(double(n))/double(n-(taps-1));if(!best||cost<bestCost){best=n;bestCost=cost;}if(best&&n>best*4)break;}return uint32_t(best);}
 void submit(VkCommandBuffer command){VkSubmitInfo s{VK_STRUCTURE_TYPE_SUBMIT_INFO};s.commandBufferCount=1;s.pCommandBuffers=&command;check(vkQueueSubmit(context_.queue,1,&s,context_.fence),"wideband submit");check(vkWaitForFences(context_.device,1,&context_.fence,VK_TRUE,5000000000ULL),"wideband wait");check(vkResetFences(context_.device,1,&context_.fence),"wideband fence reset");}
public:
 VulkanWideband(std::shared_ptr<Instance> instance,Candidate candidate,wideband_gpu::Config config):context_(std::move(instance),std::move(candidate),Context::Arithmetic::fp32),plan_(std::move(config)){
  uint32_t taps=0;for(const auto& lane:plan_.config().lanes)taps=std::max(taps,uint32_t(lane.taps.size()));const uint64_t bank=uint64_t(plan_.config().lanes.size())*plan_.config().channels;const uint64_t target=std::min<uint64_t>(32ULL<<20,context_.candidate.properties.limits.maxStorageBufferRange/2);decimation_=commonDecimation(plan_.config());const uint32_t foldedRequirement=decimation_>=2?decimation_:1;fft_=sharedSize(plan_.config(),taps,bank,target,foldedRequirement);sharedForward_=fft_!=0;if(!fft_)fft_=fftSize(taps,bank,target);overlap_=taps-1;hop_=fft_-overlap_;decimated_=sharedForward_&&decimation_>=2&&fft_%decimation_==0&&overlap_%decimation_==0;inverse_=decimated_?fft_/decimation_:fft_;blocks_=(plan_.config().maxInputSamples+hop_-1)/hop_;const uint64_t bytesPerBlock=sharedForward_?8ULL*plan_.config().channels*(fft_+uint64_t(inverse_)*plan_.config().lanes.size()):8ULL*fft_*bank;blockBatch_=uint32_t(std::min<uint64_t>(blocks_,std::max<uint64_t>(1,target/bytesPerBlock)));batches_=uint32_t(bank*blockBatch_);stride_=overlap_+plan_.config().maxInputSamples;
  // Folding/shared transforms are optimizations, not rate-based admission.
  // The generic FP32 FFT path retains the same capacity and numerical guards.
  const uint64_t workCount=uint64_t(inverse_)*batches_,outputCount=plan_.layout().outputElements;if(workCount*8>context_.candidate.properties.limits.maxStorageBufferRange)throw std::runtime_error("Wideband FFT work exceeds device range");
  const VkMemoryPropertyFlags host=VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
  const VkMemoryPropertyFlags preferred=VK_MEMORY_PROPERTY_HOST_COHERENT_BIT|VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
  const VkMemoryPropertyFlags device=VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
  // Short integer periods retain their existing fast lookup. Other offsets
  // use bounded 4096-sample turns and exact absolute anchors, never n*float(step).
  std::vector<std::complex<float>> turnValues(1,{1,0});
  turnOffsets_.resize(plan_.config().lanes.size());
  anchorOffsets_.assign(plan_.config().lanes.size(),UINT32_MAX);
  constexpr uint32_t maxLaneTurns=65536,maxTotalTurns=1048576;
  const uint32_t anchors=1+(stride_-1)/4096;
  for(size_t lane=0;lane<plan_.config().lanes.size();++lane){
   const double center=plan_.config().lanes[lane].centerOffsetHz;
   uint32_t period=0;
   if(std::trunc(center)==center){const uint64_t magnitude=uint64_t(std::abs(center));
    period=uint32_t(plan_.config().inputRateHz/std::gcd<uint64_t>(plan_.config().inputRateHz,magnitude%plan_.config().inputRateHz));
    if(period>maxLaneTurns||turnValues.size()+period>maxTotalTurns)period=0;}
   const uint32_t count=period?period:4096;
   const uint64_t total=turnValues.size()+count+(period?0:anchors);
   if(total>UINT32_MAX||total*8>context_.candidate.properties.limits.maxStorageBufferRange)
    throw std::runtime_error("Wideband phase table exceeds device range");
   turnOffsets_[lane]=uint32_t(turnValues.size());
   for(uint32_t index=0;index<count;++index)
    turnValues.emplace_back(channelizer::mixerTurn(index,center,plan_.config().inputRateHz));
   if(!period){anchorOffsets_[lane]=uint32_t(turnValues.size());turnValues.resize(total);}
  }
  // Exact modular fold roots avoid device trig error and recurrence drift;
  // reuse the canonical phase owner and the existing bounded phase buffer.
  if(decimated_){
   const uint64_t total=turnValues.size()+uint64_t(fft_);
   if(total>UINT32_MAX||total*8>context_.candidate.properties.limits.maxStorageBufferRange)
    throw std::runtime_error("Wideband fold phase table exceeds device range");
   foldRoots_=uint32_t(turnValues.size());
   for(uint32_t index=0;index<fft_;++index)
    turnValues.emplace_back(channelizer::mixerTurn(index,-1,fft_));
  }
  rawUpload_=std::make_unique<Buffer>(context_,uint64_t(stride_)*plan_.config().channels*8,host,preferred);raw_=std::make_unique<Buffer>(context_,rawUpload_->bytes,device);if(sharedForward_)forward_=std::make_unique<Buffer>(context_,uint64_t(fft_)*plan_.config().channels*blockBatch_*8,device);work_=std::make_unique<Buffer>(context_,workCount*8,device);spectra_=std::make_unique<Buffer>(context_,uint64_t(fft_)*plan_.config().lanes.size()*8,device);dispatch_=std::make_unique<Buffer>(context_,plan_.config().lanes.size()*sizeof(wideband_gpu::LaneDispatch),host,preferred);turns_=std::make_unique<Buffer>(context_,turnValues.size()*sizeof(turnValues[0]),host,preferred);std::memcpy(turns_->mapped,turnValues.data(),turns_->bytes);turns_->flush(0,turns_->bytes);output_=std::make_unique<Buffer>(context_,outputCount*8,host,preferred);Buffer spectraUpload(context_,spectra_->bytes,host,preferred);
  VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};ai.commandPool=context_.pool;ai.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;ai.commandBufferCount=1;check(vkAllocateCommandBuffers(context_.device,&ai,&command_),"wideband command");
  auto* spectra=static_cast<std::complex<float>*>(spectraUpload.mapped);std::vector<std::complex<double>> in(fft_),out(fft_);auto* a=reinterpret_cast<fftw_complex*>(in.data());auto* b=reinterpret_cast<fftw_complex*>(out.data());fftw_plan fp=fftw_plan_dft_1d(int(fft_),a,b,FFTW_FORWARD,FFTW_ESTIMATE);if(!fp)throw std::runtime_error("Wideband filter FFT plan failed");
  for(size_t lane=0;lane<plan_.config().lanes.size();++lane){std::fill(in.begin(),in.end(),std::complex<double>{});const auto&t=plan_.config().lanes[lane].taps;for(size_t i=0;i<t.size();++i)in[i]=t[i];fftw_execute(fp);for(uint32_t i=0;i<fft_;++i)spectra[lane*fft_+i]=std::complex<float>(out[i]);}fftw_destroy_plan(fp);spectraUpload.flush(0,spectraUpload.bytes);VkCommandBufferBeginInfo uploadBegin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};uploadBegin.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;check(vkBeginCommandBuffer(command_,&uploadBegin),"wideband spectra upload recording");VkBufferCopy spectraCopy{0,0,spectra_->bytes};vkCmdCopyBuffer(command_,spectraUpload.handle,spectra_->handle,1,&spectraCopy);check(vkEndCommandBuffer(command_),"wideband spectra upload recording");submit(command_);
  transform_=std::make_unique<NativePlan>(context_,*work_,inverse_,batches_);if(sharedForward_){forwardTransform_=std::make_unique<NativePlan>(context_,*forward_,fft_,plan_.config().channels*blockBatch_);pack_=std::make_unique<Kernel>(context_,sharedPackSource,std::vector<Buffer*>{raw_.get(),dispatch_.get(),forward_.get()},sizeof(WidePush));product_=std::make_unique<Kernel>(context_,decimated_?foldedProductSource:sharedProductSource,std::vector<Buffer*>{forward_.get(),spectra_.get(),dispatch_.get(),work_.get(),turns_.get()},sizeof(WidePush));}else{pack_=std::make_unique<Kernel>(context_,packSource,std::vector<Buffer*>{raw_.get(),dispatch_.get(),work_.get(),turns_.get()},sizeof(WidePush));product_=std::make_unique<Kernel>(context_,productSource,std::vector<Buffer*>{work_.get(),spectra_.get()},sizeof(WidePush));}
  const auto gatherSource=wideband_gpu::gatherShaderSource(decimated_);
  gather_=std::make_unique<Kernel>(context_,gatherSource.c_str(),std::vector<Buffer*>{work_.get(),dispatch_.get(),output_.get()},sizeof(wideband_gpu::GatherPush));
  history_.assign(uint64_t(overlap_)*plan_.config().channels,{});
 }
 ~VulkanWideband()override{if(command_)vkFreeCommandBuffers(context_.device,context_.pool,1,&command_);}
 // The wire/status identity must distinguish identical-named physical devices.
 std::string deviceName()const override{return context_.candidate.info.id;}
 void reset()override{plan_.reset();std::fill(history_.begin(),history_.end(),std::complex<float>{});}
 std::vector<wideband_gpu::LaneOutput> process(const std::vector<std::vector<std::complex<float>>>& input,uint64_t first)override{wideband_gpu::Backend::InputView view;view.samples=input.empty()?0:input.front().size();for(const auto& channel:input){if(channel.size()!=view.samples)throw std::invalid_argument("Wideband input channels unpaired");view.channels.push_back(channel.data());}return processView(view,first);}
 std::vector<wideband_gpu::LaneOutput> processView(const wideband_gpu::Backend::InputView& input,uint64_t first)override{
  const auto borrowed=processBorrowed(input,first);
  const auto& d=borrowed.dispatch;
  const auto* values=borrowed.data;
  std::vector<wideband_gpu::LaneOutput> result;
  for(size_t lane=0;lane<d.lanes.size();++lane){const auto&m=d.lanes[lane];wideband_gpu::LaneOutput o;o.frequencyHz=plan_.config().lanes[lane].frequencyHz;o.inputSamplesPerOutput=m.decimation;o.hasFirstInputSample=m.outputSamples!=0;if(o.hasFirstInputSample)o.firstInputSample=first+m.firstRemainder;o.channels.resize(plan_.config().channels);for(uint32_t c=0;c<plan_.config().channels;++c)o.channels[c].assign(values+m.outputOffset+c*m.outputSamples,values+m.outputOffset+(c+1)*m.outputSamples);result.push_back(std::move(o));}
  return result;
 }
 wideband_gpu::BorrowedOutput processBorrowed(const wideband_gpu::Backend::InputView& input,uint64_t first)override{
  if(input.channels.size()!=plan_.config().channels)
    throw std::invalid_argument("Wideband input channels changed");
  uint32_t n=uint32_t(input.samples);
  if(input.samples>plan_.config().maxInputSamples)
    throw std::invalid_argument("Wideband input exceeds capacity");
  for(const auto* channel:input.channels)if(!channel&&n)
    throw std::invalid_argument("Wideband input channel is null");
  auto d=plan_.prepare(first,n);auto* raw=static_cast<std::complex<float>*>(rawUpload_->mapped);
  // The shader bounds-checks both history and the current capture, so only
  // initialize bytes it can consume. In particular, do not clear/flush every
  // channel's max-CPI allocation when the current capture is shorter.
  for(uint32_t c=0;c<plan_.config().channels;++c){auto* history=history_.data()+uint64_t(c)*overlap_;auto* destination=raw+uint64_t(c)*stride_;if(d.resetHistory)std::fill_n(destination,overlap_,std::complex<float>{});else std::copy_n(history,overlap_,destination);if(n)std::copy_n(input.channels[c],n,destination+overlap_);if(n>=overlap_)std::copy_n(input.channels[c]+n-overlap_,overlap_,history);else{if(d.resetHistory){std::fill_n(history,overlap_-n,std::complex<float>{});}else std::move(history+n,history+overlap_,history);if(n)std::copy_n(input.channels[c],n,history+overlap_-n);}rawUpload_->flush(uint64_t(c)*stride_*8,uint64_t(overlap_+n)*8);}
  auto laneWords=d.lanes;
  for(size_t lane=0;lane<laneWords.size();++lane){
   laneWords[lane].padding2=turnOffsets_[lane];
   if(anchorOffsets_[lane]!=UINT32_MAX){
    laneWords[lane].padding3=0;laneWords[lane].padding1=anchorOffsets_[lane];
    const double center=plan_.config().lanes[lane].centerOffsetHz;
    const auto base=channelizer::mixerTurn(first,center,plan_.config().inputRateHz)*
      std::conj(channelizer::mixerTurn(overlap_,center,plan_.config().inputRateHz));
    auto* anchors=static_cast<std::complex<float>*>(turns_->mapped)+anchorOffsets_[lane];
    const uint32_t count=n?1+(overlap_+n-1)/4096:0;
    for(uint32_t index=0;index<count;++index)
     anchors[index]=std::complex<float>(base*channelizer::mixerTurn(uint64_t(index)*4096,center,plan_.config().inputRateHz));
    if(count)turns_->flush(uint64_t(anchorOffsets_[lane])*8,uint64_t(count)*8);
   }
   if(sharedForward_){const auto shift=std::llround(plan_.config().lanes[lane].centerOffsetHz*fft_/plan_.config().inputRateHz);
    laneWords[lane].padding0=uint32_t((shift%int64_t(fft_)+fft_)%fft_);}
  }
  std::memcpy(dispatch_->mapped,laneWords.data(),laneWords.size()*sizeof(laneWords[0]));dispatch_->flush(0,dispatch_->bytes);
  const uint32_t currentBlocks=n?(n+hop_-1)/hop_:0;
  if(currentBlocks){
   check(vkResetCommandBuffer(command_,0),"wideband command reset");
   VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
   check(vkBeginCommandBuffer(command_,&bi),"wideband recording");
   for(uint32_t c=0;c<plan_.config().channels;++c){VkBufferCopy copy{uint64_t(c)*stride_*8,uint64_t(c)*stride_*8,uint64_t(overlap_+n)*8};vkCmdCopyBuffer(command_,rawUpload_->handle,raw_->handle,1,&copy);}
   VkMemoryBarrier uploaded{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
   uploaded.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;uploaded.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;
   vkCmdPipelineBarrier(command_,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,1,&uploaded,0,nullptr,0,nullptr);
   WidePush p{uint32_t(uint64_t(inverse_)*batches_),plan_.config().channels,uint32_t(plan_.config().lanes.size()),fft_,hop_,overlap_,blockBatch_,stride_,n,plan_.config().inputRateHz,0,0,decimation_,inverse_,foldRoots_};
   for(uint32_t firstBlock=0;firstBlock<currentBlocks;firstBlock+=blockBatch_){
    p.firstBlock=firstBlock;p.activeBlocks=std::min(blockBatch_,currentBlocks-firstBlock);
    if(sharedForward_){p.count=uint32_t(uint64_t(fft_)*plan_.config().channels*blockBatch_);pack_->appendWide(command_,p,p.count);barrier(command_);forwardTransform_->append(command_,-1);barrier(command_);p.count=uint32_t(uint64_t(inverse_)*batches_);product_->appendWide(command_,p,p.count);}
    else{p.count=uint32_t(uint64_t(fft_)*batches_);pack_->appendWide(command_,p,p.count);barrier(command_);transform_->append(command_,-1);barrier(command_);product_->appendWide(command_,p,p.count);}
    barrier(command_);transform_->append(command_,1);barrier(command_);
    // Each output belongs to exactly one active block interval. Separate lane
    // dispatches avoid both the old full-output scan per batch and lane scans
    // in the shader, while writing the original lane/channel-major indices.
    for(uint32_t lane=0;lane<d.lanes.size();++lane){
     const auto range=wideband_gpu::gatherRange(d.lanes[lane],firstBlock,p.activeBlocks,hop_);
     if(!range.outputSamples)continue;
     const wideband_gpu::GatherPush gather{
      range.outputSamples*p.channels,p.channels,fft_,hop_,overlap_,blockBatch_,
      firstBlock,p.activeBlocks,decimation_,inverse_,lane,range.firstOutput};
     gather_->appendWide(command_,gather,gather.count);
    }
    barrier(command_);
   }
   VkMemoryBarrier h{VK_STRUCTURE_TYPE_MEMORY_BARRIER};h.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT;h.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
   vkCmdPipelineBarrier(command_,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&h,0,nullptr,0,nullptr);
   check(vkEndCommandBuffer(command_),"wideband recording");submit(command_);output_->invalidate(0,uint64_t(d.outputElements)*8);
  }
  return {std::move(d),static_cast<const std::complex<float>*>(output_->mapped)};
 }
};
}
}

extern "C" blah2::wideband_gpu::Backend* blah2_gpu_create_wideband(unsigned abi,const blah2::wideband_gpu::Config* config,const char* requested){if(abi!=blah2::wideband_gpu::moduleAbi||!config)throw std::runtime_error("Wideband GPU module version mismatch");blah2::wideband_gpu::Plan qualification(*config);if(!qualification.runtimeSelectable())throw std::runtime_error(qualification.runtimeBlockReason());auto instance=std::make_shared<blah2::Instance>();std::string reason="No compatible wideband GPU";const std::string selection=requested?requested:"auto";for(const auto& candidate:blah2::enumerate(*instance)){if(selection!="auto"&&selection!=candidate.info.id)continue;try{return new blah2::VulkanWideband(instance,candidate,*config);}catch(const std::exception&e){reason=e.what();}}throw std::runtime_error(reason);}
