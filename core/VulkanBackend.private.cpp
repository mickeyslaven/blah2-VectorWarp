// Private Vulkan primitives and resident arithmetic shared by the qualified
// resident and FP32 wideband modules. The portable radar module has its own
// canonical factory in src/process/ambiguity/VulkanBackend.cpp.
#include "GpuBackend.h"
#include "GpuClutterBatch.h"
#include "GpuMemory.h"
#include "process/tracker/ConfirmationContract.h"
#include "process/tracker/AssociationContract.h"
#include "process/detection/IntegratedCfar.h"
#include "process/ambiguity/ReplayGpuQualification.h"
#include "process/ambiguity/GpuClutterRefinement.h"
#include "GpuFrameDiagnostics.private.h"
#include "process/analysis/ResidentReplaySnapshot.h"
// The FP64 resident proof is Linux-only. The wideband module uses an
// explicitly separate FP32 contract and is allowed on supported Vulkan hosts.
#if !defined(__linux__) && !defined(VECTORWARP_GPU_FP32_MODULE)
#error "GPU-native arithmetic proof is Linux-only; production platform policy is unchanged"
#endif
#include <array>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <type_traits>
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
#if defined(__linux__)
#include <time.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <fcntl.h>
#endif
#include <cerrno>
#include <cstdio>
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
  struct Entry { uint32_t type;
  uint64_t bytes; };
  blah2::gpu_memory::AllocationBudget budget;
  std::unordered_map<VkDeviceMemory, Entry> allocations;
  DeviceAllocations(uint64_t limit, blah2::gpu_memory::Properties properties,
      bool separateHostHeaps = false)
    : budget(limit, std::move(properties), separateHostHeaps) {}
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
#ifndef VECTORWARP_GPU_FP32_MODULE
    if(std::getenv("BLAH2_REPLAY_GPU_QUALIFICATION"))
      blah2::replay_gpu_qualification::Worker("resident").allocationRejected(info->allocationSize,state.budget.limit());
