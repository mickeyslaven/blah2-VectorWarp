#pragma once

#include "process/ambiguity/GpuWidebandChannelizer.h"
#include <algorithm>
#include <string>

namespace blah2::wideband_gpu
{
// Internal overlap-save gather geometry, shared by the Vulkan module and its
// CPU/exact-shader regression tests. No public worker/module ABI is changed.
struct GatherRange
{
  uint32_t firstOutput = 0;
  uint32_t outputSamples = 0; // Per channel; dispatch outputSamples * channels.
};

inline GatherRange gatherRange(const LaneDispatch& lane, uint32_t firstBlock,
  uint32_t activeBlocks, uint32_t hop)
{
  if (!lane.decimation || !hop)
    throw std::invalid_argument("Wideband gather geometry is empty");
  // Output k owns input r + k*d. In [lo, hi), its exact output interval is
  // [ceil((lo-r)/d), ceil((hi-r)/d)), clamped to [0, outputSamples].
  // Widen before multiplying/adding: the last padded block can exceed UINT32.
  const auto lowerBound = [&](uint64_t input) {
    const uint64_t output = input <= lane.firstRemainder ? 0 :
      1 + (input - lane.firstRemainder - 1) / lane.decimation;
    return uint32_t(std::min<uint64_t>(output, lane.outputSamples));
  };
  const uint32_t first = lowerBound(uint64_t(firstBlock) * hop);
  const uint32_t end = lowerBound((uint64_t(firstBlock) + activeBlocks) * hop);
  return {first, end - first};
}

struct GatherPush
{
  uint32_t count, channels, fft, hop, overlap, blocks, firstBlock, activeBlocks;
  uint32_t decimation, inverse, lane, firstOutput;
};
static_assert(sizeof(GatherPush) == 48, "Internal gather push layout changed");

inline std::string gatherShaderSource(bool folded)
{
  std::string source = R"glsl(#version 450
layout(local_size_x=128) in;
layout(binding=0) readonly buffer Work{vec2 work[];};
layout(binding=1) readonly buffer Lanes{uvec4 words[];};
layout(binding=2) writeonly buffer Output{vec2 outputData[];};
layout(push_constant) uniform P{
 uint count;uint channels;uint fft;uint hop;uint overlap;uint blocks;
 uint firstBlock;uint activeBlocks;uint decimation;uint inverse;
 uint lane;uint firstOutput;
}p;
void main(){
 uint i=gl_GlobalInvocationID.x+gl_GlobalInvocationID.y*gl_NumWorkGroups.x*128u;
 if(i>=p.count)return;
 uvec4 a=words[p.lane*4u];
 uint samples=p.count/p.channels,channel=i/samples;
 uint outIndex=p.firstOutput+i-channel*samples;
 uint currentIndex=a.z+outIndex*a.w,block=currentIndex/p.hop;
 if(block<p.firstBlock||block>=p.firstBlock+p.activeBlocks)return;
 uint slot=block-p.firstBlock,batch=(p.lane*p.channels+channel)*p.blocks+slot;
 uint outputIndex=a.x+channel*a.y+outIndex;
)glsl";
  if (folded)
    source += R"glsl( uint phaseOffset=a.z==0u?p.decimation:a.z;
 uint small=(p.overlap+currentIndex%p.hop-phaseOffset)/p.decimation;
 outputData[outputIndex]=work[batch*p.inverse+small];
})glsl";
  else
    source += R"glsl( uint j=p.overlap+currentIndex%p.hop;
 outputData[outputIndex]=work[batch*p.fft+j];
})glsl";
  return source;
}
} // namespace blah2::wideband_gpu
