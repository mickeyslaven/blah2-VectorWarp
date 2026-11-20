// Exact overlap-save gather regression: independent full-output CPU oracle,
// batch-interval accounting, and (explicit --software only) production GLSL.
// The CPU target needs no Vulkan, worker, receiver, or runtime configuration.
#include "process/ambiguity/GpuWidebandGather.h"
#include <cstring>
#include <iostream>
#include <limits>

#ifdef VECTORWARP_TEST_GATHER_VULKAN
#define VECTORWARP_GPU_FP32_MODULE 1
#include "../../core/VulkanBackend.private.cpp"
#undef VECTORWARP_GPU_FP32_MODULE
#endif

namespace
{
using namespace blah2::wideband_gpu;
using Samples = std::vector<std::complex<float>>;
uint64_t checks = 0;
void require(bool ok, const char* message)
{
  ++checks;
  if (!ok) throw std::runtime_error(message);
}

uint64_t roundedInvocations(uint32_t count)
{
  // Kernel::appendWide uses a 2-D grid above 65535 workgroups.
  const uint64_t groups = (uint64_t(count) + 127) / 128;
  if (!groups) return 0;
  const uint64_t x = std::min<uint64_t>(groups, 65535);
  return x * ((groups + x - 1) / x) * 128;
}

struct Accounting
{
  uint64_t oldLogical = 0, logical = 0, oldRounded = 0, rounded = 0;
  uint64_t oldDispatches = 0, dispatches = 0;
  void batch(uint32_t outputs)
  {
    oldLogical += outputs;
    oldRounded += roundedInvocations(outputs);
    oldDispatches += outputs != 0;
  }
  void gather(uint32_t count)
  {
    logical += count;
    rounded += roundedInvocations(count);
    dispatches += count != 0;
  }
};

void intervalOracle()
{
  for (uint32_t decimation : {1u, 2u, 3u, 5u, 7u, 200u})
    for (uint32_t remainder = 0; remainder < decimation; ++remainder)
      for (uint32_t hop : {1u, 2u, 7u, 16u, 31u})
        for (uint32_t samples : {0u, 1u, 2u, 7u, 8u, 31u, 32u, 127u})
          for (uint32_t blocks : {1u, 2u, 3u, 8u})
          {
            LaneDispatch lane{};
            lane.firstRemainder = remainder;
            lane.decimation = decimation;
            lane.outputSamples = remainder < samples ?
              1 + (samples - remainder - 1) / decimation : 0;
            const uint32_t totalBlocks = (samples + hop - 1) / hop;
            uint32_t covered = 0;
            for (uint32_t first = 0; first < totalBlocks; first += blocks)
            {
              const uint32_t active = std::min(blocks, totalBlocks - first);
              const auto range = gatherRange(lane, first, active, hop);
              require(range.firstOutput == covered, "Gather intervals leave a gap or overlap");
              uint32_t expected = 0;
              for (uint32_t output = 0; output < lane.outputSamples; ++output)
              {
                const uint64_t input = remainder + uint64_t(output) * decimation;
                const bool owned = input / hop >= first && input / hop < first + active;
                const bool selected = output >= range.firstOutput &&
                  output - range.firstOutput < range.outputSamples;
                require(owned == selected, "Batch range disagrees with full-output oracle");
                expected += owned;
              }
              require(expected == range.outputSamples, "Batch invocation count differs");
              covered += range.outputSamples;
            }
            require(covered == lane.outputSamples, "Final partial batch lost an output");
          }
  // Exercise widened bounds without allocating the corresponding signal bank.
  LaneDispatch large{};
  large.outputSamples = UINT32_MAX;
  large.decimation = 1;
  const auto end = gatherRange(large, UINT32_MAX - 2, 4, 1);
  require(end.firstOutput == UINT32_MAX - 2 && end.outputSamples == 2,
    "Padded final block bound overflowed");
  require(gatherRange(large, 0, 0, 31).outputSamples == 0, "Empty interval is nonempty");
}

#ifdef VECTORWARP_TEST_GATHER_VULKAN
blah2::Candidate softwareCandidate(blah2::Instance& instance)
{
  uint32_t count = 0;
  blah2::check(vkEnumeratePhysicalDevices(instance.handle, &count, nullptr), "device count");
  std::vector<VkPhysicalDevice> devices(count);
  blah2::check(vkEnumeratePhysicalDevices(instance.handle, &count, devices.data()), "device discovery");
  for (auto device : devices)
  {
    blah2::Candidate candidate{};
    candidate.physical = device;
    vkGetPhysicalDeviceProperties(device, &candidate.properties);
    if (candidate.properties.deviceType != VK_PHYSICAL_DEVICE_TYPE_CPU) continue;
    candidate.info = {"test-only-software", candidate.properties.deviceName, UINT64_C(1) << 30};
    uint32_t families = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(device, &families, nullptr);
    std::vector<VkQueueFamilyProperties> queues(families);
    vkGetPhysicalDeviceQueueFamilyProperties(device, &families, queues.data());
    for (uint32_t i = 0; i < families; ++i)
      if (queues[i].queueCount && (queues[i].queueFlags & VK_QUEUE_COMPUTE_BIT))
      {
        candidate.queue = i;
        return candidate;
      }
  }
  throw std::runtime_error("--software requires CPU Vulkan; physical devices forbidden");
}

struct ShaderRunner
{
  blah2::Context& context;
  blah2::Buffer work, lanes, output;
  std::unique_ptr<blah2::Kernel> kernels[2];
  explicit ShaderRunner(blah2::Context& c) : context(c),
    work(c, 1 << 20, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, VK_MEMORY_PROPERTY_HOST_COHERENT_BIT),
    lanes(c, maxLanes * sizeof(LaneDispatch), VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, VK_MEMORY_PROPERTY_HOST_COHERENT_BIT),
    output(c, 1 << 20, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
  {
    for (int folded = 0; folded < 2; ++folded)
    {
      const auto source = gatherShaderSource(folded != 0);
      kernels[folded] = std::make_unique<blah2::Kernel>(context, source.c_str(),
        std::vector<blah2::Buffer*>{&work, &lanes, &output}, sizeof(GatherPush));
    }
  }
  void run(bool folded, const Samples& values, const std::vector<LaneDispatch>& metadata,
    const std::vector<GatherPush>& dispatches, Samples& actual)
  {
    if (dispatches.empty()) return;
    require(values.size() * sizeof(values[0]) <= work.bytes, "Test work capacity exceeded");
    require(actual.size() * sizeof(actual[0]) <= output.bytes, "Test output capacity exceeded");
    std::memcpy(work.mapped, values.data(), values.size() * sizeof(values[0]));
    std::memcpy(lanes.mapped, metadata.data(), metadata.size() * sizeof(metadata[0]));
    std::memcpy(output.mapped, actual.data(), actual.size() * sizeof(actual[0]));
    for (auto* buffer : {&work, &lanes, &output}) buffer->flush(0, buffer->bytes);
    VkCommandBuffer command = VK_NULL_HANDLE;
    VkCommandBufferAllocateInfo allocation{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocation.commandPool = context.pool;
    allocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocation.commandBufferCount = 1;
    blah2::check(vkAllocateCommandBuffers(context.device, &allocation, &command), "gather allocation");
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    blah2::check(vkBeginCommandBuffer(command, &begin), "gather begin");
    for (const auto& push : dispatches) kernels[folded]->appendWide(command, push, push.count);
    VkMemoryBarrier read{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    read.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    read.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
      0, 1, &read, 0, nullptr, 0, nullptr);
    blah2::check(vkEndCommandBuffer(command), "gather end");
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &command;
    blah2::check(vkQueueSubmit(context.queue, 1, &submit, context.fence), "gather submit");
    blah2::check(vkWaitForFences(context.device, 1, &context.fence, VK_TRUE, UINT64_C(5000000000)), "gather wait");
    blah2::check(vkResetFences(context.device, 1, &context.fence), "gather fence reset");
    output.invalidate(0, output.bytes);
    std::memcpy(actual.data(), output.mapped, actual.size() * sizeof(actual[0]));
    vkFreeCommandBuffers(context.device, context.pool, 1, &command);
  }
};
ShaderRunner* shaderRunner = nullptr;
#endif

void exactOutputs(bool folded, uint32_t fft, uint32_t overlap, uint32_t blockBatch,
  uint32_t samples, uint64_t firstSample, uint32_t channels, const std::vector<uint32_t>& rates)
{
  const uint32_t hop = fft - overlap;
  const uint32_t inverse = folded ? fft / rates[0] : fft;
  const uint32_t blocks = (samples + hop - 1) / hop;
  std::vector<LaneDispatch> lanes;
  uint32_t outputs = 0;
  for (uint32_t rate : rates)
  {
    const uint32_t first = uint32_t((rate - firstSample % rate) % rate);
    const uint32_t count = first < samples ? 1 + (samples - first - 1) / rate : 0;
    LaneDispatch lane{};
    lane.outputOffset = outputs;
    lane.outputSamples = count;
    lane.firstRemainder = first;
    lane.decimation = rate;
    lanes.push_back(lane);
    outputs += count * channels;
    require(!folded || (rate == rates[0] && fft % rate == 0 && overlap % rate == 0),
      "Invalid folded fixture");
  }
  // Guards expose workgroup tail writes even when the logical output is empty.
  const std::complex<float> poison{-987654, -765432};
  Samples oracle(outputs + 17, poison), compact = oracle;
#ifdef VECTORWARP_TEST_GATHER_VULKAN
  Samples shader = oracle;
#endif
  std::vector<uint32_t> writes(outputs);
  Accounting accounting;
  for (uint32_t firstBlock = 0; firstBlock < blocks; firstBlock += blockBatch)
  {
    const uint32_t active = std::min(blockBatch, blocks - firstBlock);
    Samples work(rates.size() * channels * blockBatch * inverse);
    for (uint32_t i = 0; i < work.size(); ++i)
      work[i] = {float(1 + i + firstBlock * work.size()), (i & 1) ? -0.0f : -float(i + 1)};
    accounting.batch(outputs);
    // Original full-bank ownership oracle, with independent nested traversal.
    for (uint32_t lane = 0; lane < lanes.size(); ++lane)
      for (uint32_t channel = 0; channel < channels; ++channel)
        for (uint32_t output = 0; output < lanes[lane].outputSamples; ++output)
        {
          const auto& a = lanes[lane];
          const uint32_t sample = a.firstRemainder + output * a.decimation;
          if (sample / hop < firstBlock || sample / hop >= firstBlock + active) continue;
          uint32_t index = overlap + sample % hop;
          if (folded) index = (index - (a.firstRemainder ? a.firstRemainder : a.decimation)) / a.decimation;
          require(index < inverse, "Oracle inverse index escaped FFT");
          const uint32_t batch = (lane * channels + channel) * blockBatch + sample / hop - firstBlock;
          const uint32_t destination = a.outputOffset + channel * a.outputSamples + output;
          oracle[destination] = work[batch * inverse + index];
        }
    std::vector<GatherPush> dispatches;
    for (uint32_t lane = 0; lane < lanes.size(); ++lane)
    {
      const auto range = gatherRange(lanes[lane], firstBlock, active, hop);
      const GatherPush push{range.outputSamples * channels, channels, fft, hop, overlap,
        blockBatch, firstBlock, active, folded ? rates[0] : 0, inverse, lane, range.firstOutput};
      accounting.gather(push.count);
      if (!push.count) continue;
      dispatches.push_back(push);
      // CPU execution of compact shader coordinates; the optional second
      // target executes the actual shared GLSL against the same oracle.
      for (uint32_t i = 0; i < roundedInvocations(push.count); ++i)
      {
        if (i >= push.count) continue;
        const auto& a = lanes[push.lane];
        const uint32_t count = push.count / push.channels, channel = i / count;
        const uint32_t output = push.firstOutput + i - channel * count;
        const uint32_t sample = a.firstRemainder + output * a.decimation;
        require(sample / hop >= firstBlock && sample / hop < firstBlock + active,
          "Compact dispatch still contains foreign-batch no-ops");
        const uint32_t batch = (lane * channels + channel) * blockBatch + sample / hop - firstBlock;
        uint32_t index = overlap + sample % hop;
        if (folded) index = (index - (a.firstRemainder ? a.firstRemainder : push.decimation)) / push.decimation;
        const uint32_t destination = a.outputOffset + channel * a.outputSamples + output;
        require(destination < outputs && batch * inverse + index < work.size(), "Gather index out of bounds");
        ++writes[destination];
        compact[destination] = work[batch * inverse + index];
      }
    }
    require(std::memcmp(compact.data(), oracle.data(), oracle.size() * sizeof(oracle[0])) == 0,
      "Compacted outputs differ bitwise from full-bank CPU oracle");
#ifdef VECTORWARP_TEST_GATHER_VULKAN
    shaderRunner->run(folded, work, lanes, dispatches, shader);
    require(std::memcmp(shader.data(), oracle.data(), oracle.size() * sizeof(oracle[0])) == 0,
      "Production gather GLSL differs bitwise from full-bank CPU oracle");
#endif
  }
  require(accounting.logical == outputs, "Compacted invocation sum differs from output elements");
  for (const auto count : writes) require(count == 1, "Output written zero or multiple times");
  if (blocks && blocks <= blockBatch)
    require(accounting.logical == accounting.oldLogical, "One-batch logical workload changed");
}

void exactMatrix()
{
  for (uint64_t first : {UINT64_C(0), UINT64_C(1), UINT64_C(4), (UINT64_C(1) << 54) + 3})
    for (uint32_t count : {0u, 1u, 24u, 25u, 26u, 127u, 251u})
    {
      exactOutputs(false, 31, 6, 3, count, first, 3, {1, 3, 5, 7});
      exactOutputs(true, 48, 12, 3, count, first, 3, {3, 3, 3});
      exactOutputs(true, 80, 20, 3, count, first, 3, {5, 5, 5});
      exactOutputs(true, 64, 16, 3, count, first, 3, {4, 4, 4});
    }
  exactOutputs(false, 31, 6, 1, 137, 23, maxChannels,
    {1, 2, 3, 5, 7, 11, 13, 17, 19, 23, 29, 31, 37, 41, 43, 47});
  exactOutputs(false, 31, 6, 32, 137, 23, 1, {1, 3, 5}); // Same outputs, one FFT batch.
  for (uint32_t count : {74u, 75u, 76u})
    exactOutputs(false, 31, 6, 3, count, 7, 2, {1, 3, 5, 7});
  for (uint32_t count : {107u, 108u, 109u})
    exactOutputs(true, 48, 12, 3, count, 0, 2, {3, 3, 3});
  for (uint32_t count : {179u, 180u, 181u})
    exactOutputs(true, 80, 20, 3, count, 4, 2, {5, 5, 5});
}

void replayAccounting()
{
  constexpr uint32_t samples = 12288000, hop = 4608, blockBatch = 234;
  constexpr uint32_t channels = 2, blocks = (samples + hop - 1) / hop;
  // Representative Plan metadata for the audited 24.576-MS/s, three-lane,
  // D4 replay. Identity filters suffice: gather ownership is tap-independent.
  Plan plan({24576000, channels, samples, {
    {509000000, 4, -9000000, {1}},
    {515000000, 4, -3000000, {1}},
    {527000000, 4, 9000000, {1}}}});
  for (uint64_t first : {UINT64_C(0), UINT64_C(1), (UINT64_C(1) << 54) + 3})
  {
    const auto dispatch = plan.prepare(first, samples);
    Accounting accounting;
    for (uint32_t block = 0; block < blocks; block += blockBatch)
    {
      accounting.batch(dispatch.outputElements);
      for (const auto& lane : dispatch.lanes)
      {
        const auto range = gatherRange(lane, block, std::min(blockBatch, blocks - block), hop);
        accounting.gather(range.outputSamples * channels);
      }
    }
    require(blocks == 2667 && accounting.oldDispatches == 12 && accounting.dispatches == 36,
      "Audited replay batch/dispatch geometry changed");
    require(dispatch.outputElements == 18432000 && accounting.logical == dispatch.outputElements,
      "Replay compact dispatch does not exactly partition outputs");
    require(accounting.oldLogical == 221184000 && accounting.oldRounded == 301985280,
      "Old replay 2-D grid accounting differs");
    require(accounting.rounded == 18432000, "Replay compact grid accounting differs");
    require(accounting.rounded - accounting.logical < 128 * accounting.dispatches,
      "Lane workgroup padding exceeds one tail group per dispatch");
    std::cout << "replay_first=" << first
      << " old_logical=" << accounting.oldLogical << " logical=" << accounting.logical
      << " old_rounded=" << accounting.oldRounded << " rounded=" << accounting.rounded
      << " old_dispatches=" << accounting.oldDispatches << " dispatches=" << accounting.dispatches << '\n';
  }
}
} // namespace

int main(int argc, char** argv) try
{
#ifdef VECTORWARP_TEST_GATHER_VULKAN
  require(argc == 2 && std::string(argv[1]) == "--software", "Usage: testGpuWidebandGatherSoftware --software");
  auto instance = std::make_shared<blah2::Instance>();
  auto candidate = softwareCandidate(*instance);
  blah2::Context context(instance, candidate, blah2::Context::Arithmetic::fp32);
  ShaderRunner runner(context);
  shaderRunner = &runner;
#else
  (void)argv;
  require(argc == 1, "CPU gather contract takes no arguments");
#endif
  intervalOracle();
  exactMatrix();
  replayAccounting();
  std::cout << "PASS wideband gather exact-output checks=" << checks << '\n';
  return 0;
}
catch (const std::exception& error)
{
  std::cerr << "FAIL wideband gather: " << error.what() << '\n';
  return 1;
}
