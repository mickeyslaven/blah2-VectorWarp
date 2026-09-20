// Portable batched ambiguity processing. Uses Vulkan compute, not graphics or
// vendor-specific CUDA/ROCm runtimes. VkFFT supplies the FFT kernels; the two
// small shaders below implement the same correlation/gather as CPU Ambiguity.
#include "GpuBackend.h"
#include "GpuDriverStatus.h"
#include "GpuMemory.h"
#include <vulkan/vulkan.h>
#include <glslang/Include/glslang_c_interface.h>
#if __has_include(<glslang/Public/resource_limits_c.h>)
#include <glslang/Public/resource_limits_c.h>
#else
// glslang 11 (Ubuntu 22.04) exports this C API but does not install its header.
extern "C" const glslang_resource_t* glslang_default_resource(void);
#endif
#include <algorithm>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#ifdef __APPLE__
#include <fftw3.h>
#endif

// VkFFT is header-only. Route its own device allocations through the same
// budget as our buffers, including transient upload and prime-length scratch.
// The definitions below call the real Vulkan functions after these macros end.
static VkResult budgetAllocateMemory(VkDevice, const VkMemoryAllocateInfo*,
  const VkAllocationCallbacks*, VkDeviceMemory*);
static void budgetFreeMemory(VkDevice, VkDeviceMemory, const VkAllocationCallbacks*);
#define vkAllocateMemory budgetAllocateMemory
#define vkFreeMemory budgetFreeMemory
#include <vkFFT.h>
#undef vkFreeMemory
#undef vkAllocateMemory

namespace {
struct DeviceAllocations {
  struct Entry { uint32_t type; uint64_t bytes; };
  blah2::gpu_memory::AllocationBudget budget;
  std::unordered_map<VkDeviceMemory, Entry> allocations;
  DeviceAllocations(uint64_t limit, blah2::gpu_memory::Properties properties)
    : budget(limit, std::move(properties)) {}
};
std::mutex allocationsMutex;
std::unordered_map<VkDevice, DeviceAllocations> deviceAllocations;
}

