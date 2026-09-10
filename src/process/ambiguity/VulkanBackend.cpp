// Portable batched ambiguity processing. Uses Vulkan compute, not graphics or
// vendor-specific CUDA/ROCm runtimes. VkFFT supplies the FFT kernels; the two
// small shaders below implement the same correlation/gather as CPU Ambiguity.
#include "GpuBackend.h"
#include <vulkan/vulkan.h>
#include <glslang/Include/glslang_c_interface.h>
#if __has_include(<glslang/Public/resource_limits_c.h>)
#include <glslang/Public/resource_limits_c.h>
#else
// glslang 11 (Ubuntu 22.04) exports this C API but does not install its header.
extern "C" const glslang_resource_t* glslang_default_resource(void);
#endif
#include <vkFFT.h>
#include <algorithm>
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <memory>
#include <sstream>
#include <stdexcept>

namespace blah2 {
namespace {
void check(VkResult result, const char* operation) {
  if (result != VK_SUCCESS)
    throw std::runtime_error(std::string("GPU ") + operation + " failed (" +
      std::to_string(result) + "); using CPU");
}
void checkFft(VkFFTResult result) {
  if (result != VKFFT_SUCCESS)
    throw std::runtime_error("GPU FFT failed (" + std::to_string(result) + "); using CPU");
}
struct Instance {
  VkInstance handle = VK_NULL_HANDLE;
  Instance() {
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "blah2"; app.apiVersion = VK_API_VERSION_1_0;
    VkInstanceCreateInfo info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    info.pApplicationInfo = &app;
    check(vkCreateInstance(&info, nullptr, &handle), "driver initialization");
  }
  ~Instance() { if (handle) vkDestroyInstance(handle, nullptr); }
};
struct Candidate {
  VkPhysicalDevice physical;
  VkPhysicalDeviceProperties properties;
  GpuDevice info;
  uint32_t queue;
};
std::vector<Candidate> enumerate(Instance& instance) {
  uint32_t count = 0;
  check(vkEnumeratePhysicalDevices(instance.handle, &count, nullptr), "device discovery");
  std::vector<VkPhysicalDevice> devices(count);
  check(vkEnumeratePhysicalDevices(instance.handle, &count, devices.data()), "device discovery");
  std::vector<Candidate> result;
  for (size_t index = 0; index < count; ++index) {
    Candidate item{}; item.physical = devices[index];
    vkGetPhysicalDeviceProperties(item.physical, &item.properties);
    // llvmpipe/lavapipe and other software drivers must never be called GPU acceleration.
    const auto type = item.properties.deviceType;
    if (type != VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU &&
        type != VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU &&
        type != VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU) continue;
    VkPhysicalDeviceMemoryProperties memory{};
    vkGetPhysicalDeviceMemoryProperties(item.physical, &memory);
    uint64_t bytes = 0;
    for (uint32_t i = 0; i < memory.memoryHeapCount; ++i)
      if (memory.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
        bytes = std::max(bytes, uint64_t(memory.memoryHeaps[i].size));
    item.info = {std::to_string(item.properties.vendorID) + ":" +
      std::to_string(item.properties.deviceID) + ":" + std::to_string(index),
      item.properties.deviceName, bytes};
    uint32_t queues = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(item.physical, &queues, nullptr);
    std::vector<VkQueueFamilyProperties> families(queues);
    vkGetPhysicalDeviceQueueFamilyProperties(item.physical, &queues, families.data());
    item.queue = UINT32_MAX;
    for (uint32_t q = 0; q < queues; ++q)
      if (families[q].queueCount && (families[q].queueFlags & VK_QUEUE_COMPUTE_BIT)) {
        item.queue = q;
        if (!(families[q].queueFlags & VK_QUEUE_GRAPHICS_BIT)) break;
      }
    if (item.queue != UINT32_MAX) result.push_back(item);
  }
  std::stable_sort(result.begin(), result.end(), [](const auto& a, const auto& b) {
    const auto rank = [](VkPhysicalDeviceType type) {
      return type == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 0 :
        type == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 1 : 2;
    };
    if (a.properties.deviceType != b.properties.deviceType)
      return rank(a.properties.deviceType) < rank(b.properties.deviceType);
    return a.info.memoryBytes > b.info.memoryBytes;
  });
  return result;
}
struct Context {
  std::shared_ptr<Instance> instance;
  Candidate candidate;
  VkDevice device = VK_NULL_HANDLE;
  VkQueue queue = VK_NULL_HANDLE;
  VkCommandPool pool = VK_NULL_HANDLE;
  VkFence fence = VK_NULL_HANDLE;
  bool compiler = false;
  Context(std::shared_ptr<Instance> owner, Candidate selected)
    : instance(std::move(owner)), candidate(std::move(selected)) {
    try {
      const float priority = 1;
      VkDeviceQueueCreateInfo queueInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
      queueInfo.queueFamilyIndex = candidate.queue;
      queueInfo.queueCount = 1; queueInfo.pQueuePriorities = &priority;
      VkDeviceCreateInfo info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
      info.queueCreateInfoCount = 1; info.pQueueCreateInfos = &queueInfo;
      check(vkCreateDevice(candidate.physical, &info, nullptr, &device), "device initialization");
      vkGetDeviceQueue(device, candidate.queue, 0, &queue);
      VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
      poolInfo.queueFamilyIndex = candidate.queue;
      check(vkCreateCommandPool(device, &poolInfo, nullptr, &pool), "command allocation");
      VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
      check(vkCreateFence(device, &fenceInfo, nullptr, &fence), "fence allocation");
      compiler = glslang_initialize_process();
      if (!compiler) throw std::runtime_error("GPU shader compiler is unavailable");
    } catch (...) { release(); throw; }
  }
  void release() {
    if (compiler) glslang_finalize_process();
    if (fence) vkDestroyFence(device, fence, nullptr);
    if (pool) vkDestroyCommandPool(device, pool, nullptr);
    if (device) vkDestroyDevice(device, nullptr);
  }
  ~Context() { release(); }
};
struct Buffer {
  Context& context;
  VkBuffer handle = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  void* mapped = nullptr;
  uint64_t bytes;
  Buffer(Context& ctx, uint64_t size, bool host) : context(ctx), bytes(size) {
    try {
      VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
      info.size = size;
      info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
        VK_BUFFER_USAGE_TRANSFER_DST_BIT;
      check(vkCreateBuffer(ctx.device, &info, nullptr, &handle), "buffer allocation");
      VkMemoryRequirements requirements{};
      vkGetBufferMemoryRequirements(ctx.device, handle, &requirements);
      VkPhysicalDeviceMemoryProperties properties{};
      vkGetPhysicalDeviceMemoryProperties(ctx.candidate.physical, &properties);
      const VkMemoryPropertyFlags flags = host ?
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT :
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
      uint32_t type = UINT32_MAX;
      for (uint32_t i = 0; i < properties.memoryTypeCount; ++i)
        if ((requirements.memoryTypeBits & (1u << i)) &&
            (properties.memoryTypes[i].propertyFlags & flags) == flags) { type = i; break; }
      if (type == UINT32_MAX) throw std::runtime_error("GPU has no suitable memory; using CPU");
      VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
      allocation.allocationSize = requirements.size; allocation.memoryTypeIndex = type;
      check(vkAllocateMemory(ctx.device, &allocation, nullptr, &memory), "memory allocation");
      check(vkBindBufferMemory(ctx.device, handle, memory, 0), "memory binding");
      if (host) check(vkMapMemory(ctx.device, memory, 0, size, 0, &mapped), "host mapping");
    } catch (...) { release(); throw; }
  }
  void release() {
    if (mapped) vkUnmapMemory(context.device, memory);
    if (handle) vkDestroyBuffer(context.device, handle, nullptr);
    if (memory) vkFreeMemory(context.device, memory, nullptr);
  }
  ~Buffer() { release(); }
};
struct Plan {
  VkFFTApplication app{};
  Buffer& buffer_;
  Plan(Context& context, Buffer& buffer, uint32_t length, uint32_t batches) : buffer_(buffer) {
    VkFFTConfiguration config{};
    config.FFTdim = 1; config.size[0] = length; config.numberBatches = batches;
    config.device = &context.device; config.queue = &context.queue;
    config.fence = &context.fence; config.commandPool = &context.pool;
    config.physicalDevice = &context.candidate.physical;
    // Bind at append time, after VkFFT has allocated prime-length FFT tables.
    // Binding during initialization can leave descriptors pointing at null LUTs.
    config.bufferSize = &buffer.bytes;
    config.isCompilerInitialized = 1;
    const auto result = initializeVkFFT(&app, config);
    if (result != VKFFT_SUCCESS) { deleteVkFFT(&app); checkFft(result); }
  }
  ~Plan() { deleteVkFFT(&app); }
  void append(VkCommandBuffer command, int direction) {
    VkFFTLaunchParams params{}; params.commandBuffer = &command;
    params.buffer = &buffer_.handle;
    checkFft(VkFFTAppend(&app, direction, &params));
  }
};
struct Push { uint32_t count, range, doppler, delays; int32_t delayMin; };
struct Kernel {
  Context& context;
  VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
  VkDescriptorPool pool = VK_NULL_HANDLE;
  VkDescriptorSet set = VK_NULL_HANDLE;
  VkPipelineLayout layout = VK_NULL_HANDLE;
  VkShaderModule shader = VK_NULL_HANDLE;
  VkPipeline pipeline = VK_NULL_HANDLE;
  Kernel(Context& ctx, const char* source, const std::vector<Buffer*>& buffers) : context(ctx) {
    try {
      glslang_input_t input{};
      input.language = GLSLANG_SOURCE_GLSL; input.stage = GLSLANG_STAGE_COMPUTE;
      input.client = GLSLANG_CLIENT_VULKAN; input.client_version = GLSLANG_TARGET_VULKAN_1_0;
      input.target_language = GLSLANG_TARGET_SPV; input.target_language_version = GLSLANG_TARGET_SPV_1_0;
      input.code = source; input.default_version = 450; input.default_profile = GLSLANG_NO_PROFILE;
      input.messages = static_cast<glslang_messages_t>(GLSLANG_MSG_SPV_RULES_BIT | GLSLANG_MSG_VULKAN_RULES_BIT);
      input.resource = glslang_default_resource();
      auto shaderSource = std::unique_ptr<glslang_shader_t, decltype(&glslang_shader_delete)>(
        glslang_shader_create(&input), glslang_shader_delete);
      if (!shaderSource || !glslang_shader_preprocess(shaderSource.get(), &input) ||
          !glslang_shader_parse(shaderSource.get(), &input))
        throw std::runtime_error("GPU shader compilation failed");
      auto program = std::unique_ptr<glslang_program_t, decltype(&glslang_program_delete)>(
        glslang_program_create(), glslang_program_delete);
      if (!program) throw std::runtime_error("GPU shader compiler allocation failed");
      glslang_program_add_shader(program.get(), shaderSource.get());
      if (!glslang_program_link(program.get(), input.messages))
        throw std::runtime_error("GPU shader link failed");
      glslang_program_SPIRV_generate(program.get(), input.stage);
      const auto words = glslang_program_SPIRV_get_size(program.get());
      if (!words) throw std::runtime_error("GPU shader generation failed");
      VkShaderModuleCreateInfo shaderInfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
      shaderInfo.codeSize = words * sizeof(uint32_t);
      shaderInfo.pCode = glslang_program_SPIRV_get_ptr(program.get());
      check(vkCreateShaderModule(ctx.device, &shaderInfo, nullptr, &shader), "shader creation");
      std::vector<VkDescriptorSetLayoutBinding> bindings(buffers.size());
      for (uint32_t i = 0; i < buffers.size(); ++i)
        bindings[i] = {i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
      VkDescriptorSetLayoutCreateInfo setInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
      setInfo.bindingCount = bindings.size(); setInfo.pBindings = bindings.data();
      check(vkCreateDescriptorSetLayout(ctx.device, &setInfo, nullptr, &setLayout), "descriptor layout");
      VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(Push)};
      VkPipelineLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
      layoutInfo.setLayoutCount = 1; layoutInfo.pSetLayouts = &setLayout;
      layoutInfo.pushConstantRangeCount = 1; layoutInfo.pPushConstantRanges = &range;
      check(vkCreatePipelineLayout(ctx.device, &layoutInfo, nullptr, &layout), "pipeline layout");
      VkComputePipelineCreateInfo pipelineInfo{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
      pipelineInfo.layout = layout;
      pipelineInfo.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
      pipelineInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
      pipelineInfo.stage.module = shader; pipelineInfo.stage.pName = "main";
      check(vkCreateComputePipelines(ctx.device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline), "compute pipeline");
      VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, uint32_t(buffers.size())};
      VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
      poolInfo.maxSets = 1; poolInfo.poolSizeCount = 1; poolInfo.pPoolSizes = &poolSize;
      check(vkCreateDescriptorPool(ctx.device, &poolInfo, nullptr, &pool), "descriptor pool");
      VkDescriptorSetAllocateInfo allocation{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
      allocation.descriptorPool = pool; allocation.descriptorSetCount = 1; allocation.pSetLayouts = &setLayout;
      check(vkAllocateDescriptorSets(ctx.device, &allocation, &set), "descriptor allocation");
      for (uint32_t i = 0; i < buffers.size(); ++i) {
        VkDescriptorBufferInfo bufferInfo{buffers[i]->handle, 0, buffers[i]->bytes};
        VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstSet = set; write.dstBinding = i; write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; write.pBufferInfo = &bufferInfo;
        vkUpdateDescriptorSets(ctx.device, 1, &write, 0, nullptr);
      }
    } catch (...) { release(); throw; }
  }
  void release() {
    if (pipeline) vkDestroyPipeline(context.device, pipeline, nullptr);
    if (shader) vkDestroyShaderModule(context.device, shader, nullptr);
    if (layout) vkDestroyPipelineLayout(context.device, layout, nullptr);
    if (pool) vkDestroyDescriptorPool(context.device, pool, nullptr);
    if (setLayout) vkDestroyDescriptorSetLayout(context.device, setLayout, nullptr);
  }
  ~Kernel() { release(); }
  void append(VkCommandBuffer command, Push push) {
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, nullptr);
    vkCmdPushConstants(command, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
    const uint32_t groups = (push.count + 127) / 128;
    const uint32_t x = std::min(groups, 65535u);
    vkCmdDispatch(command, x, (groups + x - 1) / x, 1);
  }
};
constexpr const char* multiplySource = R"glsl(#version 450
layout(local_size_x=128) in;
layout(binding=0) readonly buffer Reference { vec2 ref[]; };
layout(binding=1) buffer Surveillance { vec2 surv[]; };
layout(push_constant) uniform Parameters { uint count; uint range; uint doppler; uint delays; int delayMin; } p;
void main() {
  uint i = gl_GlobalInvocationID.x + gl_GlobalInvocationID.y * gl_NumWorkGroups.x * 128;
  if (i >= p.count) return;
  vec2 a = surv[i], b = ref[i % (p.range * p.doppler)];
  surv[i] = vec2(a.x*b.x+a.y*b.y, a.y*b.x-a.x*b.y) / float(p.range);
})glsl";
constexpr const char* gatherSource = R"glsl(#version 450
layout(local_size_x=128) in;
layout(binding=0) readonly buffer Surveillance { vec2 surv[]; };
layout(binding=1) writeonly buffer Doppler { vec2 result[]; };
layout(push_constant) uniform Parameters { uint count; uint range; uint doppler; uint delays; int delayMin; } p;
void main() {
  uint i = gl_GlobalInvocationID.x + gl_GlobalInvocationID.y * gl_NumWorkGroups.x * 128;
  if (i >= p.count) return;
  uint channel = i / (p.delays * p.doppler);
  uint delay = (i / p.doppler) % p.delays, d = i % p.doppler;
  int lag = int(delay) + p.delayMin;
  uint bin = uint(lag < 0 ? int(p.range) + lag : lag);
  result[i] = surv[(channel * p.doppler + d) * p.range + bin];
})glsl";
void barrier(VkCommandBuffer command) {
  VkMemoryBarrier memory{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
  memory.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
  memory.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
  vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
    VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &memory, 0, nullptr, 0, nullptr);
}
class VulkanBackend final : public GpuBackend {
  Context context_;
  GpuGeometry geometry_;
  // Destruction order is intentional: plans/kernels, buffers, then context.
  std::unique_ptr<Buffer> reference_, surveillance_, doppler_, input_, output_;
  std::unique_ptr<Plan> referencePlan_, rangePlan_, dopplerPlan_;
  std::unique_ptr<Kernel> multiply_, gather_;
  VkCommandBuffer command_ = VK_NULL_HANDLE;
public:
  VulkanBackend(std::shared_ptr<Instance> instance, Candidate candidate, GpuGeometry g)
    : context_(std::move(instance), std::move(candidate)), geometry_(g) {
    if (!g.range || g.range > 65535 || !g.doppler || g.doppler > 65535 ||
        !g.delays || g.delays > 65535 || !g.channels || g.channels > 8)
      throw std::runtime_error("GPU radar dimensions are unsupported; using CPU");
    const uint64_t range = uint64_t(g.range) * g.doppler;
    const uint64_t doppler = uint64_t(g.doppler) * g.delays * g.channels;
    const uint64_t total = 2 * (range * (1 + g.channels) + doppler) * sizeof(std::complex<float>);
    const auto& limits = context_.candidate.properties.limits;
    const auto budget = std::min<uint64_t>(512ULL << 20, context_.candidate.info.memoryBytes / 4);
    if (!g.range || !g.doppler || !g.delays || !g.channels || g.channels > 8 ||
        range * g.channels > UINT32_MAX / 2 || doppler > UINT32_MAX / 2 ||
        g.delayMin <= -int64_t(g.range) || int64_t(g.delayMin) + g.delays > g.range ||
        total > budget || limits.maxComputeWorkGroupInvocations < 128 ||
        limits.maxComputeWorkGroupSize[0] < 128 || limits.maxComputeWorkGroupCount[0] < 65535 ||
        std::max(range * g.channels, doppler) * sizeof(std::complex<float>) > limits.maxStorageBufferRange)
      throw std::runtime_error("GPU capacity is too small for these radar settings; using CPU");
    reference_ = std::make_unique<Buffer>(context_, range * 8, false);
    surveillance_ = std::make_unique<Buffer>(context_, range * g.channels * 8, false);
    doppler_ = std::make_unique<Buffer>(context_, doppler * 8, false);
    input_ = std::make_unique<Buffer>(context_, reference_->bytes + surveillance_->bytes, true);
    output_ = std::make_unique<Buffer>(context_, doppler_->bytes, true);
    referencePlan_ = std::make_unique<Plan>(context_, *reference_, g.range, g.doppler);
    rangePlan_ = std::make_unique<Plan>(context_, *surveillance_, g.range, g.doppler * g.channels);
    dopplerPlan_ = std::make_unique<Plan>(context_, *doppler_, g.doppler, g.delays * g.channels);
    multiply_ = std::make_unique<Kernel>(context_, multiplySource, std::vector<Buffer*>{reference_.get(), surveillance_.get()});
    gather_ = std::make_unique<Kernel>(context_, gatherSource, std::vector<Buffer*>{surveillance_.get(), doppler_.get()});
    VkCommandBufferAllocateInfo allocation{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocation.commandPool = context_.pool; allocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocation.commandBufferCount = 1;
    check(vkAllocateCommandBuffers(context_.device, &allocation, &command_), "command buffer");
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    check(vkBeginCommandBuffer(command_, &begin), "recording");
    VkBufferCopy refCopy{0, 0, reference_->bytes};
    VkBufferCopy survCopy{reference_->bytes, 0, surveillance_->bytes};
    vkCmdCopyBuffer(command_, input_->handle, reference_->handle, 1, &refCopy);
    vkCmdCopyBuffer(command_, input_->handle, surveillance_->handle, 1, &survCopy);
    barrier(command_);
    referencePlan_->append(command_, -1); rangePlan_->append(command_, -1); barrier(command_);
    Push push{uint32_t(range * g.channels), g.range, g.doppler, g.delays, g.delayMin};
    multiply_->append(command_, push); barrier(command_);
    rangePlan_->append(command_, 1); barrier(command_);
    push.count = doppler;
    gather_->append(command_, push); barrier(command_);
    dopplerPlan_->append(command_, -1); barrier(command_);
    VkBufferCopy outputCopy{0, 0, doppler_->bytes};
    vkCmdCopyBuffer(command_, doppler_->handle, output_->handle, 1, &outputCopy);
    // Fence completion alone does not make discrete-GPU staging writes visible.
    VkMemoryBarrier hostRead{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    hostRead.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; hostRead.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(command_, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
      0, 1, &hostRead, 0, nullptr, 0, nullptr);
    check(vkEndCommandBuffer(command_), "command recording");
  }
  GpuDevice device() const override { return context_.candidate.info; }
  void process(const std::vector<std::complex<float>>& reference,
    const std::vector<std::complex<float>>& surveillance,
    std::vector<std::complex<float>>& output) override {
    if (reference.size() * 8 != reference_->bytes || surveillance.size() * 8 != surveillance_->bytes)
      throw std::invalid_argument("GPU input dimensions changed");
    std::memcpy(input_->mapped, reference.data(), reference_->bytes);
    std::memcpy(static_cast<char*>(input_->mapped) + reference_->bytes, surveillance.data(), surveillance_->bytes);
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1; submit.pCommandBuffers = &command_;
    check(vkQueueSubmit(context_.queue, 1, &submit, context_.fence), "submission");
    // Device loss is surfaced by the driver. A slow-but-running queue must be
    // drained before freeing its buffers; it is not safe to pretend cancellation.
    check(vkWaitForFences(context_.device, 1, &context_.fence, VK_TRUE, UINT64_MAX), "execution");
    check(vkResetFences(context_.device, 1, &context_.fence), "fence reset");
    output.resize(output_->bytes / sizeof(std::complex<float>));
    std::memcpy(output.data(), output_->mapped, output_->bytes);
  }
};
}
}
extern "C" std::vector<blah2::GpuDevice> blah2_gpu_devices() {
  blah2::Instance instance;
  std::vector<blah2::GpuDevice> result;
  for (const auto& item : blah2::enumerate(instance)) result.push_back(item.info);
  return result;
}
extern "C" blah2::GpuBackend* blah2_gpu_create(unsigned abi, const blah2::GpuGeometry* geometry, const char* requested) {
  if (abi != blah2::GPU_ABI || !geometry) throw std::runtime_error("GPU module version mismatch");
  auto instance = std::make_shared<blah2::Instance>();
  std::string reason = "No compatible GPU is available; using CPU";
  const std::string selection = requested ? requested : "auto";
  for (const auto& candidate : blah2::enumerate(*instance)) {
    if (selection != "auto" && selection != candidate.info.id) continue;
    try { return new blah2::VulkanBackend(instance, candidate, *geometry); }
    catch (const std::exception& error) { reason = error.what(); }
  }
  throw std::runtime_error(reason);
}