#endif
    if (std::getenv("BLAH2_GPU_DIAGNOSTICS"))
      std::cerr << "GPU allocation budget rejected bytes=" << info->allocationSize
        << " type=" << info->memoryTypeIndex << " used=" << state.budget.used()
        << " limit=" << state.budget.limit()
        << " device_used=" << state.budget.deviceUsed()
        << " device_limit=" << state.budget.deviceLimit() << '\n';
    return VK_ERROR_OUT_OF_DEVICE_MEMORY;
  }
  const VkResult result = vkAllocateMemory(device, info, callbacks, memory);
  if (result == VK_SUCCESS) {
    try {
      state.allocations.emplace(*memory,
        DeviceAllocations::Entry{info->memoryTypeIndex, info->allocationSize});
      return result;
    } catch (...) {
      vkFreeMemory(device, *memory, callbacks);
      *memory = VK_NULL_HANDLE;
      state.budget.release(info->memoryTypeIndex, info->allocationSize);
      return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
  }
  state.budget.release(info->memoryTypeIndex, info->allocationSize);
  state.budget.driverRejected(info->memoryTypeIndex, info->allocationSize);
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
      std::to_string(result) + "); resident route stopped");
}
void checkFft(VkFFTResult result) {
  if (result != VKFFT_SUCCESS)
    throw std::runtime_error("GPU FFT failed (" + std::to_string(result) + "); resident route stopped");
}
struct Instance {
  VkInstance handle = VK_NULL_HANDLE;
  bool diagnosticProperties = false;
  explicit Instance(bool diagnostic = false) {
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "blah2";
    app.apiVersion = VK_API_VERSION_1_1;
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
    Candidate item{};
    item.physical = devices[index];
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
#include "GpuHostTiming.private.h"
struct AllocationExit {bool valid=false;
uint64_t used=0,peak=0,limit=0,live=0;};
struct Context {
  enum class Arithmetic { fp32, fp64 };
  AllocationExit* releaseEvidence=nullptr;
  gpu_host::Frame* hostTiming = nullptr;
  gpu_diagnostics::FrameLog diagnostics;
  std::shared_ptr<Instance> instance;
  Candidate candidate;
  VkDevice device = VK_NULL_HANDLE;
  VkQueue queue = VK_NULL_HANDLE;
  VkCommandPool pool = VK_NULL_HANDLE;
  VkFence fence = VK_NULL_HANDLE;
  bool compiler = false;
  bool portability = false;
  uint32_t timestampValidBits = 0;
  VkPhysicalDeviceFeatures supportedFeatures{};
  VkPhysicalDeviceIDProperties identity{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
  Context(std::shared_ptr<Instance> owner, Candidate selected,
      Arithmetic arithmetic = Arithmetic::fp64)
    : instance(std::move(owner)), candidate(std::move(selected)) {
    try {
      const float priority = 1;
      VkDeviceQueueCreateInfo queueInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
      queueInfo.queueFamilyIndex = candidate.queue;
      queueInfo.queueCount = 1;
      queueInfo.pQueuePriorities = &priority;
      VkDeviceCreateInfo info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
      info.queueCreateInfoCount = 1;
      info.pQueueCreateInfos = &queueInfo;
      VkPhysicalDeviceFeatures supported{};
      vkGetPhysicalDeviceFeatures(candidate.physical, &supported);
      supportedFeatures=supported;
      if (arithmetic == Arithmetic::fp64 && !supported.shaderFloat64)
        throw std::runtime_error("GPU_NATIVE_UNSUPPORTED shaderFloat64=0");
      VkPhysicalDeviceFeatures enabled{};
      enabled.shaderFloat64 = arithmetic == Arithmetic::fp64 ? VK_TRUE : VK_FALSE;
      info.pEnabledFeatures = &enabled;
      uint32_t families = 0;
      vkGetPhysicalDeviceQueueFamilyProperties(candidate.physical, &families, nullptr);
      std::vector<VkQueueFamilyProperties> queues(families);
      vkGetPhysicalDeviceQueueFamilyProperties(candidate.physical, &families, queues.data());
      if (candidate.queue >= queues.size()) throw std::runtime_error("Invalid compute queue");
      timestampValidBits = queues[candidate.queue].timestampValidBits;
      if (!timestampValidBits || timestampValidBits > 64 ||
          !(candidate.properties.limits.timestampPeriod > 0))
        throw std::runtime_error("GPU_NATIVE_UNSUPPORTED compute timestamps unavailable");
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
      VkPhysicalDeviceIDProperties ids{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
      VkPhysicalDeviceProperties2 identified{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
      identified.pNext=&ids;
      vkGetPhysicalDeviceProperties2(candidate.physical,&identified);
      identity=ids;
      identity.pNext=nullptr;
      const auto hex=[](const uint8_t* bytes) {
        const char digits[]="0123456789abcdef";
        std::string value;
        for(uint32_t i=0;i<VK_UUID_SIZE;++i) { value+=digits[bytes[i]>>4];
        value+=digits[bytes[i]&15]; }
        return value;
      };
      std::cerr<<"GPU native identity id="<<candidate.info.id
        <<" vendor="<<candidate.properties.vendorID<<" device="<<candidate.properties.deviceID
        <<" driver_version="<<candidate.properties.driverVersion
        <<" device_uuid="<<hex(ids.deviceUUID)<<" driver_uuid="<<hex(ids.driverUUID)
        <<" shader_float64="<<(supported.shaderFloat64?1:0)
        <<" arithmetic="<<(arithmetic==Arithmetic::fp64?"fp64":"fp32")
        <<" queue="<<candidate.queue<<" timestamp_valid_bits="<<timestampValidBits
        <<" timestamp_period_ns="<<candidate.properties.limits.timestampPeriod<<'\n';
      {
        const auto policy = gpu_memory::allocationPolicy(candidate.info.memoryBytes,
          std::getenv("BLAH2_GPU_ALLOCATION_LIMIT_BYTES"));
        std::lock_guard<std::mutex> lock(allocationsMutex);
        deviceAllocations.try_emplace(device, policy.bytes,
          memoryProperties(candidate.physical), policy.separateHostHeaps);
      }
      startupTrace("vkCreateDevice complete");
      vkGetDeviceQueue(device, candidate.queue, 0, &queue);
      VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
      poolInfo.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
      poolInfo.queueFamilyIndex = candidate.queue;
      check(vkCreateCommandPool(device, &poolInfo, nullptr, &pool), "command allocation");
      VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
      check(vkCreateFence(device, &fenceInfo, nullptr, &fence), "fence allocation");
      startupTrace("glslang initialization begin");
      compiler = glslang_initialize_process();
      startupTrace("glslang initialization complete");
      if (!compiler) throw std::runtime_error("GPU shader compiler is unavailable");
    } catch (...) { release();
    throw; }
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
        if(releaseEvidence)*releaseEvidence={true,found->second.budget.deviceUsed(),
          found->second.budget.devicePeak(),found->second.budget.deviceLimit(),
          found->second.allocations.size()};
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
constexpr const char* memoryFailureAction = "resident route stopped";
#include "process/ambiguity/VulkanResources.private.h"
constexpr const char* refinementAutoSource = R"glsl(#version 450
layout(local_size_x=128) in;
layout(binding=0) readonly buffer Reference { vec2 ref[]; };
layout(binding=1) writeonly buffer Auto { vec2 autocorr[]; };
layout(push_constant) uniform Parameters { uint count; uint a; uint b; uint c; int d; } p;
void main() {
  uint i = gl_GlobalInvocationID.x + gl_GlobalInvocationID.y * gl_NumWorkGroups.x * 128;
  if (i < p.count) autocorr[i] = vec2(dot(ref[i], ref[i]), 0.0);
})glsl";
#include "GpuNativeRefinement.private.h"
class ResidentModule;
class ClutterPipeline {
  friend class ResidentModule;
  Context& context_;
  uint32_t samples_, bins_, channels_, length_;
  std::unique_ptr<Buffer> reference_, estimate_;
  std::shared_ptr<Buffer> surveillance_, weights_;
  std::unique_ptr<Buffer> filterReference_, filtered_;
  std::unique_ptr<Buffer> input_, correlations_, weightUpload_, output_;
  std::unique_ptr<Plan> referencePlan_, surveillancePlan_, autoPlan_, crossPlan_;
  std::unique_ptr<Plan> filterReferencePlan_, weightPlan_, filteredPlan_;
  std::unique_ptr<Kernel> products_, multiply_, normalize_, autoProducts_;
  std::unique_ptr<gpu_native::Refinement> native_;
  std::unique_ptr<gpu_native::Timestamps> timestamps_;
  bool frameReady_ = false, frameFailed_ = false, direct_ = false, resident_ = false;
  uint64_t generation_ = 0;
  std::array<uint32_t,16> referenceEvidence_{};
  struct BatchEvidence {uint32_t first=0,active=0;
  std::array<uint32_t,16> status{};
  std::array<double,9> times{};
  double hostMs=0;};
  std::array<BatchEvidence,8> batchEvidence_{};
  uint32_t completedBatches_ = 0, completedChannels_ = 0;
  struct CommandBuffers {
    Context& context;
    VkCommandBuffer handles[2]{};
    explicit CommandBuffers(Context& value) : context(value) {}
    CommandBuffers(const CommandBuffers&) = delete;
    CommandBuffers& operator=(const CommandBuffers&) = delete;
    ~CommandBuffers() {
      for (const auto command : handles)
        if (command) vkFreeCommandBuffers(context.device, context.pool, 1, &command);
    }
  } commands_;
  // Destroy command buffers before their referenced GPU resources, even if
  // construction throws during an actual allocation-budget retry.
  VkCommandBuffer prepare_ = VK_NULL_HANDLE, batch_ = VK_NULL_HANDLE;
  void factorReference() = delete;
  void solveWeights() = delete;
  void submit(VkCommandBuffer command, const char* phase) {
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &command;
    check(gpu_host::call(context_.hostTiming,"vkQueueSubmit",0,[&]{return vkQueueSubmit(context_.queue, 1, &submit, context_.fence);}), phase);
    check(gpu_host::call(context_.hostTiming,"vkWaitForFences",0,[&]{return vkWaitForFences(context_.device, 1, &context_.fence, VK_TRUE, UINT64_MAX);}), phase);
    check(gpu_host::call(context_.hostTiming,"vkResetFences",0,[&]{return vkResetFences(context_.device, 1, &context_.fence);}), "clutter fence reset");
  }
  void appendCorrelations(VkCommandBuffer command) {
    surveillancePlan_->append(command, -1);
    barrier(command);
    products_->append(command, {samples_*channels_, samples_, channels_, 0, 0});
    barrier(command);
    crossPlan_->append(command, 1);
    barrier(command);
    for (uint32_t channel = 0; channel < channels_; ++channel) {
      VkBufferCopy copy{uint64_t(channel)*samples_*8,
        uint64_t(bins_)*(1+channel)*8, uint64_t(bins_)*8};
      vkCmdCopyBuffer(command, filtered_->handle, correlations_->handle, 1, &copy);
    }
    barrier(command);
  }
  void appendCorrection(VkCommandBuffer command) {
    vkCmdFillBuffer(command, weights_->handle, 0, weights_->bytes, 0);
    barrier(command);
    for (uint32_t channel = 0; channel < channels_; ++channel) {
      VkBufferCopy copy{uint64_t(channel)*bins_*8,
        uint64_t(channel)*length_*8, uint64_t(bins_)*8};
      vkCmdCopyBuffer(command, weightUpload_->handle, weights_->handle, 1, &copy);
    }
    barrier(command);
    weightPlan_->append(command,-1);
    barrier(command);
    multiply_->append(command,{length_*channels_,length_,channels_,0,0});
    barrier(command);
    filteredPlan_->append(command,1);
    barrier(command);
    normalize_->append(command,{samples_*channels_,samples_,length_,0,0});
    barrier(command);
  }
  void hostReadBarrier(VkCommandBuffer command) {
    VkMemoryBarrier memory{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    memory.srcAccessMask=VK_ACCESS_MEMORY_WRITE_BIT;
    memory.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_HOST_BIT,
      0,1,&memory,0,nullptr,0,nullptr);
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
  static uint64_t requiredBytes(const GpuGeometry& g, bool resident = false) {
    if (!g.clutterSamples) return 0;
    const uint64_t n = g.clutterSamples, c = g.channels;
    const uint64_t b = g.clutterBins, l = filterLength(g);
    if (!l) return std::numeric_limits<uint64_t>::max();
    // The estimate buffer is idle while the reference autocorrelation is
    // prepared, then overwritten by the saved surveillance input. Reusing it
    // avoids one full-CPI device allocation without changing either phase.
    uint64_t elements = 3*n + 4*n*c + l + 2*l*c + b*c + b*(1+c);
    if (resident) elements -= n + 3*n*c; // No legacy staging; FFT scratch aliases below.
    // Preflight allowance for the seven plan tables. Batched transforms share
    // their LUTs. Actual tables, scratch and upload allocations are additionally
    // enforced by budgetAllocateMemory; this estimate is not the final guard.
    if (twiddleLut()) elements += 4*n + 3*l;
    const auto native = gpu_native::Refinement::requiredBytes(g.clutterSamples,g.clutterBins,g.channels);
    if(native == UINT64_MAX || native > UINT64_MAX-elements*8) return UINT64_MAX;
    return elements * sizeof(std::complex<float>) + native;
  }
  ClutterPipeline(Context& context, const GpuGeometry& g,
      bool allowDirect, bool forceDirect, bool resident = false)
    : context_(context), samples_(g.clutterSamples), bins_(g.clutterBins),
      channels_(g.channels), length_(filterLength(g)), resident_(resident), commands_(context) {
    const uint64_t n = samples_, nc = uint64_t(samples_) * channels_;
    const uint64_t l = length_, lc = uint64_t(length_) * channels_;
    const auto& limit = context.candidate.properties.limits;
    if (!samples_ || bins_ > gpu_native::fft || !bins_ || bins_ > samples_ || !channels_ || channels_ > 8 ||
        !length_ || length_ < uint64_t(samples_) + bins_ - 1 ||
        nc > UINT32_MAX || lc > UINT32_MAX ||
        nc > SIZE_MAX / sizeof(std::complex<double>) ||
        std::max({n, nc, l, lc}) * sizeof(std::complex<float>) > limit.maxStorageBufferRange)
      throw std::runtime_error("GPU clutter capacity is too small; resident route stopped clutter");
    constexpr auto device = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    constexpr auto host = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
    constexpr auto coherent = VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    if (allowDirect && !resident_) {
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
      if (!direct_) { reference_.reset();
      surveillance_.reset();
      estimate_.reset(); }
    }
    if (!direct_) {
      reference_ = std::make_unique<Buffer>(context, n*8, device);
      surveillance_ = std::make_unique<Buffer>(context, (resident_?lc:nc)*8, device);
      estimate_ = std::make_unique<Buffer>(context, nc*8, device);
      if (!resident_) {
        input_ = std::make_unique<Buffer>(context, (n+nc)*8, host, coherent);
        output_ = std::make_unique<Buffer>(context, nc*8, host,
          coherent | VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
      }
    }
    startupTrace(resident_ ? "clutter resident GPU buffers selected" : direct_ ? "clutter direct mapped device memory selected" :
      "clutter persistent staging selected");
    filterReference_ = std::make_unique<Buffer>(context, l*8, device);
    // Only resident processing reuses surveillance storage for filter weights.
    if(resident_)weights_=surveillance_;
    else weights_=std::make_unique<Buffer>(context, lc*8, device);
    filtered_ = std::make_unique<Buffer>(context, lc*8, device);
    correlations_ = std::make_unique<Buffer>(context, bins_*(1+channels_)*8, host,
      coherent | VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
    weightUpload_ = std::make_unique<Buffer>(context, uint64_t(bins_)*channels_*8,
      host, coherent);
    const bool lut = twiddleLut();
    referencePlan_ = std::make_unique<Plan>(context, *reference_, samples_, 1, lut);
    surveillancePlan_ = std::make_unique<Plan>(context, *surveillance_, samples_, channels_, lut);
    autoPlan_ = std::make_unique<Plan>(context, *estimate_, samples_, 1, lut);
    // Correlation output is consumed before either correction pass starts.
    // filtered_ is larger and is only written afterwards, so the two FFT
    // plans safely share one device allocation.
    crossPlan_ = std::make_unique<Plan>(context, *filtered_, samples_, channels_, lut);
    filterReferencePlan_ = std::make_unique<Plan>(context, *filterReference_, length_, 1, lut);
    weightPlan_ = std::make_unique<Plan>(context, *weights_, length_, channels_, lut);
    filteredPlan_ = std::make_unique<Plan>(context, *filtered_, length_, channels_, lut);
    autoProducts_ = std::make_unique<Kernel>(context, refinementAutoSource,
      std::vector<Buffer*>{reference_.get(), estimate_.get()});
    products_ = std::make_unique<Kernel>(context, clutterProductsSource,
      std::vector<Buffer*>{reference_.get(), surveillance_.get(), filtered_.get()});
    multiply_ = std::make_unique<Kernel>(context, clutterMultiplySource,
      std::vector<Buffer*>{filterReference_.get(), weights_.get(), filtered_.get()});
    normalize_ = std::make_unique<Kernel>(context, clutterNormalizeSource,
      std::vector<Buffer*>{filtered_.get(), estimate_.get()});
    native_=std::make_unique<gpu_native::Refinement>(context,samples_,bins_,channels_,
      *reference_,*correlations_,*estimate_,*surveillance_,*weightUpload_);
    timestamps_=std::make_unique<gpu_native::Timestamps>(context);
    VkCommandBufferAllocateInfo allocation{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocation.commandPool=context.pool;
    allocation.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocation.commandBufferCount=2;
    check(vkAllocateCommandBuffers(context.device,&allocation,commands_.handles),"clutter native command buffers");
    prepare_=commands_.handles[0];
    batch_=commands_.handles[1];
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    check(vkBeginCommandBuffer(prepare_,&begin),"clutter native reference recording");
    VkMemoryBarrier hostWrite{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    hostWrite.srcAccessMask=VK_ACCESS_HOST_WRITE_BIT;
    hostWrite.dstAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;
    vkCmdPipelineBarrier(prepare_,VK_PIPELINE_STAGE_HOST_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
      0,1,&hostWrite,0,nullptr,0,nullptr);
    if(!direct_ && !resident_) {
      VkBufferCopy copy{0,0,n*8};
      vkCmdCopyBuffer(prepare_,input_->handle,reference_->handle,1,&copy);
    }
    barrier(prepare_);
    // The rounded reference and its suffix are consumed before any in-place FFT.
    timestamps_->begin(prepare_,0);
    native_->appendReference(prepare_);
    timestamps_->end(prepare_,0);
    timestamps_->begin(prepare_,1);
    vkCmdFillBuffer(prepare_,filterReference_->handle,0,l*8,0);
    barrier(prepare_);
    VkBufferCopy refCopy{0,0,n*8};
    vkCmdCopyBuffer(prepare_,reference_->handle,filterReference_->handle,1,&refCopy);
    barrier(prepare_);
    referencePlan_->append(prepare_,-1);
    barrier(prepare_);
    filterReferencePlan_->append(prepare_,-1);
    barrier(prepare_);
    autoProducts_->append(prepare_,{samples_,0,0,0,0});
    barrier(prepare_);
    autoPlan_->append(prepare_,1);
    barrier(prepare_);
    VkBufferCopy autoCopy{0,0,uint64_t(bins_)*8};
    vkCmdCopyBuffer(prepare_,estimate_->handle,correlations_->handle,1,&autoCopy);
    barrier(prepare_);
    timestamps_->end(prepare_,1);
    timestamps_->begin(prepare_,2);
    native_->appendFactor(prepare_);
    timestamps_->end(prepare_,2);
    hostReadBarrier(prepare_);
    check(vkEndCommandBuffer(prepare_),"clutter native reference recording");

    check(vkBeginCommandBuffer(batch_,&begin),"clutter native batch recording");
    vkCmdPipelineBarrier(batch_,VK_PIPELINE_STAGE_HOST_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
      0,1,&hostWrite,0,nullptr,0,nullptr);
    if(!direct_ && !resident_) {
      VkBufferCopy copy{n*8,0,nc*8};
      vkCmdCopyBuffer(batch_,input_->handle,surveillance_->handle,1,&copy);
    }
    barrier(batch_);
    // estimate_ temporarily preserves rounded y; its later correction output
    // overwrites it only after the GPU circular residual has consumed all y.
    VkBufferCopy savedY{0,0,nc*8};
    vkCmdCopyBuffer(batch_,surveillance_->handle,estimate_->handle,1,&savedY);
    barrier(batch_);
    timestamps_->begin(batch_,3);
    appendCorrelations(batch_);
    timestamps_->end(batch_,3);
    timestamps_->begin(batch_,4);
    native_->appendSolve(batch_,true);
    timestamps_->end(batch_,4);
    timestamps_->begin(batch_,5);
    native_->appendBaseline(batch_);
    timestamps_->end(batch_,5);
    timestamps_->begin(batch_,6);
    native_->appendResidual(batch_);
    timestamps_->end(batch_,6);
    // No host copy occurs here: correlate the resident rounded residual.
    timestamps_->begin(batch_,7);
    appendCorrelations(batch_);
    timestamps_->end(batch_,7);
    timestamps_->begin(batch_,8);
    native_->appendSolve(batch_,false);
    timestamps_->end(batch_,8);
    timestamps_->begin(batch_,9);
    appendCorrection(batch_);
    timestamps_->end(batch_,9);
    timestamps_->begin(batch_,10);
    native_->appendMerge(batch_);
    timestamps_->end(batch_,10);
    timestamps_->begin(batch_,11);
    if(!direct_ && !resident_) {
      VkBufferCopy copy{0,0,nc*8};
      vkCmdCopyBuffer(batch_,estimate_->handle,output_->handle,1,&copy);
    }
    hostReadBarrier(batch_);
    timestamps_->end(batch_,11);
    check(vkEndCommandBuffer(batch_),"clutter native batch recording");
  }
  void endFrame() {
    if(frameReady_ || frameFailed_) {
      context_.diagnostics.stream()<<"GPU_NATIVE_FRAME_END frame="<<generation_
        <<" completed_batches="<<completedBatches_<<" completed_channels="<<completedChannels_
        <<" valid="<<(frameReady_&&!frameFailed_?1:0)<<'\n';
    }
    frameReady_=false;
    frameFailed_=false;
  }
  void invalidateResidentFrame() {
    requireResident();
    if (frameReady_ || frameFailed_) { frameFailed_=true;
    endFrame(); }
  }
  bool rejectFrame(const std::exception& error,const char* phase,
      uint32_t firstChannel,uint32_t activeChannels) {
    context_.diagnostics.stream()<<"GPU clutter precision rejection samples="<<samples_
      <<" bins="<<bins_<<" channels="<<channels_<<" first_channel="<<firstChannel
      <<" active_channels="<<activeChannels<<" phase="<<phase<<" reason="<<error.what()<<'\n';
    frameFailed_=true;
    endFrame();
    return false;
  }
  Buffer& borrowedReference() { requireResident();
  return native_->borrowedReference(); }
  Buffer& borrowedWork() { requireResident();
  return native_->borrowedWork(); }
  Buffer& residentReference() { requireResident();
  return *reference_; }
  Buffer& residentSurveillance() { requireResident();
  return *surveillance_; }
  Buffer& residentEstimate() { requireResident();
  return *estimate_; }
  uint64_t residentScratchReusedBytes() const {
    return resident_&&surveillance_.get()==weights_.get()?uint64_t(samples_)*channels_*8:0;
  }
  void requireResident() const {
    if (!resident_) throw std::logic_error("GPU clutter is not a resident pipeline");
  }
  bool beginFrameResident() {
    requireResident();
    return prepareFrame(nullptr, samples_);
  }
  bool beginFrame(const std::complex<float>* reference,size_t count) {
    if (resident_) throw std::logic_error("Host upload forbidden in resident clutter");
    return prepareFrame(reference, count);
  }
private:
  bool prepareFrame(const std::complex<float>* reference,size_t count) {
    endFrame();
    ++generation_;
    completedBatches_=0;
    completedChannels_=0;
    referenceEvidence_={};
    batchEvidence_={};
    if((!resident_ && !reference) || count!=samples_) throw std::invalid_argument("GPU clutter reference dimensions changed");
    try {
      if(resident_) {
        // Reference was packed by the preceding fenced GPU command.
      } else {
        if(!reference)throw std::logic_error("GPU clutter host reference is missing");
        auto& destination=direct_?*reference_:*input_;
        std::memcpy(destination.mapped,reference,count*8);
        destination.flush(0,count*8);
      }
      native_->resetStatus(0);
      submit(prepare_,"GPU-native reference");
      const auto status=native_->readStatus();
      if(status[gpu_native::referenceBlocks]!=native_->blocks() || status[gpu_native::factorizations]!=1)
        throw std::runtime_error("GPU-native reference coverage mismatch");
      referenceEvidence_=status;
      frameReady_=true;
      context_.diagnostics.stream()<<std::setprecision(12)<<"GPU_NATIVE_REFERENCE frame="<<generation_
        <<" samples="<<samples_<<" bins="<<bins_<<" fft="<<gpu_native::fft
        <<" hop="<<native_->hop()<<" blocks="<<native_->blocks()
        <<" device_reference_blocks="<<status[gpu_native::referenceBlocks]
        <<" device_factorizations="<<status[gpu_native::factorizations]
        <<" cpu_baseline_calls=0 cpu_solve_calls=0 gpu_timestamp_valid_bits="<<context_.timestampValidBits
        <<" gpu_timestamp_period_ns="<<context_.candidate.properties.limits.timestampPeriod
        <<" gpu_reference_ms="<<timestamps_->milliseconds(0)
        <<" gpu_reference_corr_ms="<<timestamps_->milliseconds(1)
        <<" gpu_factor_ms="<<timestamps_->milliseconds(2)<<'\n';
      return true;
    } catch(const ClutterRejected& error) { return rejectFrame(error,"reference",0,0); }
      catch(const std::domain_error& error) { return rejectFrame(error,"reference",0,0); }
      catch(...) { frameFailed_=true;
      endFrame();
      throw; }
  }
public:
  void uploadSurveillance(const std::complex<float>* surveillance,size_t count) {
    if (resident_) throw std::logic_error("Host upload forbidden in resident clutter");
    if(direct_) {
      copyClutterBatch(static_cast<std::complex<float>*>(surveillance_->mapped),
        size_t(samples_)*channels_,surveillance,count);
        surveillance_->flush(0,surveillance_->bytes);
    } else {
      copyClutterBatch(static_cast<std::complex<float>*>(input_->mapped)+samples_,
        size_t(samples_)*channels_,surveillance,count);
      input_->flush(size_t(samples_)*8,size_t(samples_)*channels_*8);
    }
  }
  bool process(const std::complex<float>* reference,size_t referenceCount,
      const std::complex<float>* surveillance,size_t surveillanceCount,
      std::complex<float>* output,size_t outputCount,uint32_t firstChannel) {
    if (resident_) throw std::logic_error("Host result path forbidden in resident clutter");
    if(!reference || !surveillance || !output || referenceCount!=samples_ ||
        !surveillanceCount || surveillanceCount>uint64_t(samples_)*channels_ ||
        surveillanceCount%samples_ || outputCount!=surveillanceCount)
      throw std::invalid_argument("GPU clutter buffer dimensions changed");
    const auto start=std::chrono::steady_clock::now();
    if(!frameReady_) throw std::logic_error("GPU clutter frame is not prepared");
    return executeBatch(output, outputCount, firstChannel, uint32_t(surveillanceCount/samples_), start, surveillance);
  }
  bool processResident(uint32_t firstChannel, uint32_t active) {
    requireResident();
    if (!active || active > channels_) throw std::invalid_argument("Resident batch dimensions changed");
    return executeBatch(nullptr, size_t(samples_)*active, firstChannel, active,
      std::chrono::steady_clock::now());
  }
private:
  bool executeBatch(std::complex<float>* output, size_t outputCount,
      uint32_t firstChannel, uint32_t active, std::chrono::steady_clock::time_point start,
      const std::complex<float>* surveillance = nullptr) {
    if(!frameReady_) throw std::logic_error("GPU clutter frame is not prepared");
    const size_t surveillanceCount = size_t(samples_)*active;
    try {
      if(firstChannel!=completedChannels_) throw std::logic_error("GPU-native batch order changed");
      if (!resident_) uploadSurveillance(surveillance,surveillanceCount);
      native_->resetStatus(active);
      submit(batch_,"GPU-native clutter batch");
      const auto status=native_->readStatus();
      if(status[gpu_native::baselineBlocks]!=active*native_->blocks() ||
          status[gpu_native::residualSamples]!=surveillanceCount ||
          status[gpu_native::mergeSamples]!=surveillanceCount ||
          status[gpu_native::inputSamples]!=surveillanceCount ||
          status[gpu_native::solves]!=2*active || status[gpu_native::weightFfts]!=2*active) {
        std::ostringstream message;
        message << "GPU-native batch coverage mismatch"
          << " first_channel=" << firstChannel << " active=" << active
          << " capacity=" << channels_
          << " baseline_blocks=" << status[gpu_native::baselineBlocks]
          << "/" << active*native_->blocks()
          << " residual_samples=" << status[gpu_native::residualSamples]
          << "/" << surveillanceCount
          << " merge_samples=" << status[gpu_native::mergeSamples]
          << "/" << surveillanceCount
          << " input_samples=" << status[gpu_native::inputSamples]
          << "/" << surveillanceCount
          << " solves=" << status[gpu_native::solves] << "/" << 2*active
          << " weight_ffts=" << status[gpu_native::weightFfts] << "/" << 2*active;
        throw std::runtime_error(message.str());
      }
      if (!resident_) {if(!output)throw std::logic_error("GPU clutter host output is missing");
        Buffer& result=direct_?*estimate_:*output_;
        result.invalidate(0,result.bytes);
        std::memcpy(output,result.mapped,outputCount*8);
      }
      ++completedBatches_;
      completedChannels_+=active;
      const double wall=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
      context_.diagnostics.stream()<<std::setprecision(12)<<"GPU_NATIVE_BATCH frame="<<generation_
        <<" first_channel="<<firstChannel<<" active_channels="<<active<<" capacity_channels="<<channels_
        <<" device_baseline_blocks="<<status[gpu_native::baselineBlocks]
        <<" device_residual_samples="<<status[gpu_native::residualSamples]
        <<" device_merge_samples="<<status[gpu_native::mergeSamples]
        <<" device_solves="<<status[gpu_native::solves]<<" device_weight_ffts="<<status[gpu_native::weightFfts]
        <<" cpu_baseline_calls=0 cpu_solve_calls=0";
      const char* names[]={"initial_corr","initial_solve","baseline","residual","residual_corr",
        "correction_solve","correction_fir","merge","readback"};
      for(uint32_t i=0;i<9;++i) context_.diagnostics.stream()<<" gpu_"<<names[i]<<"_ms="<<timestamps_->milliseconds(i+3);
      if(completedBatches_>batchEvidence_.size())throw std::runtime_error("Native batch evidence overflow");
      auto& evidence=batchEvidence_[completedBatches_-1];
      evidence.first=firstChannel;
      evidence.active=active;
      evidence.status=status;
      evidence.hostMs=wall;
      for(uint32_t i=0;i<9;++i)evidence.times[i]=timestamps_->observed(i+3);
      context_.diagnostics.stream()<<" host_batch_ms="<<wall<<'\n';
      return true;
    } catch(const ClutterRejected& error) { return rejectFrame(error,"channel_batch",firstChannel,active); }
      catch(const std::domain_error& error) { return rejectFrame(error,"channel_batch",firstChannel,active); }
      catch(...) { frameFailed_=true;
      endFrame();
      throw; }
  }
};

#include "GpuResidentFrame.private.h"

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