static VkResult budgetAllocateMemory(VkDevice device, const VkMemoryAllocateInfo* info,
    const VkAllocationCallbacks* callbacks, VkDeviceMemory* memory) {
  std::lock_guard<std::mutex> lock(allocationsMutex);
  const auto found = deviceAllocations.find(device);
  if (found == deviceAllocations.end()) return VK_ERROR_INITIALIZATION_FAILED;
  auto& state = found->second;
  *memory = VK_NULL_HANDLE;
  if (!state.budget.reserve(info->memoryTypeIndex, info->allocationSize)) {
    if (std::getenv("BLAH2_GPU_DIAGNOSTICS"))
      std::cerr << "GPU allocation budget rejected bytes=" << info->allocationSize
        << " type=" << info->memoryTypeIndex << " used=" << state.budget.used()
        << " limit=" << state.budget.limit() << '\n';
    return VK_ERROR_OUT_OF_DEVICE_MEMORY;
  }
  const VkResult result = vkAllocateMemory(device, info, callbacks, memory);
  if (result == VK_SUCCESS) {
    try {
      state.allocations.emplace(*memory,
        DeviceAllocations::Entry{info->memoryTypeIndex, info->allocationSize});
      return result;
    } catch (...) {
      vkFreeMemory(device, *memory, callbacks); *memory = VK_NULL_HANDLE;
      state.budget.release(info->memoryTypeIndex, info->allocationSize);
      return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
  }
  state.budget.release(info->memoryTypeIndex, info->allocationSize);
  return result;
}
static void budgetFreeMemory(VkDevice device, VkDeviceMemory memory,
    const VkAllocationCallbacks* callbacks) {
  std::lock_guard<std::mutex> lock(allocationsMutex);
  const auto found = deviceAllocations.find(device);
  if (found != deviceAllocations.end()) {
    auto& state = found->second;
    const auto allocation = state.allocations.find(memory);
    if (allocation != state.allocations.end()) {
      state.budget.release(allocation->second.type, allocation->second.bytes);
      state.allocations.erase(allocation);
    }
  }
  vkFreeMemory(device, memory, callbacks);
}

namespace blah2 {
namespace {
gpu_memory::Properties memoryProperties(VkPhysicalDevice physical);
class ClutterRejected final : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};
static_assert(VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT == gpu_memory::deviceLocal);
static_assert(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT == gpu_memory::hostVisible);
static_assert(VK_MEMORY_PROPERTY_HOST_COHERENT_BIT == gpu_memory::hostCoherent);
static_assert(VK_MEMORY_PROPERTY_HOST_CACHED_BIT == gpu_memory::hostCached);
void startupTrace(const char* stage) {
  if (!std::getenv("BLAH2_GPU_DIAGNOSTICS")) return;
  std::cerr << "GPU startup " << std::chrono::duration_cast<std::chrono::milliseconds>(
    std::chrono::steady_clock::now().time_since_epoch()).count() << " ms: " << stage << std::endl;
}
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
  bool diagnosticProperties = false;
  explicit Instance(bool diagnostic = false) {
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "blah2"; app.apiVersion = VK_API_VERSION_1_0;
    VkInstanceCreateInfo info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    info.pApplicationInfo = &app;
    uint32_t count = 0;
    check(vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr), "extension discovery");
    std::vector<VkExtensionProperties> extensions(count);
    check(vkEnumerateInstanceExtensionProperties(nullptr, &count, extensions.data()), "extension discovery");
    const auto supports = [&](const char* name) {
      return std::any_of(extensions.begin(), extensions.end(), [&](const auto& value) {
        return std::strcmp(value.extensionName, name) == 0;
      });
    };
    std::vector<const char*> enabled;
    const char* properties = VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME;
    if (supports(properties)) {
      enabled.push_back(properties);
      diagnosticProperties = diagnostic;
    }
#ifdef VK_KHR_portability_enumeration
    // Portability drivers (including MoltenVK) are hidden by the loader unless
    // both the extension and enumeration flag are requested. Detect at runtime
    // so this also remains correct on native Vulkan implementations.
    if (supports(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME)) {
      enabled.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
      info.flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
    }
#endif
    info.enabledExtensionCount = static_cast<uint32_t>(enabled.size());
    info.ppEnabledExtensionNames = enabled.data();
    startupTrace("vkCreateInstance begin");
    check(vkCreateInstance(&info, nullptr, &handle), "driver initialization");
    startupTrace("vkCreateInstance complete");
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
  startupTrace("device enumeration begin");
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
  startupTrace("device enumeration complete");
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
  bool portability = false;
  Context(std::shared_ptr<Instance> owner, Candidate selected)
    : instance(std::move(owner)), candidate(std::move(selected)) {
    try {
      const float priority = 1;
      VkDeviceQueueCreateInfo queueInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
      queueInfo.queueFamilyIndex = candidate.queue;
      queueInfo.queueCount = 1; queueInfo.pQueuePriorities = &priority;
      VkDeviceCreateInfo info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
      info.queueCreateInfoCount = 1; info.pQueueCreateInfos = &queueInfo;
      uint32_t extensionCount = 0;
      check(vkEnumerateDeviceExtensionProperties(candidate.physical, nullptr,
        &extensionCount, nullptr), "device extension discovery");
      std::vector<VkExtensionProperties> extensions(extensionCount);
      check(vkEnumerateDeviceExtensionProperties(candidate.physical, nullptr,
        &extensionCount, extensions.data()), "device extension discovery");
      // The extension name is stable even in SDKs that gate the beta feature
      // structs. We use no optional subset features and need no beta headers.
      const char* portabilityExtension = "VK_KHR_portability_subset";
      if (std::any_of(extensions.begin(), extensions.end(), [&](const auto& value) {
        return std::strcmp(value.extensionName, portabilityExtension) == 0;
      })) {
        portability = true;
        info.enabledExtensionCount = 1;
        info.ppEnabledExtensionNames = &portabilityExtension;
      }
      startupTrace(candidate.info.name.c_str());
      startupTrace("vkCreateDevice begin");
      check(vkCreateDevice(candidate.physical, &info, nullptr, &device), "device initialization");
      {
        std::lock_guard<std::mutex> lock(allocationsMutex);
        deviceAllocations.try_emplace(device,
          gpu_memory::heapBudget(candidate.info.memoryBytes),
          memoryProperties(candidate.physical));
      }
      startupTrace("vkCreateDevice complete");
      vkGetDeviceQueue(device, candidate.queue, 0, &queue);
      VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
      poolInfo.queueFamilyIndex = candidate.queue;
      check(vkCreateCommandPool(device, &poolInfo, nullptr, &pool), "command allocation");
      VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
      check(vkCreateFence(device, &fenceInfo, nullptr, &fence), "fence allocation");
      startupTrace("glslang initialization begin");
      compiler = glslang_initialize_process();
      startupTrace("glslang initialization complete");
      if (!compiler) throw std::runtime_error("GPU shader compiler is unavailable");
    } catch (...) { release(); throw; }
  }
  void release() {
    if (compiler) glslang_finalize_process();
    if (fence) vkDestroyFence(device, fence, nullptr);
    if (pool) vkDestroyCommandPool(device, pool, nullptr);
    if (device) {
      vkDestroyDevice(device, nullptr);
      std::lock_guard<std::mutex> lock(allocationsMutex);
      const auto found = deviceAllocations.find(device);
      if (found != deviceAllocations.end()) {
        if (std::getenv("BLAH2_GPU_DIAGNOSTICS"))
          std::cerr << "GPU allocation budget peak=" << found->second.budget.peak()
            << " limit=" << found->second.budget.limit()
            << " remaining=" << found->second.budget.used() << '\n';
        deviceAllocations.erase(found);
      }
    }
  }
  ~Context() { release(); }
};
gpu_memory::Properties memoryProperties(VkPhysicalDevice physical) {
  VkPhysicalDeviceMemoryProperties source{};
  vkGetPhysicalDeviceMemoryProperties(physical, &source);
  gpu_memory::Properties result;
  for (uint32_t i = 0; i < source.memoryHeapCount; ++i)
    result.heaps.push_back({source.memoryHeaps[i].size,
      bool(source.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)});
  for (uint32_t i = 0; i < source.memoryTypeCount; ++i)
    result.types.push_back({source.memoryTypes[i].propertyFlags,
      source.memoryTypes[i].heapIndex});
  return result;
}
struct Buffer {
  Context& context;
  VkBuffer handle = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  void* mapped = nullptr;
  uint64_t bytes, allocationBytes = 0;
  VkMemoryPropertyFlags properties = 0;
  uint32_t heap = UINT32_MAX;
  Buffer(Context& ctx, uint64_t size, VkMemoryPropertyFlags required,
      VkMemoryPropertyFlags preferred = 0) : context(ctx), bytes(size) {
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
      gpu_memory::Properties view;
      for (uint32_t i = 0; i < properties.memoryHeapCount; ++i)
        view.heaps.push_back({properties.memoryHeaps[i].size,
          bool(properties.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)});
      for (uint32_t i = 0; i < properties.memoryTypeCount; ++i)
        view.types.push_back({properties.memoryTypes[i].propertyFlags,
          properties.memoryTypes[i].heapIndex});
      const uint32_t type = gpu_memory::chooseType(requirements.memoryTypeBits,
        required, preferred, view);
      if (type == gpu_memory::noType)
        throw std::runtime_error("GPU has no suitable memory; using CPU");
      this->properties = properties.memoryTypes[type].propertyFlags;
      heap = properties.memoryTypes[type].heapIndex;
      allocationBytes = requirements.size;
      VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
      allocation.allocationSize = requirements.size; allocation.memoryTypeIndex = type;
      check(budgetAllocateMemory(ctx.device, &allocation, nullptr, &memory), "memory allocation");
      check(vkBindBufferMemory(ctx.device, handle, memory, 0), "memory binding");
      if (required & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)
        check(vkMapMemory(ctx.device, memory, 0, allocationBytes, 0, &mapped), "host mapping");
    } catch (...) { release(); throw; }
  }
  void synchronize(uint64_t offset, uint64_t size, bool flush) {
    if (properties & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) return;
    const auto range = gpu_memory::alignedRange(offset, size, allocationBytes,
      context.candidate.properties.limits.nonCoherentAtomSize);
    if ((!range.bytes && !range.whole) || !mapped)
      throw std::runtime_error("GPU mapped-memory range is invalid; using CPU");
    VkMappedMemoryRange memoryRange{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
    memoryRange.memory = memory; memoryRange.offset = range.offset;
    memoryRange.size = range.whole ? VK_WHOLE_SIZE : range.bytes;
    check(flush ? vkFlushMappedMemoryRanges(context.device, 1, &memoryRange) :
      vkInvalidateMappedMemoryRanges(context.device, 1, &memoryRange),
      flush ? "host-memory flush" : "host-memory invalidate");
  }
  void flush(uint64_t offset, uint64_t size) { synchronize(offset, size, true); }
  void invalidate(uint64_t offset, uint64_t size) { synchronize(offset, size, false); }
  void release() {
    if (mapped) vkUnmapMemory(context.device, memory);
    if (handle) vkDestroyBuffer(context.device, handle, nullptr);
    if (memory) budgetFreeMemory(context.device, memory, nullptr);
  }
  ~Buffer() { release(); }
};
struct NativePlan {
  VkFFTApplication app{};
  Buffer& buffer_;
  NativePlan(Context& context, Buffer& buffer, uint32_t length, uint32_t batches,
      bool twiddleLut = false, bool firTuning = false) : buffer_(buffer) {
    VkFFTConfiguration config{};
    config.FFTdim = 1; config.size[0] = length; config.numberBatches = batches;
    // VkFFT's FP32 defaults calculate twiddles on NVIDIA/AMD but use a LUT on
    // Intel. Clutter's repeated cancellation needs the same bounded LUT path on
    // every backend; ambiguity retains the library's native tuning.
    if (twiddleLut) config.useLUT = 1;
    config.device = &context.device; config.queue = &context.queue;
    config.fence = &context.fence; config.commandPool = &context.pool;
    config.physicalDevice = &context.candidate.physical;
    // Bind at append time, after VkFFT has allocated prime-length FFT tables.
    // Binding during initialization can leave descriptors pointing at null LUTs.
    config.bufferSize = &buffer.bytes;
    config.isCompilerInitialized = 1;
    // Preserve the qualified Pi FIR tuning without changing existing radar or
    // whole-clutter VkFFT choices. normalize applies to inverse transforms.
    if (firTuning) {
      config.normalize = 1;
      config.warpSize = 32;
      config.coalescedMemory = 64;
    }
    startupTrace(("VkFFT plan begin length=" + std::to_string(length) + " batches=" + std::to_string(batches)).c_str());
    const auto result = initializeVkFFT(&app, config);
    startupTrace(("VkFFT plan complete result=" + std::to_string(result)).c_str());
    if (result != VKFFT_SUCCESS) { deleteVkFFT(&app); checkFft(result); }
  }
  ~NativePlan() { deleteVkFFT(&app); }
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
          !glslang_shader_parse(shaderSource.get(), &input)) {
        const char* info = shaderSource ? glslang_shader_get_info_log(shaderSource.get()) : nullptr;
        const char* debug = shaderSource ? glslang_shader_get_info_debug_log(shaderSource.get()) : nullptr;
        std::string reason = "GPU shader compilation failed";
        if (info && *info) reason += std::string(": ") + info;
        if (debug && *debug) reason += std::string("; ") + debug;
        throw std::runtime_error(reason);
      }
      auto program = std::unique_ptr<glslang_program_t, decltype(&glslang_program_delete)>(
        glslang_program_create(), glslang_program_delete);
      if (!program) throw std::runtime_error("GPU shader compiler allocation failed");
      glslang_program_add_shader(program.get(), shaderSource.get());
      if (!glslang_program_link(program.get(), input.messages)) {
        const char* info = glslang_program_get_info_log(program.get());
        const char* debug = glslang_program_get_info_debug_log(program.get());
        std::string reason = "GPU shader link failed";
        if (info && *info) reason += std::string(": ") + info;
        if (debug && *debug) reason += std::string("; ") + debug;
        throw std::runtime_error(reason);
      }
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
#ifdef __APPLE__
constexpr const char* chirpPrepareSource = R"glsl(#version 450
layout(local_size_x=128) in;
layout(binding=0) readonly buffer Input { vec2 inputData[]; };
layout(binding=1) writeonly buffer Work { vec2 workData[]; };
layout(binding=2) readonly buffer Chirp { vec2 chirp[]; };
layout(push_constant) uniform Parameters { uint count; uint n; uint padded; uint batches; int direction; } p;
void main() {
  uint i=gl_GlobalInvocationID.x+gl_GlobalInvocationID.y*gl_NumWorkGroups.x*128;
  if(i>=p.count) return;
  uint j=i%p.padded;
  if(j>=p.n) { workData[i]=vec2(0); return; }
  vec2 a=inputData[(i/p.padded)*p.n+j], b=chirp[j];
  if(p.direction>0) b.y=-b.y;
  workData[i]=vec2(a.x*b.x-a.y*b.y,a.x*b.y+a.y*b.x);
})glsl";
constexpr const char* chirpProductSource = R"glsl(#version 450
layout(local_size_x=128) in;
layout(binding=0) buffer Work { vec2 workData[]; };
layout(binding=1) readonly buffer Spectrum { vec2 spectrum[]; };
layout(push_constant) uniform Parameters { uint count; uint n; uint padded; uint batches; int direction; } p;
void main() {
  uint i=gl_GlobalInvocationID.x+gl_GlobalInvocationID.y*gl_NumWorkGroups.x*128;
  if(i>=p.count) return;
  vec2 a=workData[i], b=spectrum[(p.direction>0?p.padded:0)+i%p.padded];
  workData[i]=vec2(a.x*b.x-a.y*b.y,a.x*b.y+a.y*b.x);
})glsl";
constexpr const char* chirpFinishSource = R"glsl(#version 450
layout(local_size_x=128) in;
layout(binding=0) readonly buffer Work { vec2 workData[]; };
layout(binding=1) writeonly buffer Output { vec2 outputData[]; };
layout(binding=2) readonly buffer Chirp { vec2 chirp[]; };
layout(push_constant) uniform Parameters { uint count; uint n; uint padded; uint batches; int direction; } p;
void main() {
  uint i=gl_GlobalInvocationID.x+gl_GlobalInvocationID.y*gl_NumWorkGroups.x*128;
  if(i>=p.count) return;
  uint j=i%p.n;
  vec2 a=workData[(i/p.n)*p.padded+j], b=chirp[j];
  if(p.direction>0) b.y=-b.y;
  outputData[i]=vec2(a.x*b.x-a.y*b.y,a.x*b.y+a.y*b.x)/float(p.padded);
})glsl";

