#include "process/ambiguity/GpuWidebandChannelizer.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <numeric>
#include <stdexcept>

namespace blah2::wideband_gpu
{
namespace
{
constexpr double pi = 3.141592653589793238462643383279502884;
uint64_t add(uint64_t a, uint64_t b)
{
  if (b > std::numeric_limits<uint64_t>::max() - a)
    throw std::overflow_error("Wideband GPU layout overflow");
  return a + b;
}

std::vector<double> designFilterImpl(uint32_t inputRate,uint32_t outputRate,
  uint32_t occupied,uint32_t tapsPerPhase)
{
  if(!inputRate||!outputRate||inputRate%outputRate||!occupied||occupied>=outputRate||
     tapsPerPhase<4||tapsPerPhase>128)throw std::invalid_argument("Wideband GPU filter configuration invalid");
  const uint32_t decimation=inputRate/outputRate;
  size_t count=size_t(tapsPerPhase)*decimation+1;if(!(count&1))++count;
  if(count>maxFilterTaps)throw std::invalid_argument("Wideband GPU filter exceeds wire capacity");
  const double pi=std::acos(-1.),normalized=(occupied/2.0+outputRate/2.0)/2.0/inputRate,middle=(count-1)/2.0;
  std::vector<double> taps(count);double sum=0;
  for(size_t i=0;i<count;++i){const double offset=double(i)-middle;
   const double sinc=offset==0?2*normalized:std::sin(2*pi*normalized*offset)/(pi*offset);
   const double window=.54-.46*std::cos(2*pi*i/double(count-1));sum+=taps[i]=sinc*window;}
  if(!std::isfinite(sum)||std::abs(sum)<std::numeric_limits<double>::epsilon())throw std::invalid_argument("Wideband GPU filter invalid");
  for(auto& tap:taps)tap/=sum;
  return taps;
}

uint64_t multiply(uint64_t a, uint64_t b)
{
  if (a && b > std::numeric_limits<uint64_t>::max() / a)
    throw std::overflow_error("Wideband GPU layout overflow");
  return a * b;
}
}

std::vector<double> designFilter(uint32_t inputRate,uint32_t outputRate,
  uint32_t occupied,uint32_t tapsPerPhase)
{
  return designFilterImpl(inputRate,outputRate,occupied,tapsPerPhase);
}

std::vector<LaneOutput> Backend::processView(const InputView& input,uint64_t first)
{
  std::vector<std::vector<std::complex<float>>> owned(input.channels.size(),
    std::vector<std::complex<float>>(input.samples));
  for(size_t channel=0;channel<input.channels.size();++channel){
    if(!input.channels[channel]&&input.samples)throw std::invalid_argument("Wideband GPU input view is null");
    std::copy_n(input.channels[channel],input.samples,owned[channel].begin());}
  return process(owned,first);
}

std::vector<LaneOutput> Backend::processDeques(
  const std::vector<const std::deque<std::complex<double>>*>& input,uint64_t first)
{
  const size_t samples=input.empty()?0:input.front()->size();
  std::vector<std::vector<std::complex<float>>> owned(input.size(),
    std::vector<std::complex<float>>(samples));
  for(size_t channel=0;channel<input.size();++channel){
    if(!input[channel]||input[channel]->size()!=samples)throw std::invalid_argument("Wideband GPU deque input differs");
    for(size_t sample=0;sample<samples;++sample)owned[channel][sample]=
      std::complex<float>((*input[channel])[sample]);}
  return process(owned,first);
}

Plan::Plan(Config config) : config_(std::move(config))
{
  if (!config_.inputRateHz || !config_.channels ||
      config_.channels > maxChannels || !config_.maxInputSamples ||
      config_.lanes.empty() || config_.lanes.size() > maxLanes)
    throw std::invalid_argument("Wideband GPU channelizer geometry is empty");

  uint64_t maximumOutputs = 0;
  for (std::size_t index = 0; index < config_.lanes.size(); ++index)
  {
    const auto& lane = config_.lanes[index];
    if (!lane.frequencyHz || !lane.decimation ||
        config_.inputRateHz % lane.decimation ||
        !std::isfinite(lane.centerOffsetHz) || lane.taps.empty() ||
        lane.taps.size() > maxFilterTaps || !(lane.taps.size() & 1))
      throw std::invalid_argument("Wideband GPU lane configuration is invalid");
    if (!std::all_of(lane.taps.begin(), lane.taps.end(),
          [](double value) { return std::isfinite(value); }))
      throw std::invalid_argument("Wideband GPU lane filter is non-finite");
    tapOffsets_[index] = uint32_t(packedTaps_.size());
    packedTaps_.insert(packedTaps_.end(), lane.taps.begin(), lane.taps.end());
    historySamples_ = std::max(historySamples_, uint32_t(lane.taps.size() - 1));
    const uint64_t outputs =
      (uint64_t(config_.maxInputSamples) + lane.decimation - 1) /
      lane.decimation;
    maximumOutputs = add(maximumOutputs,
      multiply(outputs, config_.channels));
  }
  layout_.inputElements = multiply(config_.channels,
    add(historySamples_, config_.maxInputSamples));
  layout_.outputElements = maximumOutputs;
  layout_.tapElements = packedTaps_.size();
  layout_.dispatchBytes = multiply(config_.lanes.size(), sizeof(LaneDispatch));
  layout_.totalBytes = add(add(multiply(layout_.inputElements, 8),
    multiply(layout_.outputElements, 8)),
    add(multiply(layout_.tapElements, 8), layout_.dispatchBytes));
  if (layout_.totalBytes > (512ULL << 20))
    throw std::invalid_argument("Wideband GPU channelizer exceeds shared budget");
}

Dispatch Plan::prepare(uint64_t firstInputSample, uint32_t inputSamples)
{
  if (inputSamples > config_.maxInputSamples ||
      inputSamples > std::numeric_limits<uint64_t>::max() - firstInputSample)
    throw std::invalid_argument("Wideband GPU input block is invalid");
  Dispatch result;
  result.firstInputSample = firstInputSample;
  result.inputSamples = inputSamples;
  result.resetHistory = !started_ || firstInputSample != nextInputSample_;
  result.historySamples = result.resetHistory ? 0 : historySamples_;
  result.lanes.reserve(config_.lanes.size());
  uint64_t outputOffset = 0;
  for (std::size_t index = 0; index < config_.lanes.size(); ++index)
  {
    const auto& lane = config_.lanes[index];
    const uint32_t remainder = uint32_t(firstInputSample % lane.decimation);
    const uint32_t first = remainder ? lane.decimation - remainder : 0;
    const uint32_t count = first < inputSamples ?
      1 + (inputSamples - 1 - first) / lane.decimation : 0;
    if (outputOffset > std::numeric_limits<uint32_t>::max())
      throw std::overflow_error("Wideband GPU output offset overflow");
    uint64_t centerBits = 0;
    std::memcpy(&centerBits, &lane.centerOffsetHz, sizeof(centerBits));
    const double phase = std::remainder(-2 * pi * lane.centerOffsetHz *
      double(firstInputSample) / config_.inputRateHz, 2 * pi);
    const float baseReal = float(std::cos(phase));
    const float baseImaginary = float(std::sin(phase));
    uint32_t baseRealBits = 0, baseImaginaryBits = 0;
    std::memcpy(&baseRealBits, &baseReal, sizeof(baseRealBits));
    std::memcpy(&baseImaginaryBits, &baseImaginary,
      sizeof(baseImaginaryBits));
    result.lanes.push_back({uint32_t(outputOffset), count, first,
      lane.decimation, tapOffsets_[index], uint32_t(lane.taps.size()),
      result.historySamples, 0, uint32_t(centerBits),
      uint32_t(centerBits >> 32), 0, 0, baseRealBits,
      baseImaginaryBits, 0, 0});
    outputOffset = add(outputOffset, multiply(count, config_.channels));
  }
  if (outputOffset > layout_.outputElements ||
      outputOffset > std::numeric_limits<uint32_t>::max())
    throw std::overflow_error("Wideband GPU output layout overflow");
  result.outputElements = uint32_t(outputOffset);
  started_ = true;
  nextInputSample_ = firstInputSample + inputSamples;
  return result;
}

void Plan::reset()
{
  started_ = false;
  nextInputSample_ = 0;
}

bool Plan::runtimeSelectable() const noexcept
{
  if(config_.inputRateHz<=2400000)return true;
  if(config_.lanes.empty()||config_.lanes.front().decimation<32)return false;
  const uint32_t decimation=config_.lanes.front().decimation;
  uint32_t maximumTaps=0;uint64_t period=decimation;
  for(const auto& lane:config_.lanes)
  {
    if(lane.decimation!=decimation||!std::isfinite(lane.centerOffsetHz)||
       std::abs(lane.centerOffsetHz-std::round(lane.centerOffsetHz))>1e-9)
      return false;
    maximumTaps=std::max(maximumTaps,uint32_t(lane.taps.size()));
    const int64_t center=std::llround(lane.centerOffsetHz);
    const uint64_t magnitude=center<0?uint64_t(-(center+1))+1:uint64_t(center);
    const uint64_t needed=config_.inputRateHz/
      std::gcd<uint64_t>(config_.inputRateHz,magnitude%config_.inputRateHz);
    const uint64_t divisor=std::gcd(period,needed);
    if(period>UINT32_MAX/(needed/divisor))return false;
    period*=needed/divisor;
  }
  if(!maximumTaps||(maximumTaps-1)%decimation||period>(1u<<20))return false;
  const auto smooth=[](uint64_t value){
    for(uint64_t prime:{2,3,5,7})
      while(value%prime==0)value/=prime;
    return value==1;
  };
  const uint64_t first=(uint64_t(maximumTaps)*2+period-1)/period;
  for(uint64_t multiplier=first;multiplier*period<=(1u<<24);++multiplier)
    if(smooth(multiplier*period))return true;
  return false;
}

const char* Plan::runtimeBlockReason() const noexcept
{
  return runtimeSelectable()?"":
    "wideband GPU geometry is outside the qualified shared FFT/PFB envelope";
}

const char* Plan::qualificationShaderSource()
{
  return R"glsl(#version 450
#extension GL_ARB_gpu_shader_fp64 : require
layout(local_size_x=128) in;
layout(binding=0) readonly buffer Input { vec2 raw[]; };
layout(binding=1) readonly buffer Taps { double taps[]; };
layout(binding=2) readonly buffer Lanes { uvec4 laneWords[]; };
layout(binding=3) writeonly buffer Output { vec2 outputData[]; };
layout(push_constant) uniform Parameters {
  uint total; uint channels; uint laneCount; uint inputStride;
  uint currentOffset; uint inputSamples; uint firstLow; uint firstHigh;
  double inputRate;
} p;
const double tau=6.283185307179586476925286766559;
void main(){
 uint work=gl_GlobalInvocationID.x+gl_GlobalInvocationID.y*gl_NumWorkGroups.x*128u;
 if(work>=p.total)return;
 uint lane=0u,local=work;
 for(;lane<p.laneCount;++lane){
  uvec4 a=laneWords[lane*4u];uint n=a.y*p.channels;
  if(local<n)break;local-=n;
 }
 if(lane>=p.laneCount)return;
 uvec4 a=laneWords[lane*4u],b=laneWords[lane*4u+1u],c=laneWords[lane*4u+2u],d=laneWords[lane*4u+3u];
 uint channel=local/a.y,outIndex=local-channel*a.y;
 uint current=a.z+outIndex*a.w;
 double center=packDouble2x32(c.xy);
 vec2 base=vec2(uintBitsToFloat(d.x),uintBitsToFloat(d.y));
 dvec2 sum=dvec2(0.0);
 for(uint tap=0u;tap<b.y;++tap){
  int source=int(p.currentOffset+current)-int(tap);
  if(source<0)continue;
  vec2 x=raw[channel*p.inputStride+uint(source)];
  double relative=-tau*center*double(int(current)-int(tap))/p.inputRate;
  float phase=float(mod(relative+3.14159265358979323846,tau)-3.14159265358979323846);
  vec2 turn=vec2(cos(phase),sin(phase));
  dvec2 oscillator=dvec2(base.x*turn.x-base.y*turn.y,
                          base.x*turn.y+base.y*turn.x);
  dvec2 mixed=dvec2(x.x*oscillator.x-x.y*oscillator.y,
                    x.x*oscillator.y+x.y*oscillator.x);
  sum+=mixed*taps[b.x+tap];
 }
 outputData[a.x+channel*a.y+outIndex]=vec2(sum);
})glsl";
}

} // namespace blah2::wideband_gpu