// Native VkFFT prime plans were intermittently incorrect through MoltenVK.
// Express Bluestein explicitly using smooth-length GPU FFTs. Only constant
// chirp spectra are generated in FP64 on the CPU at setup; no IQ reaches FFTW.
struct PortablePrimePlan {
  uint32_t length_, batches_, padded_;
  std::unique_ptr<Buffer> work_, chirp_, spectrum_;
  std::unique_ptr<NativePlan> fft_;
  std::unique_ptr<Kernel> prepare_, product_, finish_;
  PortablePrimePlan(Context& context, Buffer& buffer, uint32_t length, uint32_t batches)
    : length_(length), batches_(batches), padded_(gpu_memory::nextSmooth(2ULL*length-1)) {
    const uint64_t elements=uint64_t(padded_)*batches;
    if (!padded_ || elements>UINT32_MAX/2 ||
        elements*sizeof(std::complex<float>)>context.candidate.properties.limits.maxStorageBufferRange)
      throw std::runtime_error("GPU prime FFT capacity is too small; using CPU");
    work_=std::make_unique<Buffer>(context,elements*8,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    const auto host=VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
    const auto preferred=VK_MEMORY_PROPERTY_HOST_COHERENT_BIT|VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
    chirp_=std::make_unique<Buffer>(context,uint64_t(length)*8,host,preferred);
    spectrum_=std::make_unique<Buffer>(context,uint64_t(padded_)*16,host,preferred);
    std::vector<std::complex<double>> chirps(length), input(padded_), output(padded_);
    auto* chirp=static_cast<std::complex<float>*>(chirp_->mapped);
    auto* spectrum=static_cast<std::complex<float>*>(spectrum_->mapped);
    const double pi=std::acos(-1.);
    for (uint32_t i=0;i<length;++i) {
      // Reduce the integer phase before conversion to avoid large-angle error.
      const uint64_t phase=uint64_t(i)*i%(2ULL*length);
      chirps[i]=std::polar(1.,-pi*double(phase)/length);
      chirp[i]=static_cast<std::complex<float>>(chirps[i]);
    }
    const auto plan=fftw_plan_dft_1d(padded_,reinterpret_cast<fftw_complex*>(input.data()),
      reinterpret_cast<fftw_complex*>(output.data()),FFTW_FORWARD,FFTW_ESTIMATE);
    if (!plan) throw std::runtime_error("Cannot prepare GPU prime FFT constants; using CPU");
    for (unsigned inverse=0;inverse<2;++inverse) {
      std::fill(input.begin(),input.end(),std::complex<double>{}); input[0]=1.;
      for (uint32_t i=1;i<length;++i)
        input[i]=input[padded_-i]=inverse?chirps[i]:std::conj(chirps[i]);
      fftw_execute(plan);
      for (uint32_t i=0;i<padded_;++i)
        spectrum[inverse*padded_+i]=static_cast<std::complex<float>>(output[i]);
    }
    fftw_destroy_plan(plan);
    chirp_->flush(0,chirp_->bytes); spectrum_->flush(0,spectrum_->bytes);
    fft_=std::make_unique<NativePlan>(context,*work_,padded_,batches,true);
    prepare_=std::make_unique<Kernel>(context,chirpPrepareSource,
      std::vector<Buffer*>{&buffer,work_.get(),chirp_.get()});
    product_=std::make_unique<Kernel>(context,chirpProductSource,
      std::vector<Buffer*>{work_.get(),spectrum_.get()});
    finish_=std::make_unique<Kernel>(context,chirpFinishSource,
      std::vector<Buffer*>{work_.get(),&buffer,chirp_.get()});
    startupTrace(("portable prime FFT length="+std::to_string(length)+
      " convolution="+std::to_string(padded_)).c_str());
  }
  void append(VkCommandBuffer command,int direction) {
    VkMemoryBarrier hostWrite{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    hostWrite.srcAccessMask=VK_ACCESS_HOST_WRITE_BIT;
    hostWrite.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_HOST_BIT,
      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,1,&hostWrite,0,nullptr,0,nullptr);
    Push push{padded_*batches_,length_,padded_,batches_,direction};
    prepare_->append(command,push); barrier(command);
    fft_->append(command,-1); barrier(command);
    product_->append(command,push); barrier(command);
    fft_->append(command,1); barrier(command);
    push.count=length_*batches_;
    finish_->append(command,push); barrier(command);
  }
};
#endif
struct Plan {
  std::unique_ptr<NativePlan> native_;
#ifdef __APPLE__
  std::unique_ptr<PortablePrimePlan> portable_;
#endif
  Plan(Context& context,Buffer& buffer,uint32_t length,uint32_t batches,
      bool twiddleLut=false,bool firTuning=false) {
#ifdef __APPLE__
    uint32_t rest=length;
    for (uint32_t radix:{2u,3u,5u,7u,11u,13u}) while(rest%radix==0) rest/=radix;
    if (context.portability && rest!=1) {
      portable_=std::make_unique<PortablePrimePlan>(context,buffer,length,batches);
      return;
    }
#endif
    native_=std::make_unique<NativePlan>(context,buffer,length,batches,twiddleLut,firTuning);
  }
  void append(VkCommandBuffer command,int direction) {
#ifdef __APPLE__
    if (portable_) { portable_->append(command,direction); return; }
#endif
    native_->append(command,direction);
  }
};
constexpr const char* clutterProductsSource = R"glsl(#version 450
layout(local_size_x=128) in;
layout(binding=0) readonly buffer Reference { vec2 ref[]; };
layout(binding=1) readonly buffer Surveillance { vec2 surv[]; };
layout(binding=2) writeonly buffer Auto { vec2 autocorr[]; };
layout(binding=3) writeonly buffer Cross { vec2 crosscorr[]; };
layout(push_constant) uniform Parameters { uint count; uint samples; uint channels; uint unused; int unused2; } p;
void main() {
  uint i = gl_GlobalInvocationID.x + gl_GlobalInvocationID.y * gl_NumWorkGroups.x * 128;
  if (i >= p.count) return;
  vec2 x = ref[i % p.samples], y = surv[i];
  crosscorr[i] = vec2(y.x*x.x+y.y*x.y, y.y*x.x-y.x*x.y);
  if (i < p.samples) autocorr[i] = vec2(dot(x,x), 0);
})glsl";
constexpr const char* clutterMultiplySource = R"glsl(#version 450
layout(local_size_x=128) in;
layout(binding=0) readonly buffer Reference { vec2 ref[]; };
layout(binding=1) readonly buffer Weights { vec2 weights[]; };
layout(binding=2) writeonly buffer Filtered { vec2 filtered[]; };
layout(push_constant) uniform Parameters { uint count; uint fftLength; uint channels; uint unused; int unused2; } p;
void main() {
  uint i = gl_GlobalInvocationID.x + gl_GlobalInvocationID.y * gl_NumWorkGroups.x * 128;
  if (i >= p.count) return;
  vec2 a = ref[i % p.fftLength], b = weights[i];
  filtered[i] = vec2(a.x*b.x-a.y*b.y, a.x*b.y+a.y*b.x);
})glsl";
constexpr const char* clutterNormalizeSource = R"glsl(#version 450
layout(local_size_x=128) in;
layout(binding=0) readonly buffer Filtered { vec2 filtered[]; };
layout(binding=1) writeonly buffer Estimate { vec2 estimate[]; };
layout(push_constant) uniform Parameters { uint count; uint samples; uint fftLength; uint unused; int unused2; } p;
void main() {
  uint i = gl_GlobalInvocationID.x + gl_GlobalInvocationID.y * gl_NumWorkGroups.x * 128;
  if (i >= p.count) return;
  uint channel = i / p.samples, sampleIndex = i % p.samples;
  estimate[i] = filtered[channel * p.fftLength + sampleIndex] / float(p.fftLength);
})glsl";

class ClutterPipeline {
  Context& context_;
  uint32_t samples_, bins_, channels_, length_;
  std::unique_ptr<Buffer> reference_, surveillance_, estimate_, autocorrelation_, crosscorrelation_;
  std::unique_ptr<Buffer> filterReference_, weights_, filtered_;
  std::unique_ptr<Buffer> input_, correlations_, weightUpload_, output_;
  std::unique_ptr<Plan> referencePlan_, surveillancePlan_, autoPlan_, crossPlan_;
  std::unique_ptr<Plan> filterReferencePlan_, weightPlan_, filteredPlan_;
  std::unique_ptr<Kernel> products_, multiply_, normalize_;
  VkCommandBuffer prepare_ = VK_NULL_HANDLE, finish_ = VK_NULL_HANDLE;
  bool direct_ = false;
  std::vector<std::complex<double>> first_, lower_, solved_, temporary_;

  void submit(VkCommandBuffer command, const char* phase) {
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1; submit.pCommandBuffers = &command;
    check(vkQueueSubmit(context_.queue, 1, &submit, context_.fence), phase);
    check(vkWaitForFences(context_.device, 1, &context_.fence, VK_TRUE, UINT64_MAX), phase);
    check(vkResetFences(context_.device, 1, &context_.fence), "clutter fence reset");
  }
  void solveWeights() {
    using Double = std::complex<double>;
    const auto* values = static_cast<const std::complex<float>*>(correlations_->mapped);
    auto& first = first_;
    for (uint32_t i = 0; i < bins_; ++i) first[i] = std::conj(Double(values[i])) / double(samples_);
    auto& lower = lower_;
    double smallestPivot = std::numeric_limits<double>::infinity(), largestPivot = 0;
    for (uint32_t row = 0; row < bins_; ++row)
      for (uint32_t column = 0; column <= row; ++column) {
        Double sum = row >= column ? std::conj(first[row-column]) : first[column-row];
        for (uint32_t k = 0; k < column; ++k)
          sum -= lower[size_t(row)*bins_+k] * std::conj(lower[size_t(column)*bins_+k]);
        if (row == column) {
          const double scale = std::max(std::abs(first[0]), 1e-30);
          if (!std::isfinite(sum.real()) || sum.real() <= scale * 1e-12 ||
              std::abs(sum.imag()) > scale * 1e-5)
            throw ClutterRejected("GPU clutter Cholesky failed; using CPU clutter");
          const double pivot = std::sqrt(sum.real());
          smallestPivot = std::min(smallestPivot, pivot);
          largestPivot = std::max(largestPivot, pivot);
          lower[size_t(row)*bins_+column] = pivot;
        } else {
          lower[size_t(row)*bins_+column] = sum / lower[size_t(column)*bins_+column];
        }
      }
    if (smallestPivot < largestPivot * 1e-4)
      throw ClutterRejected("GPU clutter correlation is ill-conditioned; using CPU clutter");
    auto& result = solved_;
    auto& temporary = temporary_;
    for (uint32_t channel = 0; channel < channels_; ++channel) {
      const auto* rhs = values + bins_ + size_t(channel) * bins_;
      for (uint32_t row = 0; row < bins_; ++row) {
        Double sum = Double(rhs[row]) / double(samples_);
        for (uint32_t k = 0; k < row; ++k)
          sum -= lower[size_t(row)*bins_+k] * temporary[k];
        temporary[row] = sum / lower[size_t(row)*bins_+row];
      }
      for (int64_t row = bins_-1; row >= 0; --row) {
        Double sum = temporary[row];
        for (uint32_t k = row+1; k < bins_; ++k)
          sum -= std::conj(lower[size_t(k)*bins_+row]) * result[size_t(channel)*bins_+k];
        result[size_t(channel)*bins_+row] = sum / lower[size_t(row)*bins_+row];
      }
      double residual = 0, rhsPower = 0;
      for (uint32_t row = 0; row < bins_; ++row) {
        Double calculated{};
        for (uint32_t column = 0; column < bins_; ++column) {
          const Double matrix = row >= column ? std::conj(first[row-column]) : first[column-row];
          calculated += matrix * result[size_t(channel)*bins_+column];
        }
        const Double expected = Double(rhs[row]) / double(samples_);
        residual += std::norm(calculated-expected); rhsPower += std::norm(expected);
      }
      if (residual > std::max(rhsPower * 1e-10, 1e-20))
        throw ClutterRejected("GPU clutter solve residual is unstable; using CPU clutter");
    }
  }
public:
  static bool twiddleLut() {
    const char* setting = std::getenv("BLAH2_GPU_CLUTTER_TWIDDLES");
    const std::string mode = setting ? setting : "lut";
    if (mode != "lut" && mode != "native")
      throw std::invalid_argument("BLAH2_GPU_CLUTTER_TWIDDLES must be lut or native");
    return mode == "lut";
  }
  static uint32_t filterLength(const GpuGeometry& g) {
    const char* setting = std::getenv("BLAH2_GPU_CLUTTER_PADDING");
    const std::string mode = setting ? setting : "smooth";
    if (mode == "exact") {
      const uint64_t value = uint64_t(g.clutterSamples) + g.clutterBins + 1;
      return value <= UINT32_MAX ? uint32_t(value) : 0;
    }
    if (mode != "smooth")
      throw std::invalid_argument("BLAH2_GPU_CLUTTER_PADDING must be smooth or exact");
    return gpu_memory::nextSmooth(uint64_t(g.clutterSamples) + g.clutterBins - 1);
  }
  static uint64_t requiredBytes(const GpuGeometry& g) {
    if (!g.clutterSamples) return 0;
    const uint64_t n = g.clutterSamples, c = g.channels;
    const uint64_t b = g.clutterBins, l = filterLength(g);
    if (!l) return std::numeric_limits<uint64_t>::max();
    uint64_t elements = 4*n + 5*n*c + l + 2*l*c + b*c + b*(1+c);
    // Preflight allowance for the seven plan tables. Batched transforms share
    // their LUTs. Actual tables, scratch and upload allocations are additionally
    // enforced by budgetAllocateMemory; this estimate is not the final guard.
    if (twiddleLut()) elements += 4*n + 3*l;
    return elements * sizeof(std::complex<float>);
  }
  ClutterPipeline(Context& context, const GpuGeometry& g,
      bool allowDirect, bool forceDirect)
    : context_(context), samples_(g.clutterSamples), bins_(g.clutterBins),
      channels_(g.channels), length_(filterLength(g)) {
    const uint64_t n = samples_, nc = uint64_t(samples_) * channels_;
    const uint64_t l = length_, lc = uint64_t(length_) * channels_;
    const auto& limit = context.candidate.properties.limits;
    if (!samples_ || !bins_ || bins_ > samples_ || !channels_ || channels_ > 8 ||
        !length_ || length_ < uint64_t(samples_) + bins_ - 1 ||
        nc > UINT32_MAX || lc > UINT32_MAX ||
        std::max({n, nc, l, lc}) * sizeof(std::complex<float>) > limit.maxStorageBufferRange)
      throw std::runtime_error("GPU clutter capacity is too small; using CPU clutter");
    constexpr auto device = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    constexpr auto host = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
    constexpr auto coherent = VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    if (allowDirect) {
      try {
        const auto preferred = coherent | VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
        reference_ = std::make_unique<Buffer>(context, n*8, device | host, preferred);
        surveillance_ = std::make_unique<Buffer>(context, nc*8, device | host, preferred);
        estimate_ = std::make_unique<Buffer>(context, nc*8, device | host, preferred);
        const auto memory = memoryProperties(context.candidate.physical);
        const bool oneHeap = reference_->heap == surveillance_->heap &&
          reference_->heap == estimate_->heap;
        const uint64_t bytes = reference_->allocationBytes +
          surveillance_->allocationBytes + estimate_->allocationBytes;
        // Every allocation is also charged to the existing aggregate/per-heap
        // budget. AUTO requires the same unified-memory topology as ambiguity.
        direct_ = forceDirect || (oneHeap && gpu_memory::autoDirect(
          context.candidate.properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU,
          reference_->heap, bytes, memory));
      } catch (const std::exception&) { direct_ = false; }
      if (!direct_) { reference_.reset(); surveillance_.reset(); estimate_.reset(); }
    }
    if (!direct_) {
      reference_ = std::make_unique<Buffer>(context, n*8, device);
      surveillance_ = std::make_unique<Buffer>(context, nc*8, device);
      estimate_ = std::make_unique<Buffer>(context, nc*8, device);
      input_ = std::make_unique<Buffer>(context, (n+nc)*8, host, coherent);
      output_ = std::make_unique<Buffer>(context, nc*8, host,
        coherent | VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
    }
    startupTrace(direct_ ? "clutter direct mapped device memory selected" :
      "clutter persistent staging selected");
    autocorrelation_ = std::make_unique<Buffer>(context, n*8, device);
    crosscorrelation_ = std::make_unique<Buffer>(context, nc*8, device);
    filterReference_ = std::make_unique<Buffer>(context, l*8, device);
    weights_ = std::make_unique<Buffer>(context, lc*8, device);
    filtered_ = std::make_unique<Buffer>(context, lc*8, device);
    correlations_ = std::make_unique<Buffer>(context, bins_*(1+channels_)*8, host,
      coherent | VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
    weightUpload_ = std::make_unique<Buffer>(context, uint64_t(bins_)*channels_*8,
      host, coherent);
    first_.resize(bins_);
    lower_.resize(size_t(bins_) * bins_);
    solved_.resize(size_t(channels_) * bins_);
    temporary_.resize(bins_);
    const bool lut = twiddleLut();
    referencePlan_ = std::make_unique<Plan>(context, *reference_, samples_, 1, lut);
    surveillancePlan_ = std::make_unique<Plan>(context, *surveillance_, samples_, channels_, lut);
    autoPlan_ = std::make_unique<Plan>(context, *autocorrelation_, samples_, 1, lut);
    crossPlan_ = std::make_unique<Plan>(context, *crosscorrelation_, samples_, channels_, lut);
    filterReferencePlan_ = std::make_unique<Plan>(context, *filterReference_, length_, 1, lut);
    weightPlan_ = std::make_unique<Plan>(context, *weights_, length_, channels_, lut);
    filteredPlan_ = std::make_unique<Plan>(context, *filtered_, length_, channels_, lut);
    products_ = std::make_unique<Kernel>(context, clutterProductsSource,
      std::vector<Buffer*>{reference_.get(), surveillance_.get(), autocorrelation_.get(), crosscorrelation_.get()});
    multiply_ = std::make_unique<Kernel>(context, clutterMultiplySource,
      std::vector<Buffer*>{filterReference_.get(), weights_.get(), filtered_.get()});
    normalize_ = std::make_unique<Kernel>(context, clutterNormalizeSource,
      std::vector<Buffer*>{filtered_.get(), estimate_.get()});
    VkCommandBuffer commands[2]{};
    VkCommandBufferAllocateInfo allocation{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocation.commandPool = context.pool; allocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocation.commandBufferCount = 2;
    check(vkAllocateCommandBuffers(context.device, &allocation, commands), "clutter command buffers");
    prepare_ = commands[0]; finish_ = commands[1];
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    check(vkBeginCommandBuffer(prepare_, &begin), "clutter prepare recording");
    VkMemoryBarrier hostWrite{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    hostWrite.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
    hostWrite.dstAccessMask = direct_ ?
      VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT : VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(prepare_, VK_PIPELINE_STAGE_HOST_BIT,
      direct_ ? VK_PIPELINE_STAGE_ALL_COMMANDS_BIT : VK_PIPELINE_STAGE_TRANSFER_BIT,
      0, 1, &hostWrite, 0, nullptr, 0, nullptr);
    if (!direct_) {
      VkBufferCopy ref{0,0,n*8}, surv{n*8,0,nc*8};
      vkCmdCopyBuffer(prepare_, input_->handle, reference_->handle, 1, &ref);
      vkCmdCopyBuffer(prepare_, input_->handle, surveillance_->handle, 1, &surv);
    }
    vkCmdFillBuffer(prepare_, filterReference_->handle, 0, l*8, 0);
    barrier(prepare_); // Order the zero fill before overwriting its live prefix.
    VkBufferCopy filterRef{0,0,n*8};
    vkCmdCopyBuffer(prepare_, direct_ ? reference_->handle : input_->handle,
      filterReference_->handle, 1, &filterRef);
    barrier(prepare_);
    referencePlan_->append(prepare_, -1); surveillancePlan_->append(prepare_, -1);
    filterReferencePlan_->append(prepare_, -1); barrier(prepare_);
    products_->append(prepare_, {uint32_t(nc), samples_, channels_, 0, 0}); barrier(prepare_);
    autoPlan_->append(prepare_, 1); crossPlan_->append(prepare_, 1); barrier(prepare_);
    VkBufferCopy autoCopy{0,0,uint64_t(bins_)*8};
    vkCmdCopyBuffer(prepare_, autocorrelation_->handle, correlations_->handle, 1, &autoCopy);
    for (uint32_t channel = 0; channel < channels_; ++channel) {
      VkBufferCopy copy{uint64_t(channel)*samples_*8,
        uint64_t(bins_)*(1+channel)*8, uint64_t(bins_)*8};
      vkCmdCopyBuffer(prepare_, crosscorrelation_->handle, correlations_->handle, 1, &copy);
    }
    VkMemoryBarrier hostRead{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    hostRead.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; hostRead.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(prepare_, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
      0, 1, &hostRead, 0, nullptr, 0, nullptr);
    check(vkEndCommandBuffer(prepare_), "clutter prepare recording");
    check(vkBeginCommandBuffer(finish_, &begin), "clutter finish recording");
    hostWrite.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(finish_, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
      0, 1, &hostWrite, 0, nullptr, 0, nullptr);
    vkCmdFillBuffer(finish_, weights_->handle, 0, lc*8, 0);
    barrier(finish_); // Preserve padded zeros before copying each compact weight vector.
    for (uint32_t channel = 0; channel < channels_; ++channel) {
      VkBufferCopy copy{uint64_t(channel)*bins_*8,
        uint64_t(channel)*length_*8, uint64_t(bins_)*8};
      vkCmdCopyBuffer(finish_, weightUpload_->handle, weights_->handle, 1, &copy);
    }
    barrier(finish_);
    weightPlan_->append(finish_, -1); barrier(finish_);
    multiply_->append(finish_, {uint32_t(lc), length_, channels_, 0, 0}); barrier(finish_);
    filteredPlan_->append(finish_, 1); barrier(finish_);
    normalize_->append(finish_, {uint32_t(nc), samples_, length_, 0, 0}); barrier(finish_);
    if (!direct_) {
      VkBufferCopy outputCopy{0,0,nc*8};
      vkCmdCopyBuffer(finish_, estimate_->handle, output_->handle, 1, &outputCopy);
    }
    hostRead.srcAccessMask = direct_ ? VK_ACCESS_MEMORY_WRITE_BIT : VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(finish_,
      direct_ ? VK_PIPELINE_STAGE_ALL_COMMANDS_BIT : VK_PIPELINE_STAGE_TRANSFER_BIT,
      VK_PIPELINE_STAGE_HOST_BIT,
      0, 1, &hostRead, 0, nullptr, 0, nullptr);
    check(vkEndCommandBuffer(finish_), "clutter finish recording");
  }
  bool process(const std::complex<float>* reference, size_t referenceCount,
      const std::complex<float>* surveillance, size_t surveillanceCount,
      std::complex<float>* output, size_t outputCount) {
    if (!reference || !surveillance || !output || referenceCount != samples_ ||
        surveillanceCount != uint64_t(samples_)*channels_ || outputCount != surveillanceCount)
      throw std::invalid_argument("GPU clutter buffer dimensions changed");
    if (direct_) {
      std::memcpy(reference_->mapped, reference, referenceCount*8);
      std::memcpy(surveillance_->mapped, surveillance, surveillanceCount*8);
      reference_->flush(0, reference_->bytes);
      surveillance_->flush(0, surveillance_->bytes);
    } else {
      std::memcpy(input_->mapped, reference, referenceCount*8);
      std::memcpy(static_cast<char*>(input_->mapped)+referenceCount*8,
        surveillance, surveillanceCount*8);
      input_->flush(0, input_->bytes);
    }
    submit(prepare_, "clutter correlations");
    correlations_->invalidate(0, correlations_->bytes);
    try { solveWeights(); }
    catch (const ClutterRejected&) { return false; }
    const auto& solved = solved_;
    if (!std::all_of(solved.begin(), solved.end(), [](auto value) {
          return std::isfinite(value.real()) && std::isfinite(value.imag());
        })) return false;
    auto* upload = static_cast<std::complex<float>*>(weightUpload_->mapped);
    for (uint32_t channel = 0; channel < channels_; ++channel)
      for (uint32_t bin = 0; bin < bins_; ++bin)
        upload[uint64_t(channel)*bins_+bin] = solved[uint64_t(channel)*bins_+bin];
    weightUpload_->flush(0, weightUpload_->bytes);
    submit(finish_, "clutter filtering");
    Buffer& result = direct_ ? *estimate_ : *output_;
    result.invalidate(0, result.bytes);
    std::memcpy(output, result.mapped, outputCount*8);
    return true;
  }
};

constexpr const char* firPackSource = R"glsl(#version 450
layout(local_size_x=128) in;
layout(binding=0) readonly buffer Input { vec2 inputData[]; };
layout(binding=1) writeonly buffer Work { vec2 work[]; };
layout(push_constant) uniform Parameters { uint count; uint fft; uint history; uint hop; int valid; } p;
void main() {
  uint i = gl_GlobalInvocationID.x + gl_GlobalInvocationID.y * gl_NumWorkGroups.x * 128;
  if (i >= p.count) return;
  int source = p.valid + int((i / p.fft) * p.hop + i % p.fft) - int(p.history);
  work[i] = source < 0 || source >= inputData.length() ? vec2(0.0) : inputData[source];
})glsl";
constexpr const char* firMultiplySource = R"glsl(#version 450
layout(local_size_x=128) in;
layout(binding=0) buffer Work { vec2 work[]; };
layout(binding=1) readonly buffer Weights { vec2 weights[]; };
layout(push_constant) uniform Parameters { uint count; uint fft; uint history; uint hop; int valid; } p;
void main() {
  uint i = gl_GlobalInvocationID.x + gl_GlobalInvocationID.y * gl_NumWorkGroups.x * 128;
  if (i >= p.count) return;
  vec2 x = work[i], w = weights[i % p.fft];
  work[i] = vec2(x.x*w.x-x.y*w.y, x.x*w.y+x.y*w.x);
})glsl";
constexpr const char* firGatherSource = R"glsl(#version 450
layout(local_size_x=128) in;
layout(binding=0) readonly buffer Work { vec2 work[]; };
layout(binding=1) writeonly buffer Output { vec2 outputData[]; };
layout(push_constant) uniform Parameters { uint count; uint fft; uint history; uint hop; int valid; } p;
void main() {
  uint i = gl_GlobalInvocationID.x + gl_GlobalInvocationID.y * gl_NumWorkGroups.x * 128;
  if (i >= p.count) return;
  outputData[uint(p.valid)+i] = work[(i/p.hop)*p.fft+p.history+i%p.hop];
})glsl";

void firComputeBarrier(VkCommandBuffer command) {
  VkMemoryBarrier memory{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
  memory.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
  memory.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
  vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &memory, 0, nullptr, 0, nullptr);
}

// Production FIR-only backend. The worker protocol owns sequencing; this class
// enforces its two-phase device state and never allocates radar/clutter buffers.
class VulkanFirBackend final : public GpuBackend, public GpuFirBufferBackend {
  Context context_;
  uint32_t samples_, taps_, fft_, hop_, blocks_, outputs_;
  std::unique_ptr<Buffer> input_, weights_, work_, output_;
  std::unique_ptr<Kernel> pack_, multiply_, gather_;
  std::unique_ptr<Plan> workPlan_, weightPlan_;
  VkCommandBuffer referenceCommand_ = VK_NULL_HANDLE, finalCommand_ = VK_NULL_HANDLE;
  VkFence referenceFence_ = VK_NULL_HANDLE;
  bool referencePending_ = false, finalPending_ = false, poisoned_ = false;

  void resetAfterFinal() {
    check(vkResetFences(context_.device, 1, &context_.fence), "FIR final fence reset");
    check(vkResetFences(context_.device, 1, &referenceFence_), "FIR reference fence reset");
    referencePending_ = false; finalPending_ = false;
  }
public:
  VulkanFirBackend(std::shared_ptr<Instance> instance, Candidate candidate,
      const GpuGeometry& g)
    : context_(std::move(instance), std::move(candidate)),
      samples_(g.firSamples), taps_(g.firTaps), fft_(g.firFft) {
    if (context_.candidate.properties.vendorID != 5348 ||
        context_.candidate.info.name.find("V3D") == std::string::npos)
      throw std::runtime_error("Qualified FIR-only processing requires an actual Pi V3D GPU");
    if (g.kind != GpuWorkKind::fir || g.range || g.doppler || g.delays ||
        g.channels || g.delayMin || g.clutterSamples || g.clutterBins ||
        g.clutterDelayMin || !samples_ || samples_ > 10000000 || !taps_ ||
        taps_ > samples_ || taps_ > 2048 || fft_ != 2048 || taps_ > fft_ ||
        g.firPercent != 50)
      throw std::invalid_argument("Invalid FIR-only GPU geometry");
    hop_ = fft_ - taps_ + 1;
    const uint64_t totalBlocks = (uint64_t(samples_) + hop_ - 1) / hop_;
    const uint64_t selected = std::max<uint64_t>(1, totalBlocks * g.firPercent / 100);
    const uint64_t outputs = std::min<uint64_t>(samples_, selected * hop_);
    if (!totalBlocks || selected > UINT32_MAX || outputs > UINT32_MAX ||
        selected > UINT32_MAX / fft_)
      throw std::invalid_argument("FIR-only GPU geometry overflows its bounds");
    blocks_ = uint32_t(selected); outputs_ = uint32_t(outputs);
    const uint64_t workElements = uint64_t(blocks_) * fft_;
    const auto& limits = context_.candidate.properties.limits;
    if (limits.maxComputeWorkGroupInvocations < 128 ||
        limits.maxComputeWorkGroupSize[0] < 128 ||
        limits.maxComputeWorkGroupCount[0] < 65535 ||
        workElements * sizeof(std::complex<float>) > limits.maxStorageBufferRange ||
        uint64_t(outputs_) * sizeof(std::complex<float>) > limits.maxStorageBufferRange)
      throw std::runtime_error("GPU capacity is too small for FIR-only processing");
    constexpr auto mapped = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT |
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
    constexpr auto preferred = VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
      VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
    input_ = std::make_unique<Buffer>(context_, uint64_t(outputs_) * 8, mapped, preferred);
    weights_ = std::make_unique<Buffer>(context_, uint64_t(fft_) * 8, mapped, preferred);
    // The qualified V3D path used the same unified mapped memory type for its
    // full FIR workspace; retain that choice for performance parity.
    work_ = std::make_unique<Buffer>(context_, workElements * 8, mapped, preferred);
    output_ = std::make_unique<Buffer>(context_, uint64_t(outputs_) * 8, mapped, preferred);
    pack_ = std::make_unique<Kernel>(context_, firPackSource,
      std::vector<Buffer*>{input_.get(), work_.get()});
    multiply_ = std::make_unique<Kernel>(context_, firMultiplySource,
      std::vector<Buffer*>{work_.get(), weights_.get()});
    gather_ = std::make_unique<Kernel>(context_, firGatherSource,
      std::vector<Buffer*>{work_.get(), output_.get()});
    workPlan_ = std::make_unique<Plan>(context_, *work_, fft_, blocks_, true, true);
    weightPlan_ = std::make_unique<Plan>(context_, *weights_, fft_, 1, true, true);
    VkCommandBufferAllocateInfo allocation{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocation.commandPool = context_.pool; allocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocation.commandBufferCount = 1;
    check(vkAllocateCommandBuffers(context_.device, &allocation, &referenceCommand_),
      "FIR reference command allocation");
    check(vkAllocateCommandBuffers(context_.device, &allocation, &finalCommand_),
      "FIR final command allocation");
    VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    check(vkCreateFence(context_.device, &fenceInfo, nullptr, &referenceFence_),
      "FIR reference fence allocation");
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    check(vkBeginCommandBuffer(referenceCommand_, &begin), "FIR reference recording");
    VkMemoryBarrier hostWrite{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    hostWrite.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
    hostWrite.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(referenceCommand_, VK_PIPELINE_STAGE_HOST_BIT,
      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &hostWrite, 0, nullptr, 0, nullptr);
    Push shape{uint32_t(workElements), fft_, taps_-1, hop_, 0};
    pack_->append(referenceCommand_, shape); firComputeBarrier(referenceCommand_);
    workPlan_->append(referenceCommand_, -1); firComputeBarrier(referenceCommand_);
    check(vkEndCommandBuffer(referenceCommand_), "FIR reference recording");
    check(vkBeginCommandBuffer(finalCommand_, &begin), "FIR final recording");
    hostWrite.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(finalCommand_, VK_PIPELINE_STAGE_HOST_BIT,
      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &hostWrite, 0, nullptr, 0, nullptr);
    // FIFO queue order plus this dependency makes the early FFT visible here.
    firComputeBarrier(finalCommand_);
    weightPlan_->append(finalCommand_, -1); firComputeBarrier(finalCommand_);
    multiply_->append(finalCommand_, shape); firComputeBarrier(finalCommand_);
    workPlan_->append(finalCommand_, 1); firComputeBarrier(finalCommand_);
    shape.count = outputs_;
    gather_->append(finalCommand_, shape);
    VkMemoryBarrier hostRead{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    hostRead.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    hostRead.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(finalCommand_, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
      VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &hostRead, 0, nullptr, 0, nullptr);
    check(vkEndCommandBuffer(finalCommand_), "FIR final recording");
    startupTrace("FIR-only backend ready");
  }
  ~VulkanFirBackend() override {
    if (context_.device && (referencePending_ || finalPending_))
      vkDeviceWaitIdle(context_.device);
    if (referenceCommand_) vkFreeCommandBuffers(context_.device, context_.pool, 1, &referenceCommand_);
    if (finalCommand_) vkFreeCommandBuffers(context_.device, context_.pool, 1, &finalCommand_);
    if (referenceFence_) vkDestroyFence(context_.device, referenceFence_, nullptr);
  }
  GpuDevice device() const override { return context_.candidate.info; }
  void process(const std::vector<std::complex<float>>&,
      const std::vector<std::complex<float>>&,
      std::vector<std::complex<float>>&) override {
    throw std::logic_error("FIR-only GPU backend rejects radar processing");
  }
  void submitFirReferenceBuffers(const std::complex<float>* reference,
      size_t referenceCount) override {
    if (poisoned_ || referencePending_ || finalPending_ || !reference ||
        referenceCount != outputs_)
      throw std::invalid_argument("Invalid FIR reference phase");
    if (!std::all_of(reference, reference + referenceCount, [](auto value) {
          return std::isfinite(value.real()) && std::isfinite(value.imag());
        })) throw std::invalid_argument("Non-finite FIR reference input");
    std::memcpy(input_->mapped, reference, input_->bytes);
    input_->flush(0, input_->bytes);
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1; submit.pCommandBuffers = &referenceCommand_;
    const VkResult result = vkQueueSubmit(context_.queue, 1, &submit, referenceFence_);
    if (result != VK_SUCCESS) { poisoned_ = true; check(result, "FIR reference submission"); }
    referencePending_ = true;
  }
  void processFirWeightsBuffers(const std::complex<float>* weights,
      size_t weightCount, std::complex<float>* output, size_t outputCount) override {
    if (poisoned_ || !referencePending_ || finalPending_ || !weights || !output ||
        weightCount != taps_ || outputCount != outputs_)
      throw std::invalid_argument("Invalid FIR final phase");
    if (!std::all_of(weights, weights + weightCount, [](auto value) {
          return std::isfinite(value.real()) && std::isfinite(value.imag());
        })) throw std::invalid_argument("Non-finite FIR weights");
    auto* destination = static_cast<std::complex<float>*>(weights_->mapped);
    std::copy_n(weights, taps_, destination);
    std::fill(destination + taps_, destination + fft_, std::complex<float>{});
    weights_->flush(0, weights_->bytes);
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1; submit.pCommandBuffers = &finalCommand_;
    const VkResult submitted = vkQueueSubmit(context_.queue, 1, &submit, context_.fence);
    if (submitted != VK_SUCCESS) { poisoned_ = true; check(submitted, "FIR final submission"); }
    finalPending_ = true;
    constexpr uint64_t timeoutNs = 5000000000ULL;
    const VkResult completed = vkWaitForFences(context_.device, 1,
      &context_.fence, VK_TRUE, timeoutNs);
    if (completed != VK_SUCCESS) { poisoned_ = true; check(completed, "FIR final execution"); }
    const VkResult referenceCompleted = vkWaitForFences(context_.device, 1,
      &referenceFence_, VK_TRUE, 0);
    if (referenceCompleted != VK_SUCCESS) {
      poisoned_ = true; check(referenceCompleted, "FIR reference completion");
    }
    resetAfterFinal();
    output_->invalidate(0, output_->bytes);
    const auto* source = static_cast<const std::complex<float>*>(output_->mapped);
    // The V3D mapping is uncached on the qualified Pi. Read it once, then
    // validate the cached shared result before the worker sends READY. The
    // parent cannot consume this candidate until that response succeeds.
    std::memcpy(output, source, output_->bytes);
    if (!std::all_of(output, output + outputs_, [](auto value) {
          return std::isfinite(value.real()) && std::isfinite(value.imag());
        })) { poisoned_ = true; throw std::runtime_error("Non-finite FIR GPU output"); }
  }
};

class VulkanBackend final : public GpuBackend, public GpuBufferBackend,
    public GpuClutterBufferBackend {
  Context context_;
  GpuGeometry geometry_;
  // Destruction order is intentional: plans/kernels, buffers, then context.
  std::unique_ptr<Buffer> reference_, surveillance_, doppler_, input_, output_;
  std::unique_ptr<Plan> referencePlan_, rangePlan_, dopplerPlan_;
  std::unique_ptr<Kernel> multiply_, gather_;
  VkCommandBuffer command_ = VK_NULL_HANDLE;
  bool direct_ = false;
  std::unique_ptr<ClutterPipeline> clutter_;
public:
  VulkanBackend(std::shared_ptr<Instance> instance, Candidate candidate, GpuGeometry g)
    : context_(std::move(instance), std::move(candidate)), geometry_(g) {
    if (g.kind != GpuWorkKind::radar || g.firSamples || g.firTaps ||
        g.firFft || g.firPercent)
      throw std::invalid_argument("Invalid radar GPU work geometry");
    if (!g.range || g.range > 65535 || !g.doppler || g.doppler > 65535 ||
        !g.delays || g.delays > 65535 || !g.channels || g.channels > 8)
      throw std::runtime_error("GPU radar dimensions are unsupported; using CPU");
    const uint64_t range = uint64_t(g.range) * g.doppler;
    const uint64_t doppler = uint64_t(g.doppler) * g.delays * g.channels;
    const uint64_t ambiguityBytes = 2 * (range * (1 + g.channels) + doppler) *
      sizeof(std::complex<float>);
    const uint64_t clutterBytes = ClutterPipeline::requiredBytes(g);
    const auto& limits = context_.candidate.properties.limits;
    const auto budget = gpu_memory::heapBudget(context_.candidate.info.memoryBytes);
    if (!g.range || !g.doppler || !g.delays || !g.channels || g.channels > 8 ||
        range * g.channels > UINT32_MAX / 2 || doppler > UINT32_MAX / 2 ||
        g.delayMin <= -int64_t(g.range) || int64_t(g.delayMin) + g.delays > g.range ||
        ambiguityBytes > budget || limits.maxComputeWorkGroupInvocations < 128 ||
        limits.maxComputeWorkGroupSize[0] < 128 || limits.maxComputeWorkGroupCount[0] < 65535 ||
        std::max(range * g.channels, doppler) * sizeof(std::complex<float>) > limits.maxStorageBufferRange)
      throw std::runtime_error("GPU capacity is too small for these radar settings; using CPU");
    startupTrace("buffer allocations begin");
    const auto* setting = std::getenv("BLAH2_GPU_MEMORY_PATH");
    const std::string memoryPath = setting ? setting : "auto";
    if (memoryPath != "auto" && memoryPath != "direct" && memoryPath != "staged")
      throw std::runtime_error("BLAH2_GPU_MEMORY_PATH must be auto, direct, or staged; using CPU");
    if (memoryPath != "staged") {
      try {
        constexpr auto directFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
        constexpr auto hostPreferred = VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
          VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
        reference_ = std::make_unique<Buffer>(context_, range * 8, directFlags, hostPreferred);
        surveillance_ = std::make_unique<Buffer>(context_, range * g.channels * 8,
          directFlags, hostPreferred);
        doppler_ = std::make_unique<Buffer>(context_, doppler * 8, directFlags, hostPreferred);
        const uint64_t directBytes = reference_->allocationBytes +
          surveillance_->allocationBytes + doppler_->allocationBytes;
        const bool oneHeap = reference_->heap == surveillance_->heap &&
          reference_->heap == doppler_->heap;
        const auto memory = memoryProperties(context_.candidate.physical);
        std::vector<uint64_t> heapUse(memory.heaps.size());
        for (const auto* buffer : {reference_.get(), surveillance_.get(), doppler_.get()})
          if (buffer->heap < heapUse.size()) heapUse[buffer->heap] += buffer->allocationBytes;
        bool fits = true;
        for (size_t heap = 0; heap < heapUse.size(); ++heap)
          if (heapUse[heap] > gpu_memory::heapBudget(memory.heaps[heap].bytes))
            fits = false;
        direct_ = fits && (memoryPath == "direct" || (oneHeap && gpu_memory::autoDirect(
          context_.candidate.properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU,
          reference_->heap, directBytes, memory)));
      } catch (const std::exception&) { direct_ = false; }
      if (!direct_) { reference_.reset(); surveillance_.reset(); doppler_.reset(); }
    }
    if (!direct_) {
      reference_ = std::make_unique<Buffer>(context_, range * 8,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
      surveillance_ = std::make_unique<Buffer>(context_, range * g.channels * 8,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
      doppler_ = std::make_unique<Buffer>(context_, doppler * 8,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
      input_ = std::make_unique<Buffer>(context_, reference_->bytes + surveillance_->bytes,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
      output_ = std::make_unique<Buffer>(context_, doppler_->bytes,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
    }
    startupTrace(direct_ ? "direct mapped device memory selected" : "persistent staging selected");
    startupTrace("buffer allocations complete");
    referencePlan_ = std::make_unique<Plan>(context_, *reference_, g.range, g.doppler);
    rangePlan_ = std::make_unique<Plan>(context_, *surveillance_, g.range, g.doppler * g.channels);
    dopplerPlan_ = std::make_unique<Plan>(context_, *doppler_, g.doppler, g.delays * g.channels);
    startupTrace("multiply kernel begin");
    multiply_ = std::make_unique<Kernel>(context_, multiplySource, std::vector<Buffer*>{reference_.get(), surveillance_.get()});
    startupTrace("gather kernel begin");
    gather_ = std::make_unique<Kernel>(context_, gatherSource, std::vector<Buffer*>{surveillance_.get(), doppler_.get()});
    VkCommandBufferAllocateInfo allocation{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocation.commandPool = context_.pool; allocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocation.commandBufferCount = 1;
    check(vkAllocateCommandBuffers(context_.device, &allocation, &command_), "command buffer");
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    check(vkBeginCommandBuffer(command_, &begin), "recording");
    if (direct_) {
      VkMemoryBarrier hostWrite{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
      hostWrite.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
      hostWrite.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
      vkCmdPipelineBarrier(command_, VK_PIPELINE_STAGE_HOST_BIT,
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &hostWrite, 0, nullptr, 0, nullptr);
    } else {
      VkMemoryBarrier hostWrite{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
      hostWrite.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
      hostWrite.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
      vkCmdPipelineBarrier(command_, VK_PIPELINE_STAGE_HOST_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &hostWrite, 0, nullptr, 0, nullptr);
      VkBufferCopy refCopy{0, 0, reference_->bytes};
      VkBufferCopy survCopy{reference_->bytes, 0, surveillance_->bytes};
      vkCmdCopyBuffer(command_, input_->handle, reference_->handle, 1, &refCopy);
      vkCmdCopyBuffer(command_, input_->handle, surveillance_->handle, 1, &survCopy);
      barrier(command_);
    }
    referencePlan_->append(command_, -1); rangePlan_->append(command_, -1); barrier(command_);
    Push push{uint32_t(range * g.channels), g.range, g.doppler, g.delays, g.delayMin};
    multiply_->append(command_, push); barrier(command_);
    rangePlan_->append(command_, 1); barrier(command_);
    push.count = doppler;
    gather_->append(command_, push); barrier(command_);
    dopplerPlan_->append(command_, -1); barrier(command_);
    VkMemoryBarrier hostRead{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    hostRead.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    if (direct_) {
      hostRead.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
      vkCmdPipelineBarrier(command_, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &hostRead, 0, nullptr, 0, nullptr);
    } else {
      VkBufferCopy outputCopy{0, 0, doppler_->bytes};
      vkCmdCopyBuffer(command_, doppler_->handle, output_->handle, 1, &outputCopy);
      // Fence completion alone does not make discrete-GPU staging writes visible.
      hostRead.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
      vkCmdPipelineBarrier(command_, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &hostRead, 0, nullptr, 0, nullptr);
    }
    check(vkEndCommandBuffer(command_), "command recording");
    if (g.clutterSamples && clutterBytes <= budget - ambiguityBytes) {
      startupTrace("clutter pipeline begin");
      try {
        clutter_ = std::make_unique<ClutterPipeline>(context_, g,
          direct_, memoryPath == "direct");
        startupTrace("clutter pipeline complete");
      } catch (const std::exception& error) {
        startupTrace(error.what());
        startupTrace("clutter unavailable; ambiguity remains available");
      }
    } else if (g.clutterSamples) {
      startupTrace("clutter memory budget unavailable; ambiguity remains available");
    }
    if (std::getenv("BLAH2_GPU_DIAGNOSTICS")) {
      std::lock_guard<std::mutex> lock(allocationsMutex);
      const auto& budget = deviceAllocations.at(context_.device).budget;
      std::cerr << "GPU allocation budget used=" << budget.used()
        << " peak=" << budget.peak() << " limit=" << budget.limit() << '\n';
    }
    startupTrace("backend ready");
  }
  GpuDevice device() const override { return context_.candidate.info; }
  void process(const std::vector<std::complex<float>>& reference,
    const std::vector<std::complex<float>>& surveillance,
    std::vector<std::complex<float>>& output) override {
    output.resize(doppler_->bytes / sizeof(std::complex<float>));
    processBuffers(reference.data(), reference.size(), surveillance.data(),
      surveillance.size(), output.data(), output.size());
  }
  void processBuffers(const std::complex<float>* reference, size_t referenceCount,
      const std::complex<float>* surveillance, size_t surveillanceCount,
      std::complex<float>* output, size_t outputCount) override {
    if (!reference || !surveillance || !output ||
        referenceCount != reference_->bytes / sizeof(*reference) ||
        surveillanceCount != surveillance_->bytes / sizeof(*surveillance) ||
        outputCount != doppler_->bytes / sizeof(*output))
      throw std::invalid_argument("GPU buffer dimensions changed");
    if (direct_) {
      std::memcpy(reference_->mapped, reference, reference_->bytes);
      std::memcpy(surveillance_->mapped, surveillance, surveillance_->bytes);
      reference_->flush(0, reference_->bytes);
      surveillance_->flush(0, surveillance_->bytes);
    } else {
      std::memcpy(input_->mapped, reference, reference_->bytes);
      std::memcpy(static_cast<char*>(input_->mapped) + reference_->bytes,
        surveillance, surveillance_->bytes);
      input_->flush(0, input_->bytes);
    }
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1; submit.pCommandBuffers = &command_;
    check(vkQueueSubmit(context_.queue, 1, &submit, context_.fence), "submission");
    // Device loss is surfaced by the driver. A slow-but-running queue must be
    // drained before freeing its buffers; it is not safe to pretend cancellation.
    check(vkWaitForFences(context_.device, 1, &context_.fence, VK_TRUE, UINT64_MAX), "execution");
    check(vkResetFences(context_.device, 1, &context_.fence), "fence reset");
    Buffer& result = direct_ ? *doppler_ : *output_;
    result.invalidate(0, result.bytes);
    std::memcpy(output, result.mapped, result.bytes);
  }
  bool processClutterBuffers(const std::complex<float>* reference, size_t referenceCount,
      const std::complex<float>* surveillance, size_t surveillanceCount,
      std::complex<float>* output, size_t outputCount) override {
    // This interface remains available when construction rejected only clutter
    // (for example its memory budget). Reject this stage without killing the
    // worker and discarding the already-qualified ambiguity backend.
    if (!clutter_) return false;
    return clutter_->process(reference, referenceCount, surveillance, surveillanceCount,
      output, outputCount);
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
extern "C" std::string blah2_gpu_driver_status(unsigned version) {
  if (version != 1) throw std::runtime_error("Unsupported GPU diagnostic version");
  blah2::Instance instance(true);
  auto getProperties = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2KHR>(
    vkGetInstanceProcAddr(instance.handle, "vkGetPhysicalDeviceProperties2KHR"));
  std::ostringstream json;
  json << "{\"version\":1,\"available\":true,\"qualification\":\"not-run\",\"devices\":[";
  bool first = true;
  for (const auto& item : blah2::enumerate(instance)) {
    VkPhysicalDeviceDriverProperties driver{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
    uint32_t count = 0;
    blah2::check(vkEnumerateDeviceExtensionProperties(item.physical, nullptr, &count, nullptr), "driver extension discovery");
    std::vector<VkExtensionProperties> extensions(count);
    blah2::check(vkEnumerateDeviceExtensionProperties(item.physical, nullptr, &count, extensions.data()), "driver extension discovery");
    const bool supported = item.properties.apiVersion >= VK_API_VERSION_1_2 ||
      std::any_of(extensions.begin(), extensions.end(), [](const auto& value) {
        return std::strcmp(value.extensionName, VK_KHR_DRIVER_PROPERTIES_EXTENSION_NAME) == 0;
      });
    if (instance.diagnosticProperties && getProperties && supported) {
      VkPhysicalDeviceProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
      properties.pNext = &driver;
      getProperties(item.physical, &properties);
    }
    if (!first) json << ',';
    first = false;
    json << "{\"id\":" << blah2::gpuDiagnosticString(item.info.id)
      << ",\"name\":" << blah2::gpuDiagnosticString(item.info.name)
      << ",\"vendorId\":" << item.properties.vendorID
      << ",\"deviceId\":" << item.properties.deviceID
      << ",\"driverId\":" << driver.driverID
      << ",\"driverName\":" << blah2::gpuDiagnosticString(driver.driverName)
      << ",\"driverInfo\":" << blah2::gpuDiagnosticString(driver.driverInfo)
      << ",\"driverVersionRaw\":" << item.properties.driverVersion
      << ",\"mesaVersion\":";
    // Packed Mesa versions are meaningful here only for the identified V3DV
    // driver, not NVIDIA's different encoding or an unknown vendor's driver.
    if (driver.driverID == VK_DRIVER_ID_MESA_V3DV) {
      const uint32_t v = item.properties.driverVersion;
      json << '"' << VK_VERSION_MAJOR(v) << '.' << VK_VERSION_MINOR(v) << '.' << VK_VERSION_PATCH(v) << '"';
    } else json << "null";
    json << '}';
  }
  json << "]}";
  return json.str();
}
extern "C" blah2::GpuBackend* blah2_gpu_create(unsigned abi, const blah2::GpuGeometry* geometry, const char* requested) {
  if (abi != blah2::GPU_ABI || !geometry) throw std::runtime_error("GPU module version mismatch");
  auto instance = std::make_shared<blah2::Instance>();
  std::string reason = "No compatible GPU is available; using CPU";
  const std::string selection = requested ? requested : "auto";
  for (const auto& candidate : blah2::enumerate(*instance)) {
    if (selection != "auto" && selection != candidate.info.id) continue;
    try {
      if (geometry->kind == blah2::GpuWorkKind::fir)
        return new blah2::VulkanFirBackend(instance, candidate, *geometry);
      return new blah2::VulkanBackend(instance, candidate, *geometry);
    }
    catch (const std::exception& error) { reason = error.what(); }
  }
  throw std::runtime_error(reason);
}
